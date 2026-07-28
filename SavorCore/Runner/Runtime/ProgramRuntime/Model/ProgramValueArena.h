#pragma once

#include "ProgramTypes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace savor::runtime::program {

struct ProgramValueArenaLimits
{
    std::uint64_t maximum_values = 0;
    std::uint64_t maximum_value_bytes = 0;
};

enum class ProgramValueArenaError : std::uint8_t
{
    None,
    InvalidId,
    DuplicateId,
    UnresolvedReference,
    CyclicGraph,
    UnreachableValue,
    InvalidUtf8,
    NonFiniteValue,
    StaleEpoch,
    TypeMismatch,
    SchemaMismatch,
    ValueBudgetExceeded,
    ByteBudgetExceeded,
};

struct ProgramValueArenaStatus
{
    ProgramValueArenaError error = ProgramValueArenaError::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == ProgramValueArenaError::None;
    }
};

struct ProgramValueGraphMeasureResult
{
    ProgramValueArenaStatus status;
    std::uint64_t value_count = 0;
    std::uint64_t value_bytes = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status);
    }
};

// Measures a closed graph with the same accounting used by arena import.
// Runtime budget admission and subsequent SSA bindings must use this result
// rather than a separate object-layout estimate.
[[nodiscard]] ProgramValueGraphMeasureResult
MeasureProgramValueGraph(
    const ProgramValueGraph& graph,
    ProgramValueArenaLimits limits);

// Validates a closed graph against one exact root type and the verified
// nominal schema closure. This is used at every untrusted runtime boundary:
// invocation input, reducer output, and action completion.
[[nodiscard]] ProgramValueArenaStatus ValidateProgramValueGraph(
    const ProgramValueGraph& graph,
    const TypeRef& expected_root_type,
    std::span<const TypeSchemaDefinition> schemas,
    ProgramValueArenaLimits limits,
    std::optional<StateEpoch> current_epoch = std::nullopt);

struct ProgramValueArenaInsertResult
{
    ProgramValueArenaStatus status;
    ProgramValueId value;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status) && static_cast<bool>(value);
    }
};

struct ProgramValueArenaExportResult
{
    ProgramValueArenaStatus status;
    std::optional<ProgramValueGraph> graph;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status) && graph.has_value();
    }
};

struct ProgramValueArenaLookupResult
{
    ProgramValueArenaStatus status;
    const ProgramValue* value = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status) && value != nullptr;
    }
};

// Invocation-owned arena. Appending creates a fresh immutable value; existing
// entries are never mutated or reused for update-by-copy operations.
class ProgramValueArena final
{
public:
    explicit ProgramValueArena(ProgramValueArenaLimits limits) noexcept;

    [[nodiscard]] ProgramValueArenaInsertResult Add(
        TypeRef type,
        ProgramValuePayload payload);

    [[nodiscard]] ProgramValueArenaInsertResult Bind(
        TypeRef type,
        ProgramValuePayload payload)
    {
        return Add(std::move(type), std::move(payload));
    }

    [[nodiscard]] ProgramValueArenaInsertResult Clone(
        ProgramValueId source);

    [[nodiscard]] ProgramValueArenaInsertResult CloneWithPayload(
        ProgramValueId source,
        ProgramValuePayload replacement);

    [[nodiscard]] ProgramValueArenaInsertResult Import(
        const ProgramValueGraph& graph);

    [[nodiscard]] ProgramValueArenaExportResult Export(
        ProgramValueId root) const;

    [[nodiscard]] const ProgramValue* Lookup(ProgramValueId id) const noexcept;
    [[nodiscard]] ProgramValueArenaLookupResult LookupForEpoch(
        ProgramValueId id,
        StateEpoch current_epoch) const noexcept;

    [[nodiscard]] std::uint64_t value_count() const noexcept
    {
        return values_.size();
    }
    [[nodiscard]] std::uint64_t value_bytes() const noexcept
    {
        return value_bytes_;
    }
    [[nodiscard]] const ProgramValueArenaLimits& limits() const noexcept
    {
        return limits_;
    }

private:
    [[nodiscard]] ProgramValueArenaStatus ValidateReferences(
        const ProgramValuePayload& payload) const;
    [[nodiscard]] std::uint64_t Measure(
        const TypeRef& type,
        const ProgramValuePayload& payload) const noexcept;

    ProgramValueArenaLimits limits_;
    std::map<ProgramValueId, ProgramValue> values_;
    std::uint64_t next_id_ = 1;
    std::uint64_t value_bytes_ = 0;
};

} // namespace savor::runtime::program
