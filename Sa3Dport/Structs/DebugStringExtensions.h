#pragma once

#include "Structs/Quaternion.h"
#include "Structs/Vector2.h"
#include "Structs/Vector3.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace Sa3Dport::Structs::DebugStringExtensions {

[[nodiscard]] inline std::string debug_string(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    if (value >= 0.0f) {
        stream << ' ';
    }
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

[[nodiscard]] inline std::string debug_string(Quaternion value) {
    return "(" + debug_string(value.w) + ", " +
           debug_string(value.x) + ", " +
           debug_string(value.y) + ", " +
           debug_string(value.z) + ")";
}

[[nodiscard]] inline std::string debug_string(Vector2 value) {
    return "(" + debug_string(value.x) + ", " +
           debug_string(value.y) + ")";
}

[[nodiscard]] inline std::string debug_string(Vector3 value) {
    return "(" + debug_string(value.x) + ", " +
           debug_string(value.y) + ", " +
           debug_string(value.z) + ")";
}

} // namespace Sa3Dport::Structs::DebugStringExtensions
