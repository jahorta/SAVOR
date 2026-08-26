#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::probe {

inline constexpr std::size_t kMaxProbeSamples = 1536;

enum class Subscription : std::uint8_t {
    None = 0,
    Capture = 1,
    Progress = 2,
    Control = 4,
};

constexpr Subscription operator|(Subscription lhs, Subscription rhs)
{
    return static_cast<Subscription>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr Subscription operator&(Subscription lhs, Subscription rhs)
{
    return static_cast<Subscription>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

constexpr bool has_subscription(Subscription value, Subscription flag)
{
    return (value & flag) != Subscription::None;
}

enum class ProbeKind : std::uint8_t {
    Pc = 1,
    Memory = 2,
    Marker = 3,
};

enum class MemoryAccess : std::uint8_t {
    Read = 1,
    Write = 2,
    Access = 3,
};

enum class SampleKind : std::uint8_t {
    Gpr = 1,
    Memory = 2,
    RegisterMemory = 3,
    AddressProgram = 4,
    LinkedList = 5,
    Constant = 6,
    StackTrace = 7,
    RoutedSample = 8,
};

enum class SampleWidth : std::uint8_t {
    U8 = 1,
    U16 = 2,
    U32 = 4,
    U64 = 8,
};

enum class AddressTracePolicy : std::uint8_t {
    Off = 0,
    OnFailure = 1,
    Always = 2,
};

enum class AddressProgramFailure : std::uint8_t {
    None = 0,
    MissingTerminator = 1,
    TrailingBytes = 2,
    UnknownOperation = 3,
    TruncatedOperand = 4,
    InvalidRegister = 5,
    MissingBaseResolver = 6,
    BaseResolutionFailed = 7,
    GuestReadFailed = 8,
    OperationLimit = 9,
};

struct AddressProgramValidation {
    bool valid = false;
    std::uint8_t operation_count = 0;
    std::size_t failure_offset = 0;
    AddressProgramFailure failure = AddressProgramFailure::None;
    std::string message;
};

enum class SamplingMode : std::uint8_t {
    EveryHit = 0,
    EveryN = 1,
    FirstN = 2,
    ChangedOnly = 3,
    HitRanges = 4,
    LastN = 5,
    Burst = 6,
};

enum class PredicateOp : std::uint8_t {
    PushConstant = 1,
    PushPc = 2,
    PushLr = 3,
    PushGpr = 4,
    PushHitCount = 5,
    PushSample = 7,
    PushCaller = 8,
    Equal = 16,
    NotEqual = 17,
    Less = 18,
    LessEqual = 19,
    Greater = 20,
    GreaterEqual = 21,
    MaskEqual = 22,
    LogicalAnd = 23,
    LogicalOr = 24,
    LogicalNot = 25,
};

struct PredicateInstruction {
    PredicateOp op = PredicateOp::PushConstant;
    std::uint64_t operand = 0;
    std::uint64_t operand2 = 0;
};

struct LinkedListField {
    std::string name;
    std::int32_t offset = 0;
    SampleWidth width = SampleWidth::U32;
};

struct SampleDefinition {
    std::string name;
    SampleKind kind = SampleKind::Memory;
    SampleWidth width = SampleWidth::U32;
    std::uint32_t address = 0;
    std::uint8_t base_register = 0;
    std::int32_t offset = 0;
    std::uint64_t constant = 0;
    std::uint32_t routed_sample_descriptor_id = 0;
    std::vector<std::uint8_t> address_program;
    AddressTracePolicy trace = AddressTracePolicy::Off;
    bool address_provided = false;
    std::uint32_t max_nodes = 0;
    std::uint32_t max_frames = 0;
    std::int32_t next_offset = 0;
    std::vector<LinkedListField> linked_list_fields;
};

struct SamplingPolicy {
    SamplingMode mode = SamplingMode::EveryHit;
    std::uint64_t n = 1;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> hit_ranges;
    std::uint64_t burst_period = 1;
    std::uint64_t burst_length = 1;
};

struct ProbeSymbol {
    std::string name;
    std::string function;
    std::string checkpoint;
};

struct ProbeDefinition {
    std::string id;
    std::string group;
    ProbeKind kind = ProbeKind::Pc;
    Subscription subscriptions = Subscription::Capture;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    MemoryAccess memory_access = MemoryAccess::Write;
    std::optional<std::uint32_t> activate_on_pc;
    std::optional<std::uint64_t> max_hits;
    bool one_shot = false;
    bool owns_rng_draw = false;
    AddressTracePolicy address_trace = AddressTracePolicy::Off;
    bool progress_record = true;
    bool frame_clock = false;
    std::string progress_formatter;
    std::string window_id;
    SamplingPolicy sampling;
    std::vector<PredicateInstruction> predicate;
    std::vector<SampleDefinition> samples;
    std::vector<std::uint8_t> address_program;
    ProbeSymbol symbol;
};

struct FlightRecorderDefinition {
    std::string id;
    std::vector<std::string> member_probes;
    std::uint32_t pre_events = 0;
    std::uint32_t post_events = 0;
    std::vector<std::string> trigger_probes;
    std::vector<std::string> trigger_markers;
    bool trigger_on_control = false;
};

struct WindowDefinition {
    std::string id;
    std::string open_probe;
    std::string close_probe;
    std::string open_marker;
    std::string close_marker;
    bool open_on_control = false;
    bool close_on_control = false;
    std::optional<std::uint64_t> open_frame;
    std::optional<std::uint64_t> close_frame;
    std::uint64_t event_limit = 0;
    std::uint64_t frame_limit = 0;
    bool initially_open = false;
};

struct ProfileLimits {
    std::uint64_t queue_bytes = 64ull * 1024ull * 1024ull;
    std::uint32_t max_events = 4096;
    std::uint32_t progress_events = 256;
    std::uint32_t chunk_events = 1024;
};

struct Profile {
    std::string schema = "savor.capture.profile/1";
    std::string name;
    std::uint64_t revision = 1;
    std::string expected_module_sha256;
    bool battle_progress_enabled = true;
    ProfileLimits limits;
    std::vector<ProbeDefinition> probes;
    std::vector<WindowDefinition> windows;
    std::vector<FlightRecorderDefinition> flight_recorders;
};

struct ProfileParseResult {
    std::optional<Profile> profile;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

ProfileParseResult parse_profile_json(const std::string& text);
ProfileParseResult load_profile_json(const std::filesystem::path& path);
std::string format_profile_errors(const ProfileParseResult& result);
std::string serialize_profile_json(const Profile& profile);

AddressProgramValidation validate_address_program(std::span<const std::uint8_t> program);
std::string_view address_program_operation_name(std::uint8_t operation);
std::string_view address_program_failure_name(AddressProgramFailure failure);

} // namespace savor::probe
