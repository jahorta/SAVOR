#include "SafeEnv.h"
#include <cstdlib>

namespace env
{
    std::optional<std::string> getenv_safe(const char* name)
    {
        char* buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, name) == 0 && buf)
        {
            std::string val(buf, len ? len - 1 : 0); // len includes null terminator
            free(buf);
            return val;
        }
        return std::nullopt;
    }
}
