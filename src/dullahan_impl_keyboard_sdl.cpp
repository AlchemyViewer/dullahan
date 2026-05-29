#include "dullahan_impl.h"
#include <stdint.h>

uint32_t SDL1_to_Win[ 320 ] =
{
    0,   0,   0,   0,   0,   0,   0,   0,   8,   9,   0,   0,  12,  13,   0,   0,   0,   0,   0,  19,   0,   0,   0,   0,   0,   0,   0,  27,   0,   0,   0,   0,
    32,   0,   0,   0,   0,   0,   0, 222,   0,   0,   0,   0, 188, 189, 190, 191,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,   0, 186, 226, 187,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 219, 220, 221,   0,   0,
    223,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,   0,   0,   0,   0,  46,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 110, 111, 106, 109, 107,   0,   0,  38,  40,  39,  37,  45,  36,  35,  33,  34, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126,   0,   0,   0, 144,  20, 145, 161, 160, 163, 162, 165, 164,   0,   0,  91,  92,   0,   0,  47,  44,   0,   3,  93
};

// See SDL_keysym.h (avoiding the dependency on SDL for just one header here)
enum SDL_KeyPad
{
    SDLK_KP0        = 256,
    SDLK_KP1        = 257,
    SDLK_KP2        = 258,
    SDLK_KP3        = 259,
    SDLK_KP4        = 260,
    SDLK_KP5        = 261,
    SDLK_KP6        = 262,
    SDLK_KP7        = 263,
    SDLK_KP8        = 264,
    SDLK_KP9        = 265,
    SDLK_KP_PERIOD      = 266,
    SDLK_KP_DIVIDE      = 267,
    SDLK_KP_MULTIPLY    = 268,
    SDLK_KP_MINUS       = 269,
    SDLK_KP_PLUS        = 270,
    SDLK_KP_ENTER       = 271,
    SDLK_KP_EQUALS      = 272
};

// SDL modifier-key bit values (SDL2 and SDL3 share these). Reproduced here so
// dullahan core needn't take a compile-time dependency on the SDL headers.
enum SDL_Keymod
{
    KMOD_NONE = 0x0000,
    KMOD_LSHIFT = 0x0001,
    KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL = 0x0040,
    KMOD_RCTRL = 0x0080,
    KMOD_LALT = 0x0100,
    KMOD_RALT = 0x0200,
    KMOD_LGUI = 0x0400,
    KMOD_RGUI = 0x0800,
    KMOD_NUM = 0x1000,
    KMOD_CAPS = 0x2000,
    KMOD_MODE = 0x4000,
    KMOD_RESERVED = 0x8000
};

// Translate a raw SDL keyboard modifier mask (SDL_Keymod) into the EVENTFLAG_*
// bitmask CefKeyEvent expects. Done here, in the one place that talks to CEF,
// so every caller can pass the SDL modifiers straight through. The raw SDL bit
// positions do NOT line up with EVENTFLAG_* (e.g. SDL ctrl is 0x40, which is
// EVENTFLAG_RIGHT_MOUSE_BUTTON), so the flags must be rebuilt, not reused.
static uint32_t sdlModifiersToCef(uint32_t key_modifiers)
{
    uint32_t flags = 0;
    if (key_modifiers & (KMOD_LSHIFT | KMOD_RSHIFT))
    {
        flags |= EVENTFLAG_SHIFT_DOWN;
    }
    if (key_modifiers & (KMOD_LCTRL | KMOD_RCTRL))
    {
        flags |= EVENTFLAG_CONTROL_DOWN;
    }
    if (key_modifiers & (KMOD_LALT | KMOD_RALT))
    {
        flags |= EVENTFLAG_ALT_DOWN;
    }
    if (key_modifiers & (KMOD_LGUI | KMOD_RGUI))
    {
        flags |= EVENTFLAG_COMMAND_DOWN;
    }
    if (key_modifiers & KMOD_MODE)
    {
        flags |= EVENTFLAG_ALTGR_DOWN;
    }
    if (key_modifiers & KMOD_CAPS)
    {
        flags |= EVENTFLAG_CAPS_LOCK_ON;
    }
    if (key_modifiers & KMOD_NUM)
    {
        flags |= EVENTFLAG_NUM_LOCK_ON;
    }
    return flags;
}

void dullahan_impl::nativeKeyboardEvent(dullahan::EKeyEvent key_event, uint32_t native_scan_code, uint32_t native_virtual_key, uint32_t native_modifiers)
{
    if (!mBrowser || !mBrowser->GetHost())
    {
        return;
    }

    if (native_scan_code < sizeof(SDL1_to_Win) / sizeof(uint32_t))
    {
        native_scan_code = SDL1_to_Win[ native_scan_code ];
    }
    else
    {
        native_scan_code = 0;
    }

    CefKeyEvent event = {};
    event.is_system_key = false;
    event.native_key_code = native_virtual_key;
    event.character = native_virtual_key;
    event.unmodified_character = native_virtual_key;
    event.modifiers = sdlModifiersToCef(native_modifiers);

    if (event.modifiers & EVENTFLAG_ALT_DOWN)
    {
        event.modifiers &= ~EVENTFLAG_ALT_DOWN;
        event.is_system_key = true;
    }

    if (native_scan_code >= SDLK_KP0 && native_scan_code <= SDLK_KP_EQUALS)
    {
        event.modifiers |= EVENTFLAG_IS_KEY_PAD;
    }

    event.windows_key_code = native_scan_code;

    if (key_event == dullahan::KE_KEY_DOWN)
    {
        event.type = KEYEVENT_RAWKEYDOWN;
        mBrowser->GetHost()->SendKeyEvent(event);
        if (event.character)
        {
            event.type = KEYEVENT_CHAR;
            mBrowser->GetHost()->SendKeyEvent(event);
        }
    }
    else
    {
        if (key_event == dullahan::KE_KEY_UP)
        {
            event.native_key_code |= 0xC0000000;
            event.type = KEYEVENT_KEYUP;
            mBrowser->GetHost()->SendKeyEvent(event);
        }
    }
}

// SDL2

std::map< uint32_t, uint32_t > mSDL2_to_Win
{
    { 0x0, 0x0 }
    , { 0x8, 0x8 }
    , { 0x9, 0x9 }
    , { 0xd, 0xd }
    , { 0x1b, 0x1b }
    , { 0x7f, 0x2e }
    , { 0x40000039, 0x14 }
    , { 0x4000003a, 0x70 }
    , { 0x4000003b, 0x71 }
    , { 0x4000003c, 0x72 }
    , { 0x4000003d, 0x73 }
    , { 0x4000003e, 0x74 }
    , { 0x4000003f, 0x75 }
    , { 0x40000040, 0x76 }
    , { 0x40000041, 0x77 }
    , { 0x40000042, 0x78 }
    , { 0x40000043, 0x79 }
    , { 0x40000044, 0x7a }
    , { 0x40000045, 0x7b }
    , { 0x40000048, 0x13 }
    , { 0x40000049, 0x2d }
    , { 0x4000004a, 0x24 }
    , { 0x4000004b, 0x21 }
    , { 0x4000004d, 0x23 }
    , { 0x4000004e, 0x22 }
    , { 0x4000004f, 0x27 }
    , { 0x40000050, 0x25 }
    , { 0x40000051, 0x28 }
    , { 0x40000052, 0x26 }
    , { 0x40000054, 0x6f }
    , { 0x40000055, 0x6a }
    , { 0x40000056, 0xbd }
    , { 0x40000057, 0xbb }
    , { 0x40000058, 0xd }
    , { 0x40000063, 0xbe }
    , { 0x40000068, 0x7c }
    , { 0x40000069, 0x7d }
    , { 0x4000006a, 0x7e }
    , { 0x40000075, 0x2f }
    , { 0x40000076, 0x12 }
    , { 0x4000009c, 0xc }
    , { 0x400000e0, 0x11 }
    , { 0x400000e1, 0x10 }
    , { 0x400000e2, 0x12 }
    , { 0x400000e4, 0x11 }
    , { 0x400000e5, 0x10 }
    , { 0x400000e6, 0x12 }
};

void dullahan_impl::nativeKeyboardEventSDL2(dullahan::EKeyEvent key_event, uint32_t key_data, uint32_t key_modifiers, bool keypad_input)
{
    if (!mBrowser || !mBrowser->GetHost())
    {
        return;
    }

    CefKeyEvent event = {};
    event.is_system_key = false;
    event.modifiers = sdlModifiersToCef(key_modifiers);

    if (keypad_input)
    {
        event.modifiers |= EVENTFLAG_IS_KEY_PAD;
    }

    // an Alt-modified key is a system key to CEF; the ALT flag itself is dropped
    // so it isn't double-counted as a regular modifier.
    if (event.modifiers & EVENTFLAG_ALT_DOWN)
    {
        event.modifiers &= ~EVENTFLAG_ALT_DOWN;
        event.is_system_key = true;
    }

    if (key_event == dullahan::KE_KEY_CHAR)
    {
        // CHAR events deliver the typed character. Control/Alt must not be set
        // or Chromium discards the character as part of a shortcut rather than
        // inserting it; the actual text already reflects Shift, so clear that
        // too (a CHAR with SHIFT_DOWN is treated as another accelerator).
        event.modifiers &= ~(EVENTFLAG_SHIFT_DOWN | EVENTFLAG_CONTROL_DOWN);
        event.is_system_key = false;
        event.character = key_data;
        event.unmodified_character = key_data;
        event.type = KEYEVENT_CHAR;
        mBrowser->GetHost()->SendKeyEvent(event);
        return;
    }

    // RAWKEYDOWN / KEYUP want a Windows virtual-key code. Map the special keys
    // via the table, then fold lowercase ASCII letters to their VK (uppercase)
    // value so accelerators like Ctrl+A/C/V match.
    auto itr = mSDL2_to_Win.find(key_data);
    if (itr != mSDL2_to_Win.end())
    {
        key_data = itr->second;
    }
    else if (key_data >= 'a' && key_data <= 'z')
    {
        key_data -= 0x20;
    }

    event.windows_key_code = key_data;

    if (key_event == dullahan::KE_KEY_DOWN || key_event == dullahan::KE_KEY_REPEAT)
    {
        if (key_event == dullahan::KE_KEY_REPEAT)
        {
            event.modifiers |= EVENTFLAG_IS_REPEAT;
        }
        event.type = KEYEVENT_RAWKEYDOWN;
        mBrowser->GetHost()->SendKeyEvent(event);
    }
    else if (key_event == dullahan::KE_KEY_UP)
    {
        event.native_key_code |= 0xC0000000;
        event.type = KEYEVENT_KEYUP;
        mBrowser->GetHost()->SendKeyEvent(event);
    }
}
