#pragma once

#include "MldParser.h"
#include "../Model/BlenderIrModel.h"

namespace soasim::mld::parsing {

class Sa3dBlenderIrBuilder {
public:
    [[nodiscard]] model::BlenderIrScene build(const ParseResult& parseResult) const;
};

} // namespace soasim::mld::parsing
