#pragma once
#include <future>
#include <functional>
#include <variant>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <utility>

class FutureQueue {
public:
    static void Start();
    static void Stop();

    // Generic: invoke one callback with either the value or an exception_ptr.
    // Signature:
    //   void on_either(std::variant<T, std::exception_ptr> r);
    template <typename T, typename OnEither>
    static void EnqueueEither(std::future<T> fut, OnEither&& on_either) {
        using Fn = std::decay_t<OnEither>;
        struct TaskT final : TaskBase {
            std::shared_future<T> f;
            Fn cb;
            TaskT(std::future<T>&& fut_, Fn&& fn) : f(std::move(fut_).share()), cb(std::move(fn)) {}
            bool ready() override {
                return f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }
            void run() override {
                try {
                    if constexpr (std::is_void_v<T>) {
                        f.get();
                        cb(std::variant<std::monostate, std::exception_ptr>{std::in_place_index<0>});
                    }
                    else {
                        T v = f.get();
                        cb(std::variant<T, std::exception_ptr>{std::in_place_index<0>, std::move(v)});
                    }
                }
                catch (...) {
                    if constexpr (std::is_void_v<T>) {
                        cb(std::variant<std::monostate, std::exception_ptr>{std::in_place_index<1>, std::current_exception()});
                    }
                    else {
                        cb(std::variant<T, std::exception_ptr>{std::in_place_index<1>, std::current_exception()});
                    }
                }
            }
        };
        std::unique_ptr<TaskBase> t = std::make_unique<TaskT>(std::move(fut), std::forward<OnEither>(on_either));
        {
            std::lock_guard<std::mutex> lk(mtx_);
            tasks_.push_back(std::move(t));
        }
        cv_.notify_one();
    }

    // Convenience: separate success and error callbacks.
    // Signatures:
    //   void on_success(const T& v) or void on_success(T&& v)
    //   void on_error(std::exception_ptr eptr)
    template <typename T, typename OnSuccess, typename OnError>
    static void Enqueue(std::future<T> fut, OnSuccess&& on_success, OnError&& on_error) {
        EnqueueEither<T>(std::move(fut),
            [s = std::forward<OnSuccess>(on_success), e = std::forward<OnError>(on_error)]
            (auto r) mutable {
                if constexpr (std::is_void_v<T>) {
                    if (r.index() == 0) {
                        s();
                    }
                    else {
                        e(std::get<1>(r));
                    }
                }
                else {
                    if (r.index() == 0) {
                        s(std::get<0>(r));
                    }
                    else {
                        e(std::get<1>(r));
                    }
                }
            });
    }

private:
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual bool ready() = 0;
        virtual void run() = 0;
    };

    static inline std::mutex mtx_;
    static inline std::condition_variable cv_;
    static inline std::vector<std::unique_ptr<TaskBase>> tasks_;
    static inline std::atomic<bool> running_{ false };
    static inline std::thread worker_;

    static void worker_loop_();
};
