#include "NJCMParityPath.h"
#include "SaToolsParityParser.h"

namespace soasim::mld::parsing {

model::NjcmDecodedChunk decodeNjcmChunkSaToolsParity(std::span<const std::uint8_t> njcmData,
    const std::size_t chunkOffset,
    const std::size_t chunkDataSize,
    const bool chunkSizeLittleEndian,
    const bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data) {
    NjcmParsePolicy adjusted = policy;
    adjusted.useSaToolsParityPath = false;
    adjusted.applyPof0Fixups = true;
    if (adjusted.imageBase == 0U) {
        adjusted.imageBase = static_cast<std::uint32_t>(chunkOffset);
    }

    return satools_parity::decodeWithObjectModel(njcmData,
        chunkOffset,
        chunkDataSize,
        chunkSizeLittleEndian,
        sawPof0Chunk,
        adjusted,
        pof0Data);
}

} // namespace soasim::mld::parsing
