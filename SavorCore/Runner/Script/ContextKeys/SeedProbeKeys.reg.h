#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::seed {

#define SEED_KEYS(X) \
  X(INPUT,    0x0100, "seed.input") \
  X(RNG_SEED, 0x0101, "seed.seed")

#define DECL_KEY(NAME, ID, STR) \
  inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
  static_assert(NAME >= savor::context::key::SEED_MIN && NAME <= savor::context::key::SEED_MAX, "seed key out of range");
	SEED_KEYS(DECL_KEY)
#undef DECL_KEY

		inline constexpr savor::context::key::KeyPair kKeys[] = {
		  #define ROW(NAME, ID, STR) savor::context::key::KeyPair{ static_cast<savor::context::key::KeyId>(ID), STR },
		  SEED_KEYS(ROW)
		  #undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef SEED_KEYS

} // namespace savor::context::key::seed
