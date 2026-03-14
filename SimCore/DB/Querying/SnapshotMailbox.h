#pragma once
#include <mutex>
#include <condition_variable>
#include <optional>
#include <cstdint>

template <typename T>
class SnapshotMailbox {
public:
    void push(T v) {
        {
            std::lock_guard<std::mutex> g(m_mtx);
            m_latest = std::move(v);
            ++m_seq;
        }
        m_cv.notify_all();
    }

    std::optional<T> latest() const {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_latest;
    }

    uint64_t sequence() const {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_seq;
    }

    T wait_next(uint64_t after_seq) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [&] { return m_seq > after_seq; });
        return *m_latest;
    }

    // Non-blocking helpers for UI polling
    bool has_value() const {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_latest.has_value();
    }

    T peek() const {
        std::lock_guard<std::mutex> g(m_mtx);
        return *m_latest; // caller should check has_value() first
    }

    void pop() {
        std::lock_guard<std::mutex> g(m_mtx);
        m_latest.reset();
    }

    // Optional: single-call non-blocking copy
    std::optional<T> try_peek() const {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_latest; // copy
    }

private:
    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::optional<T> m_latest;
    uint64_t m_seq{ 0 };
};
