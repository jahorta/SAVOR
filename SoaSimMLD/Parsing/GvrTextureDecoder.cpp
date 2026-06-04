#include "GvrTextureDecoder.h"

#include "../../SoaSimGvm/SoaSimGvm.h"

#include <utility>

namespace soasim::mld::parsing {

DecodedRgbaTexture decodeGvrToRgba8(const model::MldTextureEntry& entry) {
    DecodedRgbaTexture out{};
    out.width = entry.width;
    out.height = entry.height;
    out.diagnostics = entry.diagnostics;

    if (entry.decoded && !entry.rgba8.empty()) {
        out.decoded = true;
        out.rgba8 = entry.rgba8;
        return out;
    }

    auto fallback = soasim::gvm::decoding::makeErrorTexture(entry.width, entry.height);
    out.decoded = true;
    out.width = fallback.width;
    out.height = fallback.height;
    out.rgba8 = std::move(fallback.rgba8);
    out.diagnostics.push_back("GVR decode wrapper emitted ERROR checkerboard fallback.");
    return out;
}

} // namespace soasim::mld::parsing
