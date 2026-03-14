#pragma once
#include <vector>
#include <filesystem>
#include <mutex>
#include <queue>
#include <windows.h>

struct DropEvent {
    std::vector<std::filesystem::path> paths;
    POINT screen_pt{};
};

struct DropInbox {
    static void Push(DropEvent ev);
    static bool TryPop(DropEvent& out);
    static void Clear();
private:
    static inline std::mutex mtx_;
    static inline std::queue<DropEvent> q_;
};
