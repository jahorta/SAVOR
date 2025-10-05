#pragma once
#include <thread>
#include <atomic>
#include "../Models/GuiStatus.h"

class GuiHeartbeatPoller {
public:
    void start(GuiStatusModel* model);
    void stop();
private:
    void run();
    std::thread th_;
    std::atomic<bool> running_{ false };
    GuiStatusModel* model_{ nullptr };
};
