#include <gtest/gtest.h>

#include <array>

#include "ScenarioAssessment.h"
#include "common/RecordingExecutionDb.h"

TEST(E2eScenarioAssessment, TrajectoryObservationsDoNotAffectPassStatus) {
    savor::e2e::ScenarioAssessment assessment;
    assessment.Warn("trajectory contained an unexpected turn count");
    EXPECT_TRUE(assessment.Passed());
    EXPECT_TRUE(assessment.invariant_failures.empty());
    ASSERT_EQ(assessment.warnings.size(), 1u);
}

TEST(E2eScenarioAssessment, InvariantViolationFailsAssessment) {
    savor::e2e::ScenarioAssessment assessment;
    assessment.Require(false, "artifact hash drifted");
    EXPECT_FALSE(assessment.Passed());
    EXPECT_NE(assessment.FailureSummary("battle").find("artifact hash drifted"),
              std::string::npos);
}

TEST(E2eScenarioAssessment, GuardSkippedAndAnyDynamicStepCountAreSuccessful) {
    RecordingExecutionDb execution_db;
    savor::db::execution::workflow::WorkflowGraphSnapshot graph{};
    graph.instance.workflow_instance_id = 7;
    graph.instance.state = savor::db::execution::workflow::
        WorkflowInstanceState::Completed;
    for (std::int64_t index = 0; index < 7; ++index) {
        savor::db::execution::workflow::WorkflowStepRecord step{};
        step.workflow_step_id = index + 1;
        step.step_key = "dynamic_" + std::to_string(index + 1);
        step.state = index % 2 == 0
            ? savor::db::execution::workflow::WorkflowStepState::Completed
            : savor::db::execution::workflow::WorkflowStepState::Skipped;
        if (step.state
            == savor::db::execution::workflow::WorkflowStepState::Skipped) {
            step.blocked_reason = "output guard not satisfied";
        }
        graph.steps.push_back(std::move(step));
    }
    const std::array workflows{graph};
    savor::e2e::ScenarioAssessment assessment;
    savor::e2e::AssessCommonScenarioExecution(
        &execution_db, workflows, {}, {}, &assessment);
    EXPECT_TRUE(assessment.Passed()) << assessment.FailureSummary("fixture");
}

TEST(E2eScenarioAssessment, NonterminalStepIsAnInfrastructureFailure) {
    RecordingExecutionDb execution_db;
    savor::db::execution::workflow::WorkflowGraphSnapshot graph{};
    graph.instance.workflow_instance_id = 8;
    graph.instance.state = savor::db::execution::workflow::
        WorkflowInstanceState::Completed;
    savor::db::execution::workflow::WorkflowStepRecord step{};
    step.workflow_step_id = 1;
    step.step_key = "still_running";
    step.state = savor::db::execution::workflow::WorkflowStepState::Running;
    graph.steps.push_back(std::move(step));
    const std::array workflows{graph};
    savor::e2e::ScenarioAssessment assessment;
    savor::e2e::AssessCommonScenarioExecution(
        &execution_db, workflows, {}, {}, &assessment);
    EXPECT_FALSE(assessment.Passed());
}

TEST(E2eScenarioAssessment, CanceledJobsAreSuccessfulPruningOutcomes) {
    RecordingExecutionDb execution_db;
    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db.CreateJobSet({}, &job_set_id));
    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob({
        .job_set_id = job_set_id,
        .program_kind = 1,
        .program_version = 1,
        .program_ref_kind = "fixture",
        .program_ref_id = 1,
        .fingerprint = "canceled-pruning-candidate",
    }, &job_id));
    ASSERT_TRUE(execution_db.SetJobState(job_id, "CANCELED"));

    savor::db::execution::workflow::WorkflowGraphSnapshot graph{};
    graph.instance.workflow_instance_id = 9;
    graph.instance.state = savor::db::execution::workflow::
        WorkflowInstanceState::Completed;
    savor::db::execution::workflow::WorkflowStepRecord step{};
    step.workflow_step_id = 1;
    step.step_key = "pruned_population";
    step.state = savor::db::execution::workflow::
        WorkflowStepState::Completed;
    step.job_set_id = job_set_id;
    graph.steps.push_back(std::move(step));

    const std::array workflows{graph};
    savor::e2e::ScenarioAssessment assessment;
    savor::e2e::AssessCommonScenarioExecution(
        &execution_db, workflows, {}, {}, &assessment);
    EXPECT_TRUE(assessment.Passed()) << assessment.FailureSummary("fixture");
}

TEST(E2eScenarioAssessment, FailedJobsRemainInfrastructureFailures) {
    RecordingExecutionDb execution_db;
    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db.CreateJobSet({}, &job_set_id));
    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob({
        .job_set_id = job_set_id,
        .program_kind = 1,
        .program_version = 1,
        .program_ref_kind = "fixture",
        .program_ref_id = 1,
        .fingerprint = "failed-candidate",
    }, &job_id));
    ASSERT_TRUE(execution_db.SetJobState(job_id, "FAILED"));

    savor::db::execution::workflow::WorkflowGraphSnapshot graph{};
    graph.instance.workflow_instance_id = 10;
    graph.instance.state = savor::db::execution::workflow::
        WorkflowInstanceState::Completed;
    savor::db::execution::workflow::WorkflowStepRecord step{};
    step.workflow_step_id = 1;
    step.step_key = "failed_population";
    step.state = savor::db::execution::workflow::
        WorkflowStepState::Completed;
    step.job_set_id = job_set_id;
    graph.steps.push_back(std::move(step));

    const std::array workflows{graph};
    savor::e2e::ScenarioAssessment assessment;
    savor::e2e::AssessCommonScenarioExecution(
        &execution_db, workflows, {}, {}, &assessment);
    EXPECT_FALSE(assessment.Passed());
    EXPECT_NE(
        assessment.FailureSummary("fixture").find("reached FAILED"),
        std::string::npos);
}
