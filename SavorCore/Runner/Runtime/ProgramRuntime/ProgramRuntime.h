#pragma once

#include "../IProgramRuntimePort.h"
#include "Execution/ProgramExecutor.h"
#include "Registry/ActionRegistry.h"
#include "Registry/CapabilityPackRegistry.h"
#include "Registry/TypeSchemaRegistry.h"
#include "Store/ProgramDefinitionStore.h"
#include "Verify/ProgramVerifier.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program {

struct InternalProgramInvocationStartRequest
{
    WorkerCommandSequence command_sequence;
    EncodedInvocationEnvelope invocation;
    bool state_already_prepared = false;
    std::string prepared_baseline_sha256;
};

struct ProgramRuntimeConfig
{
    RuntimeCompatibility compatibility;
    // Scalar runtime-profile fields are exact compatibility constraints.
    // Capability packs remain invocation-specific and must exactly equal the
    // verified dependency lock.
    RuntimeProfile runtime_profile;
    // Infrastructure-only timeout for actor/host operations that do not
    // advance guest execution. It is process configuration, never invocation
    // identity or phase policy.
    std::chrono::milliseconds bounded_host_operation_timeout{
        std::chrono::seconds(30)};
    // When present, CompleteExact is reported only after the loaded catalog
    // exactly matches this configured production module set. Development
    // modules and extra modules can never satisfy this contract.
    std::optional<std::vector<ProgramRuntimeCatalogModule>>
        expected_exact_catalog;
};

// Computes the execution-key portion shared by independently bound workset
// items. Per-item identity, input, budgets, provenance, lineage, and the
// actor-owned session/epoch binding are intentionally excluded.
[[nodiscard]] std::string
ComputeProgramInvocationCompatibilityHashV1(
    const ProgramInvocation& invocation);

// Canonical typed runtime. It owns only immutable definitions, registries,
// verification, and invocation continuation data. Session effects cross the
// actor-marshalled ProgramActionProtocol; this object never obtains an
// EmulationSession, backend, service, database, or protocol writer.
class ProgramRuntime final : public IProgramRuntimePort
{
public:
    explicit ProgramRuntime(ProgramRuntimeConfig config);
    ~ProgramRuntime() override;

    ProgramRuntime(const ProgramRuntime&) = delete;
    ProgramRuntime& operator=(const ProgramRuntime&) = delete;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] const std::string& initialization_diagnostic()
        const noexcept;

    [[nodiscard]] WorkerCapabilityMask capabilities()
        const noexcept override;

    ProgramRuntimeSubmission PrepareModule(
        ModulePreparationRequest request,
        std::shared_ptr<IProgramRuntimeEventSink> events) override;

    ProgramRuntimeSubmission PrepareInvocationTemplate(
        InvocationTemplatePreparationRequest request,
        PreparedInvocationTemplateReceipt& receipt) override;

    ProgramRuntimeSubmission ReleaseInvocationTemplate(
        PreparedInvocationTemplateId template_id) override;

    ProgramRuntimeSubmission StartPreparedInvocation(
        PreparedInvocationStartRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events) override;

    [[nodiscard]] ProgramRuntimeCatalogSnapshot catalog()
        const override;

    ProgramRuntimeSubmission RequestCancellation(
        InvocationId invocation_id) override;

    void BindActionSink(
        std::shared_ptr<IProgramActionRequestSink> sink) override;

    ProgramRuntimeSubmission DeliverActionResolution(
        ProgramActionResolution completion) override;

    [[nodiscard]] ProgramExecutionTakeResult TakeFinishedExecution(
        InvocationId invocation_id,
        AttemptId attempt_id) override;

    [[nodiscard]] bool Pump() override;
    [[nodiscard]] std::optional<
        std::chrono::steady_clock::time_point>
    next_wake() const override;

    void Shutdown() noexcept override;

    [[nodiscard]] ProgramDefinitionStore& definitions() noexcept;
    [[nodiscard]] TypeSchemaRegistry& types() noexcept;
    [[nodiscard]] ActionRegistry& actions() noexcept;
    [[nodiscard]] CapabilityPackRegistry& capability_packs() noexcept;

private:
    ProgramRuntimeSubmission StartInvocation(
        InternalProgramInvocationStartRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<ProgramRuntime>
MakeSupportedSoaUsaProgramRuntime();

} // namespace savor::runtime::program
