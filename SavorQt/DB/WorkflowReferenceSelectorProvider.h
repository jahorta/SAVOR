#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DB/SavorDbServiceResult.h"
#include "Execution/Workflow/WorkflowComposition.h"

namespace savorqt::db {

struct WorkflowReferenceOption {
    std::int64_t ref_id = 0;
    std::string primary_label;
    std::string secondary_evidence;
    std::string status;
    std::string member_unit_kind;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
};

class WorkflowReferenceSelectorProvider {
public:
    static ServiceResult<std::vector<WorkflowReferenceOption>> List(
        const std::string& ref_kind,
        const std::string& data_kind,
        const std::string& search = {},
        int limit = 200);
    static ServiceResult<std::vector<WorkflowReferenceOption>> ListPresentationFamily(
        const std::string& presentation_family_key,
        const std::string& search = {},
        int limit = 200);
};

} // namespace savorqt::db
