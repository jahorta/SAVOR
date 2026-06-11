#include "SeedProbePayload.h"

#include <cstring>

#include "../../../Runner/IPC/Wire.h"      // PK_SeedProbe
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptVM.h" // savor::vmcore::<common keys>
#include "SeedProbeScript.h"

namespace savor::seedprobe {

    static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    }
    static inline void put_u16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    }
    static inline uint32_t rd_u32(const uint8_t* d, size_t& o, size_t n) {
        if (o + 4 > n) return 0;
        uint32_t v = uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
        o += 4; return v;
    }
    static inline uint16_t rd_u16(const uint8_t* d, size_t& o, size_t n) {
        if (o + 2 > n) return 0;
        uint16_t v = uint16_t(d[o]) | (uint16_t(d[o + 1]) << 8);
        o += 2; return v;
    }

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
    {
        out.clear();
        out.reserve(1 + 2 + 4 + 4 + sizeof(GCInputFrame));

        out.push_back(PK_SeedProbe);        // ProgramKind
        put_u16(out, 1);                    // version

        put_u32(out, spec.run_ms);
        put_u32(out, spec.vi_stall_ms);

        const uint8_t* p = reinterpret_cast<const uint8_t*>(&spec.frame);
        out.insert(out.end(), p, p + sizeof(GCInputFrame));
        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx)
    {
        const size_t need = 1 + 2 + 4 + 4 + sizeof(GCInputFrame);
        if (in.size() < need) return false;

        size_t off = 0;
        if (in[off++] != PK_SeedProbe) return false;

        const uint16_t ver = rd_u16(in.data(), off, in.size());
        if (ver != 1) return false;

        const uint32_t run_ms = rd_u32(in.data(), off, in.size());
        const uint32_t vi_stall_ms = rd_u32(in.data(), off, in.size());

        GCInputFrame frame{};
        std::memcpy(&frame, in.data() + off, sizeof(GCInputFrame));
        off += sizeof(GCInputFrame);

        // Set the input frame for the script's APPLY_INPUT_FROM(...)
        out_ctx[savor::context::key::seed::INPUT] = frame;

        // Populate standardized core knobs if provided
        out_ctx[savor::context::key::core::RUN_MS] = run_ms;
        out_ctx[savor::context::key::core::VI_STALL_MS] = vi_stall_ms;

        out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = (uint32_t)0;

        return true;
    }

} // namespace savor::seedprobe
