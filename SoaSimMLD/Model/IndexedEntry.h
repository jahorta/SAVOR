#pragma once

#include "Types.h"
#include "U32List.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace soasim::mld::model {

struct IndexedEntry {
    std::size_t tableIndex = 0;
    std::uint32_t entryId = 0;
    std::uint32_t tblId = 0;
    std::string fxnName{};
    Transform transform{};

    std::unique_ptr<U32List> groundLinks{};
    std::unique_ptr<U32List> paramList2{};
    std::unique_ptr<U32List> functionParameters{};
    std::unique_ptr<U32List> objectAddresses{};
    std::unique_ptr<U32List> groundAddresses{};
    std::unique_ptr<U32List> motionAddresses{};
    std::uint32_t texturesPointer = 0;
};

} // namespace soasim::mld::model
