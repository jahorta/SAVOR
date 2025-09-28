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
        static inline std::string sha256_of_file(const std::string& path) {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) throw std::runtime_error("open failed: " + path);
            const std::streamsize size = f.tellg();
            if (size < 0) throw std::runtime_error("tellg failed: " + path);
            std::string buf;
            buf.resize(static_cast<size_t>(size));
            f.seekg(0, std::ios::beg);
            if (!f.read(buf.data(), size)) throw std::runtime_error("read failed: " + path);
            return hash::sha256(buf.data(), buf.size());
        }

        static inline DbResult<ObjectRefRow> upsert_object_ref(DbEnv& env, const std::string& sha, Compression comp, int64_t size, const std::string& filename) {
            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO object_ref(sha256,compression,size,filename)"
                " VALUES(?,?,?,?)"
                " ON CONFLICT(sha256) DO UPDATE SET sha256=excluded.sha256"
                " RETURNING id,sha256,compression,size,filename", -1, &st, nullptr) != SQLITE_OK) {
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
                r.filename = (const char*)sqlite3_column_text(st, 4);
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

        std::future<DbResult<ObjectRefRow>> ObjectStore::FinalizeFromFileAsync(const std::string& file_path, const std::string& objdir, Compression comp, std::string filename, RetryPolicy rp) {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_finalize_from_file(e, file_path, objdir, comp, filename); });
        }

        static inline DbResult<std::string> impl_materialize(DbEnv& env, int64_t object_ref_id, const std::string& objdir, const std::string& tmp_dir) {
            auto* db = env.handle();
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT sha256,filename FROM object_ref WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<std::string>::Err({ map_sqlite_err(sqlite3_errcode(db)),
                                     sqlite3_errcode(db),
                                     sqlite3_errmsg(db) });

            }
            sqlite3_bind_int64(st, 1, object_ref_id);
            std::string sha, filename;
            if (sqlite3_step(st) == SQLITE_ROW) {
                sha = (const char*)sqlite3_column_text(st, 0);
                if (sqlite3_column_type(st, 1) != SQLITE_NULL) filename = (const char*)sqlite3_column_text(st, 1);
            }
            sqlite3_finalize(st);
            if (sha.empty()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0, "object_ref not found" });

            fs::create_directories(tmp_dir);
            const fs::path src = fs::path(objdir) / sha.substr(0, 2) / sha.substr(2, 2) / sha;
            const fs::path dst = fs::path(tmp_dir) / (filename.empty() ? sha : filename);
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            return DbResult<std::string>::Ok(dst.string());
        }

        std::future<DbResult<std::string>> ObjectStore::MaterializeToTempAsync(int64_t object_ref_id, const std::string& objdir, const std::string& tmp_dir, RetryPolicy rp) {
            return DBService::instance().submit_res<std::string>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_materialize(e, object_ref_id, objdir, tmp_dir); });
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
            const std::string& text, const std::string& objdir, std::string filename, RetryPolicy rp)
        {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_put_text(e, text, objdir, filename); });
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
            int64_t object_ref_id, const std::string& objdir, RetryPolicy rp)
        {
            return DBService::instance().submit_res<std::string>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_get_text(e, object_ref_id, objdir); });
        }

        std::future<DbResult<ObjectRefRow>> ObjectStore::FinalizeAsync(
            const std::string& staged_path, const std::string& objdir, Compression comp, std::string name, RetryPolicy rp)
        {
            return DBService::instance().submit_res<ObjectRefRow>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_finalize_from_file(e, staged_path, objdir, comp, name); });
        }

    } // namespace db
} // namespace simcore
