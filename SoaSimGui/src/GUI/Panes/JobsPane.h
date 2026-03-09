#pragma once
#include "imgui.h"
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <future>

struct JobsPane {
    static void Draw();
    static void OnActivated();
    static void FocusJobSet(int64_t job_set_id);
};
