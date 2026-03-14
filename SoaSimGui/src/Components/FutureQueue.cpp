#include "FutureQueue.h"

void FutureQueue::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread(&FutureQueue::worker_loop_);
}

void FutureQueue::Stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FutureQueue::worker_loop_() {
    using namespace std::chrono_literals;
    while (running_) {
        std::vector<std::unique_ptr<TaskBase>> ready;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (tasks_.empty())
                cv_.wait_for(lk, 100ms, [] { return !tasks_.empty() || !running_; });
            if (!running_) break;

            for (size_t i = 0; i < tasks_.size();) {
                if (tasks_[i]->ready()) {
                    ready.push_back(std::move(tasks_[i]));
                    tasks_.erase(tasks_.begin() + i);
                }
                else {
                    ++i;
                }
            }
        }
        for (auto& t : ready) t->run();
        std::this_thread::sleep_for(10ms);
    }
}
