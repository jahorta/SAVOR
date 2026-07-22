#pragma once

#include <cstddef>

#include "KeyIds.h"

namespace savor::context::key::navigation {

#define NAVIGATION_CONTEXT_KEYS(X) \
    X(CTX_BLOB, 0x0700, "navigation.CTX_BLOB")

#define DECL_KEY(NAME, ID, STR) \
    inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
    static_assert(NAME >= savor::context::key::NAVIGATION_CONTEXT_MIN \
        && NAME <= savor::context::key::NAVIGATION_CONTEXT_MAX, \
        "navigation-context key out of range");
    NAVIGATION_CONTEXT_KEYS(DECL_KEY)
#undef DECL_KEY

inline constexpr savor::context::key::KeyPair kKeys[] = {
#define ROW(NAME, ID, STR) savor::context::key::KeyPair{static_cast<savor::context::key::KeyId>(ID), STR},
    NAVIGATION_CONTEXT_KEYS(ROW)
#undef ROW
};
inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef NAVIGATION_CONTEXT_KEYS

} // namespace savor::context::key::navigation
