#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "../DBCore/Common.h"

namespace simcore::db {

    struct SavestateLite {
        int64_t id{};
        int savestate_type{};
        std::string note;
        std::optional<int64_t> object_ref_id;
        std::string filename;
        int complete{};
    };

    struct SeedProbeLite {
        int64_t id{};
        int64_t savestate_id{};
        std::optional<int64_t> neutral_seed;
        std::string status;
        std::string purpose;
        int complete{};
    };

    struct TasMovieLite {
        int64_t id{};
        int64_t base_file_id{};
        std::optional<int64_t> new_rtc;
        std::string status;
        std::optional<int64_t> created_at;
        std::optional<int64_t> started_at;
        std::optional<int64_t> completed_at;
    };

    struct ExplorerSettingsLite {
        int64_t id{};
        std::string name;
        std::string description;
        std::string purpose;
    };

    struct ObjectRefLite {
        int64_t id{};
        std::string sha256;
        Compression compression{};
        int64_t size{};
        std::string filename;
        int64_t created_at{}; // new column via migration
    };

} // namespace simcore::db
