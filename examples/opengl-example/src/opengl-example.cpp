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

#include <iostream>
#include <functional>
#include <filesystem>
#include <random>
#include <sstream>
#include <cmath>

#include "opengl-example.h"

#include "dullahan.h"

#if LL_DARWIN
// Defined in opengl-example-mac.mm. Makes the running NSApplication satisfy
// CEF's CefAppProtocol (-isHandlingSendEvent / -setHandlingSendEvent:) so
// CefDoMessageLoopWork() doesn't crash against SDL's NSApplication subclass.
void dullahanInstallCefAppCompat();
#endif

namespace
{
    // Map an SDL mouse button to the matching dullahan button.
    dullahan::EMouseButton sdlToDullahanButton(Uint8 sdl_button)
    {
        if (sdl_button == SDL_BUTTON_RIGHT)
        {
            return dullahan::MB_MOUSE_BUTTON_RIGHT;
        }
        if (sdl_button == SDL_BUTTON_MIDDLE)
        {
            return dullahan::MB_MOUSE_BUTTON_MIDDLE;
        }
        return dullahan::MB_MOUSE_BUTTON_LEFT;
    }

    // Translate an SDL keyboard modifier mask into dullahan's portable modifier
    // bitmask (KM_MODIFIER_*). dullahan maps these to the EVENTFLAG_* values CEF
    // needs, so we must not pass raw SDL keymod bits (their positions alias
    // unrelated CEF flags).
    uint32_t sdlKeymodToDullahan(SDL_Keymod km)
    {
        uint32_t mods = 0;
        if (km & SDL_KMOD_SHIFT)
        {
            mods |= dullahan::KM_MODIFIER_SHIFT;
        }
        if (km & SDL_KMOD_CTRL)
        {
            mods |= dullahan::KM_MODIFIER_CONTROL;
        }
        if (km & SDL_KMOD_ALT)
        {
            mods |= dullahan::KM_MODIFIER_ALT;
        }
        if (km & SDL_KMOD_GUI)
        {
            mods |= dullahan::KM_MODIFIER_META;
        }
        return mods;
    }

    // The live modifier state, for events (mouse) that don't carry their own.
    uint32_t currentDullahanModifiers()
    {
        return sdlKeymodToDullahan(SDL_GetModState());
    }
}

openglExample::openglExample() :
    mWindow(nullptr),
    mGLContext(nullptr),
    mShouldClose(false),
    mShowAbout(false),
    mTextureId(0),
    mDullahan(nullptr)
{
}

void openglExample::resizeCallback(int width, int height)
{
    glViewport(0, 0, width, height);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    const double pi = 3.1415926;
    const double near_plane = 0.1f;
    const double far_plane = 100.0f;
    const double fov = 60.0;
    double frustum_height = tan(fov / 360.0 * pi) * near_plane;
    double frustum_width = frustum_height * (double)width / (double)height;
    glFrustum(-frustum_width, frustum_width, -frustum_height, frustum_height, near_plane, far_plane);

    glMatrixMode(GL_MODELVIEW);

    if (mDullahan)
    {
        mDullahan->setSize(width, height);
    }
}

void openglExample::mouseButtonCallback(Uint8 sdl_button, bool down, int clicks)
{
    // Releasing a button that started an in-progress page drag always completes
    // on the browser - even if Ctrl is now held or the cursor left the quad -
    // otherwise the page thinks the button is still down (stuck selection/drag).
    if (! down && mMouseCaptured && sdl_button == mCaptureSdlButton)
    {
        int tx = mCaptureTexX;
        int ty = mCaptureTexY;
        pick(&tx, &ty);   // leaves tx/ty clamped to the nearest valid hit
        mDullahan->mouseButton(sdlToDullahanButton(mCaptureSdlButton),
                               dullahan::ME_MOUSE_UP, tx, ty, currentDullahanModifiers());
        mMouseCaptured = false;
        return;
    }

    if (SDL_GetModState() & SDL_KMOD_CTRL)
    {
        int width;
        int height;
        SDL_GetWindowSize(mWindow, &width, &height);

        float fxpos;
        float fypos;
        SDL_GetMouseState(&fxpos, &fypos);
        double xpos = (double)fxpos;
        double ypos = (double)fypos;

        mMouseOffsetStartX = xpos / (double)width;

        if (sdl_button == SDL_BUTTON_LEFT && down)
        {
            mMouseOffsetStartY = ypos / (double)height;
            mXRotationStart = mXRotation;
            mYRotationStart = mYRotation;
        }
        if (sdl_button == SDL_BUTTON_RIGHT && down)
        {
            mMouseOffsetStartY = ((double)height - ypos) / (double)height;
            mXPanStart = mXPan;
            mYPanStart = mYPan;
        }
        return;
    }

    // page interaction: only start a press when the cursor is over the quad
    int tx = 0;
    int ty = 0;
    if (down && pick(&tx, &ty))
    {
        dullahan::EMouseEvent event = (clicks >= 2) ? dullahan::ME_MOUSE_DOUBLE_CLICK
                                                    : dullahan::ME_MOUSE_DOWN;
        mDullahan->mouseButton(sdlToDullahanButton(sdl_button), event, tx, ty,
                               currentDullahanModifiers());

        mMouseCaptured = true;
        mCaptureSdlButton = sdl_button;
        mCaptureTexX = tx;
        mCaptureTexY = ty;

        // A right-click on the page raises our own context menu next frame. We
        // still forward the click above so CEF's OnBeforeContextMenu runs and
        // refreshes the edit-state the menu queries. Anchor the menu at the
        // current cursor position.
        if (sdl_button == SDL_BUTTON_RIGHT)
        {
            float fxpos;
            float fypos;
            SDL_GetMouseState(&fxpos, &fypos);
            mContextMenuPos = ImVec2(fxpos, fypos);
            mShowContextMenu = true;
        }
    }
}

void openglExample::mouseMoveCallback(float xpos, float ypos)
{
    // An active page drag keeps streaming moves to the browser regardless of
    // Ctrl state or whether the cursor is still over the quad.
    if (mMouseCaptured)
    {
        int tx = mCaptureTexX;
        int ty = mCaptureTexY;
        pick(&tx, &ty);
        mDullahan->mouseMove(tx, ty, false, currentDullahanModifiers());
        mCaptureTexX = tx;
        mCaptureTexY = ty;
        return;
    }

    if (SDL_GetModState() & SDL_KMOD_CTRL)
    {
        int width;
        int height;
        SDL_GetWindowSize(mWindow, &width, &height);

        SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);

        mMouseOffsetX = (double)xpos / (double)width;

        if (buttons & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))
        {
            mMouseOffsetY = (double)ypos / (double)height;
            mYRotation = mYRotationStart + (mMouseOffsetX - mMouseOffsetStartX) * 360.0f;
            mXRotation = mXRotationStart + (mMouseOffsetY - mMouseOffsetStartY) * 360.0f;
        }
        if (buttons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))
        {
            mMouseOffsetY = ((double)height - (double)ypos) / (double)height;
            mXPan = mXPanStart + (mMouseOffsetX - mMouseOffsetStartX) * 5.0f;
            mYPan = mYPanStart + (mMouseOffsetY - mMouseOffsetStartY) * 5.0f;
        }
        return;
    }

    // plain hover: forward moves while over the quad, and send a single
    // mouse-leave to reset :hover / tooltips when the cursor moves off it.
    int tx = 0;
    int ty = 0;
    uint32_t mods = currentDullahanModifiers();
    if (pick(&tx, &ty))
    {
        mDullahan->mouseMove(tx, ty, false, mods);
        mWasInsideQuad = true;
    }
    else if (mWasInsideQuad)
    {
        mDullahan->mouseMove(tx, ty, true, mods);
        mWasInsideQuad = false;
    }
}

void openglExample::mouseScrollCallback(float xoffset, float yoffset)
{
    if (SDL_GetModState() & SDL_KMOD_CTRL)
    {
        mCameraDist += (double)yoffset / mZoomSensitivity;

        if (mCameraDist < mZoomMin)
        {
            mCameraDist = mZoomMin;
        }
        if (mCameraDist > mZoomMax)
        {
            mCameraDist = mZoomMax;
        }
    }
    else
    {
        int tx;
        int ty;
        // ray-cast the cursor onto the page quad
        if (pick(&tx, &ty))
        {
            // Scale the (possibly fractional, e.g. trackpad) wheel deltas and
            // round so small scrolls aren't truncated to zero. Negate to match
            // the scroll direction the viewer's media plugin uses.
            const double scale = 40.0;
            int delta_x = -(int)std::lround((double)xoffset * scale);
            int delta_y = -(int)std::lround((double)yoffset * scale);
            mDullahan->mouseWheel(tx, ty, delta_x, delta_y, currentDullahanModifiers());
        }
    }
}

// Give (or remove) browser host input focus. CEF needs host focus to display a
// caret and route key events to the focused DOM element, so without this typing
// into a page text field does nothing.
//
// Deliberately NOT guarded against a cached "already focused" state: with
// offscreen rendering the very first SetFocus(true) (e.g. from the window
// gaining focus at startup) can land before the page has loaded and fail to
// latch onto the document, after which CEF never re-focuses on its own. So we
// re-assert focus on every page click - SetFocus is idempotent and this is what
// the viewer's media plugin does too.
void openglExample::setBrowserFocus(bool focused)
{
    mDullahan->setFocus(focused);
}

// Handle the standard clipboard / edit / navigation keyboard shortcuts by
// driving dullahan's edit + navigation API, returning true if the key was
// consumed. This mirrors how a browser's UI layer (not the page) owns these
// accelerators. The accelerator modifier is Cmd on macOS and Ctrl elsewhere.
bool openglExample::handleKeyboardShortcut(SDL_Keycode key, SDL_Keymod mod)
{
#if defined(__APPLE__)
    bool accel = (mod & SDL_KMOD_GUI) != 0;
#else
    bool accel = (mod & SDL_KMOD_CTRL) != 0;
#endif
    if (! accel)
    {
        return false;
    }

    bool shift = (mod & SDL_KMOD_SHIFT) != 0;

    switch (key)
    {
        case SDLK_C: mDullahan->editCopy();      return true;
        case SDLK_X: mDullahan->editCut();       return true;
        case SDLK_V: mDullahan->editPaste();     return true;
        case SDLK_A: mDullahan->editSelectAll(); return true;
        case SDLK_Z:
            // Cmd/Ctrl+Shift+Z is the conventional Redo (alongside Ctrl+Y)
            if (shift)
            {
                mDullahan->editRedo();
            }
            else
            {
                mDullahan->editUndo();
            }
            return true;
        case SDLK_Y: mDullahan->editRedo(); return true;
        case SDLK_R: mDullahan->reload(shift); return true;   // Shift = ignore cache
        case SDLK_LEFTBRACKET:  mDullahan->goBack();    return true;   // Cmd+[ back
        case SDLK_RIGHTBRACKET: mDullahan->goForward(); return true;   // Cmd+] forward
        default:
            break;
    }

    return false;
}

// Forward keyboard key presses to the browser. Dullahan provides an SDL
// keyboard path (nativeKeyboardEventSDL2) that maps SDL keycodes/modifiers
// to the native values CEF requires - this is what makes keyboard input
// work across all platforms (the old GLFW example could not do this).
void openglExample::keyboardEvent(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod, Uint16 raw, bool down)
{
    // keep the ESC key to exit as well as File -> Quit since it's useful
    if (down && key == SDLK_ESCAPE)
    {
        mDullahan->requestExit();
        return;
    }

    bool keypad = (scancode >= SDL_SCANCODE_KP_DIVIDE && scancode <= SDL_SCANCODE_KP_PERIOD);
    // the native keyboard path takes the raw SDL modifier mask - dullahan does
    // the SDL -> CEF EVENTFLAG translation internally.
    uint32_t mods = (uint32_t)mod;
    // SDL_KeyboardEvent.raw is the platform scancode CEF needs as native_key_code
    // to build the DOM event; without it key events are dropped on macOS/Linux.
    uint32_t native_scan_code = (uint32_t)raw;

    if (down)
    {
        // don't forward key-downs to the page while ImGui owns the keyboard
        // (e.g. when typing a URL into the address bar)
        if (ImGui::GetIO().WantCaptureKeyboard)
        {
            return;
        }

        // Intercept the standard edit / navigation shortcuts and drive them
        // through dullahan's edit + navigation API, the way a browser's command
        // layer (rather than the page) handles them. When one matches we consume
        // it: we neither forward the key-down nor record it, so no key-up is sent
        // for it either (the modifier key itself is still forwarded normally).
        if (handleKeyboardShortcut(key, mod))
        {
            return;
        }

        mDullahan->nativeKeyboardEventSDL2(dullahan::KE_KEY_DOWN, (uint32_t)key, mods, keypad, native_scan_code);
        mKeysSentToPage.insert(key);
    }
    else
    {
        // always deliver the key-up for any key whose down we sent to the page,
        // even if ImGui has since grabbed the keyboard, so the page can't get
        // stuck thinking the key is still held.
        if (mKeysSentToPage.erase(key) > 0)
        {
            mDullahan->nativeKeyboardEventSDL2(dullahan::KE_KEY_UP, (uint32_t)key, mods, keypad, native_scan_code);
        }
    }
}

// Forward typed characters to the browser. SDL delivers text as UTF-8 so
// decode each codepoint and send it on as a character event. CHAR events carry
// no modifiers - dullahan strips ctrl/shift from CHAR anyway (they would make
// Chromium treat the character as a shortcut and drop it).
void openglExample::textInputEvent(const char* text)
{
    if (ImGui::GetIO().WantCaptureKeyboard)
    {
        return;
    }

    for (const unsigned char* p = (const unsigned char*)text; p && *p; )
    {
        uint32_t cp = *p++;
        int extra = 0;
        if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
        else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
        else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
        while (extra-- > 0 && (*p & 0xC0) == 0x80)
        {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        mDullahan->nativeKeyboardEventSDL2(dullahan::KE_KEY_CHAR, cp, 0, false);
    }
}

bool openglExample::init()
{
    if (! SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        exit(EXIT_FAILURE);
    }

#if LL_DARWIN
    // SDL has now created its NSApplication; make it CEF-compatible before CEF's
    // message pump (CefDoMessageLoopWork) ever queries -isHandlingSendEvent.
    dullahanInstallCefAppCompat();
#endif

    // Request a legacy OpenGL 2.1 compatibility context to match the
    // fixed-function pipeline used to draw the textured quad.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // SDL_WINDOW_HIGH_PIXEL_DENSITY gives a true pixel-resolution framebuffer on
    // HiDPI / Retina displays so the page is rendered crisply at native size.
    mWindow = SDL_CreateWindow(mWindowTitle.c_str(), mWindowWidth, mWindowHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (! mWindow)
    {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        exit(EXIT_FAILURE);
    }

    mGLContext = SDL_GL_CreateContext(mWindow);
    if (! mGLContext)
    {
        std::cerr << "Failed to create OpenGL context: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(mWindow);
        SDL_Quit();
        exit(EXIT_FAILURE);
    }

    SDL_GL_MakeCurrent(mWindow, mGLContext);
#if !LL_DARWIN
    if (! gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        std::cerr << "Failed to load OpenGL functions" << std::endl;
        exit(EXIT_FAILURE);
    }
#endif

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

    // enable vsync (ignore failure, e.g. when no compositor is present)
    SDL_GL_SetSwapInterval(1);

    // start text input so typed characters are delivered as SDL_EVENT_TEXT_INPUT
    SDL_StartTextInput(mWindow);

    int width, height;
    SDL_GetWindowSizeInPixels(mWindow, &width, &height);
    resizeCallback(width, height);

    // Texture used to display browser output on the quad
    glGenTextures(1, &mTextureId);
    glBindTexture(GL_TEXTURE_2D, mTextureId);

    initUI();

    mDullahan = new dullahan();

    // Modern way of generating random numbers - need this for making the CEF root_cache_folder unique
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(100000, 999999);
    int random_number = distrib(gen);

    // As of CEF 139, the root cache folder must be unique and
    // an absolute path - std::Filesystem to the rescue
    std::filesystem::path root_cache_path = std::filesystem::absolute("./opengl-example-profile") / std::to_string(random_number);
    std::filesystem::path log_path = root_cache_path / "opengl-example-cef.log";

    dullahan::dullahan_settings settings;
    settings.log_file = log_path.string();
    settings.root_cache_path = root_cache_path.string();
    settings.initial_height = mTextureWidth;
    settings.initial_width = mTextureHeight;
    settings.disable_gpu = false;
#ifdef __APPLE__
    settings.use_mock_keychain = true;
#endif

    bool result = mDullahan->init(settings);
    if (result)
    {
        mDullahan->setOnPageChangedCallback(std::bind(&openglExample::onPageChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
        mDullahan->setOnRequestExitCallback(std::bind(&openglExample::onRequestExitCallback, this));

        mDullahan->navigate(mHomeUrl);

        // Render the page at the current window size from the start rather than
        // being limited to the initial offscreen size above.
        int win_px_w, win_px_h;
        SDL_GetWindowSizeInPixels(mWindow, &win_px_w, &win_px_h);
        mDullahan->setSize(win_px_w, win_px_h);
    }

    return true;
}

// Ray-cast the mouse cursor onto the textured quad and return the browser
// (texture) coordinate underneath it, or false if the cursor isn't over the
// quad. This replaces the old "pick texture" + glReadPixels trick: it needs no
// extra GL work, has no resolution limit, and is HiDPI independent because it
// works entirely in logical coordinates / ratios.
bool openglExample::pick(int* tx, int* ty)
{
    int win_w;
    int win_h;
    SDL_GetWindowSize(mWindow, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0)
    {
        return false;
    }

    float fxpos;
    float fypos;
    SDL_GetMouseState(&fxpos, &fypos);

    // must match the perspective set up in resizeCallback()
    const double pi = 3.1415926;
    const double near_plane = 0.1;
    const double fov = 60.0;
    double frustum_height = tan(fov / 360.0 * pi) * near_plane;
    double frustum_width = frustum_height * (double)win_w / (double)win_h;

    // mouse position -> normalized device coords (flip Y; SDL has a top-left origin)
    double ndc_x = 2.0 * ((double)fxpos / (double)win_w) - 1.0;
    double ndc_y = 1.0 - 2.0 * ((double)fypos / (double)win_h);

    // ray from the camera (eye origin) through the near plane, in eye space
    double dir_ex = ndc_x * frustum_width;
    double dir_ey = ndc_y * frustum_height;
    double dir_ez = -near_plane;

    // The modelview in draw() is T(pan, cameraDist) * Rx(mXRotation) * Ry(mYRotation),
    // so transform the eye-space ray into the quad's local space with the inverse:
    // Ry(-mYRotation) * Rx(-mXRotation) * (point - translation).
    double rx = mXRotation * pi / 180.0;
    double ry = mYRotation * pi / 180.0;
    double cx = cos(rx);
    double sx = sin(rx);
    double cy = cos(ry);
    double sy = sin(ry);

    auto inv_rotate = [&](double x, double y, double z, double& ox, double& oy, double& oz)
    {
        // Rx(-rx)
        double ax = x;
        double ay = y * cx + z * sx;
        double az = -y * sx + z * cx;
        // Ry(-ry)
        ox = ax * cy - az * sy;
        oy = ay;
        oz = ax * sy + az * cy;
    };

    double ox;
    double oy;
    double oz;
    inv_rotate(-mXPan, -mYPan, -mCameraDist, ox, oy, oz);   // ray origin in local space

    double dx;
    double dy;
    double dz;
    inv_rotate(dir_ex, dir_ey, dir_ez, dx, dy, dz);         // ray direction in local space

    // intersect the ray with the quad plane (z = 0 in local space)
    if (fabs(dz) < 1e-9)
    {
        return false;
    }
    double t = -oz / dz;
    if (t < 0.0)
    {
        return false;
    }

    double hx = ox + t * dx;
    double hy = oy + t * dy;

    // is the hit actually within the quad ([-1, 1] in x and y)?
    bool inside = (hx >= -1.0 && hx <= 1.0 && hy >= -1.0 && hy <= 1.0);

    // local hit position -> texture coordinate (matches the quad texcoords in
    // draw()). Always clamp and return a usable coordinate so a drag that leaves
    // the quad can keep tracking the nearest edge; the bool reports containment.
    double u = (hx + 1.0) * 0.5;
    double v = (1.0 - hy) * 0.5;

    int ix = (int)(u * (double)mTextureWidth);
    int iy = (int)(v * (double)mTextureHeight);
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix >= mTextureWidth) ix = mTextureWidth - 1;
    if (iy >= mTextureHeight) iy = mTextureHeight - 1;

    *tx = ix;
    *ty = iy;
    return inside;
}

void openglExample::draw()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glLoadIdentity();

    glTranslatef((GLfloat)mXPan, (GLfloat)mYPan, (GLfloat)mCameraDist);

    glRotatef((GLfloat)mXRotation, 1.0f, 0.0f, 0.0f);
    glRotatef((GLfloat)mYRotation, 0.0f, 1.0f, 0.0f);
    glRotatef(0.0f, 0.0f, 0.0f, 1.0f);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0, 1.0, 1.0);

    // draw the browser output texture on a quad spanning [-1, 1] in x and y.
    // pick() relies on this same geometry / texcoord mapping.
    glBindTexture(GL_TEXTURE_2D, (GLuint)mTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0, 1.0);
    glVertex3f(-1.0f, -1.0f, 0.0f);
    glTexCoord2f(1.0, 1.0);
    glVertex3f( 1.0f, -1.0f, 0.0f);
    glTexCoord2f(1.0, 0.0);
    glVertex3f( 1.0f,  1.0f, 0.0f);
    glTexCoord2f(0.0, 0.0);
    glVertex3f(-1.0f,  1.0f, 0.0f);
    glEnd();
}

// Triggered when browser page content changes
void openglExample::onPageChanged(const unsigned char* pixels, int x, int y, const int width, const int height)
{
    // CEF can change its render size at runtime (e.g. when the window is resized
    // and resizeCallback() calls setSize()) so track the size (pick() needs it)
    // and upload the frame at its reported dimensions.
    mTextureWidth = width;
    mTextureHeight = height;

    glBindTexture(GL_TEXTURE_2D, (GLuint)mTextureId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
}

// Triggered by Dullahan when cleanup is complete and it's okay to exit
void openglExample::onRequestExitCallback()
{
    mShouldClose = true;
}

void openglExample::initUI()
{
    IMGUI_CHECKVERSION();
    ImGui:: CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
    io.FontGlobalScale = 1.2f;
    ImGui_ImplSDL3_InitForOpenGL(mWindow, mGLContext);
    ImGui_ImplOpenGL2_Init();
}

void openglExample::updateUI()
{
    // main host for UI - URL and bookmarks drop-down
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Turn off window decoaration - we don't want
    ImGuiWindowFlags window_flags = 0;
    window_flags |= ImGuiWindowFlags_NoTitleBar;
    window_flags |= ImGuiWindowFlags_NoScrollbar;
    window_flags |= ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoResize;
    window_flags |= ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoBackground;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(0, main_viewport->WorkPos.y), 0);
    ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, ImGui::GetFrameHeight() * 3), 0);

    // Write menu bar and associated actions
    ImGui::Begin("##ui", NULL, window_flags);
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Quit"))
            {
                mShouldClose = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Actions"))
        {
            if (ImGui::MenuItem("Go Home"))
            {
                mDullahan->navigate(mHomeUrl);
            }
            if (ImGui::MenuItem("Dev Console"))
            {
                mDullahan->showDevTools();
            }

            if (ImGui::BeginMenu("Zoom"))
            {
                if (ImGui::MenuItem("0.5"))
                {
                    mDullahan->setPageZoom(0.5);
                }
                if (ImGui::MenuItem("1.0"))
                {
                    mDullahan->setPageZoom(1.0);
                }
                if (ImGui::MenuItem("2.0"))
                {
                    mDullahan->setPageZoom(2.0);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            // NOTE: the items are left always-enabled rather than driven by the
            // editCan*() queries. With offscreen rendering CEF only reports
            // edit-state (can undo/copy/paste/...) via OnBeforeContextMenu,
            // i.e. on right-click, so those queries would be stale (and read
            // "nothing allowed" until the first right-click) - making the menu
            // greyed out almost all the time. Each edit command simply no-ops
            // in CEF when it isn't applicable, so unconditionally enabling them
            // matches the keyboard shortcuts and behaves correctly.
            if (ImGui::MenuItem("Undo", "Ctrl+Z"))
            {
                mDullahan->editUndo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z"))
            {
                mDullahan->editRedo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X"))
            {
                mDullahan->editCut();
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))
            {
                mDullahan->editCopy();
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V"))
            {
                mDullahan->editPaste();
            }
            if (ImGui::MenuItem("Delete"))
            {
                mDullahan->editDelete();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A"))
            {
                mDullahan->editSelectAll();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
            {
                mShowAbout = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    if (mShowAbout)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
        if (ImGui::Begin(mWindowTitle.c_str(), &mShowAbout, ImGuiWindowFlags_NoResize))
        {
            std::ostringstream ss;

            ss << "Instructions:";
            ss << std::endl << std::endl;
            ss << "Hold Control key and move/pan/zoom using the";
            ss << std::endl;
            ss << "mouse left, right buttons and scroll wheel.";
            ss << std::endl << std::endl;
            ss << "Interact with the page using the mouse";
            ss << std::endl;
            ss << "left mouse button and scroll wheel.";
            ss << std::endl << std::endl;
            ss << "App version: " << mAppVersionStr;
            ss << std::endl << std::endl;
            ss << "Dullahan version: " << mDullahan->dullahan_version(false);
            ss << std::endl << std::endl;
            ss << "CEF version: " << mDullahan->dullahan_cef_version(false);
            ss << std::endl << std::endl;

            ImGui::Text("%s", ss.str().c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Navigation toolbar (back / forward / reload-or-stop) followed by the URL
    // entry field, laid out on one row like a browser's address bar.
    ImGui::SetCursorPos(ImVec2(0, 0));

    bool is_loading = mDullahan->isLoading();
    bool can_back = mDullahan->canGoBack();
    bool can_forward = mDullahan->canGoForward();

    // helper to render a button that can be disabled (greyed + non-interactive)
    auto toolbar_button = [](const char* label, bool enabled) -> bool
    {
        if (! enabled)
        {
            ImGui::BeginDisabled();
        }
        bool clicked = ImGui::Button(label);
        if (! enabled)
        {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        return clicked;
    };

    if (toolbar_button("<##back", can_back))
    {
        mDullahan->goBack();
    }
    if (toolbar_button(">##forward", can_forward))
    {
        mDullahan->goForward();
    }
    // While a page is loading this acts as a Stop button, otherwise Reload -
    // exactly like the combined reload/stop control in a browser.
    if (is_loading)
    {
        if (toolbar_button("x##stop", true))
        {
            mDullahan->stop();
        }
    }
    else
    {
        if (toolbar_button("R##reload", true))
        {
            mDullahan->reload(false);
        }
    }

    // URL field fills the rest of the row
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(64, 64, 100, 255));
    ImGui::SetNextItemWidth(main_viewport->Size.x - ImGui::GetCursorPosX());
    static char url_buffer[4096];
    if (ImGui::InputTextWithHint("##UrlInput", "Enter a URL",
                                 url_buffer,
                                 IM_ARRAYSIZE(url_buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
    {
        mDullahan->navigate(url_buffer);
    }
    ImGui::PopStyleColor();

    // Write bookmarks bar - a good place to add useful or interesting bookmarks
    const char* items[] =
    {
        "chrome://version",
        "https://sl-viewer-media-system.s3.amazonaws.com/bookmarks/index.html",
        "https://viewer-login.agni.lindenlab.com/",
        "https://secondlife.com"
    };
    static const char* current_item = "Select a bookmark";
    ImGui::SetNextItemWidth(main_viewport->Size.x);
    ImGui::SetCursorPos(ImVec2(0, ImGui::GetFrameHeight()));
    if (ImGui::BeginCombo("##Bookmarks", current_item))
    {
        for (int n = 0; n < IM_ARRAYSIZE(items); n++)
        {
            bool is_selected = (current_item == items[n]);
            if (ImGui::Selectable(items[n], is_selected))
            {
                current_item = items[n];
                mDullahan->navigate(current_item);
            }
            if (is_selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::End();

    drawContextMenu();

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

// Right-click context menu for the page. Opened from mouseButtonCallback() on a
// right-click (which also runs through CEF, so OnBeforeContextMenu has just
// refreshed the edit-state these items query). Unlike the always-enabled Edit
// menu in the menu bar, here the editCan*() state is fresh because the
// triggering right-click produced it, so we can grey out inapplicable items.
void openglExample::drawContextMenu()
{
    const char* popup_id = "##pageContextMenu";

    if (mShowContextMenu)
    {
        ImGui::SetNextWindowPos(mContextMenuPos);
        ImGui::OpenPopup(popup_id);
        mShowContextMenu = false;
    }

    if (ImGui::BeginPopup(popup_id))
    {
        if (ImGui::MenuItem("Back", nullptr, false, mDullahan->canGoBack()))
        {
            mDullahan->goBack();
        }
        if (ImGui::MenuItem("Forward", nullptr, false, mDullahan->canGoForward()))
        {
            mDullahan->goForward();
        }
        if (ImGui::MenuItem("Reload"))
        {
            mDullahan->reload(false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, mDullahan->editCanUndo()))
        {
            mDullahan->editUndo();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, mDullahan->editCanRedo()))
        {
            mDullahan->editRedo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, mDullahan->editCanCut()))
        {
            mDullahan->editCut();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, mDullahan->editCanCopy()))
        {
            mDullahan->editCopy();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, mDullahan->editCanPaste()))
        {
            mDullahan->editPaste();
        }
        if (ImGui::MenuItem("Delete", nullptr, false, mDullahan->editCanDelete()))
        {
            mDullahan->editDelete();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, mDullahan->editCanSelectAll()))
        {
            mDullahan->editSelectAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("View Source"))
        {
            mDullahan->viewSource();
        }
        ImGui::EndPopup();
    }
}

void openglExample::resetUI()
{
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool openglExample::run()
{
    while (! mShouldClose)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);

            switch (event.type)
            {
                case SDL_EVENT_QUIT:
                    mShouldClose = true;
                    break;

                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    resizeCallback(event.window.data1, event.window.data2);
                    break;

                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    setBrowserFocus(true);
                    break;

                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    setBrowserFocus(false);
                    break;

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (! ImGui::GetIO().WantCaptureMouse)
                    {
                        // a click on the page gives the browser host focus so it
                        // shows a caret and accepts keyboard input
                        setBrowserFocus(true);
                        mouseButtonCallback(event.button.button, true, event.button.clicks);
                    }
                    break;

                case SDL_EVENT_MOUSE_BUTTON_UP:
                    // let an in-progress page drag finish even if it ends over
                    // the ImGui UI, else the browser stays in a dragging state
                    if (mMouseCaptured || ! ImGui::GetIO().WantCaptureMouse)
                    {
                        mouseButtonCallback(event.button.button, false, event.button.clicks);
                    }
                    break;

                case SDL_EVENT_MOUSE_MOTION:
                    if (mMouseCaptured || ! ImGui::GetIO().WantCaptureMouse)
                    {
                        mouseMoveCallback(event.motion.x, event.motion.y);
                    }
                    break;

                case SDL_EVENT_MOUSE_WHEEL:
                    if (! ImGui::GetIO().WantCaptureMouse)
                    {
                        // SDL reports natural-scroll as FLIPPED rather than
                        // pre-negating, so undo it here for a consistent sign.
                        float wheel_x = event.wheel.x;
                        float wheel_y = event.wheel.y;
                        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                        {
                            wheel_x = -wheel_x;
                            wheel_y = -wheel_y;
                        }
                        mouseScrollCallback(wheel_x, wheel_y);
                    }
                    break;

                case SDL_EVENT_KEY_DOWN:
                    keyboardEvent(event.key.key, event.key.scancode, event.key.mod, event.key.raw, true);
                    break;

                case SDL_EVENT_KEY_UP:
                    keyboardEvent(event.key.key, event.key.scancode, event.key.mod, event.key.raw, false);
                    break;

                case SDL_EVENT_TEXT_INPUT:
                    textInputEvent(event.text.text);
                    break;

                default:
                    break;
            }
        }

        if (mDullahan)
        {
            mDullahan->update();
        }

        // draw the browser output
        draw();

        updateUI();

        // ImGui's SDL3 backend calls SDL_StopTextInput() whenever no ImGui
        // widget wants text (it manages text input for its own InputText
        // widgets). That also stops SDL delivering SDL_EVENT_TEXT_INPUT, which
        // is how we feed typed characters to the page - so after letting ImGui
        // run, re-assert text input for the page whenever ImGui isn't using it.
        // Without this, typing into a page text field silently does nothing once
        // an ImGui text field has been focused and then left.
        if (! ImGui::GetIO().WantTextInput && ! SDL_TextInputActive(mWindow))
        {
            SDL_StartTextInput(mWindow);
        }

        SDL_GL_SwapWindow(mWindow);
    }
    return true;
}

bool openglExample::reset()
{
    resetUI();

    SDL_GL_DestroyContext(mGLContext);

    SDL_DestroyWindow(mWindow);

    SDL_Quit();

    return true;
}

int main(int argc, char* argv[])
{
    openglExample* app = new openglExample;

    app->init();

    app->run();

    app->reset();

    exit(EXIT_SUCCESS);
}
