#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowInstanceBuilder.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "UIRead/IUiReadDb.h"

namespace soasimqt2::db {

struct WorkflowListRequest {
    std::string state;
    std::string workflow_kind;
    std::optional<simcore::db::UiReadListCursor> before;
    std::optional<simcore::db::UiReadListCursor> after;
    int limit = 50;
};

struct WorkflowStartRequest {
    std::string workflow_kind;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::string created_by = "SoaSimQt2";
    std::vector<std::string> available_inputs;
};

class SimCoreDbWorkflowService {
public:
    static ServiceResult<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>> ListWorkflowInstances(
        const WorkflowListRequest& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>>("SimCoreDB UIRead database is not running");
        }

        simcore::db::UiWorkflowInstanceListQuery query{};
        query.state = request.state;
        query.workflow_kind = request.workflow_kind;
        query.before = request.before;
        query.after = request.after;
        query.limit = request.limit;
        return ServiceResult<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>>::Ok(
            db->ListWorkflowInstances(query));
    }

    static ServiceResult<simcore::db::UiWorkflowDetail> GetWorkflowDetail(std::int64_t workflow_instance_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiWorkflowDetail>("SimCoreDB UIRead database is not running");
        }
        const auto detail = db->GetWorkflowDetail(workflow_instance_id);
        if (!detail.has_value()) {
            return NotFound<simcore::db::UiWorkflowDetail>("workflow instance not found");
        }
        return ServiceResult<simcore::db::UiWorkflowDetail>::Ok(*detail);
    }

    static ServiceResult<std::int64_t> StartWorkflow(const WorkflowStartRequest& request) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB workflow command service is not running");
        }
        if (request.workflow_kind.empty()) {
            return Invalid<std::int64_t>("workflow kind is required");
        }

        simcore::db::execution::workflow::WorkflowDefinitionRegistry registry;
        std::string error;
        if (!registry.RegisterSeedProbeDefaults(&error) || !registry.RegisterTasMovieDefaults(&error)) {
            return Failed<std::int64_t>(error);
        }

        simcore::db::execution::workflow::WorkflowInstanceValidator validator;
        simcore::db::execution::workflow::WorkflowInstanceBuilder builder(&registry, &validator);

        simcore::db::execution::workflow::WorkflowDefinitionInstantiationInput input{};
        input.workflow_kind = request.workflow_kind;
        input.root_scope_kind = request.root_scope_kind;
        input.root_scope_id = request.root_scope_id;
        input.input_ref_kind = request.input_ref_kind;
        input.input_ref_id = request.input_ref_id;
        input.created_by = request.created_by;
        input.created_at_utc = simcore::db::types::UtcNow().time_since_epoch().count();
        input.available_inputs = request.available_inputs;

        std::int64_t workflow_instance_id = 0;
        if (!builder.CreateWorkflowInstance(input, command_service, &workflow_instance_id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(workflow_instance_id);
    }

    static ServiceResult<void> CancelWorkflow(std::int64_t workflow_instance_id, std::string reason) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "SimCoreDB workflow command service is not running" });
        }
        simcore::db::execution::workflow::WorkflowCancelInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.reason = std::move(reason);
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->CancelWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> ResumeWorkflow(std::int64_t workflow_instance_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "SimCoreDB workflow command service is not running" });
        }
        simcore::db::execution::workflow::WorkflowResumeInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->ResumeWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> RetryStep(std::int64_t workflow_step_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "SimCoreDB workflow command service is not running" });
        }
        simcore::db::execution::workflow::WorkflowRetryStepCommand command{};
        command.workflow_step_id = workflow_step_id;
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->RetryFailedStep(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

private:
    static simcore::db::IUiReadDb* UiReadDb() {
        return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
    }

    static simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() {
        return soasimqt2::SimCoreDbRuntime::instance().workflowCommandService();
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Invalid(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::InvalidInput, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Failed(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Failed, std::move(message) });
    }
};

} // namespace soasimqt2::db
