#pragma once

#include <string>
#include <vector>

#include "DB/Qt2UiReadDbShim.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderPreview.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderSchemas.h"

namespace simcore::db::phasebuilder {

struct ValidationError {
    std::string field;
    std::string message;
};

class PhaseBuilderService {
public:
    static IniDoc DefaultsFor(int program_kind) {
        return SchemaComposer::DefaultIniFor(program_kind);
    }

    static std::vector<ValidationError> Validate(int, const IniDoc&) {
        return {};
    }

    static DbResult<PhasePreview> Preview(int program_kind, const IniDoc&) {
        PhasePreview preview{};
        preview.program_kind = program_kind;
        return DbResult<PhasePreview>::Ok(std::move(preview));
    }
};

} // namespace simcore::db::phasebuilder

