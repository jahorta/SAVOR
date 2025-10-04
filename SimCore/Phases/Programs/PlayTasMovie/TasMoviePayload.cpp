#include "TasMoviePayload.h"

#include <cstring>
#include <filesystem>

#include "../../../Tas/DtmFile.h"   // simcore::tas::DtmFile
#include "../../../Utils/Log.h"
#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/KeyRegistry.h"

namespace fs = std::filesystem;

namespace simcore::tasmovie {

    static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    }
    static inline void put_u16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    }
    static inline uint32_t rd_u32(const uint8_t* d, size_t& o, size_t n) {
        if (o + 4 > n) return 0; uint32_t v = uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24); o += 4; return v;
    }
    static inline uint16_t rd_u16(const uint8_t* d, size_t& o, size_t n) {
        if (o + 2 > n) return 0; uint16_t v = uint16_t(d[o]) | (uint16_t(d[o + 1]) << 8); o += 2; return v;
    }

    uint32_t compute_run_ms_from_counts(uint64_t vi_count, uint64_t input_count, double headroom)
    {
        // Use the larger of VI and input counts (inputs are typically per-VI in DTM).
        const uint64_t base = (vi_count ? vi_count : input_count);
        if (base == 0) return 60000; // fallback 60s if header is weird
        // Assume ~60 VI/s; headroom gives cushion near movie end / drift.
        const double ms = (double)base * (1000.0 / 60.0) * (headroom > 1.0 ? headroom : 1.0);
        uint64_t msi = (uint64_t)(ms + 0.5);
        if (msi < 1000) msi = 1000;
        if (msi > 60ull * 60ull * 1000ull) msi = 60ull * 60ull * 1000ull; // cap to 60 min
        return (uint32_t)msi;
    }

    std::string derive_save_path(const std::string& dtm_path)
    {
        const fs::path p(dtm_path);
        const std::string stem = p.stem().string();
        return (fs::path(dtm_path).parent_path() / (stem + ".sav")).string();
    }

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
    {
        out.clear();
        out.reserve(1 + 4 + 2 + 1 + 1 + 4 + spec.dtm_path.size() + 6 + 4 + 4 + 4);

        out.push_back(PK_TasMovie);                 // payload kind tag
        put_u16(out, 1);                            // version
        
        uint8_t flags = 0;
        if (spec.progress_enable) flags |= 0x02;
        out.push_back(flags);
        out.push_back(0);  // reserved

        // reserved 6 bytes (future use: checksum or id6 snapshot)
        out.insert(out.end(), 6, uint8_t(0));

        put_u32(out, spec.run_ms);
        put_u32(out, spec.vi_stall_ms);

        put_u32(out, (uint32_t)spec.dtm_path.size());
        out.insert(out.end(), spec.dtm_path.begin(), spec.dtm_path.end());

        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx)
    {
        if (in.size() < 1 + 4 + 2 + 2 + 4 + 6 + 4 + 4 + 4) return false;
        size_t off = 0;
        const uint8_t pk = in[off++];         // ProgramKind tag
        if (pk != PK_TasMovie) return false;

        const uint16_t ver = rd_u16(in.data(), off, in.size());
        if (ver != 1) return false;

        const uint8_t flags = in[off++]; // bit0: save_on_fail
        off += 1; // reserved

        // skip 6 reserved bytes
        off += 6;

        const uint32_t run_ms_in = rd_u32(in.data(), off, in.size());
        const uint32_t vi_stall_ms = rd_u32(in.data(), off, in.size());

        const uint32_t len_dtm = rd_u32(in.data(), off, in.size());
        if (off + len_dtm + 6 + 4 + 4 + 4 > in.size()) return false;

        const std::string dtm_path(reinterpret_cast<const char*>(in.data() + off), len_dtm);
        off += len_dtm;

        const uint32_t len_savedir = rd_u32(in.data(), off, in.size());
        if (off + len_savedir != in.size()) return false;

        // Derive final save path from <dtm_dir>/<stem(dtm)>.sav
        const std::string save_path = derive_save_path(dtm_path);

        // Read DTM header to extract id6 and counts
        simcore::tas::DtmFile df;
        std::string id6;
        uint64_t vi_count = 0, input_count = 0;

        if (df.load(dtm_path)) {
            const auto info = df.info();
            id6.assign(info.game_id.data(), 6);
            vi_count = info.vi_count;
            input_count = info.input_count;
        }
        else {
            // If we can't load it now, the program will still attempt playback; id6 left empty
            id6.assign("");
        }

        // Derive run_ms if the payload asked us to (== 0)
        const uint32_t run_ms = (run_ms_in == 0)
            ? compute_run_ms_from_counts(vi_count, input_count, /*headroom=*/1.5)
            : run_ms_in;

        // Fill TAS program context keys
        out_ctx[keys::tas::DTM_PATH] = dtm_path;
        out_ctx[keys::tas::SAVE_PATH] = save_path;
        out_ctx[keys::core::RUN_MS] = run_ms;
        out_ctx[keys::core::VI_STALL_MS] = vi_stall_ms;
        out_ctx[keys::tas::SAVE_ON_FAIL] = (uint32_t)0;
        out_ctx[keys::core::PROGRESS_TEXT_FILENAME] = static_cast<uint32_t>((flags & 0x02) ? 1 : 0);
        out_ctx[keys::tas::DISC_ID6] = id6;

        return true;
    }

} // namespace simcore::tasmovie
