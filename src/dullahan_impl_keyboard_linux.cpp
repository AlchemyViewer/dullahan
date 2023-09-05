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
    event.modifiers = native_modifiers;

    if (native_modifiers & EVENTFLAG_ALT_DOWN)
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

#define _SDLK_KP0          1073741912
#define _SDLK_KP1          1073741913
#define _SDLK_KP2          1073741914
#define _SDLK_KP3          1073741915
#define _SDLK_KP4          1073741916
#define _SDLK_KP5          1073741917
#define _SDLK_KP6          1073741918
#define _SDLK_KP7          1073741919
#define _SDLK_KP8          1073741920
#define _SDLK_KP9          1073741921
#define _SDLK_SCROLL_LOCK  1073741895
#define _SDLK_INSERT       1073741897
#define _SDLK_HOME         1073741898
#define _SDLK_PAGEUP       1073741899
#define _SDLK_END          1073741901
#define _SDLK_PAGEDOWN     1073741902
#define _SDLK_NUM          1073741907
#define _SDLK_NUM_DIVIDE   1073741908
#define _SDLK_NUM_MULTIPLY 1073741909
#define _SDLK_NUM_SUBTRACT 1073741910
#define _SDLK_NUM_ADD      1073741911
#define _SDLK_NUM_DECIMAL  1073741923
#define _SDLK_SELECT       1073741925

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

struct Modifiers
{
    bool shift;
    bool ctrl;
    bool alt;
    bool num_lock;
    bool caps_lock;
    bool uppercase;

    explicit Modifiers(int mod)
    {
        shift = (mod & KMOD_LSHIFT) || (mod & KMOD_RSHIFT);
        ctrl = (mod & KMOD_LCTRL) || (mod & KMOD_RCTRL);
        alt = (mod & KMOD_LALT) || (mod & KMOD_RALT);
        num_lock = !(mod & KMOD_NUM);
        caps_lock = static_cast<bool>(mod & KMOD_CAPS);
        uppercase = caps_lock == !shift;
    }

    uint32_t getCode()
    {
        uint32_t modifiersCode = 0;

        if (shift)
        {
            modifiersCode += EVENTFLAG_SHIFT_DOWN;
        }

        if (ctrl)
        {
            modifiersCode += EVENTFLAG_CONTROL_DOWN;
        }

        if (alt)
        {
            modifiersCode += EVENTFLAG_ALT_DOWN;
        }

        if (num_lock)
        {
            modifiersCode += EVENTFLAG_NUM_LOCK_ON;
        }

        if (caps_lock)
        {
            modifiersCode += EVENTFLAG_CAPS_LOCK_ON;
        }

        return modifiersCode;
    }
};

struct KeyCodes
{
    int key_code;
    int char_code;

    KeyCodes(int keyCode, int charCode) :
        key_code(keyCode),
        char_code(charCode)
    {
    }

    char16_t getChar16()
    {
        return static_cast<char16_t>(char_code);
    }
};

KeyCodes getAlphabetCodes(int code, Modifiers &modifiers)
{
    int diff = code - 97;
    return {65 + diff, (modifiers.uppercase ? 'A' : 'a') + diff};
}

KeyCodes getNumberRowCodes(int code, Modifiers &modifiers)
{
    int diff = code - 48;
    int char_code = '0' + diff;
    int key_code = 48 + diff;

    if (modifiers.shift)
    {
        switch (code)
        {
            case 49:
                char_code = '!';
                break;
            case 50:
                char_code = '@';
                break;
            case 51:
                char_code = '#';
                break;
            case 52:
                char_code = '$';
                break;
            case 53:
                char_code = '%';
                break;
            case 54:
                char_code = '^';
                break;
            case 55:
                char_code = '&';
                break;
            case 56:
                char_code = '*';
                break;
            case 57:
                char_code = '(';
                break;
            case 48:
                char_code = ')';
                break;
            default:
                break;
        }
    }

    return {key_code, char_code};
}

KeyCodes getFunctionKeyCodes(int code)
{
    return {112 + (code - 1073741882), -1};
}

KeyCodes getSpecialKeyCodes(int code, Modifiers &modifiers)
{
    int key_code = 0;
    int char_code = -1;

    switch (code) {
        case 32:
            key_code = 32;
            char_code = ' ';
            break;
        case 8:
            key_code = 8;
            break;
        case 9:
            key_code = 9;
            break;
        case 13:
            key_code = 13;
            char_code = 13;
            break;
        case 1073741896:
            key_code = 19;
            break;
        case _SDLK_SCROLL_LOCK:
            key_code = 145;
            break;
        case 27:
            key_code = 27;
            break;
        case 1073741904:
            key_code = 37;
            break;
        case 1073741906:
            key_code = 38;
            break;
        case 1073741903:
            key_code = 39;
            break;
        case 1073741905:
            key_code = 40;
            break;
        case _SDLK_HOME:
            key_code = 36;
            break;
        case _SDLK_END:
            key_code = 35;
            break;
        case _SDLK_PAGEUP:
            key_code = 33;
            break;
        case _SDLK_PAGEDOWN:
            key_code = 34;
            break;
        case _SDLK_INSERT:
            key_code = 45;
            break;
        case 127:
            key_code = 46;
            char_code = 127;
            break;
        case _SDLK_NUM_DIVIDE:
            key_code = 111;
            char_code = 47;
            break;
        case _SDLK_NUM_MULTIPLY:
            key_code = 106;
            char_code = 42;
            break;
        case _SDLK_NUM_SUBTRACT:
            key_code = 109;
            char_code = 45;
            break;
        case _SDLK_NUM_ADD:
            key_code = 107;
            char_code = 43;
            break;
        case _SDLK_NUM_DECIMAL:
            if (modifiers.num_lock)
            {
                key_code = 110; // keyboard layout dependent!
                char_code = 46;
            }
            else
            {
                key_code = 46;
            }
            break;
        case _SDLK_KP0:
            key_code = 45;
            break;
        case _SDLK_KP1:
            key_code = 35;
            break;
        case _SDLK_KP2:
            key_code = 40;
            break;
        case _SDLK_KP3:
            key_code = 34;
            break;
        case _SDLK_KP4:
            key_code = 37;
            break;
        case _SDLK_KP5:
            key_code = 12;
            break;
        case _SDLK_KP6:
            key_code = 39;
            break;
        case _SDLK_KP7:
            key_code = 36;
            break;
        case _SDLK_KP8:
            key_code = 38;
            break;
        case _SDLK_KP9:
            key_code = 33;
            break;
        case 1073741881:
            key_code = 20;
            break;
        case _SDLK_NUM:
            key_code = 144;
            break;
        case 1073742048:
        case 1073742052:
            key_code = 17;
            break;
        case 1073742049:
        case 1073742053:
            key_code = 16;
            break;
        case 1073742050:
        case 1073742054:
            key_code = 18;
            break;
        case 1073742051:
            key_code = 91;
            break;
        case 1073742055:
            key_code = 92;
            break;
        case _SDLK_SELECT:
            key_code = 93;
            break;
        case 59:
            key_code = 186;
            char_code = modifiers.shift ? ':' : ';';
            break;
        case 39:
            key_code = 222;
            char_code = modifiers.shift ? '"' : '\'';
            break;
        case 61:
            key_code = 187;
            char_code = modifiers.shift ? '+' : '=';
            break;
        case 44:
            key_code = 188;
            char_code = modifiers.shift ? '<' : ',';
            break;
        case 45:
            key_code = 189;
            char_code = modifiers.shift ? '_' : '-';
            break;
        case 46:
            key_code = 190;
            char_code = modifiers.shift ? '>' : '.';
            break;
        case 47:
            key_code = 191;
            char_code = modifiers.shift ? '?' : '/';
            break;
        case 96:
            key_code = 192;
            char_code = modifiers.shift ? '~' : '`';
            break;
        case 91:
            key_code = 219;
            char_code = modifiers.shift ? '{' : '[';
            break;
        case 92:
            key_code = 220;
            char_code = modifiers.shift ? '|' : '\\';
            break;
        case 93:
            key_code = 221;
            char_code = modifiers.shift ? '}' : ']';
            break;
        default:
            break;
    }

    return {key_code, char_code};
}

void dullahan_impl::nativeKeyboardEventSDL2(dullahan::EKeyEvent key_event, uint32_t key_data, uint32_t key_modifiers, uint32_t uni_char, bool keypad_input)
{
    Modifiers modifiers(key_modifiers);
    KeyCodes keyCodes(0, -1);

    if (key_data >= 97 && key_data <= 122)
    {
        keyCodes = getAlphabetCodes(key_data, modifiers);
    }
    else if (key_data >= 48 && key_data <= 57) 
    {
        keyCodes = getNumberRowCodes(key_data, modifiers);
    }
    else if (key_data >= 1073741882 && key_data <= 1073741893)
    {
        keyCodes = getFunctionKeyCodes(key_data);
    }
    else
    {
        keyCodes = getSpecialKeyCodes(key_data, modifiers);
    }

    // We couldn't map the keycode to anything, so just pass it on unchanged.
    if (keyCodes.key_code == 0)
    {
        keyCodes.key_code = key_data;
    }

    CefKeyEvent cef_key_event;
    cef_key_event.modifiers = modifiers.getCode();
    cef_key_event.windows_key_code = keyCodes.key_code;
    cef_key_event.native_key_code = keyCodes.key_code;
    cef_key_event.character = keyCodes.getChar16();
    cef_key_event.unmodified_character = keyCodes.getChar16();

    switch (key_event)
    {
    case dullahan::KE_KEY_DOWN:
        cef_key_event.type = KEYEVENT_KEYDOWN;
        break;
    
    case dullahan::KE_KEY_UP:
        cef_key_event.type = KEYEVENT_KEYUP;
        break;
    
    default:
        cef_key_event.type = KEYEVENT_CHAR;
        cef_key_event.character = uni_char;
        break;
    }

    mBrowser->GetHost()->SendKeyEvent(cef_key_event);
}
