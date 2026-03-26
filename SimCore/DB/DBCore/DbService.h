// simcore/db/DBService.h
// Central database service for coordinating SQLite access.

#pragma once

#include "DbEnv.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <string>
#include <array>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include "DbResult.h"
#include "DbRetryPolicy.h"

namespace simcore {
    namespace db {

        // Operation category. Use Write for statements that mutate data,
        // Read for pure selects, and Admin for schema or maintenance jobs.
        enum class OpType { Read, Write, Admin };

        // Simple priority bands for scheduling.
        enum class Priority { High, Normal };

        // Base interface for a queued task. Subclasses hold a promise for the
        // result type and a callable to execute within a DbEnv context.
        struct TaskBase {
            OpType type;
            Priority priority;
            virtual ~TaskBase() = default;
            virtual void execute(DbEnv& env) = 0;
        };

        // Templated task storing a promise and a function returning T.
        template <typename T>
        struct Task : public TaskBase {
            explicit Task(OpType t, Priority p, std::function<T(DbEnv&)> fn)
                : func(std::move(fn)) {
                type = t;
                priority = p;
            }
            std::promise<T> promise;
            std::function<T(DbEnv&)> func;

            void execute(DbEnv& env) override {
                try {
                    T result = func(env);
                    promise.set_value(result);
                }
                catch (...) {
                    // Propagate exception to the future
                    promise.set_exception(std::current_exception());
                }
            }
        };

        // DBService manages a single writer thread and a queue of tasks.
        // It must be started with a database path before use.
        class DBService {
        public:
            struct Stats {
                uint64_t submitted{ 0 };
                uint64_t completed{ 0 };
                uint64_t failed{ 0 };
                uint64_t retried{ 0 };
                uint64_t peak_queue{ 0 };
                uint64_t queued_high{ 0 };
                uint64_t queued_normal{ 0 };

                // moving averages in microseconds
                double avg_wait_us{ 0.0 };
                double avg_exec_us{ 0.0 };
            };

            // fetchTask now also returns the time enqueued
            struct QueuedTask {
                std::shared_ptr<TaskBase> task;
                std::chrono::steady_clock::time_point enq_tp{};
                Priority prio{ Priority::Normal };
            };

            // Return the singleton instance.
            static DBService& instance();

            // Start the service with a path to the SQLite database. This will
            // open the underlying DbEnv and spin up worker threads. If already
            // started, this has no effect.
            // It will always create/start a database located at (exe dir)/db/SoaSimDB.sqlite3
            void start();

            // Optional override for the database root directory.
            // Must be set before start().
            void set_database_root(std::filesystem::path root);

            // The active database root (or default root if never overridden).
            std::filesystem::path database_root() const;
            bool is_running() const;

            // Switch the active DB root without copying data. The selected root must already
            // contain a SoaSimDB.sqlite3 file.
            bool switch_database_root(const std::filesystem::path& new_root, std::string& error);

            // Relocate the DB root by stopping the service, copying existing data,
            // updating root, and restarting. If cleanup_source is true, the previous
            // root is removed after a successful switch.
            bool relocate_database_root(const std::filesystem::path& new_root, bool cleanup_source, std::string& error);

            // Delete and recreate the active DB root, then restart the service so a
            // fresh database and supporting directories are bootstrapped again.
            bool reset_database_root(std::string& error);

            // Stop the service, flush pending tasks and join threads.
            // After stop(), no more tasks can be submitted until start().
            void stop();



            // ===== template bodies =====

            template <typename T>
            inline std::future<T> submit(OpType type, Priority prio, std::function<T(DbEnv&)> fn) {
                auto task = std::make_shared<Task<T>>(type, prio, std::move(fn));
                auto fut = task->promise.get_future();
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_notFull.wait(lock, [this]() { return !m_running || m_size < m_maxQueue; });
                    if (!m_running) {
                        task->promise.set_exception(std::make_exception_ptr(std::runtime_error("DBService is stopped")));
                        return fut;
                    }
                    record_submit(prio);
                    m_queues[static_cast<std::size_t>(prio)].push(QueuedTask{ task, std::chrono::steady_clock::now(), prio });
                    ++m_size;
                    m_stats.peak_queue = (std::max)(m_stats.peak_queue, static_cast<uint64_t>(m_size));
                }
                m_hasTask.notify_one();
                return fut;
            }

            template <typename T>
            inline std::future<DbResult<T>> submit_res(OpType type, Priority prio, RetryPolicy policy,
                std::function<DbResult<T>(DbEnv&)> fn) {
                // Wrap DbResult<T> into a Task<DbResult<T>>
                auto exec = [this, type, policy, fn = std::move(fn)](DbEnv& env) -> DbResult<T> {
                    int attempt = 0;
                    auto backoff = policy.initial_backoff;
                    while (true) {
                        ++attempt;
                        DbResult<T> r;
                        try {
                            r = fn(env);
                        }
                        catch (...) {
                            // Map unknown exceptions to Unknown error
                            r = DbResult<T>::Err(DbError{ DbErrorKind::Unknown, 0, "exception" });
                        }
                        if (r.ok) return r;

                        const auto k = r.error.kind;
                        const bool retryable = (k == DbErrorKind::Busy || k == DbErrorKind::Locked);
                        if (!policy.enabled() || !retryable || attempt >= policy.max_attempts) {
                            return r;
                        }
                        {
                            std::lock_guard<std::mutex> g(m_metrics_mtx);
                            ++m_stats.retried;
                        }
                        std::this_thread::sleep_for(backoff);
                        auto next_us = static_cast<int64_t>(backoff.count() * policy.backoff_multiplier);
                        if (next_us > policy.max_backoff.count()) next_us = policy.max_backoff.count();
                        backoff = std::chrono::milliseconds(next_us);
                    }
                    };
                return submit<DbResult<T>>(type, prio, std::move(exec));
            }


            Stats stats() const;

        private:
            DBService();
            ~DBService();
            DBService(const DBService&) = delete;
            DBService& operator=(const DBService&) = delete;

            // Worker loop run on a single thread. Processes queued tasks.
            void workerLoop();

            // Underlying DB environment. Owned by service.
            std::unique_ptr<DbEnv> m_env;

            // Worker thread handle.
            std::thread m_worker;

            // Two priority queues: index 0 = High, 1 = Normal.
            std::array<std::queue<QueuedTask>, 2> m_queues;
            std::size_t m_size{ 0 };
            const std::size_t m_maxQueue{ 10000 };

            // Synchronization
            std::mutex m_mutex;
            std::condition_variable m_hasTask;
            std::condition_variable m_notFull;

            std::atomic<bool> m_running{ false };
            std::filesystem::path m_db_root;

            mutable std::mutex m_metrics_mtx;
            Stats m_stats{};

            void record_submit(Priority prio);
            void record_dequeue(Priority prio, uint64_t wait_us);
            void record_result(bool ok, bool did_retry, uint64_t exec_us);
            DBService::QueuedTask fetch_qt();
        };

    } // namespace db
} // namespace simcore
