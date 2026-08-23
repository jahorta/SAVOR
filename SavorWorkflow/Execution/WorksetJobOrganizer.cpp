#include "WorksetJobOrganizer.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace savor::runner::parallel::savordb {
namespace {

struct CompatibilityKey {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t root_job_set_id = 0;
    std::int32_t program_kind = 0;
    std::string module_canonical_id;
    std::string entrypoint;
    std::string contract_key;
    std::string derived_state_binding_sha256;
    std::string capture_binding_sha256;
    std::string progress_plan_sha256;

    auto operator<=>(const CompatibilityKey&) const = default;
};

CompatibilityKey MakeKey(const savor::db::FailedWorkflowWorksetJobRecord& job)
{
    const auto& source = job.source_workset;
    return {
        .workflow_step_id = job.workflow_step_id,
        .job_set_id = job.job_set_id,
        .root_job_set_id = job.root_job_set_id,
        .program_kind = source.program_kind,
        .module_canonical_id = source.contract.module_canonical_id,
        .entrypoint = source.contract.entrypoint,
        .contract_key = source.contract.contract_key,
        .derived_state_binding_sha256 = source.derived_state.binding_sha256,
        .capture_binding_sha256 = source.observation.capture_binding_sha256,
        .progress_plan_sha256 = source.observation.progress_plan_sha256,
    };
}

bool SameCanaryContract(
    const savor::db::FailedWorkflowWorksetJobRecord& lhs,
    const savor::db::FailedWorkflowWorksetJobRecord& rhs)
{
    const auto& a = lhs.source_workset;
    const auto& b = rhs.source_workset;
    return a.program_version == b.program_version
        && a.contract.module_version == b.contract.module_version
        && a.contract.module_sha256 == b.contract.module_sha256
        && a.contract.verified_dependency_sha256 == b.contract.verified_dependency_sha256
        && a.contract.runtime_profile_sha256 == b.contract.runtime_profile_sha256
        && a.contract.program_package_sha256 == b.contract.program_package_sha256;
}

} // namespace

WorksetJobOrganizer::WorksetJobOrganizer(Config config)
    : config_(std::move(config))
{
}

bool WorksetJobOrganizer::Organize(
    std::int64_t workflow_instance_id,
    std::vector<savor::db::FailedWorkflowWorksetJobRecord> jobs,
    savor::db::WorksetJobReorganizationPlan* plan_out,
    std::string* error_out) const
{
    if (plan_out == nullptr || workflow_instance_id <= 0
        || config_.maximum_items_per_workset == 0
        || config_.maximum_encoded_workset_bytes == 0) {
        if (error_out) *error_out = "invalid workset job organizer request";
        return false;
    }
    if (jobs.empty()) {
        if (error_out) *error_out = "workflow has no eligible failed or interrupted jobs";
        return false;
    }
    std::sort(jobs.begin(), jobs.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.job_id < rhs.job_id;
    });

    std::map<CompatibilityKey, std::vector<savor::db::FailedWorkflowWorksetJobRecord>> buckets;
    for (auto& job : jobs) {
        if (job.workflow_instance_id != workflow_instance_id || job.job_id <= 0
            || job.source_workset.program_kind <= 0
            || job.source_workset.contract.contract_key.empty()) {
            if (error_out) *error_out = "failed job organizer candidate is incomplete";
            return false;
        }
        buckets[MakeKey(job)].push_back(std::move(job));
    }

    savor::db::WorksetJobReorganizationPlan plan{};
    plan.workflow_instance_id = workflow_instance_id;
    plan.requested_by = "SavorQt";
    for (auto& [key, bucket] : buckets) {
        const auto& representative = bucket.front();
        for (const auto& candidate : bucket) {
            if (!SameCanaryContract(representative, candidate)) {
                if (error_out) *error_out = "workset reorganization canary failed: runtime contract drift";
                return false;
            }
        }

        std::size_t cursor = 0;
        while (cursor < bucket.size()) {
            savor::db::ReorganizedWorksetPlanEntry entry{};
            entry.workflow_step_id = representative.workflow_step_id;
            entry.job_set_id = representative.job_set_id;
            entry.root_job_set_id = representative.root_job_set_id;
            entry.program_kind = representative.source_workset.program_kind;
            entry.program_version = representative.source_workset.program_version;
            entry.contract = representative.source_workset.contract;
            entry.derived_state = representative.source_workset.derived_state;
            entry.observation = representative.source_workset.observation;

            while (cursor < bucket.size()
                && entry.ordered_job_ids.size() < config_.maximum_items_per_workset) {
                const auto& candidate = bucket[cursor];
                const auto projected = entry.estimated_payload_bytes
                    + candidate.estimated_item_payload_bytes;
                if (!entry.ordered_job_ids.empty()
                    && projected > config_.maximum_encoded_workset_bytes) {
                    break;
                }
                entry.ordered_job_ids.push_back(candidate.job_id);
                entry.estimated_payload_bytes = projected;
                entry.priority = std::max(entry.priority, candidate.priority);
                ++cursor;
            }
            if (entry.ordered_job_ids.empty()) {
                if (error_out) *error_out = "failed job exceeds workset encoded-size limit";
                return false;
            }
            plan.worksets.push_back(std::move(entry));
        }
    }
    *plan_out = std::move(plan);
    if (error_out) error_out->clear();
    return true;
}

} // namespace savor::runner::parallel::savordb
