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
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl2.h"

#if LL_DARWIN
#include <OpenGL/gl.h>
#else
#include <glad/glad.h>
#endif

#include <SDL3/SDL.h>

#include "opengl-example-accelpaint.h"

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
        // handle clipboard/edit/navigation accelerators; returns true if consumed
        bool handleKeyboardShortcut(SDL_Keycode key, SDL_Keymod mod);
        void textInputEvent(const char* text);
        void initUI();
        void updateUI();
        void resetUI();
        bool reset();

        // callbacks
        void onPageChanged(int tab_index, const unsigned char* pixels, int x, int y, const int width, const int height);
        // zero-copy paint: CEF handed us a GPU shared-texture handle instead of a
        // CPU pixel buffer (only when accelerated paint is enabled + supported).
        void onAcceleratedPaint(int tab_index, void* native_handle, int format, int width, int height);
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
        // The active tab's browser + GL texture, mirrored from mTabs[mActiveTab]
        // so the bulk of the example (input, picking, drawing, UI) keeps driving
        // a single "current" browser.
        GLuint mTextureId;
        int mTextureWidth = 1024;
        int mTextureHeight = 1024;
        dullahan* mDullahan;

        // Zero-copy paint: when enabled + supported, CEF hands us a GPU shared
        // texture (OnAcceleratedPaint) which mAccelPaint aliases into the tab's GL
        // texture - no glTexImage2D upload. Set at startup from interop init (and
        // can be forced off with DULLAHAN_FORCE_CPU_PAINT for an A/B comparison);
        // the CPU onPageChanged path remains as the fallback.
        bool mAcceleratedPaint = false;
        AcceleratedPaintInterop mAccelPaint;

        // One browser tab. Each tab is its own dullahan browser, but they all
        // share a single process-global CEF runtime (dullahan_runtime) - this
        // example doubles as the proof that multiple CEF browsers can live in one
        // process, which the old one-CefInitialize-per-instance model forbade.
        struct Tab
        {
            dullahan* browser = nullptr;
            GLuint texture = 0;
            int tex_width = 1024;
            int tex_height = 1024;
            std::string url;
        };
        std::vector<Tab> mTabs;
        int mActiveTab = 0;

        // switch the displayed tab: mirror it into mDullahan/mTextureId/... and
        // move browser host focus to it.
        void setActiveTab(int index);

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

        // set when the user right-clicks the page: triggers an ImGui context
        // menu the next frame, positioned here (in window pixels). The page's
        // right-click also runs through CEF so its OnBeforeContextMenu refreshes
        // the edit-state the menu reads.
        bool mShowContextMenu = false;
        ImVec2 mContextMenuPos;
        // builds/handles the right-click context menu (edit + navigation items)
        void drawContextMenu();

        // give (or remove) browser host input focus; called on a page click and
        // on window focus gained/lost. Re-asserts every time (see definition).
        void setBrowserFocus(bool focused);

        // returns true if the cursor is over the quad; tx/ty are always set to
        // the clamped texture coordinate of the ray/plane hit when one exists.
        bool pick(int* tx, int* ty);
};
