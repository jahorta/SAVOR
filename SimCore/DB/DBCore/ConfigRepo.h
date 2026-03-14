// simcore/db/Repos/ConfigRepo.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <type_traits>
#include <optional>
#include <sqlite3.h>
#include "DbResult.h"
#include "DbService.h"

namespace simcore::db {

#define SIMCORE_CONFIG_KEYS(X) \
  X(Synchronous,                  std::string, "synchronous") \
  X(ObjectStoreDir,               std::string, "object_store_dir") \
  X(TempDir,                      std::string, "temp_dir") \
  X(BusyTimeoutMs,                int64_t,     "busy_timeout_ms") \
  X(ForeignKeys,                  bool,        "foreign_keys") \
  X(WalAutocheckpointPages,       int64_t,     "wal_autocheckpoint_pages") \
  X(MaxWorkers,                   int64_t,     "max_workers") \
  X(ProcessReuse,                 bool,        "process_reuse") \
  X(RetryInitialBackoffMs,        int64_t,     "retry_initial_backoff_ms") \
  X(RetryBackoffMultiplierX100,   int64_t,     "retry_backoff_multiplier_x100") \
  X(RetryMaxBackoffMs,            int64_t,     "retry_max_backoff_ms") \
  X(LeaseTimeoutMs,               int64_t,     "lease_timeout_ms") \
  X(HeartbeatIntervalMs,          int64_t,     "heartbeat_interval_ms")

    namespace cfg {
#define SIMCORE_CFG_MAKE_TAG(Name, T, ColName) \
  struct Name##_t { using value_type = T; static constexpr const char* col = ColName; }; \
  inline constexpr Name##_t Name{};
        SIMCORE_CONFIG_KEYS(SIMCORE_CFG_MAKE_TAG)
#undef SIMCORE_CFG_MAKE_TAG
    } // namespace cfg

    struct SeedAny {
        enum class Kind : uint8_t { Text, Int64, Bool };
        const char* col;
        Kind kind;
        std::string text;  // used when kind==Text
        int64_t i64 = 0;   // used when kind==Int64
        bool b = false;    // used when kind==Bool
    };

    template<class KeyTag>
    inline SeedAny make_seed(KeyTag, const typename KeyTag::value_type& v) {
        using T = typename KeyTag::value_type;
        if constexpr (std::is_same_v<T, std::string>) {
            SeedAny s{ KeyTag::col, SeedAny::Kind::Text }; s.text = v; return s;
        }
        else if constexpr (std::is_same_v<T, int64_t>) {
            return SeedAny{ KeyTag::col, SeedAny::Kind::Int64, {}, v, false };
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return SeedAny{ KeyTag::col, SeedAny::Kind::Bool, {}, 0, v };
        }
        else {
            static_assert(!sizeof(T), "Unsupported value_type for config key");
        }
    }

    class ConfigRepo {
    public:
        template<class KeyTag>
        static DbResult<typename KeyTag::value_type> Get(KeyTag) {
            using T = typename KeyTag::value_type;
            return DBService::instance().submit_res<T>(OpType::Write, Priority::High, RetryPolicy{},
                [](DbEnv& env)->DbResult<T> {
                    auto db = env.handle();
                    sqlite3_stmt* st = nullptr;
                    std::string sql = std::string("SELECT ") + KeyTag::col + " FROM config WHERE config_id=1;";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                        return DbResult<T>::Err({ DbErrorKind::IO, sqlite3_errcode(db), sqlite3_errmsg(db)});
                    }
                    int rc = sqlite3_step(st);
                    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<T>::Err({ DbErrorKind::NotFound, sqlite3_errcode(db), "no row" }); }
                    if (sqlite3_column_type(st, 0) == SQLITE_NULL) { sqlite3_finalize(st); return DbResult<T>::Err({ DbErrorKind::NotFound, sqlite3_errcode(db), "NULL" }); }
                    if constexpr (std::is_same_v<T, std::string>) {
                        const unsigned char* p = sqlite3_column_text(st, 0);
                        std::string out(reinterpret_cast<const char*>(p));
                        sqlite3_finalize(st);
                        return DbResult<T>::Ok(std::move(out));
                    }
                    else if constexpr (std::is_same_v<T, int64_t>) {
                        int64_t v = sqlite3_column_int64(st, 0);
                        sqlite3_finalize(st);
                        return DbResult<T>::Ok(v);
                    }
                    else if constexpr (std::is_same_v<T, bool>) {
                        int64_t v = sqlite3_column_int64(st, 0);
                        sqlite3_finalize(st);
                        return DbResult<T>::Ok(v != 0);
                    }
                    else {
                        sqlite3_finalize(st);
                        return DbResult<T>::Err({ DbErrorKind::InvalidArg, sqlite3_errcode(db), "unsupported type" });
                    }
                }).get();
        }

        template<class KeyTag>
        static DbResult<int> Set(KeyTag, const typename KeyTag::value_type& v) {
            using T = typename KeyTag::value_type;
            return DBService::instance().submit_res<int>(OpType::Write, Priority::High, RetryPolicy{},
                [&v](DbEnv& env)->DbResult<int> {
                    auto db = env.handle();
                    sqlite3_stmt* st = nullptr;
                    std::string sql = std::string("UPDATE config SET ") + KeyTag::col + " = ? WHERE config_id=1;";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                        return DbResult<int>::Err({ DbErrorKind::IO, sqlite3_errcode(db), sqlite3_errmsg(db) });
                    }
                    if constexpr (std::is_same_v<T, std::string>) {
                        sqlite3_bind_text(st, 1, v.c_str(), -1, SQLITE_TRANSIENT);
                    }
                    else if constexpr (std::is_same_v<T, int64_t>) {
                        sqlite3_bind_int64(st, 1, v);
                    }
                    else if constexpr (std::is_same_v<T, bool>) {
                        sqlite3_bind_int64(st, 1, v ? 1 : 0);
                    }
                    else {
                        sqlite3_finalize(st);
                        return DbResult<int>::Err({ DbErrorKind::InvalidArg, sqlite3_errcode(db), "unsupported type" });
                    }
                    int rc = sqlite3_step(st);
                    int changes = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
                    sqlite3_finalize(st);
                    if (rc != SQLITE_DONE) return DbResult<int>::Err({ DbErrorKind::IO, sqlite3_errcode(db), "UPDATE failed" });
                    return DbResult<int>::Ok(changes);
                }).get();
        }

        static DbResult<int> EnsureDefaults(const std::vector<SeedAny>& items);
    };

} // namespace simcore::db
