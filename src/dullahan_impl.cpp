/*
    @brief Dullahan - a headless browser rendering engine
           based around the Chromium Embedded Framework
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


#include "dullahan_impl.h"
#include "dullahan_runtime.h"
#include "dullahan_render_handler.h"
#include "dullahan_browser_client.h"
#include "dullahan_callback_manager.h"

#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_waitable_event.h"
#include "include/base/cef_logging.h"

#include "dullahan_version.h"
#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#endif

#if WIN32
#include <winnls.h> // for WideCharToMultiByte
#endif

#include <iostream>
#include <chrono>
#include <thread>

#if LL_LINUX
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <pwd.h>
#include <string>
#include <fstream>
#include <dirent.h>
#include <iostream>
#endif

dullahan_impl::dullahan_impl() :
    mInitialized(false),
    mBrowser(nullptr),
    mCallbackManager(new dullahan_callback_manager),
    mViewWidth(0),
    mViewHeight(0),
    mFlipPixelsY(false),
    mFlipMouseY(false),
    mRequestContext(nullptr),
    mRequestedPageZoom(1.0)
{
    DLNOUT("dullahan_impl::dullahan_impl()");
}

dullahan_impl::~dullahan_impl()
{
    DLNOUT("dullahan_impl::~dullahan_impl()");
    delete mCallbackManager;
    mCallbackManager = nullptr;
}

bool dullahan_impl::init(dullahan::dullahan_settings& user_settings)
{
    DLNOUT("dullahan_impl::init()");

    // per-browser flip settings (the process-global flags are owned by the runtime)
    mFlipPixelsY = user_settings.flip_pixels_y;
    mFlipMouseY = user_settings.flip_mouse_y;

    // Resolve a user-agent string before bringing up the (possibly shared) CEF
    // runtime - it only honours the user_agent from the first browser to
    // initialize it. makeCompatibleUserAgentString lives here (per-browser), so
    // fill it in now rather than in the runtime.
    if (user_settings.user_agent_substring.empty())
    {
        user_settings.user_agent_substring = makeCompatibleUserAgentString("");
    }

    // bring up the process-global CEF runtime, or join the already-running one
    if (!dullahan_runtime::instance().acquire(user_settings))
    {
        return false;
    }

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = user_settings.frame_rate;
    browser_settings.webgl = user_settings.webgl_enabled ? STATE_ENABLED : STATE_DISABLED;
    browser_settings.javascript = user_settings.javascript_enabled ? STATE_ENABLED : STATE_DISABLED;
    browser_settings.background_color = user_settings.background_color;
    browser_settings.image_shrink_standalone_to_fit = user_settings.image_shrink_standalone_to_fit ? STATE_ENABLED : STATE_DISABLED;

    mRenderHandler = new dullahan_render_handler(this);
    mBrowserClient = new dullahan_browser_client(this, mRenderHandler);

    // Windowspecific settings for OSR
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
    window_info.windowless_rendering_enabled = true;
    // Zero-copy paint: ask CEF to hand us a GPU shared texture (OnAcceleratedPaint)
    // instead of a CPU pixel buffer. Requires GPU compositing, which the runtime
    // forces on for this mode (see dullahan_runtime::initCEF).
    window_info.shared_texture_enabled = user_settings.accelerated_paint ? true : false;
    const int width = user_settings.initial_width;
    const int height = user_settings.initial_height;
    window_info.bounds = { 0, 0, width, height };

    // Every browser in this process shares the global request context. The
    // one-time settle that lets it finish initializing now happens once in
    // dullahan_runtime::acquire() (above), so a daemon tab created later does not
    // re-pay it - it is ready by the time we get here.
    mRequestContext = CefRequestContext::GetGlobalContext();

    // browser for this instance - empty URL and no extra_info
    mBrowser = CefBrowserHost::CreateBrowserSync(window_info, mBrowserClient.get(), std::string(), browser_settings, nullptr, mRequestContext.get());

    // important: set the size *after* we create a browser
    setSize(user_settings.initial_width, user_settings.initial_height);

    // recent versions of CEF seem to be pickier (rightly so) about calling dullahan_impl::update()
    // before initialization has completed so we should block that until we're fully complete here
    mInitialized = true;

    return true;
}

void dullahan_impl::shutdown()
{
    mBrowser = nullptr;
    mRenderHandler = nullptr;
    mBrowserClient = nullptr;
    mRequestContext = nullptr;
    mInitialized = false;

    // Release this browser's hold on the shared runtime; CEF is shut down once
    // the last live browser in the process releases it.
    dullahan_runtime::instance().release();
}

void dullahan_impl::requestExit()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        flushAllCookies();

        bool force_close = false;

        mBrowser->GetHost()->CloseBrowser(force_close);
    }
}

void dullahan_impl::getSize(int& width, int& height)
{
    width = mViewWidth;
    height = mViewHeight;
}

void dullahan_impl::setSize(int width, int height)
{
    DLNOUT("dullahan_impl::setSize() " << width << " x " << height);

    // Always record the requested size, even before the browser exists, so
    // getSize() is correct and a size set early isn't silently dropped. Only
    // notify the host of a resize once it's actually available.
    mViewWidth = width;
    mViewHeight = height;

    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->GetHost()->WasResized();
    }
}

int dullahan_impl::getDepth()
{
    return mViewDepth;
}

bool dullahan_impl::getFlipPixelsY()
{
    return mFlipPixelsY;
}

bool dullahan_impl::getFlipMouseY()
{
    return mFlipMouseY;
}

void dullahan_impl::run()
{
    dullahan_runtime::instance().run();
}

void dullahan_impl::update()
{
    if (! mInitialized)
    {
        return;
    }

    // pump the shared CEF message loop
    dullahan_runtime::instance().update();

    // CEF/Chromium resets page zoom in between pages
    // so we continually try to set it to the value selected
    // in calls to setPageZoom. Once the required zoom
    // level is reached this call is almost free.
    requestPageZoom();
}

bool dullahan_impl::canGoBack()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        return mBrowser->CanGoBack();
    }

    // no browser yet => there is no history to go back to
    return false;
}

void dullahan_impl::goBack()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->GoBack();
    }
}

bool dullahan_impl::canGoForward()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        return mBrowser->CanGoForward();
    }

    // no browser yet => there is no forward history
    return false;
}

void dullahan_impl::goForward()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->GoForward();
    }
}

bool dullahan_impl::isLoading()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        return mBrowser->IsLoading();
    }

    // no browser yet => nothing is loading
    return false;
}

void dullahan_impl::reload(const bool ignore_cache)
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        if (ignore_cache)
        {
            mBrowser->ReloadIgnoreCache();
        }
        else
        {
            mBrowser->Reload();
        }
    }
}

void dullahan_impl::stop()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->StopLoad();
    }
}

std::string dullahan_impl::makeCompatibleUserAgentString(const std::string base)
{
    std::string frag = "";
    frag += "(";
    frag += "Dullahan";
    frag += ":";
    frag += dullahan_version(true);
    frag += " - ";
    frag += base;
    frag += ")";
    frag += "  ";
    frag += "Chrome/";
    frag += dullahan_chrome_version(true);

    return frag;
}

void dullahan_impl::navigate(const std::string url)
{
    if (mBrowser.get() && mBrowser->GetMainFrame())
    {
        mBrowser->GetMainFrame()->LoadURL(url);
    }
}

void dullahan_impl::setFocus(bool focused)
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->GetHost()->SetFocus(focused);
    }
}


// The editCan*() queries report CEF's edit-state flags as captured by the
// browser client's OnBeforeContextMenu (CM_EDITFLAG_*). CEF provides no
// synchronous "can I do this?" call for offscreen rendering, so this reflects
// the state as of the last context-menu event (e.g. a right-click); it can be
// stale between such events. Before any context menu has fired mEditStateFlags
// is 0, so these return false.
bool dullahan_impl::editCanUndo()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_UNDO) != 0;
}

bool dullahan_impl::editCanRedo()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_REDO) != 0;
}

bool dullahan_impl::editCanCopy()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_COPY) != 0;
}

bool dullahan_impl::editCanCut()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_CUT) != 0;
}

bool dullahan_impl::editCanPaste()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_PASTE) != 0;
}

bool dullahan_impl::editCanDelete()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_DELETE) != 0;
}

bool dullahan_impl::editCanSelectAll()
{
    return (mEditStateFlags & CM_EDITFLAG_CAN_SELECT_ALL) != 0;
}

// GetFocusedFrame() can return null (e.g. between page loads, or when no frame
// holds focus), so each of these captures it once and null-checks before use
// rather than calling through the result of GetFocusedFrame() directly.
void dullahan_impl::editUndo()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Undo();
    }
}

void dullahan_impl::editRedo()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Redo();
    }
}

void dullahan_impl::editCopy()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Copy();
    }
}

void dullahan_impl::editCut()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Cut();
    }
}

void dullahan_impl::editPaste()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Paste();
    }
}

void dullahan_impl::editDelete()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->Delete();
    }
}

void dullahan_impl::editSelectAll()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->SelectAll();
    }
}

void dullahan_impl::viewSource()
{
    if (!mBrowser.get())
    {
        return;
    }
    CefRefPtr<CefFrame> frame = mBrowser->GetFocusedFrame();
    if (frame)
    {
        frame->ViewSource();
    }
}

// Internal call to request that zoom level you asked for with setPageZoom() be actioned.
// We need to do it like this because a plain call to the CEF code SetZoomLevel() can fail
// if then complex multi-process nature of CEF isn't yet established. This is potentially
// called multiple times - likely from CefLoadHandler::OnLoadingStateChange(..)
void dullahan_impl::requestPageZoom()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        // special case the non-zoomed version since slight floating point rounding errors
        // in the formula below result in a few pixels difference - best example of this is
        // when 1024x1024 images in 1024x1024 browser cause scroll bars to appear
        if (mRequestedPageZoom == 1.0)
        {
            // only set zoom if needed remembering Dullahan zoom 1.0 == CEF zoon 0.0 :)
            if (mBrowser->GetHost()->GetZoomLevel() != 0.0)
            {
                // reset zoom level according to CEF docs
                mBrowser->GetHost()->SetZoomLevel(0.0);
                return;
            }
        }

        // Convert "Dullahan page zoom" to "CEF/Chromium page zoom"
        // The value we pass into CEF::SetZoomLevel is not on a linear scale and described here:
        // http://www.magpcss.org/ceforum/viewtopic.php?f=6&t=11491
        // Dullahan scale: 1.0 is 1:1 scale, 2.0 is double, 0.5 is half etc.
        // CEF scale is more complex :) and from that post above, this is the best we can do for now:
        // note: CEF/Chromium max scale seems to be 5 x normal - values higher than that are ignored
        double cef_zoom_level = 5.46149645 * log(mRequestedPageZoom * 100.0) - 25.1511206;

        // if the zoom has not been established (being careful for floating point issues)
        if (fabs(mBrowser->GetHost()->GetZoomLevel() - cef_zoom_level) > 0.001)
        {
            mBrowser->GetHost()->SetZoomLevel(cef_zoom_level);
        }
    }
}

// Set the page zoom directly.  once the page is loaded and waiting say, this will
// work as expected but if init() was just called, the CEF setup might still be in
// progress and this will not do anything - however, requestPageZoom() is called
// often and the zoom will eventually be actioned.
void dullahan_impl::setPageZoom(const double zoom_val)
{
    mRequestedPageZoom = zoom_val;

    requestPageZoom();
}

void dullahan_impl::showDevTools()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        CefWindowInfo window_info;
        window_info.bounds = { 0,0, 600, 800 };
#ifdef WIN32
        window_info.SetAsPopup(nullptr, "Dullahan Dev Tools");
#elif __APPLE__
        // TODO: need Apple equivalent
#endif
        CefRefPtr<CefClient> client = mBrowserClient;
        CefBrowserSettings browser_settings;
        CefPoint inspect_element_at;
        mBrowser->GetHost()->ShowDevTools(window_info, client, browser_settings,
                                          inspect_element_at);
    }
}

void dullahan_impl::closeDevTools()
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        mBrowser->GetHost()->CloseDevTools();
    }
}

void dullahan_impl::printToPDF(const std::string path)
{
    if (mBrowser.get() && mBrowser->GetHost())
    {
        CefPdfPrintSettings settings;
        settings.print_background = true;
        settings.display_header_footer = true;
        settings.landscape = true;
        CefRefPtr<CefPdfPrintCallback> callback = this;
        mBrowser->GetHost()->PrintToPDF(path, settings, callback);
    }
}

void dullahan_impl::OnPdfPrintFinished(const CefString& path, bool ok)
{
    // this can fire asynchronously, potentially during teardown after the
    // callback manager has been destroyed, so guard against that
    if (mCallbackManager)
    {
        mCallbackManager->onPdfPrintFinished(path, ok);
    }
}

bool dullahan_impl::setCookie(const std::string url, const std::string name,
                              const std::string value, const std::string domain,
                              const std::string path, bool httponly, bool secure)
{
    CefRefPtr<CefCookieManager> manager;

    if (mRequestContext)
    {
        manager = mRequestContext->GetCookieManager(nullptr);
    }
    else
    {
        manager = CefCookieManager::GetGlobalManager(nullptr);
    }

    if (manager)
    {
        CefCookie cookie;
        CefString(&cookie.name) = name;
        CefString(&cookie.value) = value;
        CefString(&cookie.domain) = domain;
        CefString(&cookie.path) = path;

        cookie.httponly = httponly;
        cookie.secure = secure;

        cookie.has_expires = false;

        // wait for cookie to be set in setCookie callback
        class setCookieCallback :
            public CefSetCookieCallback
        {
            public:
                explicit setCookieCallback(CefRefPtr<CefWaitableEvent> event)
                    : mEvent(event)
                {
                }

                void OnComplete(bool success) override
                {
                    mEvent->Signal();
                }

            private:
                CefRefPtr<CefWaitableEvent> mEvent;

                IMPLEMENT_REFCOUNTING(setCookieCallback);
        };

        bool automatically_reset = true;
        bool initially_signaled = false;
        CefRefPtr<CefWaitableEvent> event = CefWaitableEvent::CreateWaitableEvent(automatically_reset, initially_signaled);

        bool result = manager->SetCookie(url, cookie, new setCookieCallback(event));

        event->Wait();

        flushAllCookies();

        return result;
    }

    return false;
}

// TODO: consider adding a cookie class and use that to represent a cookie
//       vs. just the name as a string
const std::vector<std::string> dullahan_impl::getAllCookies()
{
    // VisitAllCookies() is asynchronous: CEF calls Visit() on the IO thread for
    // each cookie and destroys the visitor when it's done. The visitor writes
    // into the caller's vector (by reference) and signals a waitable event from
    // its destructor so we can block until enumeration has actually completed -
    // the previous version both copied the vector by value (so results were
    // discarded) and returned before the async visit had run.
    class CookieVisitor : public CefCookieVisitor
    {
        public:
            CookieVisitor(std::vector<std::string>& cookies, CefRefPtr<CefWaitableEvent> event) :
                mCookies(cookies),
                mEvent(event)
            {
            }

            ~CookieVisitor() override
            {
                mEvent->Signal();
            }

            bool Visit(const CefCookie& cookie, int count, int total, bool& deleteCookie) override
            {
                const std::string name = std::string(CefString(&cookie.name));

                mCookies.push_back(name);
                deleteCookie = false;
                return true;
            }

        private:
            std::vector<std::string>& mCookies;
            CefRefPtr<CefWaitableEvent> mEvent;

            IMPLEMENT_REFCOUNTING(CookieVisitor);
    };

    std::vector<std::string> cookies;
    CefRefPtr<CefCookieManager> manager;
    if (mRequestContext)
    {
        manager = mRequestContext->GetCookieManager(nullptr);
    }
    else
    {
        manager = CefCookieManager::GetGlobalManager(nullptr);
    }
    if (manager)
    {
        bool automatically_reset = true;
        bool initially_signaled = false;
        CefRefPtr<CefWaitableEvent> event = CefWaitableEvent::CreateWaitableEvent(automatically_reset, initially_signaled);

        if (manager->VisitAllCookies(new CookieVisitor(cookies, event)))
        {
            // only wait if the visit was actually accepted, otherwise Visit()
            // (and the destructor that signals) never runs and we'd hang
            event->Wait();
        }
    }

    return cookies;
}

void dullahan_impl::deleteAllCookies()
{
    CefRefPtr<CefCookieManager> manager;
    if (mRequestContext)
    {
        manager = mRequestContext->GetCookieManager(nullptr);
    }
    else
    {
        manager = CefCookieManager::GetGlobalManager(nullptr);
    }
    if (manager)
    {
        // empty URL deletes all cookies for all domains
        const CefString url("");
        const CefString name("");
        const CefRefPtr<CefDeleteCookiesCallback> callback = nullptr;
        manager->DeleteCookies(url, name, callback);
    }
}

void dullahan_impl::flushAllCookies()
{
    CefRefPtr<CefCookieManager> manager;
    if (mRequestContext)
    {
        manager = mRequestContext->GetCookieManager(nullptr);
    }
    else
    {
        manager = CefCookieManager::GetGlobalManager(nullptr);
    }

    if (manager)
    {
        class flushStoreCallback :
            public CefCompletionCallback
        {
            public:
                explicit flushStoreCallback(CefRefPtr<CefWaitableEvent> event)
                    : mEvent(event)
                {
                }

                void OnComplete() override
                {
                    mEvent->Signal();
                }

            private:
                CefRefPtr<CefWaitableEvent> mEvent;

                IMPLEMENT_REFCOUNTING(flushStoreCallback);
        };

        bool automatically_reset = true;
        bool initially_signaled = false;
        CefRefPtr<CefWaitableEvent> event = CefWaitableEvent::CreateWaitableEvent(automatically_reset, initially_signaled);

        const CefRefPtr<CefCompletionCallback> flush_store_callback = new flushStoreCallback(event);
        manager->FlushStore(flush_store_callback);

        event->Wait();
    }
}

void dullahan_impl::postData(const std::string url, const std::string data,
                             const std::string headers)
{
    if (mBrowser.get() && mBrowser->GetMainFrame())
    {
        CefRefPtr<CefRequest> request = CefRequest::Create();

        // this is a POST request
        request->SetURL(url);
        request->SetMethod("POST");

        // TODO - get this from the headers parameter
        CefRequest::HeaderMap headerMap;
        headerMap.insert(std::make_pair("Accept", "*/*"));
        headerMap.insert(std::make_pair("Content-Type", "application/x-www-form-urlencoded"));
        request->SetHeaderMap(headerMap);

        // set up data
        const std::string& upload_data = data;
        CefRefPtr<CefPostData> postData = CefPostData::Create();
        CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
        element->SetToBytes(upload_data.size(), upload_data.c_str());
        postData->AddElement(element);
        request->SetPostData(postData);

        // make the post
        mBrowser->GetMainFrame()->LoadRequest(request);

        // TODO - where do we catch what comes back?
    }
}

bool dullahan_impl::executeJavaScript(const std::string cmd)
{
    if (mBrowser.get() && mBrowser->GetMainFrame())
    {
        mBrowser->GetMainFrame()->ExecuteJavaScript(cmd, std::string(), 0);
        return true;
    }
    return false;
}

dullahan_callback_manager* dullahan_impl::getCallbackManager()
{
    return mCallbackManager;
}

void dullahan_impl::setCustomSchemes(std::vector<std::string> custom_schemes)
{
    mCustomSchemes = custom_schemes;
}

std::vector<std::string>& dullahan_impl::getCustomSchemes()
{
    return mCustomSchemes;
}

CefRefPtr<CefBrowser> dullahan_impl::getBrowser()
{
    return mBrowser;
}

void dullahan_impl::setBrowser(CefRefPtr<CefBrowser> browser)
{
    mBrowser = browser;
}

void dullahan_impl::showBrowserMessage(const std::string msg)
{
    std::stringstream url;

    url << "data:text/html;charset=utf-8,<html>%0D%0A<head>%0D%0A<style>%0D%0Abody%20%7B%0D%0Abackground-color%3A%20%23633%3B%0D%0Acolor%3A%23ccc%3B%0D%0A%7D%0D%0A%23msg%20%7B%0D%0Amargin-top";
    url << "%3A%20128px%3B%0D%0Amargin-left%3A%20128px%3B%0D%0Amargin-right%3A%20128px%3B%0D%0Afont-family%3AVerdana%3B%0D%0Afont-size%3A1.5em%3B%0D%0Abackground-color%3A%20%23422%3B%0D%0A";
    url << "line-height%3A%20150%25%3B%0D%0Apadding%3A%208px%3B%0D%0A%7D%0D%0A<%2Fstyle>%0D%0A<%2Fhead>%0D%0A<body>%0D%0A<div%20id%3D%27msg%27>%0D%0A";
    url << msg;
    url << "<%2Fdiv>%0D%0A<%2Fbody>%0D%0A<%2Fhtml>";

    navigate(url.str());
}

const std::string dullahan_impl::append_bitwidth_string(std::ostringstream& stream, bool show_bitwidth)
{
    if (show_bitwidth)
    {
        size_t bit_width = sizeof(void*) * 8;
        stream << " ";
        stream << "[";
        stream << bit_width;
        stream << "bit";
        stream << "]";
    }

    return stream.str();
}

const std::string dullahan_impl::dullahan_cef_version(bool show_bitwidth)
{
    std::ostringstream s;
    s << CEF_VERSION;

    return append_bitwidth_string(s, show_bitwidth);
}

const std::string dullahan_impl::dullahan_chrome_version(bool show_bitwidth)
{
    std::ostringstream s;
    s << CHROME_VERSION_MAJOR;
    s << ".";
    s << CHROME_VERSION_MINOR;
    s << ".";
    s << CHROME_VERSION_BUILD;
    s << ".";
    s << CHROME_VERSION_PATCH;

    return append_bitwidth_string(s, show_bitwidth);
}

const std::string dullahan_impl::dullahan_version(bool show_bitwidth)
{
    std::ostringstream s;

    s << DULLAHAN_VERSION_MAJOR;
    s << ".";
    s << DULLAHAN_VERSION_MINOR;
    s << ".";
    s << DULLAHAN_VERSION_POINT;
    s << ".";
    s << DULLAHAN_VERSION_BUILD;

    return append_bitwidth_string(s, show_bitwidth);
}

const std::string dullahan_impl::composite_version()
{
    std::ostringstream version;

    version << "Dullahan: ";
    version << dullahan_version(false);
    version << " (CEF: ";
    version << dullahan_cef_version(false);
    version << " - Chrome: ";
    version << dullahan_chrome_version(false);
    version << ")";

    return append_bitwidth_string(version, true);
}
