#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::seedprobe {

inline constexpr std::string_view kSeedProbeJobSection =
    "SeedProbe.Request";

enum class SeedProbeJobStage {
    Unknown = 0,
    Survey,
    Search,
    Confirm,
};

inline std::string_view ToString(SeedProbeJobStage stage) {
    switch (stage) {
    case SeedProbeJobStage::Survey:
        return "SURVEY";
    case SeedProbeJobStage::Search:
        return "SEARCH";
    case SeedProbeJobStage::Confirm:
        return "CONFIRM";
    default:
        return "";
    }
}

inline SeedProbeJobStage ParseSeedProbeJobStage(
    std::string_view value) {
    if (value == "SURVEY") {
        return SeedProbeJobStage::Survey;
    }
    if (value == "SEARCH") {
        return SeedProbeJobStage::Search;
    }
    if (value == "CONFIRM") {
        return SeedProbeJobStage::Confirm;
    }
    return SeedProbeJobStage::Unknown;
}

struct SeedProbeJobSpec {
    std::int32_t version = 1;
    SeedProbeJobStage stage = SeedProbeJobStage::Unknown;
    std::int64_t input_frame_id = 0;
    std::int32_t sample_ordinal = 0;
    std::optional<std::int32_t> desired_delta;
    std::optional<std::int64_t> confirmation_of_probe_result_id;
};

inline std::optional<std::string> SeedProbeCancellationGroupKey(
    std::int64_t probe_run_id,
    std::int32_t desired_delta) {
    if (probe_run_id <= 0) {
        return std::nullopt;
    }
    return "seedprobe.run."
        + std::to_string(probe_run_id)
        + ".search.delta."
        + std::to_string(desired_delta);
}

inline std::optional<std::string> SeedProbeCancellationGroupKey(
    std::int64_t probe_run_id,
    const SeedProbeJobSpec& spec) {
    if (probe_run_id <= 0
        || spec.stage != SeedProbeJobStage::Search
        || !spec.desired_delta.has_value()) {
        return std::nullopt;
    }
    return SeedProbeCancellationGroupKey(
        probe_run_id,
        *spec.desired_delta);
}

inline std::string EncodeSeedProbeJobSpec(
    const SeedProbeJobSpec& spec) {
    IniDoc ini;
    const auto section = std::string(kSeedProbeJobSection);
    ini.set(section, "version", std::to_string(spec.version));
    ini.set(section, "stage", std::string(ToString(spec.stage)));
    ini.set(
        section,
        "input_frame_id",
        std::to_string(spec.input_frame_id));
    ini.set(
        section,
        "sample_ordinal",
        std::to_string(spec.sample_ordinal));
    if (spec.desired_delta.has_value()) {
        ini.set(
            section,
            "desired_delta",
            std::to_string(*spec.desired_delta));
    }
    if (spec.confirmation_of_probe_result_id.has_value()) {
        ini.set(
            section,
            "confirmation_of_probe_result_id",
            std::to_string(
                *spec.confirmation_of_probe_result_id));
    }
    return ini.to_string_sorted();
}

inline std::optional<SeedProbeJobSpec> DecodeSeedProbeJobSpec(
    const std::string& text,
    std::string* error_out = nullptr) {
    const auto fail = [&](std::string message)
        -> std::optional<SeedProbeJobSpec> {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return std::nullopt;
    };

    const auto ini = IniDoc::parse(text);
    const auto section = std::string(kSeedProbeJobSection);
    if (!ini.has_section(section)) {
        return fail("SeedProbe.Request section is missing");
    }

    SeedProbeJobSpec spec{};
    const auto version = ini.get_i64(section, "version", 0);
    const auto input_frame_id =
        ini.get_i64(section, "input_frame_id", 0);
    const auto sample_ordinal =
        ini.get_i64(section, "sample_ordinal", -1);
    spec.stage = ParseSeedProbeJobStage(
        ini.get(section, "stage", ""));
    if (version != 1
        || spec.stage == SeedProbeJobStage::Unknown
        || input_frame_id <= 0
        || sample_ordinal < 0
        || sample_ordinal
            > (std::numeric_limits<std::int32_t>::max)()) {
        return fail("SeedProbe request contains invalid common fields");
    }
    spec.version = static_cast<std::int32_t>(version);
    spec.input_frame_id = input_frame_id;
    spec.sample_ordinal =
        static_cast<std::int32_t>(sample_ordinal);

    if (ini.has(section, "desired_delta")) {
        const auto value =
            ini.get_i64(section, "desired_delta", 0);
        if (value
                < (std::numeric_limits<std::int32_t>::min)()
            || value
                > (std::numeric_limits<std::int32_t>::max)()) {
            return fail("SeedProbe desired_delta is outside int32");
        }
        spec.desired_delta =
            static_cast<std::int32_t>(value);
    }
    if (ini.has(
            section,
            "confirmation_of_probe_result_id")) {
        const auto value = ini.get_i64(
            section,
            "confirmation_of_probe_result_id",
            0);
        if (value <= 0) {
            return fail(
                "SeedProbe confirmation result id must be positive");
        }
        spec.confirmation_of_probe_result_id = value;
    }

    if (spec.stage == SeedProbeJobStage::Survey
        && (spec.desired_delta.has_value()
            || spec.confirmation_of_probe_result_id.has_value())) {
        return fail(
            "Survey requests cannot contain Search or Confirm intent");
    }
    if (spec.stage == SeedProbeJobStage::Search
        && (!spec.desired_delta.has_value()
            || spec.confirmation_of_probe_result_id.has_value())) {
        return fail(
            "Search requests require only desired_delta");
    }
    if (spec.stage == SeedProbeJobStage::Confirm
        && (spec.desired_delta.has_value()
            || !spec.confirmation_of_probe_result_id.has_value())) {
        return fail(
            "Confirm requests require only a provisional result id");
    }

    if (error_out != nullptr) {
        error_out->clear();
    }
    return spec;
}

} // namespace savor::db::execution::programdb::seedprobe
