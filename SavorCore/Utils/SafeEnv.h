#include <string>
#include <optional>

namespace env{
	std::optional<std::string> getenv_safe(const char* name);
}
