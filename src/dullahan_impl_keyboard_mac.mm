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

#import <Cocoa/Cocoa.h>

#include "dullahan_impl.h"

namespace dullahanImplMacAssist
{
    uint32_t modifiersForModifierFlags(uint32_t modifierFlags)
    {
        uint32_t modifers = EVENTFLAG_NONE;

        if (modifierFlags & NSEventModifierFlagCapsLock)
            modifers |= EVENTFLAG_CAPS_LOCK_ON;

        if (modifierFlags & NSEventModifierFlagShift)
            modifers |= EVENTFLAG_SHIFT_DOWN;

        if (modifierFlags & NSEventModifierFlagControl)
            modifers |= EVENTFLAG_CONTROL_DOWN;

        if (modifierFlags & NSEventModifierFlagOption)
            modifers |= EVENTFLAG_ALT_DOWN;

        if (modifierFlags & NSEventModifierFlagCommand)
            modifers |= EVENTFLAG_COMMAND_DOWN;

        if (modifierFlags & NSEventModifierFlagNumericPad)
            modifers |= EVENTFLAG_IS_KEY_PAD;

        return modifers;
    }
}

void dullahan_impl::nativeKeyboardEventOSX(void* event)
{
    if (mBrowser.get())
    {
        if (mBrowser->GetHost())
        {
            static uint32_t lastModifiers = dullahanImplMacAssist::modifiersForModifierFlags([NSEvent modifierFlags]);
            static uint32_t newModifiers = dullahanImplMacAssist::modifiersForModifierFlags([NSEvent modifierFlags]);

            NSEvent* ns_event = (__bridge NSEvent*)event;

            lastModifiers = newModifiers;
            newModifiers = dullahanImplMacAssist::modifiersForModifierFlags([ns_event modifierFlags]);

            if (([ns_event type] == NSEventTypeKeyDown) || ([ns_event type] == NSEventTypeKeyUp))
            {
                CefKeyEvent keyEvent;

                NSString *c = [ns_event characters];
                if ([c length] > 0)
                {
                    keyEvent.character = [c characterAtIndex:0];
                }

                NSString *cim = [ns_event charactersIgnoringModifiers];
                if ([cim length] > 0)
                {
                    keyEvent.unmodified_character = [cim characterAtIndex:0];
                }

                keyEvent.native_key_code = [ns_event keyCode];
                keyEvent.is_system_key = false;
                keyEvent.modifiers = newModifiers;

                if ([ns_event type] == NSEventTypeKeyDown)
                {
                    keyEvent.type =  KEYEVENT_KEYDOWN;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);
                }
                else
                    if ([ns_event type] == NSEventTypeKeyUp)
                {
                    keyEvent.type =  KEYEVENT_KEYUP;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);

                    keyEvent.type =  KEYEVENT_CHAR;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);
                }
            }
        }
    }
}

void dullahan_impl::nativeKeyboardEventOSX(dullahan::EKeyEvent event_type,
                                           uint32_t event_modifiers,
                                           uint32_t event_keycode,
                                           uint32_t event_chars,
                                           uint32_t event_umodchars,
                                           bool event_isrepeat)
{
    if (mBrowser.get())
    {
        if (mBrowser->GetHost())
        {
            static uint32_t lastModifiers = dullahanImplMacAssist::modifiersForModifierFlags(event_modifiers);
            static uint32_t newModifiers = dullahanImplMacAssist::modifiersForModifierFlags(event_modifiers);

            lastModifiers = newModifiers;
            newModifiers = dullahanImplMacAssist::modifiersForModifierFlags(event_modifiers);

            if (event_type == dullahan::KE_KEY_DOWN || event_type == dullahan::KE_KEY_UP)
            {
                CefKeyEvent keyEvent;

                keyEvent.character = event_chars;
                keyEvent.unmodified_character = event_umodchars;
                keyEvent.native_key_code = event_keycode;
                keyEvent.is_system_key = false;
                keyEvent.modifiers = newModifiers;

                if (event_type == dullahan::KE_KEY_DOWN)
                {
                    keyEvent.type =  KEYEVENT_KEYDOWN;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);

                }
                else
                {
                    keyEvent.type =  KEYEVENT_KEYUP;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);

                    keyEvent.type =  KEYEVENT_CHAR;
                    mBrowser->GetHost()->SendKeyEvent(keyEvent);
                }
            }
        }
    }
}
