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

    // Cap on cached registrations per GL texture (CEF cycles a small pool of
    // shared textures; keep a few registered so we don't re-register every frame
    // but bound the leak risk if the pool churns).
    const size_t MAX_REGS_PER_TEXTURE = 8;

    struct Reg
    {
        ID3D11Texture2D* tex = nullptr;   // opened CEF shared texture
        HANDLE obj = nullptr;             // wglDXRegisterObjectNV handle
    };

    struct TexState
    {
        std::unordered_map<void*, Reg> regs;   // keyed by CEF shared handle
        HANDLE current = nullptr;              // obj for the most recent import
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

        void releaseReg(Reg& r)
        {
            if (r.obj)
            {
                wglDXUnregisterObjectNV(wgl_device, r.obj);
                r.obj = nullptr;
            }
            if (r.tex)
            {
                r.tex->Release();
                r.tex = nullptr;
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
        TexState& ts = kv.second;
        if (ts.locked && ts.current)
        {
            w->wglDXUnlockObjectsNV(w->wgl_device, 1, &ts.current);
            ts.locked = false;
        }
        for (auto& rkv : ts.regs)
        {
            w->releaseReg(rkv.second);
        }
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

    // Already registered this shared handle to this GL texture? Just make it the
    // current one (CEF reuses a small pool of handles, so this is the hot path).
    auto it = ts.regs.find(shared_handle);
    if (it != ts.regs.end())
    {
        ts.current = it->second.obj;
        return true;
    }

    // New handle: open it as a D3D texture and register it against the GL texture
    // name. WGL_ACCESS_READ_ONLY_NV - we only sample it.
    ID3D11Texture2D* tex = w->openShared(shared_handle);
    if (!tex)
    {
        return false;
    }
    HANDLE obj = w->wglDXRegisterObjectNV(w->wgl_device, tex, gl_texture, GL_TEXTURE_2D, DX_WGL_ACCESS_READ_ONLY_NV);
    if (!obj)
    {
        std::ostringstream os;
        os << "[accelpaint] wglDXRegisterObjectNV failed (GetLastError=" << GetLastError()
           << ", glGetError=0x" << std::hex << glGetError() << ")";
        apLog(os.str());
        tex->Release();
        return false;
    }
    if (!w->logged_register)
    {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        std::ostringstream os;
        os << "[accelpaint] registered shared texture: glTex=" << gl_texture
           << " obj=" << obj << " dxfmt=" << d.Format
           << " " << d.Width << "x" << d.Height
           << " misc=0x" << std::hex << d.MiscFlags;
        apLog(os.str());
        w->logged_register = true;
    }

    Reg r;
    r.tex = tex;
    r.obj = obj;
    ts.regs[shared_handle] = r;
    ts.current = obj;

    // Bound the cache: drop registrations other than the current one.
    if (ts.regs.size() > MAX_REGS_PER_TEXTURE)
    {
        for (auto rit = ts.regs.begin(); rit != ts.regs.end(); )
        {
            if (rit->second.obj != ts.current)
            {
                w->releaseReg(rit->second);
                rit = ts.regs.erase(rit);
            }
            else
            {
                ++rit;
            }
        }
    }
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
    if (ts.current && !ts.locked)
    {
        if (!w->wglDXLockObjectsNV(w->wgl_device, 1, &ts.current) && !w->logged_lock_fail)
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
    if (ts.current && ts.locked)
    {
        w->wglDXUnlockObjectsNV(w->wgl_device, 1, &ts.current);
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
