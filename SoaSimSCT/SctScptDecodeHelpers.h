#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace soasim::sct::detail {

[[nodiscard]] std::string toHexWord(std::uint32_t value);
[[nodiscard]] std::string stackValue(const std::array<std::string, 20>& stack, std::int32_t index);
[[nodiscard]] const char* compareSymbol(std::uint32_t value);
[[nodiscard]] const char* arithmeticSymbol(std::uint32_t value);
[[nodiscard]] const char* inputPrefix(std::uint32_t action);
[[nodiscard]] std::string secondaryLabel(std::uint32_t value);
[[nodiscard]] float floatFromWordBits(std::uint32_t value);

} // namespace soasim::sct::detail
