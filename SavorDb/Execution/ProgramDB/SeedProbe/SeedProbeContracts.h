#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../../SavorCore/Utils/Hex.h"
#include "../../../../SavorCore/Utils/IniDoc.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Script/PhaseScriptVM.h"

namespace savor::db::execution::programdb::seedprobe {

struct SeedProbeTimingConfig {
    uint32_t run_ms = 0;
    uint32_t vi_stall_ms = 0;
};

static inline uint32_t clamp_to_u32(std::int64_t value) {
    if (value <= 0) {
        return 0;
    }
    if (value > static_cast<std::int64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(value);
}

static inline int clamp_to_int(std::int64_t value) {
    if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    if (value > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

static inline std::optional<SeedProbeTimingConfig> resolve_timing_from_authoring_spec(
    const savor::db::IAnalysisDb* analysis_db,
    const savor::db::IAuthoringDb* authoring_db,
    std::int64_t probe_run_id) {
    if (analysis_db == nullptr || authoring_db == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }

    const auto probe_run = analysis_db->GetSeedProbeRun(probe_run_id);
    if (!probe_run.has_value()) {
        return std::nullopt;
    }

    const auto spec = authoring_db->GetSeedProbeSpec(probe_run->seed_probe_spec_id);
    if (!spec.has_value()) {
        return std::nullopt;
    }

    return SeedProbeTimingConfig{
        .run_ms = clamp_to_u32(spec->run_ms),
        .vi_stall_ms = clamp_to_u32(spec->vi_stall_ms),
    };
}

struct GridIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Grid";

    int samples_per_axis = 5;
    uint8_t min_value = 0;
    uint8_t max_value = 255;
    bool cap_trigger_top = true;
    bool ignore_trigger_minmax = false;

    static inline GridIni from_section(const IniDoc& doc) {
        GridIni grid{};
        if (!doc.has_section(SECTION_NAME)) return grid;
        IniKV section = doc.section_kv(SECTION_NAME);
        grid.samples_per_axis = clamp_to_int(section.get_i64("samples_per_axis", grid.samples_per_axis));
        grid.min_value = section.get_u8("min_value", grid.min_value);
        grid.max_value = section.get_u8("max_value", grid.max_value);
        grid.cap_trigger_top = section.get_bool("cap_trigger_top", grid.cap_trigger_top);
        grid.ignore_trigger_minmax = section.get_bool("ignore_trigger_minmax", grid.ignore_trigger_minmax);
        return grid;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "samples_per_axis", std::to_string(samples_per_axis));
        doc.set(SECTION_NAME, "min_value", std::to_string(min_value));
        doc.set(SECTION_NAME, "max_value", std::to_string(max_value));
        doc.set(SECTION_NAME, "cap_trigger_top", cap_trigger_top ? "1" : "0");
        doc.set(SECTION_NAME, "ignore_trigger_minmax", ignore_trigger_minmax ? "1" : "0");
    }
};

struct UniqueIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Unique";

    int combo_attempts_per_target = 128;
    int combo_sampler_tries = 8;

    static inline UniqueIni from_section(const IniDoc& doc) {
        UniqueIni unique{};
        if (!doc.has_section(SECTION_NAME)) return unique;
        IniKV section = doc.section_kv(SECTION_NAME);
        unique.combo_attempts_per_target = clamp_to_int(section.get_i64("combo_attempts_per_target", unique.combo_attempts_per_target));
        unique.combo_sampler_tries = clamp_to_int(section.get_i64("combo_sampler_tries", unique.combo_sampler_tries));
        return unique;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "combo_attempts_per_target", std::to_string(combo_attempts_per_target));
        doc.set(SECTION_NAME, "combo_sampler_tries", std::to_string(combo_sampler_tries));
    }
};

enum SeedProbePhase : uint32_t {
    None,
    Neutral,
    Grid,
    Unique,
    Done
};

struct BlueprintIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Blueprint";

    int64_t root_jobset_id{ -1 };
    int64_t savestate_id{ -1 };
    int64_t probe_id{ -1 };
    uint32_t run_ms;
    uint32_t vi_stall_ms;
    SeedProbePhase cur_phase{ SeedProbePhase::None };
    bool clear_result_winners = true;
    bool auto_schedule_battle_run = false;
    int priority = 0;

    static inline BlueprintIni from_section(const IniDoc& doc) {
        BlueprintIni bp{};
        if (!doc.has_section(SECTION_NAME)) return bp;
        IniKV section = doc.section_kv(SECTION_NAME);
        bp.root_jobset_id = section.get_i64("root_jobset_id", -1);
        bp.savestate_id = section.get_i64("savestate_id", -1);
        bp.probe_id = section.get_i64("probe_id", -1);
        bp.run_ms = section.get_u32("run_ms", 0);
        bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
        bp.cur_phase = (SeedProbePhase)section.get_u32("cur_phase", 0);
        bp.clear_result_winners = section.get_bool("clear_result_winners", true);
        bp.auto_schedule_battle_run = section.get_bool("auto_schedule_battle_run", false);
        bp.priority = clamp_to_int(section.get_i64("priority", 0));
        return bp;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "root_jobset_id", std::to_string(root_jobset_id));
        doc.set(SECTION_NAME, "savestate_id", std::to_string(savestate_id));
        doc.set(SECTION_NAME, "probe_id", std::to_string(probe_id));
        doc.set(SECTION_NAME, "run_ms", std::to_string(run_ms));
        doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
        doc.set(SECTION_NAME, "cur_phase", std::to_string((uint32_t)cur_phase));
        doc.set(SECTION_NAME, "clear_result_winners", clear_result_winners ? "1" : "0");
        doc.set(SECTION_NAME, "auto_schedule_battle_run", auto_schedule_battle_run ? "1" : "0");
        doc.set(SECTION_NAME, "priority", std::to_string(priority));
    }
};

struct JobIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Job";

    std::string frame_hex{ "" };
    int32_t expected_delta{ 0 };
    int64_t delta_set_id{ -1 };
    std::string unique_tag{ "" };

    static inline JobIni from_section(const IniDoc& doc) {
        JobIni job{};
        if (!doc.has_section(SECTION_NAME)) return job;
        IniKV section = doc.section_kv(SECTION_NAME);
        job.frame_hex = section.get("frame_hex", "");
        job.expected_delta = section.get_u32("expected_delta", 0);
        job.delta_set_id = section.get_i64("delta_set_id", -1);
        job.unique_tag = section.get("unique_tag", "");
        return job;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "frame_hex", frame_hex);
        doc.set(SECTION_NAME, "expected_delta", std::to_string(expected_delta));
        doc.set(SECTION_NAME, "delta_set_id", std::to_string(delta_set_id));
        doc.set(SECTION_NAME, "unique_tag", unique_tag);
    }

    IniDoc append_section(const IniDoc& doc) const {
        IniDoc new_doc{ doc };
        set_section(new_doc);
        return new_doc;
    }
};

struct ResultsIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Results";

    uint32_t w_err{ 0 };
    uint32_t dw_err{ 0 };
    uint32_t vi_start{ 0 };
    uint32_t vi_end{ 0 };
    uint32_t rng_seed{ 0 };

    static inline ResultsIni from_section(const IniDoc& doc) {
        ResultsIni results{};
        if (!doc.has_section(SECTION_NAME)) return results;
        IniKV section = doc.section_kv(SECTION_NAME);
        results.rng_seed = section.get_u32("rng_seed", 0);
        results.w_err = section.get_u32("w_err", (uint32_t)savor::WERR_UnknownError);
        results.dw_err = section.get_u32("dw_err", (uint32_t)savor::RunToBpOutcome::Unknown);
        results.vi_start = section.get_u32("vi_start", -1);
        results.vi_end = section.get_u32("vi_end", -1);
        return results;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "rng_seed", std::to_string(rng_seed));
        doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
        doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
        doc.set(SECTION_NAME, "vi_start", std::to_string(vi_start));
        doc.set(SECTION_NAME, "vi_end", std::to_string(vi_end));
    }

    IniDoc append_section(const IniDoc& doc) const {
        IniDoc new_doc{ doc };
        set_section(new_doc);
        return new_doc;
    }
};

struct CleanupIni {
    static constexpr const char* SECTION_NAME = "SeedProbe.Cleanup";

    bool clear_winners{ true };

    static inline CleanupIni from_section(const IniDoc& doc) {
        CleanupIni cleanup{};
        if (!doc.has_section(SECTION_NAME)) return cleanup;
        IniKV section = doc.section_kv(SECTION_NAME);
        cleanup.clear_winners = section.get_bool("clear_winners", true);
        return cleanup;
    }

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "clear_winners", clear_winners ? "1" : "0");
    }
};

static inline std::string fingerprint_for(int64_t probe_id, const std::string& frame_hex, uint32_t run_ms, uint32_t vi_stall_ms) {
    std::ostringstream oss;
    oss << "PK=3;PV=1;probe_id=" << probe_id
        << ";frame=" << frame_hex << ";run_ms=" << run_ms << ";vi=" << vi_stall_ms;
    return oss.str();
}

static inline std::optional<std::string> fingerprint_value(const std::string& fingerprint, const std::string& key) {
    const std::string token = key + "=";
    const auto token_pos = fingerprint.find(token);
    if (token_pos == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t value_begin = token_pos + token.size();
    std::size_t value_end = fingerprint.find(';', value_begin);
    if (value_end == std::string::npos) {
        value_end = fingerprint.size();
    }
    return fingerprint.substr(value_begin, value_end - value_begin);
}

static inline std::optional<uint32_t> parse_u32_or_null(const std::optional<std::string>& text) {
    if (!text.has_value() || text->empty()) {
        return std::nullopt;
    }
    try {
        return static_cast<uint32_t>(std::stoul(*text));
    } catch (...) {
        return std::nullopt;
    }
}

static inline std::optional<savor::GCInputFrame> parse_frame_hex_or_null(const std::optional<std::string>& frame_hex) {
    if (!frame_hex.has_value() || frame_hex->empty()) {
        return std::nullopt;
    }
    const auto bytes = hex_to_bytes(*frame_hex);
    if (bytes.size() != sizeof(savor::GCInputFrame)) {
        return std::nullopt;
    }

    savor::GCInputFrame frame{};
    std::memcpy(&frame, bytes.data(), sizeof(savor::GCInputFrame));
    return frame;
}

static inline savor::seedprobe::EncodeSpec build_encode_spec_from_fingerprint(const std::string& fingerprint) {
    savor::seedprobe::EncodeSpec spec{};
    const auto frame = parse_frame_hex_or_null(fingerprint_value(fingerprint, "frame"));
    if (frame.has_value()) {
        spec.frame = *frame;
    }

    if (const auto run_ms = parse_u32_or_null(fingerprint_value(fingerprint, "run_ms")); run_ms.has_value()) {
        spec.run_ms = *run_ms;
    }
    if (const auto vi_stall_ms = parse_u32_or_null(fingerprint_value(fingerprint, "vi")); vi_stall_ms.has_value()) {
        spec.vi_stall_ms = *vi_stall_ms;
    }

    return spec;
}

} // namespace savor::db::execution::programdb::seedprobe
