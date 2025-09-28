#pragma once
#include "DbResult.h"
#include "DbRetryPolicy.h"
#include "DbService.h"
#include <string>
#include <optional>
#include <cstdint>
#include <future>
#include <vector>

namespace simcore {
    namespace db {

        enum class Compression { None = 0, Zstd = 1, lz4 = 2 };

        struct ObjectRefRow {
            int64_t id{};
            std::string sha256;
            Compression compression{ Compression::None };
            uint64_t size{};
            std::string filename{};
        };

        class ObjectStore {
        public:
            static std::future<DbResult<ObjectRefRow>> FinalizeAsync(const std::string& staged_path,
                const std::string& objdir, Compression comp = Compression::None, std::string name = "", RetryPolicy rp = {});

            static std::future<DbResult<ObjectRefRow>> FinalizeFromFileAsync(const std::string& file_path,
                const std::string& objdir, Compression comp = Compression::None, std::string filename = "", RetryPolicy rp = {});

            static std::future<DbResult<std::string>> MaterializeToTempAsync(int64_t object_ref_id, const std::string& objdir, const std::string& tmp_dir, RetryPolicy rp = {});

            static std::future<DbResult<ObjectRefRow>> PutTextAsync(
                const std::string& text,
                const std::string& objdir,
                std::string filename = "",
                RetryPolicy rp = {}
            );

            static std::future<DbResult<std::string>> GetTextAsync(
                int64_t object_ref_id,
                const std::string& objdir,
                RetryPolicy rp = {}
            );

            // Blocking
            static inline DbResult<ObjectRefRow> Finalize(const std::string& staged_path,
                const std::string& objdir, Compression comp = Compression::None, std::string name = "") {
                return FinalizeAsync(staged_path, objdir, comp, name).get();
            }
            static inline DbResult<ObjectRefRow> FinalizeFromFile(const std::string& file_path,
                const std::string& objdir, Compression comp = Compression::None, std::string filename = "") {
                return FinalizeFromFileAsync(file_path, objdir, comp, filename).get();
            }
            static inline DbResult<std::string> MaterializeToTemp(int64_t object_ref_id, const std::string& objdir, const std::string& tmp_dir) {
                return MaterializeToTempAsync(object_ref_id, objdir, tmp_dir).get();
            }
            static inline DbResult<ObjectRefRow> PutText(const std::string& text, const std::string& objdir, std::string filename = "") {
                return PutTextAsync(text, objdir, std::move(filename)).get();
            }
            static inline DbResult<std::string> GetText(int64_t object_ref_id, const std::string& objdir) {
                return GetTextAsync(object_ref_id, objdir).get();
            }

        };

    } // namespace db
} // namespace simcore
