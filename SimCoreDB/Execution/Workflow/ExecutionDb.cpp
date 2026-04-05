#include "ExecutionDb.h"

#include "../../Common/Events/EventPayloadDispatch.h"
#include "../../Common/Events/EventPayloadValidation.h"
#include "SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

ExecutionDb::ExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_)) {
}

IWorkflowOrchestrationQueryService* ExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* ExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateExecutionWorkflowJobPayloadV1(envelope)) {
        return std::nullopt;
    }
    return ResolveExecutionWorkflowJobPayload(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::ExecutionWorkflowJobV1) {
        return std::nullopt;
    }
    if (payload_ref_kind != "workflow_event" || payload_ref_id <= 0) {
        return std::nullopt;
    }

    (void)payload_ref_id;

    // Stage-3 surface contract only. SQL-backed field hydration will populate typed view fields.
    return std::nullopt;
}

} // namespace simcore::db::execution::workflow
