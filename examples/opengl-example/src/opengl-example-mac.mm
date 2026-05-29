/**
    @brief  Dullahan OpenGL Example - macOS / CEF application glue

            CEF on macOS requires the running NSApplication to implement the
            CrAppProtocol / CefAppProtocol selectors -isHandlingSendEvent and
            -setHandlingSendEvent:. CEF's external message pump
            (CefDoMessageLoopWork) sends -isHandlingSendEvent to NSApp directly,
            so if the app object doesn't respond it crashes with:

                -[SDL3Application isHandlingSendEvent]: unrecognized selector

            SDL installs its own NSApplication subclass (SDL3Application) which
            does not implement these, so this file:

              1. Adds the two selectors to NSApplication via a category. The
                 ObjC runtime installs categories at image load, before main(),
                 so SDL's subclass inherits them and the pump never hits an
                 unrecognized selector - no matter when it first runs.

              2. Swizzles the live application's -sendEvent: to bracket a flag,
                 so -isHandlingSendEvent reports the true state (whether we are
                 currently inside -[NSApplication sendEvent:]) the way a
                 hand-written CefAppProtocol subclass would.

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

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

// Whether -[NSApplication sendEvent:] is currently on the stack. CEF reads this
// via -isHandlingSendEvent. A plain static is fine: there is only one NSApp.
static BOOL sHandlingSendEvent = NO;

// Add the CefAppProtocol selectors to every NSApplication (hence SDL's
// subclass). Declared informally - we deliberately don't import the CEF headers
// here so the example stays decoupled from them; the runtime only needs the
// selectors to exist.
@interface NSApplication (DullahanCefAppProtocol)
- (BOOL)isHandlingSendEvent;
- (void)setHandlingSendEvent:(BOOL)handlingSendEvent;
@end

@implementation NSApplication (DullahanCefAppProtocol)
- (BOOL)isHandlingSendEvent
{
    return sHandlingSendEvent;
}
- (void)setHandlingSendEvent:(BOOL)handlingSendEvent
{
    sHandlingSendEvent = handlingSendEvent;
}
@end

// Swizzle the running app's -sendEvent: so the flag is true for the duration of
// each event dispatch. Call once, after SDL_Init has created NSApp. Targeting
// [NSApp class] handles both the case where SDL's subclass overrides sendEvent:
// and where it inherits NSApplication's.
void dullahanInstallCefAppCompat()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;

    Class cls = [NSApp class];
    if (cls == nil)
    {
        return;
    }

    SEL sel = @selector(sendEvent:);
    Method method = class_getInstanceMethod(cls, sel);
    if (method == NULL)
    {
        return;
    }

    __block IMP original = method_getImplementation(method);
    IMP replacement = imp_implementationWithBlock(^(id self, NSEvent* event) {
        BOOL previous = sHandlingSendEvent;
        sHandlingSendEvent = YES;
        ((void (*)(id, SEL, NSEvent*))original)(self, sel, event);
        sHandlingSendEvent = previous;
    });
    method_setImplementation(method, replacement);
}
