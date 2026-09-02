/*
    @brief Dullahan - a headless browser rendering engine
           based around the Chromium Embedded Framework

           dullahan_runtime: the process-global CEF runtime (see header).

    @author Callum Prentice 2017

    Copyright (c) 2017, Linden Research, Inc.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif

#include "dullahan_runtime.h"
#include "dullahan_debug.h"

#include <thread>

#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#endif

#if WIN32
#include <winnls.h> // for WideCharToMultiByte
#endif

#include <vector>

#if LL_LINUX
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <iostream>
#endif

// Watchdog ceiling for the external message pump: never let CefDoMessageLoopWork()
// go un-called for longer than this, even if OnScheduleMessagePumpWork hasn't asked
// for it. CEF's external-pump scheduling only re-notifies when the next-needed-pump
// time changes, so a coalesced schedule (notably during a resize, where a needed
// repaint can be left unscheduled) could otherwise leave the surface blank with
// nothing to re-trigger a pump. ~10 no-op pumps/sec when idle is far below the old
// every-tick (~100/sec/tab) pumping, so the CPU win is preserved.
static constexpr std::chrono::milliseconds DULLAHAN_MAX_PUMP_INTERVAL{ 100 };

namespace
{
#ifdef WIN32
    // copied from viewer's llstring.h
    std::string convert_wide_to_string(const wchar_t* in, unsigned int code_page)
    {
        std::string out;
        if (in)
        {
            int len_in = (int)wcslen(in);
            int len_out = WideCharToMultiByte(code_page, 0, in, len_in, NULL, 0, 0, 0);
            // We will need two more bytes for the double NULL ending
            // created in WideCharToMultiByte().
            char* pout = new char[len_out + 2];
            memset(pout, 0, len_out + 2);
            if (pout)
            {
                WideCharToMultiByte(code_page, 0, in, len_in, pout, len_out, 0, 0);
                out.assign(pout);
                delete[] pout;
            }
        }
        return out;
    }
#endif

#if LL_LINUX
    std::string getExeCwd()
    {
        char path[ 4096 ];
        int len = readlink("/proc/self/exe", path, sizeof(path));
        if (len == -1)
            return "";

        path[len] = 0;
        return dirname(path) ;
    }
#endif
}

// static
dullahan_runtime& dullahan_runtime::instance()
{
    // Held alive for the life of the process by this CefRefPtr; CefInitialize()
    // also retains a reference for as long as CEF is running.
    static CefRefPtr<dullahan_runtime> sInstance(new dullahan_runtime());
    return *sInstance;
}

dullahan_runtime::dullahan_runtime() :
    mInitialized(false),
    mLiveBrowsers(0),
    mSandboxInfo(nullptr),
    mHostHandlesSubprocesses(false),
    mPersistent(false),
    mTerminated(false),
    mPumpPending(true),                 // pump once on startup to get CEF going
    mHasPumpDeadline(false),
    mMediaStreamEnabled(false),
    mBeginFrameScheduling(false),
    mForceWaveAudio(false),
    mDisableGPU(true),
    mDisableWebSecurity(false),
    mAllowFileAccessFromFiles(false),
    mUseMockKeyChain(false),
    mAutoPlayWithoutGesture(false),
    mFakeUIForMediaStream(false)
{
    DLNOUT("dullahan_runtime::dullahan_runtime()");
}

bool dullahan_runtime::acquire(dullahan::dullahan_settings& user_settings)
{
    if (!mInitialized)
    {
        if (mTerminated)
        {
            // CEF was already shut down in this process and cannot be brought
            // back (CefInitialize is once-per-process). Refuse rather than crash;
            // a persistent host keeps CEF up so it should never reach here.
            DLNOUT("dullahan_runtime::acquire() refused - CEF already shut down");
            return false;
        }

        platormInitWidevine(user_settings.root_cache_path);

        if (!initCEF(user_settings))
        {
            return false;
        }

        mInitialized = true;

        // One-time settle. After CefInitialize the process-global request context
        // (CefRequestContext::GetGlobalContext(), shared by every browser in this
        // process) finishes initializing asynchronously; creating a browser
        // against it too soon intermittently fails. Pump the loop briefly to let
        // it complete. This runs ONCE per process - only the first browser pays
        // it, and at that point there are no other browsers to stall. Later daemon
        // tabs join the already-initialized runtime and skip straight to creating
        // their browser. (Previously every dullahan_impl::init() paid this ~50ms,
        // which in the daemon froze every other tab on each new tab.)
        const int settle_loops = 10;
        const int settle_sleep_ms = 5;
        for (int i = 0; i < settle_loops; ++i)
        {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(settle_sleep_ms));
        }
    }

    ++mLiveBrowsers;
    return mInitialized;
}

void dullahan_runtime::release()
{
    if (mLiveBrowsers > 0)
    {
        --mLiveBrowsers;
    }

    // A persistent host (the SLPluginCEF bootstrap / shared daemon) keeps CEF up
    // across zero-browser gaps and shuts it down once at process exit via
    // shutdownIfRunning(). Tearing CEF down here would crash the next acquire(),
    // since CEF cannot be re-initialized in a process - that is the login-time
    // crash when the login web surface closes just before the next one opens.
    if (mLiveBrowsers == 0 && mInitialized && !mPersistent)
    {
        CefShutdown();
        mInitialized = false;
        mTerminated = true;
    }
}

void dullahan_runtime::shutdownIfRunning()
{
    if (mInitialized)
    {
        CefShutdown();
        mInitialized = false;
        mTerminated = true;
        mLiveBrowsers = 0;
    }
}

void dullahan_runtime::update()
{
    if (!mInitialized)
    {
        return;
    }

    // Only pump CEF when it has asked us to (via OnScheduleMessagePumpWork) or
    // the requested delay has elapsed. When the browser is idle CEF schedules
    // far-future work, so most host idle ticks become no-ops here - that is the
    // whole point: no more pumping the message loop 100x/sec for nothing. In the
    // daemon this deadline is process-global, so N tabs calling update() in one
    // frame still pump the shared loop at most once.
    const auto now = std::chrono::steady_clock::now();
    bool do_pump = false;
    {
        std::lock_guard<std::mutex> lock(mPumpMutex);
        if (mPumpPending)
        {
            do_pump = true;
            mPumpPending = false;
        }
        else if (mHasPumpDeadline && now >= mPumpDeadline)
        {
            do_pump = true;
            mHasPumpDeadline = false;
        }
        else if (now - mLastPumpTime >= DULLAHAN_MAX_PUMP_INTERVAL)
        {
            // Watchdog: a needed pump may have been coalesced away (see
            // DULLAHAN_MAX_PUMP_INTERVAL) - keep the loop alive so e.g. a resized
            // surface can't blank out permanently.
            do_pump = true;
        }

        if (do_pump)
        {
            mLastPumpTime = now;
        }
    }

    if (do_pump)
    {
        // CefDoMessageLoopWork() will normally re-arm us via a fresh
        // OnScheduleMessagePumpWork() call before it returns.
        CefDoMessageLoopWork();
    }
}

void dullahan_runtime::run()
{
    CefRunMessageLoop();
}

void dullahan_runtime::OnScheduleMessagePumpWork(int64_t delay_ms)
{
    // Called from any thread. Record what CEF asked for; update() acts on it on
    // the main/host thread (the only thread allowed to call CefDoMessageLoopWork).
    std::lock_guard<std::mutex> lock(mPumpMutex);
    if (delay_ms <= 0)
    {
        // "reasonably soon" - pump on the next update() tick.
        mPumpPending = true;
        mHasPumpDeadline = false;
    }
    else
    {
        // Schedule after the delay, cancelling any previously pending delayed
        // call (per the CefBrowserProcessHandler contract). A pending immediate
        // pump still wins - don't downgrade it to a delayed one.
        mHasPumpDeadline = true;
        mPumpDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    }
}

void dullahan_runtime::OnBeforeCommandLineProcessing(const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line)
{
    if (process_type.empty())
    {
        if (mMediaStreamEnabled == true)
        {
            command_line->AppendSwitch("enable-media-stream");
        }

        if (mBeginFrameScheduling == true)
        {
            command_line->AppendSwitch("enable-begin-frame-scheduling");
        }

        // The ability to access local files used to be a member of CefBrowserSettings but
        // now is is configured globally via command line switch (https://github.com/cefsharp/CefSharp/issues/3668)
        if (mAllowFileAccessFromFiles == true)
        {
            command_line->AppendSwitch("allow-file-access-from-files");
        }

        // <ND> n.b. be careful enabling this. At least on Linux it will break sites like twitch.tv mixer.com, dlive.com.
        // Probably this also makes only sense for Win32?
        if (mDisableGPU == true)
        {
            command_line->AppendSwitch("disable-gpu");
            command_line->AppendSwitch("disable-gpu-compositing");
        }

        if (mDisableWebSecurity)
        {
            command_line->AppendSwitch("disable-web-security");
        }

        if (mUseMockKeyChain)
        {
            command_line->AppendSwitch("use-mock-keychain");
        }

        if (mAutoPlayWithoutGesture)
        {
            command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        }

        if (mFakeUIForMediaStream)
        {
            command_line->AppendSwitch("use-fake-ui-for-media-stream");
        }

        if (mProxyHostPort.length())
        {
            command_line->AppendSwitchWithValue("--proxy-server", mProxyHostPort);
        }

        // Suppress the EULA/first-run dialog — dullahan is an embedded
        // browser, not a standalone Chrome instance.
        command_line->AppendSwitch("no-first-run");
        command_line->AppendSwitch("no-default-browser-check");

        platformAddCommandLines(command_line);
    }
}

bool dullahan_runtime::initCEF(dullahan::dullahan_settings& user_settings)
{
#ifdef WIN32
    CefMainArgs args(GetModuleHandle(nullptr));
#elif __APPLE__
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain())
    {
        return false;
    }

    CefMainArgs args(0, nullptr);
#endif
#ifdef __linux__
    CefMainArgs args(0, nullptr);
#endif

    CefSettings settings;

    // point to host application helper
#ifdef WIN32
    // The sandbox is enabled only when a sandbox-info object was supplied (by
    // the CEF bootstrap host). This is independent of who runs the sub-processes.
    settings.no_sandbox = (mSandboxInfo == nullptr);

    if (mSandboxInfo || mHostHandlesSubprocesses)
    {
        // SLPluginCEF (a CEF bootstrap host): CEF re-launches this same
        // executable image for its sub-processes - the renamed bootstrap.exe
        // loads our RunWinMain DLL, which runs CefExecuteProcess. So leave
        // browser_subprocess_path unset (CEF defaults to the current image).
        // Required by the sandbox, and used whether or not the sandbox is active
        // so the dedicated host never falls back to the dullahan_host helper.
    }
    else
    {
        // Legacy dlopen host (generic SLPlugin + media_plugin_cef.dll): the
        // dlopened plugin has no CefExecuteProcess entry, so a separate
        // dullahan_host.exe runs the CEF sub-processes.
        //
        // Note: as of CEF 83, it appears that on Windows builds, the path to the
        // host helper application must be an absolute path vs the existing,
        // relative path. If the user has not specified a path to the helper
        // explicitly, then we can, as a first pass, assume it's located next to
        // the executable (it often is) and for use cases where it is located
        // elsewhere, the consumer can specify the absolute path directly.
        std::string host_process_path = user_settings.host_process_path;
        if (host_process_path.empty())
        {
            // path is not specified so assume it's adjacent to the executable
            std::vector<wchar_t> exe_path(MAX_PATH + 1);
            GetModuleFileNameW(NULL, &exe_path[0], MAX_PATH);
            std::string cur_exe_path = convert_wide_to_string(&exe_path[0], CP_UTF8);
            const size_t last_slash_idx = cur_exe_path.find_last_of("\\/");
            if (last_slash_idx == std::string::npos)
            {
                return false;
            }
            host_process_path = cur_exe_path.erase(last_slash_idx + 1);
        }

        // finally, tell CEF where to find the host process helper
        CefString(&settings.browser_subprocess_path) = host_process_path + "\\" + user_settings.host_process_filename;
    }
#elif __APPLE__
    NSString* appBundlePath = [[NSBundle mainBundle] bundlePath];
    CefString(&settings.browser_subprocess_path) =
        [[NSString stringWithFormat:
          @"%@/Contents/Frameworks/DullahanHelper.app/Contents/MacOS/DullahanHelper", appBundlePath] UTF8String];

    CefString(&settings.framework_dir_path) =
    [[NSString stringWithFormat:
      @"%@/Contents/Frameworks/Chromium Embedded Framework.framework", appBundlePath] UTF8String];

    settings.no_sandbox = true;
#elif __linux__
    CefString(&settings.browser_subprocess_path) = getExeCwd() + "/dullahan_host";
    bool useSandbox = false;
    std::string sandboxName = getExeCwd() + "/chrome-sandbox";
    struct stat st;

    if (!stat(sandboxName.c_str(), &st))
    {
        // Sandbox must be owned by root:root and has the suid bit set, otherwise cef won't use it.
        if (st.st_uid == 0 && st.st_gid == 0 && (st.st_mode & S_ISUID) == S_ISUID)
        {
            useSandbox = true;
        }
    }

    settings.no_sandbox = !useSandbox;
#else
#error "Unsupported Platform"
#endif
    // required for CEF 72+ to indicate headless
    settings.windowless_rendering_enabled = true;

    // CEF header file suggest that we need this now
    settings.external_message_pump = true;

    // use a single thread for the message loop
    settings.multi_threaded_message_loop = false;

    // act like a browser and do not persist session cookies ever
    settings.persist_session_cookies = user_settings.cookies_enabled;

    // explicitly set the path to the locales folder since defaults no longer work on some systems
    CefString(&settings.locales_dir_path) = user_settings.locales_dir_path;

    // set path to root cache if enabled and set
    CefString(&settings.root_cache_path) = user_settings.root_cache_path;
#ifdef WIN32
    CefString(&settings.cache_path) = user_settings.root_cache_path + "\\" + "cache";
#else
    CefString(&settings.cache_path) = user_settings.root_cache_path + "/" + "cache";
#endif

    // as of CEF 90, the new way to disable cookies
    if (user_settings.cookies_enabled == false)
    {
        CefString(&settings.cookieable_schemes_list) = "";
        settings.cookieable_schemes_exclude_defaults = true;
    }

    // insert a new string into user agent. dullahan_impl::init() guarantees a
    // resolved user-agent string is present before the runtime is acquired.
    if (user_settings.user_agent_substring.length())
    {
        std::string user_agent(user_settings.user_agent_substring);
        cef_string_utf8_to_utf16(user_agent.c_str(), user_agent.size(), &settings.user_agent_product);
    }

    // the proxy host:port to use
    mProxyHostPort = user_settings.proxy_host_port;

    // Linux: the host app can pin the Ozone backend (X11 vs Wayland) here; see
    // platformAddCommandLines. Empty leaves the env-based auto-detect in place.
    mOzonePlatform = user_settings.ozone_platform;

    // list of language locale codes used to configure the Accept-Language HTTP header value
    if (user_settings.accept_language_list.length())
    {
        std::string accept_language_list(user_settings.accept_language_list);
        cef_string_utf8_to_utf16(accept_language_list.c_str(),
                                 accept_language_list.size(), &settings.accept_language_list);
    }

    // enable/disable media stream (web cams etc.)
    // IMPORTANT: there is no "Use Your WebCam OK?" dialog so enable this at your peril
    mMediaStreamEnabled = user_settings.media_stream_enabled;

    // this flag needed for some video cards to force onPaints to work - off by default
    mBeginFrameScheduling = user_settings.begin_frame_scheduling;

#ifdef WIN32
    // this flag forces Windows WaveOut/In audio API even if Core Audio is supported
    mForceWaveAudio = user_settings.force_wave_audio;
#endif

    // this flag is set to enable zero-copy gpu paint
    mAcceleratedPaint = user_settings.accelerated_paint;

    // this flag if set, adds command line options to disable the GPU and GPU compositing.
    // Accelerated paint hands us a GPU shared texture, which requires GPU
    // compositing - so it always wins over a disable_gpu request.
    mDisableGPU = user_settings.disable_gpu && !user_settings.accelerated_paint;

    // this flag if set, adds command line parameters to disable the web security component
    mDisableWebSecurity = user_settings.disable_web_security;

    // this flag allows access to local files - now must be set via the command line
    mAllowFileAccessFromFiles = user_settings.file_access_from_file_urls;

    // this flag if set, bypasses the macOS "Chrome wants access to passwords" dialog
    mUseMockKeyChain = user_settings.use_mock_keychain;

    // this flag, if set, allows video/audio to autoplay
    mAutoPlayWithoutGesture = user_settings.autoplay_without_gesture;

    // this flag, if set allows you to bypass UI like "This page wants to use your microphone"
    mFakeUIForMediaStream = user_settings.fake_ui_for_media_stream;

    // log file settings
    CefString(&settings.log_file) = user_settings.log_file;
    settings.log_severity = user_settings.log_verbose ? LOGSEVERITY_VERBOSE : LOGSEVERITY_DEFAULT;

    if (user_settings.enable_remote_debug)
    {
        // allow Chrome (or other CEF windoW) to debug at http://localhost::PORT_NUMBER
        settings.remote_debugging_port = user_settings.remote_debugging_port;
    }

    // initiaize CEF (mSandboxInfo is non-null only in the Windows bootstrap
    // sandbox host; nullptr everywhere else preserves the legacy behaviour)
    bool result = CefInitialize(args, settings, this, mSandboxInfo);
    return result;
}
