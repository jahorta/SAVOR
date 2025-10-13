#pragma once
#include "imgui.h"
#include <cstdint>
#include <optional>
#include <string>

// Pure UI controller: delegates to the App-owned coordinator.
struct CoordinatorPane {
    static void Draw();
};
