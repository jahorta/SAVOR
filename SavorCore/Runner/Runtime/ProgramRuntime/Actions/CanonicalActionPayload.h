#pragma once

#include "../Model/ProgramTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace savor::runtime::program {

// SAP1 is the bounded binary representation used by nominal request and
// ordinary-result schemas. Callers must supply the exact expected schema;
// sharing the representation never makes two action contracts
// interchangeable.
enum class CanonicalActionPayloadField : std::uint16_t
{
    Address = 1,
    Width = 2,
    Expected = 3,
    Replacement = 4,
    Mask = 5,
    Count = 6,
    TimeoutMilliseconds = 7,
    Path = 8,
    Label = 9,
    Handle = 10,
    ParentHandle = 11,
    Port = 12,
    Priority = 13,
    Flags = 14,
    InputFrame = 15,
    InputFrames = 16,
    RetryLimit = 17,
    PcAlternatives = 18,
    GroupId = 19,
    SourceId = 20,
    SubscriptionId = 21,
    Delivery = 22,
    RoutingPolicy = 23,
    EpochPolicy = 24,
    Lifetime = 25,
    CurrentPointPolicy = 26,
    MovieEndedPolicy = 27,
    ThrottlePolicy = 28,
    ViStallWarmupMilliseconds = 29,
    ViStallMaximumMilliseconds = 30,
    ProfileJson = 31,
    MarkerId = 32,
    MarkerValue = 33,
    TelemetrySource = 34,
    TelemetryKind = 35,
    TelemetrySeverity = 36,
    TelemetryPayload = 37,
    Required = 38,
    ArtifactHash = 39,
    DtmPath = 40,
    DtmHash = 41,
    GameId = 42,
    StorageReference = 43,
    BaselineKey = 44,
    Diagnostic = 45,
    Publication = 46,
    CompletedCount = 47,
    ResultStatus = 48,
    ResultValue = 49,
    ResultEpoch = 50,
    ResultSequence = 51,
    ResultSize = 52,
    ResultHash = 53,
    ResultArtifactId = 54,
    ResultPc = 55,
    ResultStopSequence = 56,
    ResultFrame = 57,
    ResultInputCount = 58,
    ResultAcknowledged = 59,
    ResultTainted = 60,
    MemorySize = 61,
    MemoryAccess = 62,
    InterruptionPolicy = 63,
    Lossless = 64,
    RequireNeutralAcknowledgement = 65,
    Suspendable = 66,
    InterruptionBorrowable = 67,
    MovieExclusive = 68,
    ReadOnly = 69,
    RecordProgress = 70,
    TelemetryLossPolicy = 71,
};

enum class CanonicalActionPayloadKind : std::uint8_t
{
    Boolean = 1,
    Unsigned = 2,
    Signed = 3,
    Utf8 = 4,
    Bytes = 5,
};

using CanonicalActionPayloadValue = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    std::string,
    std::vector<Byte>>;

struct CanonicalActionPayloadEntry
{
    CanonicalActionPayloadField field =
        CanonicalActionPayloadField::Address;
    CanonicalActionPayloadValue value = false;

    auto operator<=>(const CanonicalActionPayloadEntry&) const = default;
};

struct CanonicalActionPayloadLimits
{
    std::size_t maximum_encoded_bytes = 64u * 1024u;
    std::size_t maximum_fields = 128;
    std::size_t maximum_text_bytes = 32u * 1024u;
    std::size_t maximum_blob_bytes = 60u * 1024u;
};

struct CanonicalActionPayloadResult
{
    bool ok = false;
    ProgramValueGraph graph;
    std::string diagnostic;
};

class CanonicalActionPayload final
{
public:
    [[nodiscard]] bool AddBoolean(
        CanonicalActionPayloadField field,
        bool value);
    [[nodiscard]] bool AddUnsigned(
        CanonicalActionPayloadField field,
        std::uint64_t value);
    [[nodiscard]] bool AddSigned(
        CanonicalActionPayloadField field,
        std::int64_t value);
    [[nodiscard]] bool AddUtf8(
        CanonicalActionPayloadField field,
        std::string value);
    [[nodiscard]] bool AddBytes(
        CanonicalActionPayloadField field,
        std::vector<Byte> value);

    [[nodiscard]] std::optional<bool> Boolean(
        CanonicalActionPayloadField field) const;
    [[nodiscard]] std::optional<std::uint64_t> Unsigned(
        CanonicalActionPayloadField field) const;
    [[nodiscard]] std::optional<std::int64_t> Signed(
        CanonicalActionPayloadField field) const;
    [[nodiscard]] std::optional<std::string_view> Utf8(
        CanonicalActionPayloadField field) const;
    [[nodiscard]] std::optional<std::span<const Byte>> Bytes(
        CanonicalActionPayloadField field) const;
    [[nodiscard]] bool Contains(
        CanonicalActionPayloadField field) const noexcept;

    [[nodiscard]] const std::vector<CanonicalActionPayloadEntry>&
    entries() const noexcept
    {
        return entries_;
    }

private:
    [[nodiscard]] bool Add(CanonicalActionPayloadEntry entry);
    [[nodiscard]] const CanonicalActionPayloadEntry* Find(
        CanonicalActionPayloadField field) const noexcept;

    std::vector<CanonicalActionPayloadEntry> entries_;
};

[[nodiscard]] CanonicalActionPayloadResult EncodeCanonicalActionPayload(
    const CanonicalActionPayload& payload,
    const SchemaIdentity& nominal_schema,
    CanonicalActionPayloadLimits limits = {});

[[nodiscard]] bool DecodeCanonicalActionPayload(
    const ProgramValueGraph& graph,
    const SchemaIdentity& expected_schema,
    CanonicalActionPayload& payload,
    std::string* diagnostic = nullptr,
    CanonicalActionPayloadLimits limits = {});

} // namespace savor::runtime::program
