#pragma once

#include "MldParser.h"
#include "../Model/BlenderIrModel.h"

namespace soasim::mld::parsing {

class BlenderIrBuilder {
public:
    [[nodiscard]] model::BlenderIrScene build(const ParseResult& parseResult) const;
};

} // namespace soasim::mld::parsing
