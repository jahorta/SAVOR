#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::tas {

#define TAS_KEYS(X) \
  X(DTM_PATH,     0x0200, "tas.dtm_path")     \
  X(SAVE_PATH,    0x0201, "tas.save_path")    \
  X(SAVE_ON_FAIL, 0x0202, "tas.save_on_fail") \
  X(DISC_ID6,     0x0203, "tas.disc_id6")     \
  X(MOVIE_FAILED, 0x0204, "tas.movie_failed")     

#define DECL_KEY(NAME, ID, STR) \
  inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
  static_assert(NAME >= savor::context::key::TAS_MIN && NAME <= savor::context::key::TAS_MAX, "tas key out of range");
	TAS_KEYS(DECL_KEY)
#undef DECL_KEY

		inline constexpr savor::context::key::KeyPair kKeys[] = {
		  #define ROW(NAME, ID, STR) savor::context::key::KeyPair{ static_cast<savor::context::key::KeyId>(ID), STR },
		  TAS_KEYS(ROW)
		  #undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef TAS_KEYS

} // namespace savor::context::key::tas
