#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace simcore::keys::tas {

#define TAS_KEYS(X) \
  X(DTM_PATH,     0x0200, "tas.dtm_path")     \
  X(SAVE_PATH,    0x0201, "tas.save_path")    \
  X(SAVE_ON_FAIL, 0x0202, "tas.save_on_fail") \
  X(DISC_ID6,     0x0203, "tas.disc_id6")     \
  X(MOVIE_FAILED, 0x0204, "tas.movie_failed")     

#define DECL_KEY(NAME, ID, STR) \
  inline constexpr simcore::keys::KeyId NAME = static_cast<simcore::keys::KeyId>(ID); \
  static_assert(NAME >= simcore::keys::TAS_MIN && NAME <= simcore::keys::TAS_MAX, "tas key out of range");
	TAS_KEYS(DECL_KEY)
#undef DECL_KEY

		inline constexpr simcore::keys::KeyPair kKeys[] = {
		  #define ROW(NAME, ID, STR) simcore::keys::KeyPair{ static_cast<simcore::keys::KeyId>(ID), STR },
		  TAS_KEYS(ROW)
		  #undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef TAS_KEYS

} // namespace simcore::keys::tas
