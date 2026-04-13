#include "SaToolsParityParser.h"

#include "NJCMDecodeContext.h"
#include "SaToolsParityAttachParser.h"
#include "SaToolsParityObjectParser.h"

#include <unordered_set>
#include <vector>

namespace soasim::mld::parsing::satools_parity {

model::NjcmDecodedChunk decodeWithObjectModel(std::span<const std::uint8_t> njcmData,
    const std::size_t chunkOffset,
    const std::size_t chunkDataSize,
    const bool chunkSizeLittleEndian,
    const bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data) {
    model::NjcmDecodedChunk out{};
    out.chunkOffset = chunkOffset;
    out.chunkDataSize = chunkDataSize;
    out.chunkSizeLittleEndian = chunkSizeLittleEndian;
    out.payloadLittleEndian = policy.payloadLittleEndian;
    out.sawPof0Chunk = sawPof0Chunk;
    out.imageBase = (policy.imageBase == 0U) ? static_cast<std::uint32_t>(chunkOffset) : policy.imageBase;

    std::vector<std::uint8_t> decoded(njcmData.begin(), njcmData.end());
    if (sawPof0Chunk && !pof0Data.empty()) {
        const auto deltas = decodePof0Deltas(pof0Data);
        applyPof0Fixups(decoded, deltas, out.imageBase, out.payloadLittleEndian);
        out.usedPof0Fixup = true;
    }

    NjcmDecodeContext ctx{
        .decoded = std::span<const std::uint8_t>(decoded.data(), decoded.size()),
        .imageBase = out.imageBase,
        .littleEndian = out.payloadLittleEndian,
        .out = &out,
    };

    std::unordered_set<std::size_t> visitedObjects{};
    std::unordered_set<std::size_t> visitedAttaches{};
    std::vector<std::size_t> stack{};
    stack.push_back(0);

    while (!stack.empty()) {
        const std::size_t objOff = stack.back();
        stack.pop_back();
        if (visitedObjects.find(objOff) != visitedObjects.end()) {
            continue;
        }
        visitedObjects.insert(objOff);

        const auto maybeObj = parseObject(ctx, objOff, stack);
        if (!maybeObj.has_value()) {
            continue;
        }

        model::NjObjectRecord obj = *maybeObj;
        if (obj.hasAttach && visitedAttaches.find(obj.attachOffset) == visitedAttaches.end()) {
            visitedAttaches.insert(obj.attachOffset);
            const auto maybeAttach = parseAttach(ctx, obj.attachOffset);
            if (maybeAttach.has_value()) {
                out.attaches.push_back(std::move(*maybeAttach));
            }
        }

        out.objects.push_back(std::move(obj));
    }

    out.parseSucceeded = !out.objects.empty();
    if (!out.parseSucceeded && policy.allowHeuristicFallback) {
        out.parsedWithHeuristicFallback = true;
    }

    return out;
}

} // namespace soasim::mld::parsing::satools_parity
