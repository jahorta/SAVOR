#pragma once

#include "MldParser.h"
#include "../Model/GeometryModel.h"

namespace soasim::mld::parsing {

class GeometryBuilder {
public:
    [[nodiscard]] model::GeometryBuildResult build(const ParseResult& parseResult) const;
};

} // namespace soasim::mld::parsing
