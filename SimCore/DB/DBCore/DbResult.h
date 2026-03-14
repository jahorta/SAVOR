#pragma once
#include <string>
#include <utility>
#include <sqlite3.h>

namespace simcore::db {

        enum class DbErrorKind {
            Busy,
            Locked,
            Constraint,
            IO,
            Unique,
            NotFound,
            InvalidArgument,
            InvalidState,
            InvalidData,
            Unknown
        };

        struct DbError {
            DbErrorKind kind{ DbErrorKind::Unknown };
            int sqlite_code{ 0 };
            std::string message;
        };

        static DbErrorKind map_sqlite_err(int code) {
            if (code == SQLITE_BUSY) return DbErrorKind::Busy;
            if (code == SQLITE_LOCKED) return DbErrorKind::Locked;
            if (code == SQLITE_CONSTRAINT || code == SQLITE_CONSTRAINT_PRIMARYKEY)
                return DbErrorKind::Constraint;
            if (code == SQLITE_CONSTRAINT_UNIQUE)
                return DbErrorKind::Unique;
            if (code == SQLITE_IOERR) return DbErrorKind::IO;
            return DbErrorKind::Unknown;
        }

        template <typename T>
        struct DbResult {
            bool ok{ false };
            T value{};
            DbError error{};

            static DbResult Ok(T v) { DbResult r; r.ok = true; r.value = std::move(v); return r; }
            static DbResult Err(DbError e) { DbResult r; r.ok = false; r.error = std::move(e); return r; }
            
        };

        template <>
        struct DbResult<void> {
            bool ok{ false };
            DbError error{};

            static DbResult Ok() {
                DbResult r;
                r.ok = true;
                return r;
            }
            static DbResult Err(DbError e) {
                DbResult r;
                r.ok = false;
                r.error = std::move(e);
                return r;
            }
        };



} // namespace simcore::db
