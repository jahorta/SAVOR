#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <chrono>

enum class GuiToastSeverity { Info, Success, Warn, Error };

struct GuiToast {
    GuiToastSeverity severity{};
    std::string message;
    std::optional<std::string> details;
    std::chrono::steady_clock::time_point created{};
    int ttl_ms{ 4000 };
    int count{ 1 };
};

class GuiToastBus {
public:
    static void Post(GuiToastSeverity sev, std::string msg, std::optional<std::string> details = std::nullopt, int ttl_ms = 4000);
    static void Info(const std::string& m, std::optional<std::string> d = std::nullopt, int ttl = 4000) { Post(GuiToastSeverity::Info, m, d, ttl); }
    static void Success(const std::string& m, std::optional<std::string> d = std::nullopt, int ttl = 4000) { Post(GuiToastSeverity::Success, m, d, ttl); }
    static void Warn(const std::string& m, std::optional<std::string> d = std::nullopt, int ttl = 5000) { Post(GuiToastSeverity::Warn, m, d, ttl); }
    static void Error(const std::string& m, std::optional<std::string> d = std::nullopt, int ttl = 6000) { Post(GuiToastSeverity::Error, m, d, ttl); }

    static std::vector<GuiToast> SnapshotActive();

private:
    static inline std::mutex mtx_;
    static inline std::vector<GuiToast> q_;
    static void prune_expired_();
};
