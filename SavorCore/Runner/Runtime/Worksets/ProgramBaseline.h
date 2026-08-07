#pragma once

#include "WorksetTypes.h"
#include "../EmulationSession.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace savor::runtime {

struct ProgramBaselineComponentResult
{
    bool ok = false;
    RuntimeError error;

    [[nodiscard]] static ProgramBaselineComponentResult Success()
    {
        return {true, {}};
    }

    [[nodiscard]] static ProgramBaselineComponentResult Failure(
        WorkerRejectionCode code,
        std::string message)
    {
        return {false, {code, std::move(message)}};
    }
};

class IProgramBaselineComponentProvider
{
public:
    virtual ~IProgramBaselineComponentProvider() = default;

    [[nodiscard]] virtual std::string canonical_id() const = 0;
    [[nodiscard]] virtual std::uint32_t revision() const noexcept = 0;

    // Host-only validation/derivation. This operation may not obtain session
    // authority or mutate Dolphin.
    [[nodiscard]] virtual ProgramBaselineComponentResult Stage(
        ProgramBaselineComponent& component) = 0;

    // Actor-owned activation/reset after the guest state is prepared.
    [[nodiscard]] virtual ProgramBaselineComponentResult Activate(
        const ProgramBaselineComponent& component,
        EmulationSession& session,
        bool reset) = 0;
};

class ProgramBaselineComponentRegistry final
{
public:
    [[nodiscard]] ProgramBaselineComponentResult Register(
        std::shared_ptr<IProgramBaselineComponentProvider> provider);

    // WorkerRuntime freezes the provider set before either its actor or host
    // stager can observe it. Stage and Activate calls are serialized because
    // the same provider may own validation/derivation state used by both
    // sides of the workset pipeline.
    void Freeze() noexcept;
    [[nodiscard]] bool frozen() const noexcept;

    [[nodiscard]] ProgramBaselineComponentResult Stage(
        ProgramBaselineDefinition& definition) const;

    [[nodiscard]] ProgramBaselineComponentResult Activate(
        const ProgramBaselineDefinition& definition,
        EmulationSession& session,
        bool reset) const;

private:
    [[nodiscard]] static std::string Key(
        std::string_view canonical_id,
        std::uint32_t revision);

    mutable std::mutex mutex_;
    bool frozen_ = false;
    std::unordered_map<
        std::string,
        std::shared_ptr<IProgramBaselineComponentProvider>>
        providers_;
};

class InlineProgramBaselineComponentProvider final
    : public IProgramBaselineComponentProvider
{
public:
    explicit InlineProgramBaselineComponentProvider(
        std::string canonical_id,
        std::uint32_t revision = 1);

    [[nodiscard]] std::string canonical_id() const override;
    [[nodiscard]] std::uint32_t revision() const noexcept override;
    [[nodiscard]] ProgramBaselineComponentResult Stage(
        ProgramBaselineComponent& component) override;
    [[nodiscard]] ProgramBaselineComponentResult Activate(
        const ProgramBaselineComponent& component,
        EmulationSession& session,
        bool reset) override;

private:
    std::string canonical_id_;
    std::uint32_t revision_ = 1;
};

struct WorksetBaselineSnapshot
{
    bool active = false;
    bool multi_item = false;
    ProgramBaselineKey key;
    PreparedProgramBaselineReceipt receipt;
    ResourceScopeId resource_scope;
};

class WorksetStateCoordinator final
{
public:
    WorksetStateCoordinator(
        EmulationSession& session,
        WorkerWorksetLimits limits,
        std::shared_ptr<ProgramBaselineComponentRegistry> components);
    ~WorksetStateCoordinator();

    WorksetStateCoordinator(const WorksetStateCoordinator&) = delete;
    WorksetStateCoordinator& operator=(const WorksetStateCoordinator&) =
        delete;

    [[nodiscard]] ProgramBaselineComponentResult Stage(
        ProgramBaselineDefinition& definition) const;

    [[nodiscard]] ProgramBaselineComponentResult Prepare(
        WorkerWorksetId workset_id,
        const ProgramBaselineDefinition& definition,
        bool multi_item,
        PreparedProgramBaselineReceipt& receipt_out);

    [[nodiscard]] ProgramBaselineComponentResult RestoreForNextItem(
        PreparedProgramBaselineReceipt& receipt_out);

    [[nodiscard]] ProgramBaselineComponentResult Release();
    [[nodiscard]] ProgramBaselineComponentResult Shutdown();

    [[nodiscard]] WorksetBaselineSnapshot snapshot() const noexcept;
private:
    [[nodiscard]] ProgramBaselineComponentResult PrepareSource(
        const ProgramBaselineDefinition& definition);
    [[nodiscard]] ProgramBaselineComponentResult OpenScope(
        WorkerWorksetId workset_id);
    [[nodiscard]] ProgramBaselineComponentResult CloseScope();
    EmulationSession& session_;
    WorkerWorksetLimits limits_;
    std::shared_ptr<ProgramBaselineComponentRegistry> components_;
    ProgramBaselineDefinition active_definition_;
    WorkerWorksetId active_workset_id_;
    ProgramBaselineKey active_key_;
    PreparedProgramBaselineReceipt active_receipt_;
    std::optional<SavestateHandleReceipt> active_handle_;
    ResourceScopeId workset_scope_;
    bool multi_item_ = false;
    bool stopped_ = false;
};

} // namespace savor::runtime
