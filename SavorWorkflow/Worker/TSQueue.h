#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>

template <class T>
class TSQueue {
public:
    void push(T v) {
        { std::lock_guard<std::mutex> lk(m_); q_.emplace_back(std::move(v)); }
        cv_.notify_one();
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    bool pop_wait(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        cv_.notify_all();
    }

    void reset() {
        { std::lock_guard<std::mutex> lk(m_); q_.clear(); closed_ = false; }
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return q_.size();
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> q_;
    bool closed_{ false };
};
