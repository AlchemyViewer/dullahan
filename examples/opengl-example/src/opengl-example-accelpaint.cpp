/**
    @brief  Dullahan OpenGL Example - accelerated (zero-copy) paint interop impl

    @author Alchemy Viewer Project - 2026

    Copyright (c) 2026, Linden Research, Inc.

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
**/

#include "opengl-example-accelpaint.h"

#include <iostream>

#if defined(_WIN32)

#include <windows.h>
#include <d3d11_1.h>
#include <unordered_map>
#include <sstream>

// The example runs as a GUI-subsystem process (CEF's bootstrap.exe), so stdout
// is usually invisible. Mirror diagnostics to the debugger (Visual Studio Output
// window / DebugView) as well so they can actually be seen.
void accelPaintLog(const std::string& msg)
{
    std::cout << msg << std::endl;
    OutputDebugStringA((msg + "\n").c_str());
}

// file-scope short alias usable from both the helpers and the class methods
static inline void apLog(const std::string& msg) { accelPaintLog(msg); }

namespace
{
    // --- WGL_NV_DX_interop / interop2 (declared here; glad provides no WGL) ---
    #define DX_WGL_ACCESS_READ_ONLY_NV 0x0000

    typedef HANDLE(WINAPI* PFN_wglDXOpenDeviceNV)(void* dxDevice);
    typedef BOOL  (WINAPI* PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
    typedef HANDLE(WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void* dxObject, GLuint name, GLenum type, GLenum access);
    typedef BOOL  (WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
    typedef BOOL  (WINAPI* PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);
    typedef BOOL  (WINAPI* PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);

    // Per GL texture: an intermediate D3D texture WE own (created in our interop
    // device), registered once with GL via WGL_NV_DX_interop2, plus a cache of
    // the CEF shared textures opened from its pooled handles.
    //
    // Why the intermediate: WGL_NV_DX_interop2 cannot register a D3D texture that
    // was opened from another device through an NT-handle share - which is exactly
    // how modern Chromium creates the OnAcceleratedPaint texture
    // (D3D11_RESOURCE_MISC_SHARED_NTHANDLE). OpenSharedResource1 succeeds but
    // wglDXRegisterObjectNV then fails (ERROR_OPEN_FAILED). A texture created in
    // our own device registers fine, so each frame we GPU-copy the CEF texture
    // into it (CopyResource - no CPU readback, so still zero-copy CPU-side).
    struct TexState
    {
        ID3D11Texture2D* interop = nullptr;                   // our texture, GL-registered
        HANDLE obj = nullptr;                                 // wglDXRegisterObjectNV handle
        int width = 0;
        int height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool locked = false;
    };

    struct WinInterop
    {
        ID3D11Device* device = nullptr;
        ID3D11Device1* device1 = nullptr;
        ID3D11DeviceContext* context = nullptr;
        HANDLE wgl_device = nullptr;
        ID3D11Query* copy_fence = nullptr;   // event query: wait for a copy to finish

        PFN_wglDXOpenDeviceNV       wglDXOpenDeviceNV = nullptr;
        PFN_wglDXCloseDeviceNV      wglDXCloseDeviceNV = nullptr;
        PFN_wglDXRegisterObjectNV   wglDXRegisterObjectNV = nullptr;
        PFN_wglDXUnregisterObjectNV wglDXUnregisterObjectNV = nullptr;
        PFN_wglDXLockObjectsNV      wglDXLockObjectsNV = nullptr;
        PFN_wglDXUnlockObjectsNV    wglDXUnlockObjectsNV = nullptr;

        std::unordered_map<GLuint, TexState> textures;
        bool logged_open_path = false;
        bool logged_register = false;
        bool logged_lock_fail = false;

        // Open a CEF shared-texture handle as a D3D11 texture. Modern Chromium
        // uses NT handles (OpenSharedResource1); older builds a legacy global
        // share handle (OpenSharedResource). Try the NT path first.
        ID3D11Texture2D* openShared(void* shared_handle)
        {
            ID3D11Texture2D* tex = nullptr;
            HANDLE h = reinterpret_cast<HANDLE>(shared_handle);

            if (device1)
            {
                HRESULT hr = device1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
                if (SUCCEEDED(hr) && tex)
                {
                    if (!logged_open_path) { apLog("[accelpaint] shared texture via OpenSharedResource1 (NT handle)"); logged_open_path = true; }
                    return tex;
                }
            }

            HRESULT hr = device->OpenSharedResource(h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            if (SUCCEEDED(hr) && tex)
            {
                if (!logged_open_path) { apLog("[accelpaint] shared texture via OpenSharedResource (legacy handle)"); logged_open_path = true; }
                return tex;
            }

            std::ostringstream os;
            os << "[accelpaint] OpenSharedResource(1) failed (hr=0x" << std::hex << hr << ")";
            apLog(os.str());
            return nullptr;
        }

        // Create the intermediate texture for ts (our device) matching the CEF
        // texture desc, and register it with the GL texture. Returns false on
        // failure. Releases/reregisters if one already exists.
        bool makeInterop(TexState& ts, GLuint gl_texture, const D3D11_TEXTURE2D_DESC& cef_desc)
        {
            if (ts.obj) { wglDXUnregisterObjectNV(wgl_device, ts.obj); ts.obj = nullptr; }
            if (ts.interop) { ts.interop->Release(); ts.interop = nullptr; }

            D3D11_TEXTURE2D_DESC d = {};
            d.Width = cef_desc.Width;
            d.Height = cef_desc.Height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = cef_desc.Format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            d.MiscFlags = 0;

            // Some NV/AMD interop drivers want the registered texture to also be a
            // render target; fall back to shader-resource-only if that's rejected
            // (e.g. for a format that can't be an RT).
            HRESULT hr;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            hr = device->CreateTexture2D(&d, nullptr, &ts.interop);
            if (FAILED(hr))
            {
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                hr = device->CreateTexture2D(&d, nullptr, &ts.interop);
            }
            if (FAILED(hr) || !ts.interop)
            {
                std::ostringstream os;
                os << "[accelpaint] CreateTexture2D(interop) failed (hr=0x" << std::hex << hr << ")";
                apLog(os.str());
                ts.interop = nullptr;
                return false;
            }

            ts.obj = wglDXRegisterObjectNV(wgl_device, ts.interop, gl_texture, GL_TEXTURE_2D, DX_WGL_ACCESS_READ_ONLY_NV);
            if (!ts.obj)
            {
                std::ostringstream os;
                os << "[accelpaint] wglDXRegisterObjectNV failed (GetLastError=" << GetLastError()
                   << ", glGetError=0x" << std::hex << glGetError() << ")";
                apLog(os.str());
                ts.interop->Release();
                ts.interop = nullptr;
                return false;
            }

            ts.width = (int)cef_desc.Width;
            ts.height = (int)cef_desc.Height;
            ts.format = cef_desc.Format;

            if (!logged_register)
            {
                std::ostringstream os;
                os << "[accelpaint] registered interop texture: glTex=" << gl_texture
                   << " obj=" << ts.obj << " dxfmt=" << cef_desc.Format
                   << " " << cef_desc.Width << "x" << cef_desc.Height;
                apLog(os.str());
                logged_register = true;
            }
            return true;
        }

        void releaseTexState(TexState& ts)
        {
            if (ts.locked && ts.obj)
            {
                wglDXUnlockObjectsNV(wgl_device, 1, &ts.obj);
                ts.locked = false;
            }
            if (ts.obj) { wglDXUnregisterObjectNV(wgl_device, ts.obj); ts.obj = nullptr; }
            if (ts.interop) { ts.interop->Release(); ts.interop = nullptr; }
        }

        // Block until previously-submitted GPU work (our CopyResource) has
        // executed. Needed because CEF recycles the shared texture once the
        // OnAcceleratedPaint callback returns and it carries no keyed mutex, so we
        // must finish reading it before returning.
        void waitForCopy()
        {
            if (!copy_fence)
            {
                context->Flush();
                return;
            }
            context->End(copy_fence);
            context->Flush();
            BOOL done = FALSE;
            for (int i = 0; i < 1000000; ++i)
            {
                if (context->GetData(copy_fence, &done, sizeof(done), 0) == S_OK)
                {
                    break;
                }
            }
        }
    };
}

AcceleratedPaintInterop::AcceleratedPaintInterop() = default;

AcceleratedPaintInterop::~AcceleratedPaintInterop()
{
    shutdown();
}

bool AcceleratedPaintInterop::init()
{
    WinInterop* w = new WinInterop();

    // Resolve the WGL_NV_DX_interop2 entry points (needs a current GL context).
    w->wglDXOpenDeviceNV       = (PFN_wglDXOpenDeviceNV)       wglGetProcAddress("wglDXOpenDeviceNV");
    w->wglDXCloseDeviceNV      = (PFN_wglDXCloseDeviceNV)      wglGetProcAddress("wglDXCloseDeviceNV");
    w->wglDXRegisterObjectNV   = (PFN_wglDXRegisterObjectNV)   wglGetProcAddress("wglDXRegisterObjectNV");
    w->wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV) wglGetProcAddress("wglDXUnregisterObjectNV");
    w->wglDXLockObjectsNV      = (PFN_wglDXLockObjectsNV)      wglGetProcAddress("wglDXLockObjectsNV");
    w->wglDXUnlockObjectsNV    = (PFN_wglDXUnlockObjectsNV)    wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!w->wglDXOpenDeviceNV || !w->wglDXRegisterObjectNV || !w->wglDXLockObjectsNV ||
        !w->wglDXUnlockObjectsNV || !w->wglDXUnregisterObjectNV || !w->wglDXCloseDeviceNV)
    {
        apLog("[accelpaint] WGL_NV_DX_interop2 not available; using CPU paint");
        delete w;
        return false;
    }

    // Create a D3D11 device. Default adapter / hardware driver; BGRA support so
    // the BGRA8 shared texture binds. (Single-GPU systems share fine; a strict
    // implementation would match CEF's adapter LUID.)
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &w->device, &got, &w->context);
    if (FAILED(hr) || !w->device)
    {
        std::ostringstream os;
        os << "[accelpaint] D3D11CreateDevice failed (hr=0x" << std::hex << hr << "); using CPU paint";
        apLog(os.str());
        delete w;
        return false;
    }
    w->device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&w->device1));

    w->wgl_device = w->wglDXOpenDeviceNV(w->device);
    if (!w->wgl_device)
    {
        apLog("[accelpaint] wglDXOpenDeviceNV failed; using CPU paint");
        if (w->device1) w->device1->Release();
        if (w->context) w->context->Release();
        if (w->device) w->device->Release();
        delete w;
        return false;
    }

    {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_EVENT;
        w->device->CreateQuery(&qd, &w->copy_fence);   // best-effort; waitForCopy falls back to Flush
    }

    mImpl = w;
    mValid = true;
    apLog("[accelpaint] zero-copy paint enabled (D3D11 + WGL_NV_DX_interop2)");
    return true;
}

void AcceleratedPaintInterop::shutdown()
{
    if (!mImpl)
    {
        return;
    }
    WinInterop* w = static_cast<WinInterop*>(mImpl);

    for (auto& kv : w->textures)
    {
        w->releaseTexState(kv.second);
    }
    w->textures.clear();

    if (w->copy_fence) { w->copy_fence->Release(); w->copy_fence = nullptr; }
    if (w->wgl_device) { w->wglDXCloseDeviceNV(w->wgl_device); w->wgl_device = nullptr; }
    if (w->device1) { w->device1->Release(); w->device1 = nullptr; }
    if (w->context) { w->context->Release(); w->context = nullptr; }
    if (w->device) { w->device->Release(); w->device = nullptr; }

    delete w;
    mImpl = nullptr;
    mValid = false;
}

bool AcceleratedPaintInterop::import(GLuint gl_texture, void* shared_handle, int width, int height)
{
    (void)width;
    (void)height;
    if (!mValid || !shared_handle)
    {
        return false;
    }
    WinInterop* w = static_cast<WinInterop*>(mImpl);
    TexState& ts = w->textures[gl_texture];

    // Open the CEF shared texture FRESH each frame. CEF hands out a pool of
    // textures and recycles them when this callback returns, and a handle value
    // can be remapped to a different resource - so a cached open could read a
    // stale frame (the page-change flicker). The handle is valid only now.
    ID3D11Texture2D* cef_tex = w->openShared(shared_handle);
    if (!cef_tex)
    {
        return false;
    }

    // (Re)create our interop texture if the CEF texture's size/format changed.
    D3D11_TEXTURE2D_DESC cd = {};
    cef_tex->GetDesc(&cd);
    if (!ts.interop || ts.width != (int)cd.Width || ts.height != (int)cd.Height || ts.format != cd.Format)
    {
        if (!w->makeInterop(ts, gl_texture, cd))
        {
            cef_tex->Release();
            return false;
        }
    }

    // GPU-copy this frame's CEF content into our (GL-registered) interop texture.
    // The interop object is only locked during draw, so DX owns it here - the
    // copy is valid. Wait for the copy to actually execute before releasing /
    // returning, so we read the source before CEF recycles it (no keyed mutex).
    w->context->CopyResource(ts.interop, cef_tex);
    w->waitForCopy();
    cef_tex->Release();
    return true;
}

void AcceleratedPaintInterop::lockForDraw(GLuint gl_texture)
{
    if (!mValid)
    {
        return;
    }
    WinInterop* w = static_cast<WinInterop*>(mImpl);
    auto it = w->textures.find(gl_texture);
    if (it == w->textures.end())
    {
        return;
    }
    TexState& ts = it->second;
    if (ts.obj && !ts.locked)
    {
        if (!w->wglDXLockObjectsNV(w->wgl_device, 1, &ts.obj) && !w->logged_lock_fail)
        {
            std::ostringstream os;
            os << "[accelpaint] wglDXLockObjectsNV failed (GetLastError=" << GetLastError() << ")";
            apLog(os.str());
            w->logged_lock_fail = true;
        }
        ts.locked = true;
    }
}

void AcceleratedPaintInterop::unlockAfterDraw(GLuint gl_texture)
{
    if (!mValid)
    {
        return;
    }
    WinInterop* w = static_cast<WinInterop*>(mImpl);
    auto it = w->textures.find(gl_texture);
    if (it == w->textures.end())
    {
        return;
    }
    TexState& ts = it->second;
    if (ts.obj && ts.locked)
    {
        w->wglDXUnlockObjectsNV(w->wgl_device, 1, &ts.obj);
        ts.locked = false;
    }
}

// dma-buf is a Linux concept; Windows uses the D3D11 shared-handle import above.
bool AcceleratedPaintInterop::importDmabuf(GLuint, int, const int*, const unsigned int*,
                                           const unsigned long long*, int, int, int, unsigned long long)
{
    return false;
}

#elif defined(__APPLE__)

#include <OpenGL/CGLCurrent.h>       // CGLGetCurrentContext
#include <OpenGL/CGLIOSurface.h>     // CGLTexImageIOSurface2D
#include <IOSurface/IOSurfaceRef.h>  // IOSurfaceRef

#include <unordered_map>
#include <sstream>

void accelPaintLog(const std::string& msg)
{
    std::cout << msg << std::endl;
}

static inline void apLog(const std::string& msg) { accelPaintLog(msg); }

namespace
{
    // OpenGL 4.1 Core only binds an IOSurface to a rectangle texture (not
    // GL_TEXTURE_2D), so the alias texture uses GL_TEXTURE_RECTANGLE - core since
    // GL 3.1 (declared by <OpenGL/gl3.h>); no ARB-suffixed fallback needed.

    // Per consumer GL texture: a rectangle texture that aliases the current
    // frame's IOSurface (via CGLTexImageIOSurface2D), recreated when the frame
    // size changes. Each frame we GPU-blit it into the consumer's GL_TEXTURE_2D so
    // the example's draw path (GL_TEXTURE_2D + normalized texcoords) is unchanged
    // - the analog of the Windows path's GPU CopyResource into the GL-registered
    // intermediate. No CPU readback, so it stays zero-copy CPU-side.
    struct TexState
    {
        GLuint rect_tex = 0;   // GL_TEXTURE_RECTANGLE aliasing the IOSurface
        int width = 0;         // current IOSurface size
        int height = 0;
        int dst_width = 0;     // size the consumer GL_TEXTURE_2D is allocated at
        int dst_height = 0;
    };

    struct MacInterop
    {
        GLuint read_fbo = 0;   // rectangle (IOSurface) source attachment
        GLuint draw_fbo = 0;   // consumer GL_TEXTURE_2D destination attachment
        std::unordered_map<GLuint, TexState> textures;
        bool logged_import = false;
    };
}

AcceleratedPaintInterop::AcceleratedPaintInterop() = default;

AcceleratedPaintInterop::~AcceleratedPaintInterop()
{
    shutdown();
}

bool AcceleratedPaintInterop::init()
{
    if (!CGLGetCurrentContext())
    {
        apLog("[accelpaint] no current CGL context; using CPU paint");
        return false;
    }

    MacInterop* m = new MacInterop();
    glGenFramebuffers(1, &m->read_fbo);
    glGenFramebuffers(1, &m->draw_fbo);
    if (!m->read_fbo || !m->draw_fbo)
    {
        apLog("[accelpaint] glGenFramebuffers failed; using CPU paint");
        if (m->read_fbo) glDeleteFramebuffers(1, &m->read_fbo);
        if (m->draw_fbo) glDeleteFramebuffers(1, &m->draw_fbo);
        delete m;
        return false;
    }

    mImpl = m;
    mValid = true;
    apLog("[accelpaint] zero-copy paint enabled (IOSurface + CGLTexImageIOSurface2D)");
    return true;
}

void AcceleratedPaintInterop::shutdown()
{
    if (!mImpl)
    {
        return;
    }
    MacInterop* m = static_cast<MacInterop*>(mImpl);

    for (auto& kv : m->textures)
    {
        if (kv.second.rect_tex) glDeleteTextures(1, &kv.second.rect_tex);
    }
    m->textures.clear();

    if (m->read_fbo) glDeleteFramebuffers(1, &m->read_fbo);
    if (m->draw_fbo) glDeleteFramebuffers(1, &m->draw_fbo);

    delete m;
    mImpl = nullptr;
    mValid = false;
}

bool AcceleratedPaintInterop::import(GLuint gl_texture, void* shared_handle, int width, int height)
{
    if (!mValid || !shared_handle || width <= 0 || height <= 0)
    {
        return false;
    }

    // On macOS the OnAcceleratedPaint "native handle" is an IOSurfaceRef (see
    // dullahan_render_handler::OnAcceleratedPaint). It is valid only during this
    // callback, so bind + blit it now.
    IOSurfaceRef io_surface = reinterpret_cast<IOSurfaceRef>(shared_handle);
    CGLContextObj cgl = CGLGetCurrentContext();
    if (!cgl)
    {
        return false;
    }

    MacInterop* m = static_cast<MacInterop*>(mImpl);
    TexState& ts = m->textures[gl_texture];

    // Save the bindings we touch so the example's GL state is left undisturbed.
    GLint prev_tex2d = 0, prev_read_fbo = 0, prev_draw_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

    if (!ts.rect_tex)
    {
        glGenTextures(1, &ts.rect_tex);
    }
    ts.width = width;
    ts.height = height;

    // Alias this frame's IOSurface into the rectangle texture (no copy).
    glBindTexture(GL_TEXTURE_RECTANGLE, ts.rect_tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    CGLError err = CGLTexImageIOSurface2D(cgl, GL_TEXTURE_RECTANGLE,
        GL_RGBA, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, io_surface, 0);
    if (err != kCGLNoError)
    {
        std::ostringstream os;
        os << "[accelpaint] CGLTexImageIOSurface2D failed (CGLError=" << err << ")";
        apLog(os.str());
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
        return false;
    }

    // Ensure the consumer's GL_TEXTURE_2D is allocated at the frame size.
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    if (ts.dst_width != width || ts.dst_height != height)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        ts.dst_width = width;
        ts.dst_height = height;
    }

    // GPU blit IOSurface(rectangle) -> consumer GL_TEXTURE_2D, texel for texel.
    // Orientation is preserved (no Y flip), matching the Windows accelerated path
    // which aliases the GPU texture directly; if a device shows the page inverted,
    // swap the dst Y coordinates below (0,height,width,0).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m->read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_RECTANGLE, ts.rect_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m->draw_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, gl_texture, 0);

    bool ok = (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
               glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (ok)
    {
        glBlitFramebuffer(0, 0, width, height,
                             0, 0, width, height,
                             GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Detach + restore the bindings we saved.
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);

    if (!ok)
    {
        apLog("[accelpaint] blit framebuffer incomplete; using CPU paint for this frame");
        return false;
    }

    if (!m->logged_import)
    {
        std::ostringstream os;
        os << "[accelpaint] IOSurface bound + blit: glTex=" << gl_texture
           << " " << width << "x" << height;
        apLog(os.str());
        m->logged_import = true;
    }
    return true;
}

// The blit already produced a self-contained GL_TEXTURE_2D, so there is no live
// alias to lock around the draw (unlike the Windows GL<->DX interop).
void AcceleratedPaintInterop::lockForDraw(GLuint) {}
void AcceleratedPaintInterop::unlockAfterDraw(GLuint) {}

// macOS uses the IOSurface shared-handle import above, not a dma-buf.
bool AcceleratedPaintInterop::importDmabuf(GLuint, int, const int*, const unsigned int*,
                                           const unsigned long long*, int, int, int, unsigned long long)
{
    return false;
}

#elif defined(__linux__)  // import CEF's dma-buf via EGL, GPU-blit into the GL texture

#include <glad/glad.h>
#include <SDL3/SDL.h>

// Dynamically loaded EGL entry points - the example links GL (via glad) but not
// EGL, so resolve these through SDL at init() and avoid an EGL link dependency.
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unordered_map>
#include <sstream>
#include <cstring>

void accelPaintLog(const std::string& msg)
{
    std::cout << msg << std::endl;
}

static inline void apLog(const std::string& msg) { accelPaintLog(msg); }

namespace
{
    // GLeglImageOES is just a void*; declare the GL_OES_EGL_image entry point
    // locally so we don't have to pull in <GLES2/gl2ext.h> alongside glad.
    typedef void (*PFN_glEGLImageTargetTexture2DOES)(GLenum target, void* image);
    typedef const char* (*PFN_eglQueryString)(EGLDisplay dpy, EGLint name);

    // DRM "no explicit modifier" sentinel (from drm_fourcc.h; declared here to
    // avoid a hard dependency on it). Must NOT be handed to eglCreateImageKHR as
    // an explicit modifier - doing so fails the import.
    const unsigned long long DH_DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;

    inline unsigned int dh_fourcc(char a, char b, char c, char d)
    {
        return (unsigned)a | ((unsigned)b << 8) | ((unsigned)c << 16) | ((unsigned)d << 24);
    }

    struct LinuxInterop
    {
        EGLDisplay display = EGL_NO_DISPLAY;
        bool has_import_modifiers = false;

        PFNEGLCREATEIMAGEKHRPROC  eglCreateImageKHR = nullptr;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
        PFN_glEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES = nullptr;
        PFN_eglQueryString eglQueryString = nullptr;

        GLuint alias_tex = 0;   // GL_TEXTURE_2D the per-frame EGLImage binds to
        GLuint read_fbo = 0;    // alias source attachment
        GLuint draw_fbo = 0;    // consumer GL_TEXTURE_2D destination attachment
        std::unordered_map<GLuint, std::pair<int,int>> dst_size;  // consumer tex -> alloc size
        bool logged_import = false;
    };
}

AcceleratedPaintInterop::AcceleratedPaintInterop() = default;

AcceleratedPaintInterop::~AcceleratedPaintInterop()
{
    shutdown();
}

bool AcceleratedPaintInterop::init()
{
    EGLDisplay display = (EGLDisplay)SDL_EGL_GetCurrentDisplay();
    if (!display)
    {
        apLog("[accelpaint] no current EGL display (X11/GLX?); run under Wayland or "
              "set SDL_VIDEO_FORCE_EGL=1; using CPU paint");
        return false;
    }

    LinuxInterop* l = new LinuxInterop();
    l->display = display;

    // egl* via the EGL loader, glEGLImageTargetTexture2DOES via the GL loader.
    l->eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC)  SDL_EGL_GetProcAddress("eglCreateImageKHR");
    l->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) SDL_EGL_GetProcAddress("eglDestroyImageKHR");
    l->eglQueryString     = (PFN_eglQueryString)        SDL_EGL_GetProcAddress("eglQueryString");
    l->glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES) SDL_GL_GetProcAddress("glEGLImageTargetTexture2DOES");

    if (!l->eglCreateImageKHR || !l->eglDestroyImageKHR || !l->glEGLImageTargetTexture2DOES)
    {
        apLog("[accelpaint] EGL dma-buf import entry points missing; using CPU paint");
        delete l;
        return false;
    }

    // Modifier attributes are only legal when the driver advertises this
    // extension; without it we must let the driver assume the default layout.
    const char* exts = l->eglQueryString ? l->eglQueryString(l->display, EGL_EXTENSIONS) : nullptr;
    l->has_import_modifiers = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;

    glGenTextures(1, &l->alias_tex);
    glGenFramebuffers(1, &l->read_fbo);
    glGenFramebuffers(1, &l->draw_fbo);

    mImpl = l;
    mValid = true;
    apLog(std::string("[accelpaint] zero-copy paint enabled (EGL dma-buf import; modifiers=")
          + (l->has_import_modifiers ? "yes" : "no") + ")");
    return true;
}

void AcceleratedPaintInterop::shutdown()
{
    if (!mImpl)
    {
        return;
    }
    LinuxInterop* l = static_cast<LinuxInterop*>(mImpl);
    if (l->alias_tex) glDeleteTextures(1, &l->alias_tex);
    if (l->read_fbo) glDeleteFramebuffers(1, &l->read_fbo);
    if (l->draw_fbo) glDeleteFramebuffers(1, &l->draw_fbo);
    delete l;
    mImpl = nullptr;
    mValid = false;
}

// dma-buf isn't a single shared handle, so the generic import() doesn't apply on
// Linux - importDmabuf() below is the entry point. Keep a stub so the symbol
// exists for any cross-platform caller.
bool AcceleratedPaintInterop::import(GLuint, void*, int, int)
{
    return false;
}

bool AcceleratedPaintInterop::importDmabuf(GLuint gl_texture, int plane_count,
                                           const int* fds, const unsigned int* strides, const unsigned long long* offsets,
                                           int format, int width, int height, unsigned long long modifier)
{
    if (!mValid || !fds || plane_count <= 0 || width <= 0 || height <= 0)
    {
        return false;
    }
    int n = plane_count > 4 ? 4 : plane_count;
    LinuxInterop* l = static_cast<LinuxInterop*>(mImpl);

    // CEF formats: 0 = RGBA_8888, 1 = BGRA_8888 (cef_color_type_t) -> DRM fourcc.
    unsigned int fourcc = (format == 0) ? dh_fourcc('A','B','2','4')    // DRM_FORMAT_ABGR8888 (RGBA)
                                        : dh_fourcc('A','R','2','4');   // DRM_FORMAT_ARGB8888 (BGRA)

    // Pass the DRM modifier per plane ONLY when it's a real value and the driver
    // supports import-with-modifiers. Passing DRM_FORMAT_MOD_INVALID (or any
    // modifier without the extension) fails eglCreateImageKHR -> grey surface.
    const bool use_modifier = l->has_import_modifiers && modifier != DH_DRM_FORMAT_MOD_INVALID;

    static const EGLint FD_ATTR[4]  = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint OFF_ATTR[4] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint PIT_ATTR[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint MLO_ATTR[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint MHI_ATTR[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };

    // In-process: CEF's fds are valid for this callback, so import them directly.
    EGLint attrs[64];
    int a = 0;
    attrs[a++] = EGL_WIDTH;                attrs[a++] = width;
    attrs[a++] = EGL_HEIGHT;               attrs[a++] = height;
    attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[a++] = (EGLint)fourcc;
    for (int i = 0; i < n; ++i)
    {
        attrs[a++] = FD_ATTR[i];  attrs[a++] = fds[i];
        attrs[a++] = OFF_ATTR[i]; attrs[a++] = (EGLint)(offsets ? offsets[i] : 0);
        attrs[a++] = PIT_ATTR[i]; attrs[a++] = (EGLint)(strides ? strides[i] : 0);
        if (use_modifier)
        {
            attrs[a++] = MLO_ATTR[i]; attrs[a++] = (EGLint)(modifier & 0xFFFFFFFFu);
            attrs[a++] = MHI_ATTR[i]; attrs[a++] = (EGLint)(modifier >> 32);
        }
    }
    attrs[a++] = EGL_NONE;

    EGLImageKHR image = l->eglCreateImageKHR(l->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)0, attrs);

    if (!l->logged_import)
    {
        std::ostringstream os;
        os << "[accelpaint] dma-buf import: " << (image != EGL_NO_IMAGE_KHR ? "ok" : "FAILED")
           << " planes=" << n << " fourcc=0x" << std::hex << fourcc
           << " modifier=0x" << modifier << std::dec
           << " mod_ext=" << (l->has_import_modifiers ? 1 : 0)
           << " used_mod=" << (use_modifier ? 1 : 0)
           << " " << width << "x" << height;
        apLog(os.str());
        l->logged_import = true;
    }

    if (image == EGL_NO_IMAGE_KHR)
    {
        return false;
    }

    // Save the GL bindings we touch so the example's state is left undisturbed.
    GLint prev_tex2d = 0, prev_read_fbo = 0, prev_draw_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

    // Alias the dma-buf into our GL_TEXTURE_2D (no copy).
    glBindTexture(GL_TEXTURE_2D, l->alias_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    l->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    // Ensure the consumer's GL_TEXTURE_2D is allocated at the frame size.
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    std::pair<int,int>& sz = l->dst_size[gl_texture];
    if (sz.first != width || sz.second != height)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        sz.first = width;
        sz.second = height;
    }

    // GPU blit alias -> consumer GL_TEXTURE_2D. No Y flip (the example does not set
    // flip_pixels_y, so CEF's buffer matches the CPU path's orientation); if a
    // device shows the page inverted, swap the dst Y coords (0,height,width,0).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, l->read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l->alias_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, l->draw_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_texture, 0);

    bool ok = (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
               glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (ok)
    {
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Detach + restore the bindings, then drop the EGLImage. The blit already
    // produced a standalone GL_TEXTURE_2D, so we don't keep the alias alive.
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);

    l->eglDestroyImageKHR(l->display, image);

    if (!ok)
    {
        apLog("[accelpaint] dma-buf blit framebuffer incomplete; using CPU paint this frame");
        return false;
    }
    return true;
}

// The blit produced a self-contained GL_TEXTURE_2D, so there is no live alias to
// lock around the draw (same as the macOS path).
void AcceleratedPaintInterop::lockForDraw(GLuint) {}
void AcceleratedPaintInterop::unlockAfterDraw(GLuint) {}

#else  // other platforms - zero-copy interop not implemented yet

void accelPaintLog(const std::string& msg)
{
    std::cout << msg << std::endl;
}

AcceleratedPaintInterop::AcceleratedPaintInterop() = default;
AcceleratedPaintInterop::~AcceleratedPaintInterop() { shutdown(); }
bool AcceleratedPaintInterop::init() { return false; }
void AcceleratedPaintInterop::shutdown() {}
bool AcceleratedPaintInterop::import(GLuint, void*, int, int) { return false; }
bool AcceleratedPaintInterop::importDmabuf(GLuint, int, const int*, const unsigned int*,
                                           const unsigned long long*, int, int, int, unsigned long long) { return false; }
void AcceleratedPaintInterop::lockForDraw(GLuint) {}
void AcceleratedPaintInterop::unlockAfterDraw(GLuint) {}

#endif
