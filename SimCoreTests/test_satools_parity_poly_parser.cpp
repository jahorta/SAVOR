#if 0
#include <gtest/gtest.h>

#include "../SoaSimMLD/Model/NjcmModel.h"
#include "../SoaSimMLD/Parsing/NJCMDecodeContext.h"
#include "../SoaSimMLD/Parsing/SaToolsParityPolyParser.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> makeStripChunkPayload(const std::uint8_t type) {
    // Little-endian chunk:
    // [header(type/flags), sizeWords16, header2(userOffset/stripCount=1), strip(flagLen=3), idx1, idx2, idx3, end]
    return {
        type, 0x00,      // header
        0x05, 0x00,      // sizeWords16 (5 words => 10 bytes payload, 14-byte total chunk)
        0x01, 0x00,      // header2: userOffset=0, stripCount=1
        0x03, 0x00,      // strip length=3
        0x01, 0x00,      // index 1
        0x02, 0x00,      // index 2
        0x03, 0x00,      // index 3
        0xFF, 0x00       // end chunk
    };
}

TEST(SaToolsParityPolyParser, UnsupportedStripTypeIsDiagnosedAndSkipped) {
    auto decoded = makeStripChunkPayload(67U); // Strip_StripNormal (unsupported by parity strip decode)

    soasim::mld::model::NjcmDecodedChunk out{};
    soasim::mld::parsing::NjcmDecodeContext ctx{
        std::span<const std::uint8_t>(decoded.data(), decoded.size()),
        0,
        true,
        &out
    };

    soasim::mld::model::NjAttachRecord attach{};
    soasim::mld::parsing::satools_parity::parsePolyChunks(ctx, 0, attach);

    ASSERT_EQ(attach.polyChunks.size(), 1U);
    EXPECT_EQ(attach.polyChunks[0].offset, 0U);
    EXPECT_EQ(attach.polyChunks[0].type, 67U);

    ASSERT_EQ(attach.semanticPolygons.size(), 1U);
    EXPECT_TRUE(attach.semanticPolygons[0].indices.empty());
    EXPECT_EQ(attach.decodedTriangleCount, 0U);
    EXPECT_TRUE(attach.semanticPrimitives.empty());

    const auto diagIt = std::find_if(out.diagnostics.begin(), out.diagnostics.end(), [](const std::string& d) {
        return d.find("unsupported strip chunk type 67") != std::string::npos &&
            d.find("offset 0") != std::string::npos;
    });
    EXPECT_NE(diagIt, out.diagnostics.end());
}

TEST(SaToolsParityPolyParser, SupportedStripTypeStillDecodesGeometry) {
    auto decoded = makeStripChunkPayload(64U); // Strip_Strip

    soasim::mld::model::NjcmDecodedChunk out{};
    soasim::mld::parsing::NjcmDecodeContext ctx{
        std::span<const std::uint8_t>(decoded.data(), decoded.size()),
        0,
        true,
        &out
    };

    soasim::mld::model::NjAttachRecord attach{};
    soasim::mld::parsing::satools_parity::parsePolyChunks(ctx, 0, attach);

    ASSERT_EQ(attach.polyChunks.size(), 1U);
    ASSERT_EQ(attach.semanticPolygons.size(), 1U);
    EXPECT_EQ(attach.decodedTriangleCount, 1U);
    EXPECT_EQ(attach.semanticPolygons[0].indices.size(), 3U);
    EXPECT_EQ(attach.semanticPrimitives.size(), 1U);
}

} // namespace
#endif
