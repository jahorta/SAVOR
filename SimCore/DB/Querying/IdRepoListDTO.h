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
        int complete{};
    };

    struct SeedProbeLite {
        int64_t id{};
        int64_t savestate_id{};
        std::optional<int64_t> neutral_seed;
        std::string status;
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

    struct BattleRunGroupLite {
        int64_t group_id{};
        int64_t settings_id{};
        int64_t seed_probe_id{};
        std::string name;
        std::string description;
        int64_t created_at{};
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
