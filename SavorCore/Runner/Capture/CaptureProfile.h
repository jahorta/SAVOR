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

enum class WatchpointAccess : std::uint8_t {
    Read = 1,
    Write = 2,
    Access = 3,
};

enum class WatchpointScope : std::uint8_t {
    Normal = 0,
    InputMacro = 1,
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

struct AddressProgramSampleSpec {
    std::string name;
    std::vector<std::uint8_t> program;
    SampleWidth width = SampleWidth::U32;
};

struct CheckpointSpec {
    std::string id;
    std::string name;
    std::string function;
    std::string checkpoint;
    std::uint32_t pc = 0;
    bool owns_rng_draw = false;
    bool address_program_trace = false;
    std::vector<MemorySampleSpec> memory_samples;
    std::vector<GprSampleSpec> gpr_samples;
    std::vector<RegisterMemorySampleSpec> register_memory_samples;
    std::vector<AddressProgramSampleSpec> address_program_samples;
};

struct MemoryWatchpointSpec {
    std::string id;
    std::uint32_t address = 0;
    SampleWidth size = SampleWidth::U32;
    WatchpointAccess access = WatchpointAccess::Write;
    WatchpointScope scope = WatchpointScope::Normal;
};

struct DynamicMemoryWatchpointSpec {
    std::string id;
    std::uint32_t pc = 0;
    bool use_absolute_address = false;
    std::uint32_t address = 0;
    bool use_address_program = false;
    std::vector<std::uint8_t> address_program;
    std::uint8_t base_reg = 0;
    std::int32_t offset = 0;
    SampleWidth size = SampleWidth::U32;
    WatchpointAccess access = WatchpointAccess::Write;
    WatchpointScope scope = WatchpointScope::Normal;
    bool one_shot = false;
};

struct CaptureProfile {
    std::string name;
    std::uint32_t schema_version = 1;
    std::vector<MemorySampleSpec> default_memory_samples;
    std::vector<GprSampleSpec> default_gpr_samples;
    std::vector<RegisterMemorySampleSpec> default_register_memory_samples;
    std::vector<AddressProgramSampleSpec> default_address_program_samples;
    bool default_address_program_trace = false;
    std::vector<CheckpointSpec> checkpoints;
    std::vector<MemoryWatchpointSpec> memory_watchpoints;
    std::vector<DynamicMemoryWatchpointSpec> dynamic_memory_watchpoints;

    const CheckpointSpec* find_checkpoint(std::uint32_t pc) const;
    std::vector<const CheckpointSpec*> find_checkpoints(std::uint32_t pc) const;
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
