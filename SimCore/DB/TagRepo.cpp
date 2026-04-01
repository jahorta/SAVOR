#include "TagRepo.h"

#include "DBCore/DbService.h"

#include <sqlite3.h>
#include <algorithm>
#include <cctype>

namespace simcore::db {
namespace {

    static inline bool is_blank(const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    }

    static inline DbResult<void> invalid_arg(const char* msg) {
        return DbResult<void>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, msg });
    }

    static inline DbResult<TagRecord> invalid_arg_tag(const char* msg) {
        return DbResult<TagRecord>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, msg });
    }

    static inline DbResult<std::vector<TagRecord>> invalid_arg_tags(const char* msg) {
        return DbResult<std::vector<TagRecord>>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, msg });
    }

    static inline DbResult<std::vector<int64_t>> invalid_arg_ids(const char* msg) {
        return DbResult<std::vector<int64_t>>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, msg });
    }

    struct ParsedTag {
        std::string tag_key;
        std::string namespace_name;
        std::string leaf_name;
        std::optional<std::string> parent_key;
    };

    static DbResult<ParsedTag> parse_tag(std::string tag_key_in) {
        if (tag_key_in.empty() || is_blank(tag_key_in)) {
            return DbResult<ParsedTag>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, "tag_key cannot be blank" });
        }

        // trim spaces around whole string
        while (!tag_key_in.empty() && std::isspace(static_cast<unsigned char>(tag_key_in.front()))) tag_key_in.erase(tag_key_in.begin());
        while (!tag_key_in.empty() && std::isspace(static_cast<unsigned char>(tag_key_in.back()))) tag_key_in.pop_back();

        ParsedTag out{};
        out.tag_key = tag_key_in;

        const auto first_sep = tag_key_in.find("::");
        if (first_sep == std::string::npos) {
            out.namespace_name = tag_key_in;
            out.leaf_name = tag_key_in;
            out.parent_key = std::nullopt;
            return DbResult<ParsedTag>::Ok(std::move(out));
        }

        const auto next_sep = tag_key_in.find("::", first_sep + 2);
        if (next_sep != std::string::npos) {
            return DbResult<ParsedTag>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, "tag_key supports at most one '::' separator" });
        }

        const std::string parent = tag_key_in.substr(0, first_sep);
        const std::string leaf = tag_key_in.substr(first_sep + 2);
        if (parent.empty() || leaf.empty()) {
            return DbResult<ParsedTag>::Err({ DbErrorKind::InvalidArgument, SQLITE_MISUSE, "tag_key cannot have empty parent or child" });
        }

        out.namespace_name = parent;
        out.leaf_name = leaf;
        out.parent_key = parent;
        return DbResult<ParsedTag>::Ok(std::move(out));
    }

    static DbResult<std::optional<TagRecord>> find_tag_by_key(sqlite3* db, const std::string& tag_key) {
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT tag_id, tag_key, namespace, leaf_name, parent_tag_id, description, created_at, updated_at "
            "FROM tags WHERE tag_key = ?;";
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        if (rc != SQLITE_OK) {
            return DbResult<std::optional<TagRecord>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
        }

        sqlite3_bind_text(st, 1, tag_key.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(st);
            return DbResult<std::optional<TagRecord>>::Ok(std::nullopt);
        }
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(st);
            return DbResult<std::optional<TagRecord>>::Err({ map_sqlite_err(rc), rc, "select tag by key" });
        }

        TagRecord t{};
        t.tag_id = sqlite3_column_int64(st, 0);
        t.tag_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        t.namespace_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        t.leaf_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) t.parent_tag_id = sqlite3_column_int64(st, 4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        t.created_at = sqlite3_column_int64(st, 6);
        t.updated_at = sqlite3_column_int64(st, 7);
        sqlite3_finalize(st);
        return DbResult<std::optional<TagRecord>>::Ok(std::move(t));
    }

    static DbResult<TagRecord> insert_tag(sqlite3* db, const ParsedTag& parsed, std::optional<int64_t> parent_id, std::optional<std::string> description) {
        sqlite3_stmt* st{};
        const char* sql =
            "INSERT INTO tags(tag_key, namespace, leaf_name, parent_tag_id, description, created_at, updated_at) "
            "VALUES(?,?,?,?,?,strftime('%s','now'),strftime('%s','now')) "
            "RETURNING tag_id, tag_key, namespace, leaf_name, parent_tag_id, description, created_at, updated_at;";
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        if (rc != SQLITE_OK) {
            return DbResult<TagRecord>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
        }

        sqlite3_bind_text(st, 1, parsed.tag_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, parsed.namespace_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, parsed.leaf_name.c_str(), -1, SQLITE_TRANSIENT);
        if (parent_id.has_value()) sqlite3_bind_int64(st, 4, *parent_id);
        else sqlite3_bind_null(st, 4);
        if (description.has_value()) sqlite3_bind_text(st, 5, description->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 5);

        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(st);
            return DbResult<TagRecord>::Err({ map_sqlite_err(rc), rc, "insert tag" });
        }

        TagRecord t{};
        t.tag_id = sqlite3_column_int64(st, 0);
        t.tag_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        t.namespace_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        t.leaf_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) t.parent_tag_id = sqlite3_column_int64(st, 4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        t.created_at = sqlite3_column_int64(st, 6);
        t.updated_at = sqlite3_column_int64(st, 7);

        sqlite3_finalize(st);
        return DbResult<TagRecord>::Ok(std::move(t));
    }

    static DbResult<TagRecord> ensure_tag_impl(DbEnv& env, const std::string& tag_key, std::optional<std::string> description) {
        auto parsed_res = parse_tag(tag_key);
        if (!parsed_res.ok()) return DbResult<TagRecord>::Err(parsed_res.error());
        const auto parsed = parsed_res.value();

        sqlite3* db = env.handle();

        auto existing = find_tag_by_key(db, parsed.tag_key);
        if (!existing.ok()) return DbResult<TagRecord>::Err(existing.error());
        if (existing.value().has_value()) return DbResult<TagRecord>::Ok(*existing.value());

        std::optional<int64_t> parent_id = std::nullopt;
        if (parsed.parent_key.has_value()) {
            auto parent = find_tag_by_key(db, *parsed.parent_key);
            if (!parent.ok()) return DbResult<TagRecord>::Err(parent.error());
            if (!parent.value().has_value()) {
                auto parent_parsed_res = parse_tag(*parsed.parent_key);
                if (!parent_parsed_res.ok()) return DbResult<TagRecord>::Err(parent_parsed_res.error());
                auto parent_insert = insert_tag(db, parent_parsed_res.value(), std::nullopt, std::nullopt);
                if (!parent_insert.ok()) return parent_insert;
                parent_id = parent_insert.value().tag_id;
            }
            else {
                parent_id = parent.value()->tag_id;
            }
        }

        auto inserted = insert_tag(db, parsed, parent_id, std::move(description));
        if (!inserted.ok()) {
            // Potential race on UNIQUE(tag_key). Re-select once.
            auto after = find_tag_by_key(db, parsed.tag_key);
            if (after.ok() && after.value().has_value()) return DbResult<TagRecord>::Ok(*after.value());
            return inserted;
        }
        return inserted;
    }

} // namespace

std::future<DbResult<TagRecord>> TagRepo::EnsureTagAsync(const std::string& tag_key, std::optional<std::string> description, RetryPolicy rp) {
    if (tag_key.empty() || is_blank(tag_key)) {
        std::promise<DbResult<TagRecord>> p;
        p.set_value(invalid_arg_tag("tag_key cannot be blank"));
        return p.get_future();
    }

    return DBService::instance().submit_res<TagRecord>(OpType::Write, Priority::Normal, rp,
        [tag_key, description = std::move(description)](DbEnv& env) mutable {
            return ensure_tag_impl(env, tag_key, std::move(description));
        });
}

std::future<DbResult<std::vector<TagRecord>>> TagRepo::ListTagsAsync(std::optional<std::string> namespace_filter, RetryPolicy rp) {
    return DBService::instance().submit_res<std::vector<TagRecord>>(OpType::Read, Priority::Normal, rp,
        [namespace_filter = std::move(namespace_filter)](DbEnv& env) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};

            const char* sql_all =
                "SELECT tag_id, tag_key, namespace, leaf_name, parent_tag_id, description, created_at, updated_at "
                "FROM tags ORDER BY namespace ASC, leaf_name ASC, tag_id ASC;";
            const char* sql_ns =
                "SELECT tag_id, tag_key, namespace, leaf_name, parent_tag_id, description, created_at, updated_at "
                "FROM tags WHERE namespace = ? ORDER BY leaf_name ASC, tag_id ASC;";

            int rc = sqlite3_prepare_v2(db, namespace_filter.has_value() ? sql_ns : sql_all, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }
            if (namespace_filter.has_value()) sqlite3_bind_text(st, 1, namespace_filter->c_str(), -1, SQLITE_TRANSIENT);

            std::vector<TagRecord> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                TagRecord t{};
                t.tag_id = sqlite3_column_int64(st, 0);
                t.tag_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                t.namespace_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                t.leaf_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) t.parent_tag_id = sqlite3_column_int64(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
                t.created_at = sqlite3_column_int64(st, 6);
                t.updated_at = sqlite3_column_int64(st, 7);
                out.push_back(std::move(t));
            }

            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(rc), rc, "list tags" });
            }
            return DbResult<std::vector<TagRecord>>::Ok(std::move(out));
        });
}

std::future<DbResult<std::vector<TagRecord>>> TagRepo::ListTagsForEntityKindAsync(const std::string& entity_kind, RetryPolicy rp) {
    if (entity_kind.empty() || is_blank(entity_kind)) {
        std::promise<DbResult<std::vector<TagRecord>>> p;
        p.set_value(invalid_arg_tags("entity_kind cannot be blank"));
        return p.get_future();
    }

    return DBService::instance().submit_res<std::vector<TagRecord>>(OpType::Read, Priority::Normal, rp,
        [entity_kind](DbEnv& env) {
            sqlite3* db = env.handle();
            const char* sql =
                "SELECT DISTINCT t.tag_id, t.tag_key, t.namespace, t.leaf_name, t.parent_tag_id, t.description, t.created_at, t.updated_at "
                "FROM entity_tags et "
                "JOIN tags t ON t.tag_id = et.tag_id "
                "WHERE et.entity_kind = ? "
                "ORDER BY t.namespace ASC, t.leaf_name ASC, t.tag_id ASC;";

            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }
            sqlite3_bind_text(st, 1, entity_kind.c_str(), -1, SQLITE_TRANSIENT);

            std::vector<TagRecord> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                TagRecord t{};
                t.tag_id = sqlite3_column_int64(st, 0);
                t.tag_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                t.namespace = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                t.leaf_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) t.parent_tag_id = sqlite3_column_int64(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
                t.created_at = sqlite3_column_int64(st, 6);
                t.updated_at = sqlite3_column_int64(st, 7);
                out.push_back(std::move(t));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(rc), rc, "list tags for entity kind" });
            }
            return DbResult<std::vector<TagRecord>>::Ok(std::move(out));
        });
}

std::future<DbResult<void>> TagRepo::AttachTagToEntityAsync(const std::string& entity_kind, int64_t entity_id, const std::string& tag_key, std::optional<std::string> created_by, RetryPolicy rp) {
    if (entity_kind.empty() || is_blank(entity_kind)) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("entity_kind cannot be blank"));
        return p.get_future();
    }
    if (entity_id <= 0) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("entity_id must be > 0"));
        return p.get_future();
    }
    if (tag_key.empty() || is_blank(tag_key)) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("tag_key cannot be blank"));
        return p.get_future();
    }

    return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
        [entity_kind, entity_id, tag_key, created_by = std::move(created_by)](DbEnv& env) mutable {
            auto tag = ensure_tag_impl(env, tag_key, std::nullopt);
            if (!tag.ok()) return DbResult<void>::Err(tag.error());

            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "INSERT OR IGNORE INTO entity_tags(entity_kind, entity_id, tag_id, created_at, created_by) "
                "VALUES(?,?,?,strftime('%s','now'),?);";
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }

            sqlite3_bind_text(st, 1, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, entity_id);
            sqlite3_bind_int64(st, 3, tag.value().tag_id);
            if (created_by.has_value()) sqlite3_bind_text(st, 4, created_by->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(st, 4);

            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<void>::Err({ map_sqlite_err(rc), rc, "insert entity tag" });
            }
            return DbResult<void>::Ok();
        });
}

std::future<DbResult<void>> TagRepo::DetachTagFromEntityAsync(const std::string& entity_kind, int64_t entity_id, const std::string& tag_key, RetryPolicy rp) {
    if (entity_kind.empty() || is_blank(entity_kind)) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("entity_kind cannot be blank"));
        return p.get_future();
    }
    if (entity_id <= 0) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("entity_id must be > 0"));
        return p.get_future();
    }
    if (tag_key.empty() || is_blank(tag_key)) {
        std::promise<DbResult<void>> p;
        p.set_value(invalid_arg("tag_key cannot be blank"));
        return p.get_future();
    }

    return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
        [entity_kind, entity_id, tag_key](DbEnv& env) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "DELETE FROM entity_tags "
                "WHERE entity_kind = ? AND entity_id = ? "
                "  AND tag_id = (SELECT tag_id FROM tags WHERE tag_key = ?);";
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }
            sqlite3_bind_text(st, 1, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, entity_id);
            sqlite3_bind_text(st, 3, tag_key.c_str(), -1, SQLITE_TRANSIENT);

            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete entity tag" });
            }
            return DbResult<void>::Ok();
        });
}

std::future<DbResult<std::vector<TagRecord>>> TagRepo::ListEntityTagsAsync(const std::string& entity_kind, int64_t entity_id, RetryPolicy rp) {
    if (entity_kind.empty() || is_blank(entity_kind)) {
        std::promise<DbResult<std::vector<TagRecord>>> p;
        p.set_value(invalid_arg_tags("entity_kind cannot be blank"));
        return p.get_future();
    }
    if (entity_id <= 0) {
        std::promise<DbResult<std::vector<TagRecord>>> p;
        p.set_value(invalid_arg_tags("entity_id must be > 0"));
        return p.get_future();
    }

    return DBService::instance().submit_res<std::vector<TagRecord>>(OpType::Read, Priority::Normal, rp,
        [entity_kind, entity_id](DbEnv& env) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "SELECT t.tag_id, t.tag_key, t.namespace, t.leaf_name, t.parent_tag_id, t.description, t.created_at, t.updated_at "
                "FROM entity_tags et "
                "JOIN tags t ON t.tag_id = et.tag_id "
                "WHERE et.entity_kind = ? AND et.entity_id = ? "
                "ORDER BY t.namespace ASC, t.leaf_name ASC, t.tag_id ASC;";
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }
            sqlite3_bind_text(st, 1, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, entity_id);

            std::vector<TagRecord> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                TagRecord t{};
                t.tag_id = sqlite3_column_int64(st, 0);
                t.tag_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                t.namespace_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                t.leaf_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) t.parent_tag_id = sqlite3_column_int64(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) t.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
                t.created_at = sqlite3_column_int64(st, 6);
                t.updated_at = sqlite3_column_int64(st, 7);
                out.push_back(std::move(t));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<std::vector<TagRecord>>::Err({ map_sqlite_err(rc), rc, "list entity tags" });
            }
            return DbResult<std::vector<TagRecord>>::Ok(std::move(out));
        });
}

std::future<DbResult<std::vector<int64_t>>> TagRepo::FindEntityIdsByTagAsync(const std::string& entity_kind, const std::string& tag_key, bool include_children, RetryPolicy rp) {
    if (entity_kind.empty() || is_blank(entity_kind)) {
        std::promise<DbResult<std::vector<int64_t>>> p;
        p.set_value(invalid_arg_ids("entity_kind cannot be blank"));
        return p.get_future();
    }
    if (tag_key.empty() || is_blank(tag_key)) {
        std::promise<DbResult<std::vector<int64_t>>> p;
        p.set_value(invalid_arg_ids("tag_key cannot be blank"));
        return p.get_future();
    }

    return DBService::instance().submit_res<std::vector<int64_t>>(OpType::Read, Priority::Normal, rp,
        [entity_kind, tag_key, include_children](DbEnv& env) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};

            const char* sql_exact =
                "SELECT DISTINCT et.entity_id "
                "FROM entity_tags et "
                "JOIN tags t ON t.tag_id = et.tag_id "
                "WHERE et.entity_kind = ? AND t.tag_key = ? "
                "ORDER BY et.entity_id ASC;";

            const char* sql_with_children =
                "WITH parent AS (SELECT tag_id FROM tags WHERE tag_key = ?) "
                "SELECT DISTINCT et.entity_id "
                "FROM entity_tags et "
                "JOIN tags t ON t.tag_id = et.tag_id "
                "WHERE et.entity_kind = ? "
                "  AND (t.tag_key = ? OR t.parent_tag_id IN parent) "
                "ORDER BY et.entity_id ASC;";

            int rc = sqlite3_prepare_v2(db, include_children ? sql_with_children : sql_exact, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
            }

            if (include_children) {
                sqlite3_bind_text(st, 1, tag_key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, tag_key.c_str(), -1, SQLITE_TRANSIENT);
            }
            else {
                sqlite3_bind_text(st, 1, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, tag_key.c_str(), -1, SQLITE_TRANSIENT);
            }

            std::vector<int64_t> ids;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                ids.push_back(sqlite3_column_int64(st, 0));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "find entities by tag" });
            }
            return DbResult<std::vector<int64_t>>::Ok(std::move(ids));
        });
}

} // namespace simcore::db
