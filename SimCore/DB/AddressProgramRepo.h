#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace simcore {
    namespace db {

        struct AddressProgramRow {
            int64_t id{};
            int32_t program_version{};
            std::vector<uint8_t> prog_bytes;
            std::optional<int32_t> derived_buffer_version;
            std::optional<std::string> derived_buffer_schema_hash;
            std::optional<std::string> soa_structs_hash;
            std::string description;
        };

        struct AddressProgramRepo {
            static std::future<DbResult<int64_t>> EnsureAsync(
                int32_t program_version,
                std::vector<uint8_t> prog_bytes,
                std::optional<int32_t> derived_buffer_version,
                std::optional<std::string> derived_buffer_schema_hash,
                std::optional<std::string> soa_structs_hash,
                std::string description,
                RetryPolicy rp = {});

            static std::future<DbResult<AddressProgramRow>> GetAsync(int64_t id, RetryPolicy rp = {});

            static inline DbResult<int64_t> Ensure(
                int32_t program_version,
                std::vector<uint8_t> prog_bytes,
                std::optional<int32_t> derived_buffer_version,
                std::optional<std::string> derived_buffer_schema_hash,
                std::optional<std::string> soa_structs_hash,
                std::string description) {
                return EnsureAsync(program_version, std::move(prog_bytes), derived_buffer_version,
                    std::move(derived_buffer_schema_hash), std::move(soa_structs_hash),
                    std::move(description)).get();
            }

            static inline DbResult<AddressProgramRow> Get(int64_t id) {
                return GetAsync(id).get();
            }
        };

    } // namespace db
} // namespace simcore
