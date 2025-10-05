#pragma once
#include <string>
#include <mutex>
#include <chrono>

struct GuiStatusSnapshot {
    bool connected{ false };
    std::string env_label{ "prod" };
    std::chrono::steady_clock::time_point last_heartbeat_tp{};
    std::string last_error{};
};

class GuiStatusModel {
public:
    void set(const GuiStatusSnapshot& s) {
        std::lock_guard<std::mutex> g(m_);
        snap_ = s;
    }
    GuiStatusSnapshot get() const {
        std::lock_guard<std::mutex> g(m_);
        return snap_;
    }
private:
    mutable std::mutex m_;
    GuiStatusSnapshot snap_{};
};
