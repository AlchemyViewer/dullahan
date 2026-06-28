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
        std::unordered_map<void*, ID3D11Texture2D*> opened;   // CEF textures by handle
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
            for (auto& kv : ts.opened)
            {
                if (kv.second) kv.second->Release();
            }
            ts.opened.clear();
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

    // Open (and cache) the CEF shared texture for this pooled handle. CEF reuses a
    // small set of handles, so this is a one-time open per handle.
    ID3D11Texture2D* cef_tex = nullptr;
    auto it = ts.opened.find(shared_handle);
    if (it != ts.opened.end())
    {
        cef_tex = it->second;
    }
    else
    {
        cef_tex = w->openShared(shared_handle);
        if (!cef_tex)
        {
            return false;
        }
        ts.opened[shared_handle] = cef_tex;
    }

    // (Re)create our interop texture if the CEF texture's size/format changed.
    D3D11_TEXTURE2D_DESC cd = {};
    cef_tex->GetDesc(&cd);
    if (!ts.interop || ts.width != (int)cd.Width || ts.height != (int)cd.Height || ts.format != cd.Format)
    {
        if (!w->makeInterop(ts, gl_texture, cd))
        {
            return false;
        }
    }

    // GPU-copy this frame's CEF content into our (GL-registered) interop texture.
    // The interop object is only locked during draw, so DX owns it here - the
    // copy is valid. Flush so the copy is submitted before the draw locks it.
    w->context->CopyResource(ts.interop, cef_tex);
    w->context->Flush();
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

#else  // !_WIN32 - zero-copy interop not implemented on this platform yet

void accelPaintLog(const std::string& msg)
{
    std::cout << msg << std::endl;
}

AcceleratedPaintInterop::AcceleratedPaintInterop() = default;
AcceleratedPaintInterop::~AcceleratedPaintInterop() { shutdown(); }
bool AcceleratedPaintInterop::init() { return false; }
void AcceleratedPaintInterop::shutdown() {}
bool AcceleratedPaintInterop::import(GLuint, void*, int, int) { return false; }
void AcceleratedPaintInterop::lockForDraw(GLuint) {}
void AcceleratedPaintInterop::unlockAfterDraw(GLuint) {}

#endif
