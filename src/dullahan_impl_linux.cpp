#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <pwd.h>
#include <string>
#include <fstream>
#include <dirent.h>

namespace
{
    std::string getExeCwd()
    {
        char path[ 4096 ];
        int len = readlink("/proc/self/exe", path, sizeof(path));
        if (len != -1)
        {
            path[len] = 0;
            return dirname(path) ;
        }
        return "";
    }
}

void dullahan_impl::platormInitWidevine(std::string cachePath)
{
}

void dullahan_impl::platformAddCommandLines(CefRefPtr<CefCommandLine> command_line)
{
}

