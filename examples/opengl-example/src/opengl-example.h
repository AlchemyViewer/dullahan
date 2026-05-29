/**
    @brief  Dullahan OpenGL Example application

            Cross platform example for illustration and standalone testing
            of Dullahan features. Renders output to an OpenGL 2.1 quad
            and allows interaction using the mouse and keyboard.

            Windowing, input and the OpenGL context are provided by SDL3.

    @author Callum Prentice - August 2025

    Copyright (c) 2025, Linden Research, Inc.

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

#include <string>
#include <set>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl2.h"

#if LL_DARWIN
#include <OpenGL/gl.h>
#else
#include <glad/glad.h>
#endif

#include <SDL3/SDL.h>

class dullahan;

class openglExample
{
    public:
        openglExample();

        bool init();
        bool run();
        void draw();
        void resizeCallback(int width, int height);
        void mouseButtonCallback(Uint8 sdl_button, bool down, int clicks);
        void mouseMoveCallback(float xpos, float ypos);
        void mouseScrollCallback(float xoffset, float yoffset);
        void keyboardEvent(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod, Uint16 raw, bool down);
        void textInputEvent(const char* text);
        void initUI();
        void updateUI();
        void resetUI();
        bool reset();

        // callbacks
        void onPageChanged(const unsigned char* pixels, int x, int y, const int width, const int height);
        void onRequestExitCallback();

    private:
        SDL_Window* mWindow;
        SDL_GLContext mGLContext;
        bool mShouldClose;
        const std::string mWindowTitle = "Dullahan OpenGL Example";
        const std::string mAppVersionStr = "0.0.1";
        const std::string mHomeUrl = "https://sl-viewer-media-system.s3.amazonaws.com/bookmarks/index.html";
        const int mWindowWidth = 1280;
        const int mWindowHeight = 1280;
        bool mShowAbout;
        double mCameraDist = -2.1;
        double mMouseOffsetX = 0.0;
        double mMouseOffsetY = 0.0;
        double mMouseOffsetStartX = 0.0;
        double mMouseOffsetStartY = 0.0;
        double mXRotationStart = 0;
        double mXRotation = 0;
        double mYRotationStart = 0;
        double mYRotation = 0;
        double mXPanStart = 0;
        double mXPan = 0;
        double mYPanStart = 0;
        double mYPan = 0;
        const double mZoomSensitivity = 10.0;
        const double mZoomMin = -20.0;
        const double mZoomMax = -0.2;
        GLuint mTextureId;
        int mTextureWidth = 1024;
        int mTextureHeight = 1024;
        dullahan* mDullahan;

        // mouse-drag capture: once a button goes down on the page quad we keep
        // routing moves and the eventual up to the browser - even if the cursor
        // leaves the quad - so scrollbar drags and text selection don't get stuck.
        bool mMouseCaptured = false;
        Uint8 mCaptureSdlButton = 0;
        int mCaptureTexX = 0;
        int mCaptureTexY = 0;
        // tracks whether the cursor was last over the quad, so we can send a
        // single mouse-leave to the page when it moves off.
        bool mWasInsideQuad = false;

        // SDL keycodes whose key-down we forwarded to the page. We always send
        // the matching key-up (even if ImGui later grabs the keyboard) so the
        // page never sees a stuck key.
        std::set<SDL_Keycode> mKeysSentToPage;

        // give (or remove) browser host input focus; called on a page click and
        // on window focus gained/lost. Re-asserts every time (see definition).
        void setBrowserFocus(bool focused);

        // returns true if the cursor is over the quad; tx/ty are always set to
        // the clamped texture coordinate of the ray/plane hit when one exists.
        bool pick(int* tx, int* ty);
};
