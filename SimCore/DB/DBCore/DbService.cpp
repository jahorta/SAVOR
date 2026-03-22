#include "DbService.h"

#include <thread>
#include <queue>
#include <future>
#include <sqlite3.h>
#include <filesystem>
#include <system_error>

#include "MigrationRunner.h"
#include "MigrationRunner_Embedded.h"
#include "DbHealth.h"
#include "DbEventsRepo.h"
#include "CoordinatorClock.h"
#include "RegisterProgramKinds.h"
#include "ConfigRepo.h"
#include "ObjectStore.h"
#include "../../Utils/Log.h"
#include "../../Utils/ModulePath.h"
#include "../ProgramDB/IProgramDBCodec.h"

namespace simcore {
    namespace db {
        namespace fs = std::filesystem;

        namespace {
            static std::vector<SeedAny> create_cfg_defaults(std::string path) {
                using namespace simcore::db::cfg;

                fs::path root = path;
                std::vector<SeedAny> defaults = {
                    make_seed(ObjectStoreDir,             (root / "objects").string()),
                    make_seed(TempDir,                    (root / "tmp").string()),

                    make_seed(BusyTimeoutMs,              int64_t(2000)),
                    make_seed(ForeignKeys,                true),
                    make_seed(Synchronous,                std::string("NORMAL")),
                    make_seed(WalAutocheckpointPages,     int64_t(1000)),

                    make_seed(MaxWorkers,                 std::max<int64_t>(1, (int64_t)std::thread::hardware_concurrency() - 2)),
                    make_seed(ProcessReuse,               true),

                    make_seed(RetryInitialBackoffMs,      int64_t(50)),
                    make_seed(RetryBackoffMultiplierX100, int64_t(150)),
                    make_seed(RetryMaxBackoffMs,          int64_t(2000)),
                    make_seed(LeaseTimeoutMs,             int64_t(60000)),
                    make_seed(HeartbeatIntervalMs,        int64_t(5000)),
                };

                return defaults;
            }

            fs::path canonicalish(const fs::path& path) {
                std::error_code ec;
                fs::path canon = fs::weakly_canonical(path, ec);
                return ec ? path.lexically_normal() : canon;
            }

            bool path_is_within(const fs::path& child, const fs::path& parent) {
                const fs::path childNorm = canonicalish(child);
                const fs::path parentNorm = canonicalish(parent);
                auto pit = parentNorm.begin();
                auto cit = childNorm.begin();
                for (; pit != parentNorm.end(); ++pit, ++cit) {
                    if (cit == childNorm.end() || *cit != *pit) {
                        return false;
                    }
                }
                return true;
            }

            bool validate_existing_root(const fs::path& root, std::string& error) {
                std::error_code ec;
                if (root.empty()) {
                    error = "Target path cannot be empty";
                    return false;
                }
                if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
                    error = "Target database root does not exist or is not a directory";
                    return false;
                }
                if (!fs::exists(root / "SoaSimDB.sqlite3", ec) || !fs::is_regular_file(root / "SoaSimDB.sqlite3", ec)) {
                    error = "Target database root must contain SoaSimDB.sqlite3";
                    return false;
                }
                return true;
            }

            bool remove_path_if_exists(const fs::path& target, std::string& error) {
                std::error_code ec;
                if (!fs::exists(target, ec)) {
                    return true;
                }
                fs::remove_all(target, ec);
                if (ec) {
                    error = "Failed to remove existing path: " + target.string();
                    return false;
                }
                return true;
            }

            bool copy_directory_contents(const fs::path& source, const fs::path& target, std::string& error) {
                std::error_code ec;
                fs::create_directories(target, ec);
                if (ec) {
                    error = "Failed to create target directory";
                    return false;
                }
                for (const auto& entry : fs::directory_iterator(source, ec)) {
                    if (ec) {
                        error = "Failed to enumerate source directory";
                        return false;
                    }
                    fs::copy(entry.path(), target / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        error = "Failed to copy existing database files";
                        return false;
                    }
                }
                return true;
            }

        }

        DBService::DBService()
            : m_db_root(utils::getExecutablePath() / ".db") {}
        DBService::~DBService() { stop(); }
        DBService& DBService::instance() { static DBService inst; return inst; }

        void DBService::start() {
            bool expected = false;
            if (!m_running.compare_exchange_strong(expected, true)) return;

            fs::path db_root = m_db_root.empty() ? (utils::getExecutablePath() / ".db") : m_db_root;
            std::error_code ec;
            fs::create_directories(db_root, ec);
            std::string db_path = (db_root / "SoaSimDB.sqlite3").string();
            m_env = DbEnv::open(db_path);

            CoordinatorClock::instance().boot();

            ApplyEmbeddedMigrations(*m_env);

            auto health = RunDbHealth(*m_env, false);

            if (!health.ok) {
                SCLOGE("[db] health failed: wal=%d fkeys=%d sync=%s ver_ok=%d ver=%d",
                    (int)health.wal, (int)health.foreign_keys, health.synchronous.c_str(),
                    (int)health.schema_version_ok, health.schema_version);
                for (auto& n : health.notes) SCLOGE("[db] note: %s", n.c_str());
                throw std::runtime_error("DB health check failed");
            }
            else {
                SCLOGI("[db] health ok: ver=%d, sync=%s, busy=%dms, boot_id=%s, boot_wall=%s",
                    health.schema_version, health.synchronous.c_str(), health.busy_timeout_ms,
                    CoordinatorClock::instance().boot_id().c_str(),
                    CoordinatorClock::instance().boot_wall_utc_iso().c_str());
            }

            m_worker = std::thread([this]() { workerLoop(); });
            db::codec::ensure_codecs_registered();
            (void)RegisterProgramKinds();

            (void)ConfigRepo::EnsureDefaults(create_cfg_defaults(db_root.string()));
            (void)ConfigRepo::Set(cfg::ObjectStoreDir, (db_root / "objects").string());
            (void)ConfigRepo::Set(cfg::TempDir, (db_root / "tmp").string());

            auto objdir_res = ConfigRepo::Get(cfg::ObjectStoreDir);
            auto tmpdir_res = ConfigRepo::Get(cfg::TempDir);
            if (objdir_res.ok && tmpdir_res.ok) {
                ObjectStore::SetRoots(objdir_res.value, tmpdir_res.value);
            }
            else {
                ObjectStore::SetRoots((db_root / "objects").string(), (db_root / "tmp").string());
            }

            (void)DbEventsRepo::InsertBootEvent("coordinator_booted", "");
        }

        void DBService::set_database_root(fs::path root) {
            if (m_running) return;
            if (root.empty()) {
                m_db_root = utils::getExecutablePath() / ".db";
                return;
            }
            m_db_root = std::move(root);
        }

        fs::path DBService::database_root() const {
            return m_db_root.empty() ? (utils::getExecutablePath() / ".db") : m_db_root;
        }

        bool DBService::is_running() const {
            return m_running.load();
        }

        bool DBService::switch_database_root(const fs::path& new_root, std::string& error) {
            error.clear();
            fs::path target = new_root;
            if (!validate_existing_root(target, error)) {
                return false;
            }

            const fs::path sourceCanon = canonicalish(database_root());
            const fs::path targetCanon = canonicalish(target);
            if (sourceCanon == targetCanon) {
                return true;
            }

            const bool was_running = m_running.load();
            if (was_running) stop();

            set_database_root(targetCanon);
            if (was_running) start();
            return true;
        }

        bool DBService::relocate_database_root(const fs::path& new_root, bool cleanup_source, std::string& error) {
            error.clear();

            fs::path target = new_root;
            if (target.empty()) {
                error = "Target path cannot be empty";
                return false;
            }

            const fs::path source = database_root();
            const fs::path source_canon = canonicalish(source);
            fs::path target_canon = canonicalish(target);

            if (source_canon == target_canon) return true;
            if (path_is_within(target_canon, source_canon)) {
                error = "Target directory cannot be inside the active database root when moving data";
                return false;
            }

            const bool was_running = m_running.load();
            if (was_running) stop();

            std::error_code ec;
            fs::create_directories(target_canon, ec);
            if (ec) {
                error = "Failed to create target directory";
                if (was_running) start();
                return false;
            }

            if (fs::exists(source, ec) && !fs::is_empty(source, ec)) {
                if (!copy_directory_contents(source, target_canon, error)) {
                    if (was_running) start();
                    return false;
                }
            }

            set_database_root(target_canon);
            if (was_running) start();

            if (cleanup_source && fs::exists(source_canon) && source_canon != target_canon) {
                std::string cleanupError;
                if (!remove_path_if_exists(source_canon, cleanupError)) {
                    error = cleanupError;
                    return false;
                }
            }
            return true;
        }

        void DBService::stop() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running.exchange(false)) return;
            }
            m_hasTask.notify_all();
            m_notFull.notify_all();
            if (m_worker.joinable()) m_worker.join();
            m_env.reset();
            for (auto& q : m_queues) while (!q.empty()) q.pop();
            m_size = 0;
        }

        void DBService::record_submit(Priority prio) {
            std::lock_guard<std::mutex> g(m_metrics_mtx);
            ++m_stats.submitted;
            if (prio == Priority::High) ++m_stats.queued_high;
            else ++m_stats.queued_normal;
        }

        void DBService::record_dequeue(Priority prio, uint64_t wait_us) {
            std::lock_guard<std::mutex> g(m_metrics_mtx);
            if (prio == Priority::High) {
                if (m_stats.queued_high) --m_stats.queued_high;
            }
            else {
                if (m_stats.queued_normal) --m_stats.queued_normal;
            }
            const double alpha = 0.1;
            m_stats.avg_wait_us = (1.0 - alpha) * m_stats.avg_wait_us + alpha * static_cast<double>(wait_us);
        }

        void DBService::record_result(bool ok, bool /*did_retry*/, uint64_t exec_us) {
            std::lock_guard<std::mutex> g(m_metrics_mtx);
            if (ok) ++m_stats.completed; else ++m_stats.failed;
            const double alpha = 0.1;
            m_stats.avg_exec_us = (1.0 - alpha) * m_stats.avg_exec_us + alpha * static_cast<double>(exec_us);
        }

        DBService::Stats DBService::stats() const {
            std::lock_guard<std::mutex> g(m_metrics_mtx);
            return m_stats;
        }

        DBService::QueuedTask DBService::fetch_qt() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_hasTask.wait(lock, [&]() { return !m_running || m_size > 0; });
            if (!m_running && m_size == 0) return {};
            DBService::QueuedTask qt{};
            for (std::size_t i = 0; i < m_queues.size(); ++i) {
                auto& q = m_queues[i];
                if (!q.empty()) {
                    qt = q.front();
                    q.pop();
                    --m_size;
                    break;
                }
            }
            m_notFull.notify_one();
            return qt;
        }

        void DBService::workerLoop() {
            while (m_running) {
                auto qt = fetch_qt();
                if (!qt.task) break;

                const auto now = std::chrono::steady_clock::now();
                const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(now - qt.enq_tp).count();
                record_dequeue(qt.prio, static_cast<uint64_t>(wait_us));

                const auto exec_start = std::chrono::steady_clock::now();
                bool ok = true;
                try {
                    if (!m_env) throw std::runtime_error("DBService not initialized");

                    if (qt.task->type == OpType::Write) {
                        DbEnv::Tx tx(*m_env);
                        qt.task->execute(*m_env);
                        try { tx.commit(); }
                        catch (...) { ok = false; }
                    }
                    else {
                        qt.task->execute(*m_env);
                    }
                }
                catch (...) {
                    ok = false;
                }
                const auto exec_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - exec_start).count();
                record_result(ok, false, static_cast<uint64_t>(exec_us));
            }
        }

    } // namespace db
} // namespace simcore
