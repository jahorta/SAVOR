#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::tasframedetector {

#define TAS_FRAME_DETECTOR_KEYS(X) \
  X(DTM_PATH,     0x0400, "tas_frame_detector.dtm_path")     \
  X(DISC_ID6,     0x0401, "tas_frame_detector.disc_id6")      \
  X(MOVIE_ENDED,  0x0402, "tas_frame_detector.movie_ended")   \
  X(EMU_VERSION,  0x0403, "tas_frame_detector.emu_version")   \
  X(STREAM_INI,   0x0404, "tas_frame_detector.stream_ini")    \
  X(HEADER_IDENTITY, 0x0405, "tas_frame_detector.header_identity") \
  X(INPUT_COUNT,  0x0406, "tas_frame_detector.input_count")   \
  X(SAMPLE_COUNT, 0x0407, "tas_frame_detector.sample_count")  \
  X(MOVIE_FAILED, 0x0408, "tas_frame_detector.movie_failed")

#define DECL_KEY(NAME, ID, STR) \
  inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
  static_assert(NAME >= savor::context::key::TAS_FRAME_DETECTOR_MIN && NAME <= savor::context::key::TAS_FRAME_DETECTOR_MAX, "tas input detector key out of range");
	TAS_FRAME_DETECTOR_KEYS(DECL_KEY)
#undef DECL_KEY

	inline constexpr savor::context::key::KeyPair kKeys[] = {
	  #define ROW(NAME, ID, STR) savor::context::key::KeyPair{ static_cast<savor::context::key::KeyId>(ID), STR },
	  TAS_FRAME_DETECTOR_KEYS(ROW)
	  #undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef TAS_FRAME_DETECTOR_KEYS

} // namespace savor::context::key::tasframedetector
