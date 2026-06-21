#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::capture {

enum class SampleWidth : std::uint8_t {
    U8 = 1,
    U16 = 2,
    U32 = 4,
    U64 = 8,
};

struct MemorySampleSpec {
    std::string name;
    std::uint32_t address = 0;
    SampleWidth width = SampleWidth::U32;
};

struct GprSampleSpec {
    std::string name;
    std::uint8_t reg = 0;
};

struct RegisterMemorySampleSpec {
    std::string name;
    std::uint8_t base_reg = 0;
    std::int32_t offset = 0;
    SampleWidth width = SampleWidth::U32;
};

struct CheckpointSpec {
    std::string id;
    std::string name;
    std::string function;
    std::string checkpoint;
    std::uint32_t pc = 0;
    bool owns_rng_draw = false;
    std::vector<MemorySampleSpec> memory_samples;
    std::vector<GprSampleSpec> gpr_samples;
    std::vector<RegisterMemorySampleSpec> register_memory_samples;
};

struct CaptureProfile {
    std::string name;
    std::uint32_t schema_version = 1;
    std::vector<MemorySampleSpec> default_memory_samples;
    std::vector<GprSampleSpec> default_gpr_samples;
    std::vector<RegisterMemorySampleSpec> default_register_memory_samples;
    std::vector<CheckpointSpec> checkpoints;

    const CheckpointSpec* find_checkpoint(std::uint32_t pc) const;
    std::vector<std::uint32_t> pcs() const;
};

struct CaptureProfileParseResult {
    std::optional<CaptureProfile> profile;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

CaptureProfileParseResult ParseCaptureProfileText(const std::string& text);
CaptureProfileParseResult LoadCaptureProfileFile(const std::string& path);
std::string FormatCaptureProfileError(const CaptureProfileParseResult& result);

} // namespace savor::capture
