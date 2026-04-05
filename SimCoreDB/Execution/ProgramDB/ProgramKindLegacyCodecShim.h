#pragma once

#include <memory>
#include <optional>

#include "ProgramKindDescriptor.h"

namespace simcore::db::execution::programdb {

// Transitional interface representing the legacy monolithic codec contract.
// Stage 3c can wire concrete adapters through this shape while migration is
// still in DualWriteObserve mode.
struct ILegacyProgramCodec {
    virtual ~ILegacyProgramCodec() = default;

    virtual JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const = 0;
    virtual std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const = 0;
    virtual RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const = 0;
    virtual ResultMapPayload MapPrimaryResult(std::int64_t job_id) const = 0;
    virtual std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const = 0;
    virtual WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const = 0;
};

// Utility to build a Stage-3b descriptor from a legacy codec implementation.
// The adapters are intentionally thin so behavior stays with the legacy codec
// until Stage 3c runner integration is validated.
ProgramKindDescriptor BuildDescriptorFromLegacyCodec(
    std::int32_t program_kind,
    std::string program_name,
    const std::shared_ptr<ILegacyProgramCodec>& legacy_codec,
    bool supports_workflow_orchestration,
    bool supports_legacy_trigger_bridge);

} // namespace simcore::db::execution::programdb
