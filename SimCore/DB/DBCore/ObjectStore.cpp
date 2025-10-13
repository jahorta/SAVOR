#include "ObjectStore.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include "../../Utils/Hash.h"

namespace fs = std::filesystem;

namespace simcore {
    namespace db {

        std::string ObjectStore::s_objdir;
        std::string ObjectStore::s_tmpdir;
        bool ObjectStore::s_inited = false;

        void ObjectStore::SetRoots(const std::string& objdir, const std::string& tmpdir) {
            fs::path o = fs::absolute(objdir);
            fs::path t = fs::absolute(tmpdir);
            std::error_code ec;
            fs::create_directories(o, ec);
            fs::create_directories(t, ec);
            s_objdir = o.string();
            s_tmpdir = t.string();
            s_inited = true;
        }

        const std::string& ObjectStore::ObjDir() { return s_objdir; }
        const std::string& ObjectStore::TmpDir() { return s_tmpdir; }

        // small helper at file-scope
        static inline DbResult<std::string> err_not_inited() {
            return DbResult<std::string>::Err({ DbErrorKind::InvalidArgument, 0, "ObjectStore roots not initialized" });
        }

        static inline DbResult<ObjectRefRow> upsert_object_ref(DbEnv& env,
            const std::string& sha, Compression comp, int64_t size, const std::string& filename) {
            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO object_ref(sha256,compression,size,filename)"
                " VALUES(?,?,?,?)"
                " ON CONFLICT(sha256) DO UPDATE SET sha256=excluded.sha256"
                " RETURNING id,sha256,compression,size,filename,temp_path",
                -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<ObjectRefRow>::Err({ map_sqlite_err(sqlite3_errcode(db)),
                                                     sqlite3_errcode(db),
                                                     sqlite3_errmsg(db) });
            }
            sqlite3_bind_text(st, 1, sha.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, static_cast<int>(comp));
            sqlite3_bind_int64(st, 3, size);
            sqlite3_bind_text(st, 4, filename.c_str(), -1, SQLITE_TRANSIENT);

            ObjectRefRow r{};
            if (sqlite3_step(st) == SQLITE_ROW) {
                r.id = sqlite3_column_int64(st, 0);
                r.sha256 = (const char*)sqlite3_column_text(st, 1);
                r.compression = static_cast<Compression>(sqlite3_column_int(st, 2));
                r.size = sqlite3_column_int64(st, 3);
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.filename = (const char*)sqlite3_column_text(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.temp_path = (const char*)sqlite3_column_text(st, 5);
                sqlite3_finalize(st);
                return DbResult<ObjectRefRow>::Ok(std::move(r));
            }
            sqlite3_finalize(st);
            return DbResult<ObjectRefRow>::Err({ DbErrorKind::IO, 0, "object_ref upsert failed" });
        }

        static inline DbResult<ObjectRefRow> impl_finalize_from_file(DbEnv& env, const std::string& file_path, const std::string& objdir, Compression comp, const std::string& filename) {
            const std::string sha = sha256_of_file(file_path);
            const fs::path dst_dir = fs::path(objdir) / sha.substr(0, 2) / sha.substr(2, 2);
            const fs::path dst_path = dst_dir / sha;
            fs::create_directories(dst_dir);
            if (!fs::exists(dst_path)) {
                fs::copy_file(file_path, dst_path, fs::copy_options::overwrite_existing);
            }
            const auto size = (int64_t)fs::file_size(dst_path);
            return upsert_object_ref(env, sha, comp, size, filename);
        }

        std::future<DbResult<ObjectRefRow>> ObjectStore::FinalizeFromFileAsync(
            const std::string& file_path, Compression comp, std::string filename, RetryPolicy rp)
        {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) -> DbResult<ObjectRefRow> {
                    if (!s_inited) return DbResult<ObjectRefRow>::Err({ DbErrorKind::InvalidArgument, 0, "ObjectStore roots not initialized" });
                    // impl_finalize_from_file now uses ObjDir() internally
                    return impl_finalize_from_file(e, file_path, ObjDir(), comp, filename);
                });
        }

        static inline DbResult<std::string> impl_materialize(DbEnv& env,
            int64_t object_ref_id, const std::string& objdir, const std::string& tmp_dir) {

            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "SELECT sha256,filename,size,temp_path FROM object_ref WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<std::string>::Err({ map_sqlite_err(sqlite3_errcode(db)),
                                                    sqlite3_errcode(db),
                                                    sqlite3_errmsg(db) });
            }
            sqlite3_bind_int64(st, 1, object_ref_id);

            std::string sha, filename, temp_path;
            int64_t size = 0;
            if (sqlite3_step(st) == SQLITE_ROW) {
                if (sqlite3_column_type(st, 0) != SQLITE_NULL) sha = (const char*)sqlite3_column_text(st, 0);
                if (sqlite3_column_type(st, 1) != SQLITE_NULL) filename = (const char*)sqlite3_column_text(st, 1);
                size = sqlite3_column_int64(st, 2);
                if (sqlite3_column_type(st, 3) != SQLITE_NULL) temp_path = (const char*)sqlite3_column_text(st, 3);
            }
            sqlite3_finalize(st);
            if (sha.empty()) {
                return DbResult<std::string>::Err({ DbErrorKind::NotFound, SQLITE_NOTFOUND, "object_ref not found" });
            }

            // Helper to validate size + sha
            auto file_ok = [&](const fs::path& p) -> bool {
                std::error_code ec;
                if (!fs::exists(p, ec)) return false;
                if (size > 0) {
                    std::uintmax_t s = fs::file_size(p, ec);
                    if (ec) return false;
                    if ((int64_t)s != size) return false;
                }
                try {
                    const std::string have = sha256_of_file(p.string());
                    return have == sha;
                }
                catch (...) {
                    return false;
                }
                };

            // Fast path: reuse valid temp_path
            if (!temp_path.empty()) {
                fs::path tp(temp_path);
                if (file_ok(tp)) {
                    return DbResult<std::string>::Ok(tp.string());
                }
                else {
                    // Clear stale hint (best-effort)
                    sqlite3_stmt* ust = nullptr;
                    if (sqlite3_prepare_v2(db,
                        "UPDATE object_ref SET temp_path=NULL WHERE id=? AND temp_path=?",
                        -1, &ust, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int64(ust, 1, object_ref_id);
                        sqlite3_bind_text(ust, 2, temp_path.c_str(), -1, SQLITE_TRANSIENT);
                        (void)sqlite3_step(ust);
                    }
                    if (ust) sqlite3_finalize(ust);
                }
            }

            // Canonical source inside the object store
            const fs::path src = fs::path(objdir) / sha.substr(0, 2) / sha.substr(2, 2) / sha;
            if (!fs::exists(src)) {
                return DbResult<std::string>::Err({ DbErrorKind::NotFound, SQLITE_NOTFOUND, "object file missing: " + src.string() });
            }

            // Deterministic destination in tmp_dir; fall back to sha if collision doesn't match
            fs::path dst = fs::path(tmp_dir) / (filename.empty() ? sha : filename);
            if (fs::exists(dst) && !file_ok(dst)) {
                dst = fs::path(tmp_dir) / sha;
            }

            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            try {
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            }
            catch (const std::exception& e) {
                return DbResult<std::string>::Err({ DbErrorKind::IO, 0, std::string("copy failed: ") + e.what() });
            }

            if (!file_ok(dst)) {
                return DbResult<std::string>::Err({ DbErrorKind::IO, 0, "materialized file failed verification" });
            }

            // Persist verified hint
            const std::string canon = fs::absolute(dst).string();
            sqlite3_stmt* ins = nullptr;
            if (sqlite3_prepare_v2(db, "UPDATE object_ref SET temp_path=? WHERE id=?", -1, &ins, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, canon.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(ins, 2, object_ref_id);
                (void)sqlite3_step(ins);
            }
            if (ins) sqlite3_finalize(ins);

            return DbResult<std::string>::Ok(canon);
        }

        std::future<DbResult<std::string>> ObjectStore::MaterializeToTempAsync(
            int64_t object_ref_id, RetryPolicy rp)
        {
            return DBService::instance().submit_res<std::string>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) -> DbResult<std::string> {
                    if (!s_inited) return err_not_inited();
                    return impl_materialize(e, object_ref_id, ObjDir(), TmpDir());
                });
        }

        static inline DbResult<ObjectRefRow> upsert_from_bytes(DbEnv& env,
            const std::string& sha, Compression comp, int64_t size, const std::string& filename)
        {
            return upsert_object_ref(env, sha, comp, size, filename);
        }

        static inline DbResult<ObjectRefRow> impl_put_text(DbEnv& env,
            const std::string& text, const std::string& objdir, const std::string& filename)
        {
            const std::string sha = hash::sha256(text.data(), text.size());
            const fs::path dst_dir = fs::path(objdir) / sha.substr(0, 2) / sha.substr(2, 2);
            const fs::path dst_path = dst_dir / sha;
            fs::create_directories(dst_dir);
            if (!fs::exists(dst_path)) {
                std::ofstream o(dst_path, std::ios::binary);
                if (!o) return DbResult<ObjectRefRow>::Err({ DbErrorKind::IO, 0, "open failed: " + dst_path.string() });
                o.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!o) return DbResult<ObjectRefRow>::Err({ DbErrorKind::IO, 0, "write failed: " + dst_path.string() });
            }
            return upsert_from_bytes(env, sha, Compression::None, static_cast<int64_t>(text.size()), filename);
        }

        std::future<DbResult<ObjectRefRow>> ObjectStore::PutTextAsync(
            const std::string& text, std::string filename, RetryPolicy rp)
        {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) -> DbResult<ObjectRefRow> {
                    if (!s_inited) return DbResult<ObjectRefRow>::Err({ DbErrorKind::InvalidArgument, 0, "ObjectStore roots not initialized" });
                    return impl_put_text(e, text, ObjDir(), filename);
                });
        }

        static inline DbResult<std::string> impl_get_text(DbEnv& env, int64_t object_ref_id, const std::string& objdir)
        {
            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT sha256,size FROM object_ref WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<std::string>::Err({ map_sqlite_err(sqlite3_errcode(db)),
                                                    sqlite3_errcode(db),
                                                    sqlite3_errmsg(db) });
            }
            sqlite3_bind_int64(st, 1, object_ref_id);
            std::string sha;
            int64_t size = -1;
            if (sqlite3_step(st) == SQLITE_ROW) {
                sha = (const char*)sqlite3_column_text(st, 0);
                size = sqlite3_column_int64(st, 1);
            }
            sqlite3_finalize(st);
            if (sha.empty()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0, "object_ref not found" });

            const fs::path src = fs::path(objdir) / sha.substr(0, 2) / sha.substr(2, 2) / sha;
            std::ifstream i(src, std::ios::binary | std::ios::ate);
            if (!i) return DbResult<std::string>::Err({ DbErrorKind::IO, 0, "open failed: " + src.string() });
            const std::streamsize fsz = i.tellg();
            if (fsz < 0) return DbResult<std::string>::Err({ DbErrorKind::IO, 0, "tellg failed: " + src.string() });
            std::string out;
            out.resize(static_cast<size_t>(fsz));
            i.seekg(0, std::ios::beg);
            if (!i.read(out.data(), fsz)) return DbResult<std::string>::Err({ DbErrorKind::IO, 0, "read failed: " + src.string() });
            return DbResult<std::string>::Ok(std::move(out));
        }

        std::future<DbResult<std::string>> ObjectStore::GetTextAsync(
            int64_t object_ref_id, RetryPolicy rp)
        {
            return DBService::instance().submit_res<std::string>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) -> DbResult<std::string> {
                    if (!s_inited) return err_not_inited();
                    return impl_get_text(e, object_ref_id, ObjDir());
                });
        }

        static inline DbResult<ObjectRefRow> impl_get_obj(DbEnv& env, int64_t object_ref_id) {
            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT id,sha256,compression,size,filename,temp_path FROM object_ref WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<ObjectRefRow>::Err({ map_sqlite_err(sqlite3_errcode(db)),
                                                     sqlite3_errcode(db),
                                                     sqlite3_errmsg(db) });
            }
            sqlite3_bind_int64(st, 1, object_ref_id);
            ObjectRefRow r{};
            if (sqlite3_step(st) == SQLITE_ROW) {
                r.id = sqlite3_column_int64(st, 0);
                if (sqlite3_column_type(st, 1) != SQLITE_NULL) r.sha256 = (const char*)sqlite3_column_text(st, 1);
                r.compression = static_cast<Compression>(sqlite3_column_int(st, 2));
                r.size = sqlite3_column_int64(st, 3);
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.filename = (const char*)sqlite3_column_text(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.temp_path = (const char*)sqlite3_column_text(st, 5);
                sqlite3_finalize(st);
                return DbResult<ObjectRefRow>::Ok(std::move(r));
            }
            sqlite3_finalize(st);
            return DbResult<ObjectRefRow>::Err({ DbErrorKind::NotFound, 0, "object_ref not found" });
        }

        std::future<DbResult<ObjectRefRow>> ObjectStore::GetAsync(
            int64_t object_ref_id, RetryPolicy rp)
        {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) -> DbResult<ObjectRefRow> {
                    if (!s_inited) return DbResult<ObjectRefRow>::Err({ DbErrorKind::InvalidArgument, 0, "ObjectStore roots not initialized" });
                    return impl_get_obj(e, object_ref_id);
                });
        }

        static inline void bind_like(sqlite3_stmt* st, int idx, const std::string& s) {
            std::string pat = "%" + s + "%";
            sqlite3_bind_text(st, idx, pat.c_str(), -1, SQLITE_TRANSIENT);
        }

        static inline DbResult<Page<ObjectRefLite>> Impl_List(DbEnv& env, const PagedQuery<>& q, const std::string& search, const std::string& ext) {
            sqlite3* db = env.handle();
            std::string sql =
                "SELECT id,sha256,compression,size,COALESCE(filename,''),created_at "
                "FROM object_ref ";
            std::string where;
            if (!search.empty()) { where += (where.empty() ? "WHERE " : " AND "); where += "filename LIKE ? "; }
            if (!ext.empty()) { where += (where.empty() ? "WHERE " : " AND "); where += "filename LIKE ? "; }
            std::string order = " ORDER BY created_at DESC, id DESC ";
            std::string keyset;
            KeysetCursor cur{};
            bool has_cursor = false;
            if (q.before) { has_cursor = true; cur = *q.before; keyset = " AND (created_at < ? OR (created_at = ? AND id < ?)) "; }
            if (q.after) { has_cursor = true; cur = *q.after;  keyset = " AND (created_at > ? OR (created_at = ? AND id > ?)) "; order = " ORDER BY created_at ASC, id ASC "; }
            if (has_cursor) where += (where.empty() ? "WHERE " : " AND "), where += keyset.substr(5);
            sql += where + order + " LIMIT ?;";
            sqlite3_stmt* st{};
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
                return DbResult<Page<ObjectRefLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

            int b = 1;
            if (!search.empty()) bind_like(st, b++, search);
            if (!ext.empty()) {
                std::string pat = "%" + ext;
                sqlite3_bind_text(st, b++, pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (has_cursor) { sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.secondary); }
            sqlite3_bind_int(st, b++, q.limit);

            Page<ObjectRefLite> page{};
            while (sqlite3_step(st) == SQLITE_ROW) {
                ObjectRefLite r{};
                r.id = sqlite3_column_int64(st, 0);
                r.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                r.compression = static_cast<Compression>(sqlite3_column_int(st, 2));
                r.size = sqlite3_column_int64(st, 3);
                r.filename = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.created_at = sqlite3_column_int64(st, 5);
                page.items.push_back(std::move(r));
            }
            sqlite3_finalize(st);

            if (q.after && !page.items.empty()) { std::reverse(page.items.begin(), page.items.end()); }

            if (!page.items.empty()) {
                const auto& first = page.items.front();
                const auto& last = page.items.back();
                page.prev = KeysetCursor{ first.created_at, (int64_t)first.id };
                page.next = KeysetCursor{ last.created_at,  (int64_t)last.id };
            }
            return DbResult<Page<ObjectRefLite>>::Ok(std::move(page));
        }

        std::future<DbResult<Page<ObjectRefLite>>> ObjectRefList::ListPagedAsync(const PagedQuery<>& q, const std::string& search, const std::string& ext_filter, RetryPolicy rp) {
            return DBService::instance().submit_res<Page<ObjectRefLite>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_List(e, q, search, ext_filter); });
        }

} // namespace db
} // namespace simcore
