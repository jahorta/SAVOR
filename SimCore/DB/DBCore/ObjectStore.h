#pragma once
#include "DbResult.h"
#include "DbRetryPolicy.h"
#include "DbService.h"
#include "../Querying/IdRepoListDTO.h"
#include "../Querying/Paging.h"
#include "../Querying/PagedQuery.h"
#include "Common.h"
#include <string>
#include <optional>
#include <cstdint>
#include <future>
#include <vector>

namespace simcore {
    namespace db {

        struct ObjectRefRow {
            int64_t id{};
            std::string sha256;
            Compression compression{ Compression::None };
            uint64_t size{};
            std::string filename{};
            std::string temp_path; // new: absolute path to verified temp materialization (nullable in DB)
            int64_t created_at{};
        };

        struct ObjectRefListScope {};

        struct ObjectRefList {
            static std::future<DbResult<Page<ObjectRefLite>>> ListPagedAsync(
                const PagedQuery<>& q,
                const std::string& search,
                const std::string& ext_filter,
                RetryPolicy rp = {}
            );
            static inline DbResult<Page<ObjectRefLite>> ListPaged(const PagedQuery<>& q, const std::string& s, const std::string& ext) {
                return ListPagedAsync(q, s, ext).get();
            }
        };

        class ObjectStore {
        public:

            static std::future<DbResult<ObjectRefRow>> FinalizeFromFileAsync(
                const std::string& file_path, Compression comp, std::string filename, RetryPolicy rp = {});

            static std::future<DbResult<std::string>> MaterializeToTempAsync(
                int64_t object_ref_id, RetryPolicy rp = {});

            static std::future<DbResult<ObjectRefRow>> PutTextAsync(
                const std::string& text, std::string filename = "", RetryPolicy rp = {});

            static std::future<DbResult<std::string>> GetTextAsync(
                int64_t object_ref_id, RetryPolicy rp = {});

            // Sync convenience wrappers

            static inline DbResult<ObjectRefRow> FinalizeFromFile(
                const std::string& file_path, Compression comp, std::string filename) {
                return FinalizeFromFileAsync(file_path, comp, std::move(filename)).get();
            }

            static inline DbResult<std::string> MaterializeToTemp(int64_t object_ref_id) {
                return MaterializeToTempAsync(object_ref_id).get();
            }

            static inline DbResult<ObjectRefRow> PutText(
                const std::string& text, std::string filename = "") {
                return PutTextAsync(text, std::move(filename)).get();
            }

            static inline DbResult<std::string> GetText(int64_t object_ref_id) {
                return GetTextAsync(object_ref_id).get();
            }

            static std::future<DbResult<ObjectRefRow>> GetAsync(
                int64_t object_ref_id, RetryPolicy rp = {});

            static inline DbResult<ObjectRefRow> Get(int64_t object_ref_id) {
                return GetAsync(object_ref_id).get();
            }

            // Setting/Getting root directories
            static void SetRoots(const std::string& objdir, const std::string& tmpdir);
            static const std::string& ObjDir();
            static const std::string& TmpDir();

        private:
            static std::string s_objdir;
            static std::string s_tmpdir;
            static bool s_inited;

        };

    } // namespace db
} // namespace simcore
