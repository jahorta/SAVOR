#pragma once

#include "../IProgramRuntimePort.h"
#include "../ProgramRuntime/Model/ProgramModel.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::fullphase {

struct FullPhaseProgramIdentity
{
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    std::string canonical_id;
    std::uint32_t contract_revision = 0;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return program_kind > 0 && program_version > 0 &&
            !canonical_id.empty() && contract_revision > 0 &&
            canonical_sha256.size() == 64;
    }

    auto operator<=>(const FullPhaseProgramIdentity&) const = default;
};

struct FullPhaseRuntimeContract
{
    ProgramModuleIdentity module;
    std::string entrypoint;
    std::string dependency_lock_sha256;
    // The historical name is retained at the workset boundary. This value is
    // the full ProgramInvocation compatibility hash, not the dependency-lock
    // hash used for prepared-module admission.
    std::string verified_dependency_sha256;
    std::string runtime_profile_sha256;
    program::InvocationStatePolicy state_policy =
        program::InvocationStatePolicy::RestoreBaseline;
    program::InvocationExecutionPolicy execution;
    program::ProgramBudgets limits;
    std::string baseline_lineage;
    std::string movie_policy_sha256;
    std::string service_policy_sha256;

    auto operator<=>(const FullPhaseRuntimeContract&) const = default;
};

// Exact prepared phase program supplied by a workset. The static registry
// identifies only the program-kind codec/handler; executable IR and all
// runtime invariants live in this immutable package.
struct FullPhaseProgramPackage
{
    FullPhaseProgramIdentity identity;
    FullPhaseRuntimeContract runtime_contract;
    std::vector<EncodedModuleEnvelope> module_closure;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(identity) &&
            !runtime_contract.module.canonical_id.empty() &&
            !runtime_contract.entrypoint.empty() &&
            !module_closure.empty() && canonical_sha256.size() == 64;
    }

    auto operator<=>(const FullPhaseProgramPackage&) const = default;
};

struct FullPhaseCommonInput
{
    std::string schema_id;
    std::uint32_t schema_version = 0;
    std::vector<std::uint8_t> payload;
    std::string content_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !schema_id.empty() && schema_version > 0 &&
            content_sha256.size() == 64;
    }

    auto operator<=>(const FullPhaseCommonInput&) const = default;
};

// Static program-kind policy enforced during host-only workset admission.
// It describes phase semantics, not worker capability or scheduling affinity.
struct FullPhaseWorksetPolicy
{
    std::uint32_t minimum_item_count = 1;
    std::uint32_t maximum_item_count =
        std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] bool valid() const noexcept
    {
        return minimum_item_count > 0 &&
            minimum_item_count <= maximum_item_count;
    }

    [[nodiscard]] bool accepts(std::size_t item_count) const noexcept
    {
        return valid() && item_count >= minimum_item_count &&
            item_count <= maximum_item_count;
    }

    auto operator<=>(const FullPhaseWorksetPolicy&) const = default;
};

[[nodiscard]] std::string ComputeFullPhaseProgramPackageHash(
    const FullPhaseProgramPackage& package);
[[nodiscard]] FullPhaseProgramPackage BuildFullPhaseProgramPackage(
    const class IFullPhaseProgramDefinition& definition);
[[nodiscard]] FullPhaseCommonInput MakeFullPhaseCommonInput(
    std::string schema_id,
    std::uint32_t schema_version,
    std::vector<std::uint8_t> payload = {});

// Immutable, compiled definition of one complete phase method. The generic
// registry exposes common runtime invariants; a phase may additionally expose
// typed, pure planning helpers from its own header.
class IFullPhaseProgramDefinition
{
public:
    virtual ~IFullPhaseProgramDefinition() = default;

    [[nodiscard]] virtual const FullPhaseProgramIdentity& identity()
        const noexcept = 0;
    [[nodiscard]] virtual const FullPhaseRuntimeContract& runtime_contract()
        const noexcept = 0;
    [[nodiscard]] virtual const EncodedModuleEnvelope& module_envelope()
        const noexcept = 0;
    [[nodiscard]] virtual FullPhaseWorksetPolicy workset_policy()
        const noexcept
    {
        return {};
    }

    // Builds the private executor input from a scalar workset binding. Phase
    // and runtime invariants come from this definition, never from the item.
    [[nodiscard]] virtual std::optional<program::ProgramInvocation>
    BuildResolvedExecution(
        std::span<const std::uint8_t> input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic = nullptr) const = 0;

    // Workset-facing kind-handler seam. Existing fixed Full Phase kinds use
    // this implementation; prepared variants may override it while retaining
    // one static handler per program kind.
    [[nodiscard]] virtual std::optional<program::ProgramInvocation>
    BuildResolvedExecution(
        const FullPhaseProgramPackage& package,
        std::span<const std::uint8_t> common_input_payload,
        std::span<const std::uint8_t> item_input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic = nullptr) const;
};

class FullPhaseProgramRegistry
{
public:
    bool Register(std::shared_ptr<const IFullPhaseProgramDefinition> definition);

    [[nodiscard]] const IFullPhaseProgramDefinition* Find(
        std::int32_t program_kind) const noexcept;
    [[nodiscard]] const IFullPhaseProgramDefinition* Find(
        const FullPhaseProgramIdentity& identity) const noexcept;
    [[nodiscard]] std::vector<FullPhaseProgramIdentity> identities() const;

private:
    std::vector<std::shared_ptr<const IFullPhaseProgramDefinition>>
        definitions_;
};

// Closed production registry for the program kinds that have completed the
// Full Phase cutover.
[[nodiscard]] const FullPhaseProgramRegistry& ProductionRegistry();

} // namespace savor::runtime::fullphase
