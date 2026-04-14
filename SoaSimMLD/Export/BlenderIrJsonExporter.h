#pragma once

#include "../Model/BlenderIrModel.h"

#include <string>

namespace soasim::mld::exporting {

class BlenderIrJsonExporter {
public:
    [[nodiscard]] std::string toJson(const model::BlenderIrScene& scene) const;
};

} // namespace soasim::mld::exporting
