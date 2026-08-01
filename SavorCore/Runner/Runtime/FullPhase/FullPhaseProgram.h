#pragma once

#include "../IProgramRuntimePort.h"
#include "../ProgramRuntime/Model/ProgramModel.h"

#include <compare>
#include <cstdint>
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
        program::InvocationStatePolicy::Boot;
    program::InvocationExecutionPolicy execution;
    program::ProgramBudgets limits;
    WorkerCapabilityMask required_capabilities = 0;
    std::string baseline_lineage;
    std::string movie_policy_sha256;
    std::string service_policy_sha256;

    auto operator<=>(const FullPhaseRuntimeContract&) const = default;
};

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

    // Builds the private executor input from a scalar workset binding. Phase
    // and runtime invariants come from this definition, never from the item.
    [[nodiscard]] virtual std::optional<program::ProgramInvocation>
    BuildResolvedExecution(
        std::span<const std::uint8_t> input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic = nullptr) const = 0;
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
// Full Phase cutover. This slice intentionally contains SeedProbe only.
[[nodiscard]] const FullPhaseProgramRegistry& ProductionRegistry();

} // namespace savor::runtime::fullphase
