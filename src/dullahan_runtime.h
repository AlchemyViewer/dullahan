/*
    @brief Dullahan - a headless browser rendering engine
           based around the Chromium Embedded Framework

           dullahan_runtime: the process-global CEF runtime.

           CEF allows exactly one CefInitialize() per process and a single
           CefApp. This class owns both, reference counted across every
           dullahan browser instance in the process, so that many browsers can
           share one CEF runtime (the basis for the shared tab-manager daemon).
           The first browser to acquire() configures the process-global
           command-line flags; the last to release() shuts CEF down.

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

#ifndef _DULLAHAN_RUNTIME
#define _DULLAHAN_RUNTIME

#include <string>
#include <mutex>
#include <chrono>

#include "cef_app.h"
#include "cef_browser_process_handler.h"

#include "dullahan.h"

class dullahan_runtime :
    public CefApp,
    public CefBrowserProcessHandler
{
    public:
        // The process-wide singleton. Safe to call before CEF is initialized.
        static dullahan_runtime& instance();

        // Ensure CEF is initialized (idempotent) and register one live browser.
        // The first successful call captures the process-global command-line
        // flags and settings from user_settings; later calls reuse the running
        // runtime and ignore their settings (CEF is process-global). Returns
        // true if the CEF runtime is up.
        bool acquire(dullahan::dullahan_settings& user_settings);

        // Unregister one live browser. In the default (per-process) mode this
        // shuts CEF down once the last live browser is released - correct when the
        // process is about to exit anyway. In a persistent host (see
        // setPersistent) it only decrements the count and leaves CEF running, so a
        // brief zero-browser gap (e.g. the login web surface closing just before
        // the next one opens) does NOT tear CEF down - re-CefInitialize would
        // crash, since CEF cannot be initialized twice in a process.
        void release();

        // A persistent host (the SLPluginCEF bootstrap - dedicated single tab or
        // the shared daemon) keeps one CEF runtime for the life of the process and
        // shuts it down exactly once, via shutdownIfRunning() at host-loop exit,
        // rather than letting the browser refcount drive CefShutdown. Must be set
        // before the first acquire(). Without this, a zero-browser moment shuts
        // CEF down and the next acquire() crashes trying to re-initialize it.
        void setPersistent(bool b) { mPersistent = b; }

        // Final teardown for a persistent host: shut CEF down once if it is still
        // up. Call after the host message loop returns and before process exit.
        // A no-op in per-process mode (release() already shut CEF down).
        void shutdownIfRunning();

        bool isInitialized() const { return mInitialized; }

        // Provide the Windows sandbox information object (from the CEF bootstrap
        // host's RunWinMain / a CefScopedSandboxInfo). When set, CefInitialize
        // runs with the sandbox enabled and CEF re-launches this same executable
        // image for its sub-processes (so browser_subprocess_path is not set).
        // NULL (the default) keeps the legacy no-sandbox + dullahan_host helper
        // behaviour. Must be called before the first acquire().
        void setSandboxInfo(void* sandbox_info) { mSandboxInfo = sandbox_info; }
        void* getSandboxInfo() const { return mSandboxInfo; }

        // When true, this host dispatches CEF sub-processes itself by having CEF
        // re-launch this same executable image (its entry runs CefExecuteProcess
        // - e.g. the SLPluginCEF bootstrap), so browser_subprocess_path is left
        // unset (CEF defaults to the current image). When false (the legacy
        // dlopen host) a separate dullahan_host.exe is used. INDEPENDENT of the
        // sandbox - a dedicated host should never fall back to dullahan_host just
        // because the sandbox is off. Set before the first acquire().
        void setHostHandlesSubprocesses(bool b) { mHostHandlesSubprocesses = b; }

        // shared message pump - see dullahan_impl::update() / run()
        void update();
        void run();

        // CefApp overrides
        void OnBeforeCommandLineProcessing(const CefString& process_type,
                                           CefRefPtr<CefCommandLine> command_line) override;
        CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

        // CefBrowserProcessHandler overrides
        // With external_message_pump, CEF calls this (from any thread) to ask for
        // a CefDoMessageLoopWork() at most |delay_ms| from now. We record the
        // deadline and let update() honour it, so the host pump only runs CEF work
        // when CEF actually has some - instead of every idle tick (the fixed-
        // cadence pump was the cause of the constant idle CPU use).
        void OnScheduleMessagePumpWork(int64_t delay_ms) override;

    private:
        dullahan_runtime();

        bool initCEF(dullahan::dullahan_settings& user_settings);
        void platormInitWidevine(std::string cachePath);
        void platformAddCommandLines(CefRefPtr<CefCommandLine> command_line);

        bool mInitialized;
        int  mLiveBrowsers;
        void* mSandboxInfo;
        bool mHostHandlesSubprocesses;
        // Persistent host: keep CEF up across zero-browser gaps (see setPersistent).
        bool mPersistent;
        // Latched once CefShutdown() has run. CEF cannot be re-initialized in a
        // process, so acquire() refuses to try (returns false) rather than crash.
        bool mTerminated;

        // external-message-pump scheduling (see OnScheduleMessagePumpWork). Guards
        // a single pending deadline shared by every tab's update() call. mutable so
        // update() can consume it. Touched from the CEF UI thread (schedule) and
        // the host pump thread (consume), so guarded by mPumpMutex.
        std::mutex mPumpMutex;
        bool mPumpPending;                                  // pump as soon as possible
        bool mHasPumpDeadline;                              // a delayed pump is scheduled
        std::chrono::steady_clock::time_point mPumpDeadline;
        std::chrono::steady_clock::time_point mLastPumpTime; // watchdog: last actual pump

        // process-global command-line flags, captured on the first acquire()
        bool mMediaStreamEnabled;
        bool mBeginFrameScheduling;
        bool mForceWaveAudio;
        bool mDisableGPU;
        bool mDisableWebSecurity;
        bool mAllowFileAccessFromFiles;
        bool mUseMockKeyChain;
        bool mAutoPlayWithoutGesture;
        bool mFakeUIForMediaStream;
        std::string mProxyHostPort;

        IMPLEMENT_REFCOUNTING(dullahan_runtime);
};

#endif // _DULLAHAN_RUNTIME
