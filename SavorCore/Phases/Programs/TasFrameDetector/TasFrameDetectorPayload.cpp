#include "TasFrameDetectorPayload.h"

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Tas/DtmFile.h"
#include "../../../Utils/Hash.h"
#include <algorithm>

namespace savor::tasframedetector {

static constexpr uint16_t kPayloadVersion = 1;

static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
static inline void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
}
static inline uint32_t rd_u32(const uint8_t* d, size_t& o, size_t n) {
    if (o + 4 > n) return 0;
    uint32_t v = uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
    o += 4;
    return v;
}
static inline uint16_t rd_u16(const uint8_t* d, size_t& o, size_t n) {
    if (o + 2 > n) return 0;
    uint16_t v = uint16_t(d[o]) | (uint16_t(d[o + 1]) << 8);
    o += 2;
    return v;
}

bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(1 + 2 + 4 + 8 + 4 + spec.dtm_path.size());
    out.push_back(PK_TasInputStreamDetector);
    put_u16(out, kPayloadVersion);
    put_u32(out, spec.vi_stall_ms);
    out.insert(out.end(), 8, uint8_t(0)); // reserved
    put_u32(out, static_cast<uint32_t>(spec.dtm_path.size()));
    out.insert(out.end(), spec.dtm_path.begin(), spec.dtm_path.end());
    return true;
}

bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx)
{
    if (in.size() < 1 + 2 + 4 + 8 + 4) return false;
    size_t off = 0;
    const uint8_t pk = in[off++];
    if (pk != PK_TasInputStreamDetector) return false;

    const uint16_t ver = rd_u16(in.data(), off, in.size());
    if (ver != kPayloadVersion) return false;

    const uint32_t vi_stall_ms = rd_u32(in.data(), off, in.size());
    off += 8; // reserved

    const uint32_t len_dtm = rd_u32(in.data(), off, in.size());
    if (off + len_dtm > in.size()) return false;
    const std::string dtm_path(reinterpret_cast<const char*>(in.data() + off), len_dtm);

    savor::tas::DtmFile df;
    std::string id6;
    std::string header_identity;
    if (df.load(dtm_path)) {
        const auto info = df.info();
        id6.assign(info.game_id.data(), 6);
        const auto& bytes = df.bytes();
        const size_t n = std::min(bytes.size(), savor::tas::DtmFile::kMinHeader);
        std::vector<uint8_t> header(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        if (header.size() >= savor::tas::DtmFile::kOffRecordingStartTime + sizeof(uint64_t)) {
            std::fill(header.begin() + static_cast<std::ptrdiff_t>(savor::tas::DtmFile::kOffRecordingStartTime),
                header.begin() + static_cast<std::ptrdiff_t>(savor::tas::DtmFile::kOffRecordingStartTime + sizeof(uint64_t)),
                uint8_t(0));
        }
        header_identity = hash::sha256(header.data(), header.size());
    }

    out_ctx[savor::context::key::tasframedetector::DTM_PATH] = dtm_path;
    out_ctx[savor::context::key::tasframedetector::DISC_ID6] = id6;
    out_ctx[savor::context::key::core::VI_STALL_MS] = vi_stall_ms;
    out_ctx[savor::context::key::tasframedetector::HEADER_IDENTITY] = header_identity;
    out_ctx[savor::context::key::tasframedetector::EMU_VERSION] = std::string("unknown");
    return true;
}

} // namespace savor::tasframedetector
