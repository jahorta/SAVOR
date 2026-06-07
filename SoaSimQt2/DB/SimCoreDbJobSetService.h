#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/SimCoreDbServiceResult.h"
#include "UIRead/IUiReadDb.h"

namespace soasimqt2::db {

struct WorkflowJobSetRow {
    simcore::db::UiWorkflowStepSummary step;
    std::optional<simcore::db::UiJobSetDetail> detail;
    std::string program_kind_label;
};

class SimCoreDbJobSetService {
public:
    static std::string ProgramKindLabel(int program_kind) {
        switch (program_kind) {
        case 1:
            return "Seed Probe";
        case 2:
            return "TAS Movie";
        case 3:
            return "Battle Turn Runner";
        case 4:
            return "Battle Context Probe";
        case 5:
            return "Battle Single Turn Runner";
        case 6:
            return "TAS Input Stream Detector";
        default:
            return program_kind > 0
                ? "Program Kind " + std::to_string(program_kind)
                : "Unknown";
        }
    }

    static ServiceResult<std::vector<WorkflowJobSetRow>> ListWorkflowJobSets(
        const simcore::db::UiWorkflowDetail& workflow_detail,
        int jobs_limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return ServiceResult<std::vector<WorkflowJobSetRow>>::Err({
                ServiceErrorKind::Unavailable,
                "legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice",
            });
        }

        std::vector<WorkflowJobSetRow> rows;
        for (const auto& step : workflow_detail.steps) {
            if (!step.job_set_id.has_value() || *step.job_set_id <= 0) {
                continue;
            }

            WorkflowJobSetRow row{};
            row.step = step;
            row.detail = db->GetJobSetDetail(*step.job_set_id, jobs_limit);

            int program_kind = 0;
            if (row.detail.has_value()) {
                program_kind = row.detail->summary.program_kind;
            } else if (step.job_count > 0) {
                program_kind = 0;
            }
            row.program_kind_label = ProgramKindLabel(program_kind);
            rows.push_back(std::move(row));
        }

        return ServiceResult<std::vector<WorkflowJobSetRow>>::Ok(std::move(rows));
    }

private:
    static simcore::db::IUiReadDb* UiReadDb() {
        return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
    }
};

} // namespace soasimqt2::db
