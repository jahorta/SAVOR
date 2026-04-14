#pragma once

#include "../Model/BlenderIrModel.h"

namespace soasim::mld::parsing {

class BlenderIrDiagnostics {
public:
    static void finalizeMesh(model::BlenderIrMesh& mesh);
};

} // namespace soasim::mld::parsing
