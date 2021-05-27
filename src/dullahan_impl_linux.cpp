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
        char path[PATH_MAX];
        return (getcwd(path, sizeof(path)-1) ? std::string(path) : std::string(""));
    }
}

void dullahan_impl::platormInitWidevine(std::string cachePath)
{
}

void dullahan_impl::platformAddCommandLines(CefRefPtr<CefCommandLine> command_line)
{
}

