#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::keys::seed {

#define SEED_KEYS(X) \
  X(INPUT,    0x0100, "seed.input") \
  X(RNG_SEED, 0x0101, "seed.seed")

#define DECL_KEY(NAME, ID, STR) \
  inline constexpr savor::keys::KeyId NAME = static_cast<savor::keys::KeyId>(ID); \
  static_assert(NAME >= savor::keys::SEED_MIN && NAME <= savor::keys::SEED_MAX, "seed key out of range");
	SEED_KEYS(DECL_KEY)
#undef DECL_KEY

		inline constexpr savor::keys::KeyPair kKeys[] = {
		  #define ROW(NAME, ID, STR) savor::keys::KeyPair{ static_cast<savor::keys::KeyId>(ID), STR },
		  SEED_KEYS(ROW)
		  #undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef SEED_KEYS

} // namespace savor::keys::seed
