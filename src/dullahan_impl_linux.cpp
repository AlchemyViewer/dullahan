#include "dullahan_runtime.h"

#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <pwd.h>
#include <string>
#include <fstream>
#include <dirent.h>
#include <iostream>

void dullahan_runtime::platormInitWidevine(std::string cachePath)
{
}

void dullahan_runtime::platformAddCommandLines(CefRefPtr<CefCommandLine> command_line)
{
    if (mAcceleratedPaint)
    {
        command_line->AppendSwitchWithValue("use-angle", "gl-egl");
    }

    // If the host application told us which windowing backend it is using
    // (e.g. the viewer knows whether SDL chose Wayland or X11), honour that
    // directly instead of guessing from the environment of this subprocess.
    std::string ozone_platform = mOzonePlatform;

    if (ozone_platform.empty())
    {
        // No hint from the host - fall back to sniffing the environment.
        // XWayland disabled: DISPLAY unset; XWayland enabled: DISPLAY == ":%d".
        auto *pDisplay = getenv("DISPLAY");
        auto *pSDLVideoDriver = getenv("SDL_VIDEODRIVER");

        bool use_wayland = false;

        if (pDisplay == nullptr || strlen(pDisplay) == 0)
            use_wayland = true;

        if (pSDLVideoDriver && strcmp(pSDLVideoDriver, "wayland") == 0)
            use_wayland = true;

        if (use_wayland)
            ozone_platform = "wayland";
    }

    if (!ozone_platform.empty())
    {
        std::cerr << "Using ozone-platform: " << ozone_platform << std::endl;
        command_line->AppendSwitchWithValue("ozone-platform", ozone_platform);
    }
}

