#pragma once

#include "Utils/IniDoc.h"

namespace simcore::db::phasebuilder {

struct SchemaComposer {
    static IniDoc DefaultIniFor(int) {
        return IniDoc{};
    }
};

} // namespace simcore::db::phasebuilder

