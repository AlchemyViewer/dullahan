/**
    @brief  Dullahan OpenGL Example - accelerated (zero-copy) paint interop

            Turns a CEF OnAcceleratedPaint shared-texture handle into an OpenGL
            texture with no CPU copy. On Windows this is D3D11 + the
            WGL_NV_DX_interop2 extension (the same approach that ports to the
            viewer's media-texture path). On macOS the handle is an IOSurface,
            bound via CGLTexImageIOSurface2D and GPU-blitted into the GL texture.
            On Linux the frame is a dma-buf (one or more planes + a DRM modifier),
            imported as an EGLImage and GPU-blitted into the GL texture; this needs
            an EGL context, so run under Wayland (or force EGL on X11 with
            SDL_VIDEO_FORCE_EGL=1), else init() returns false and the example falls
            back to the CPU onPageChanged path.

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

#pragma once

// GL types (GLuint / GLenum). Match opengl-example.h's GL include so the two
// headers can be used together without a circular include. OpenGL 4.1 Core.
#if LL_DARWIN
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
#include <glad/glad.h>
#endif

#include <string>

// Diagnostic log that is visible even when the example runs as a GUI-subsystem
// process: writes to stdout and (on Windows) the debugger output / DebugView.
void accelPaintLog(const std::string& msg);

// Bridges CEF's GPU shared texture (OnAcceleratedPaint) to an OpenGL texture
// without a CPU round-trip. Call init() once with a current GL context; if it
// returns false the platform/driver can't do zero-copy and the caller should
// keep using the CPU path. import() makes a GL texture alias the shared texture;
// the GL texture must then be locked around any read (draw).
class AcceleratedPaintInterop
{
    public:
        AcceleratedPaintInterop();
        ~AcceleratedPaintInterop();

        // Create the interop device (D3D11 + WGL_NV_DX_interop2 on Windows).
        // Requires a current GL context. Returns false if unsupported.
        bool init();
        void shutdown();
        bool valid() const { return mValid; }

        // Point gl_texture at the OnAcceleratedPaint shared-texture handle. The
        // handle is only valid during the callback, so call this from there.
        // Returns true if gl_texture now aliases the frame.
        bool import(GLuint gl_texture, void* shared_handle, int width, int height);

        // Linux dma-buf variant of import(): bring CEF's per-plane dma-buf into
        // gl_texture (imported as an EGLImage, then GPU-blitted so gl_texture is a
        // standalone GL_TEXTURE_2D - no lock needed around the draw). The fds are
        // valid only during the callback, so call this from there. Implemented on
        // Linux; a no-op (returns false) elsewhere. plane_count<=4; a compressed
        // (CCS) modifier carries an auxiliary plane that must be passed too.
        bool importDmabuf(GLuint gl_texture, int plane_count,
                          const int* fds, const unsigned int* strides, const unsigned long long* offsets,
                          int format, int width, int height, unsigned long long modifier);

        // Lock / unlock the interop object around a GL read of gl_texture (i.e.
        // wrap the quad draw). No-ops if gl_texture isn't a known interop texture.
        void lockForDraw(GLuint gl_texture);
        void unlockAfterDraw(GLuint gl_texture);

    private:
        bool mValid = false;
        void* mImpl = nullptr;   // platform state (Windows only)
};
