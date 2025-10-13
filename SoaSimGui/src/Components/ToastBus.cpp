#include "ToastBus.h"
#include <algorithm>

void GuiToastBus::Post(GuiToastSeverity sev, std::string msg, std::optional<std::string> details, int ttl_ms) {
    using clock = std::chrono::steady_clock;
    std::lock_guard lk(mtx_);
    prune_expired_();
    auto now = clock::now();
    for (auto& t : q_) {
        if (t.severity == sev && t.message == msg) {
            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.created).count();
            if (age_ms <= 1500) {
                t.count += 1;
                t.created = now;
                t.ttl_ms = std::max(t.ttl_ms, ttl_ms);
                return;
            }
        }
    }
    GuiToast t{};
    t.severity = sev;
    t.message = std::move(msg);
    t.details = std::move(details);
    t.created = now;
    t.ttl_ms = ttl_ms;
    q_.push_back(std::move(t));
}

std::vector<GuiToast> GuiToastBus::SnapshotActive() {
    std::lock_guard lk(mtx_);
    prune_expired_();
    return q_;
}

void GuiToastBus::prune_expired_() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    q_.erase(std::remove_if(q_.begin(), q_.end(), [&](const GuiToast& t) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.created).count();
        return age > t.ttl_ms;
        }), q_.end());
}
