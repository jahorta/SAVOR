#include "TasMoviePayload.h"

#include <cstring>
#include <filesystem>

#include "../../../Tas/DtmFile.h"   // savor::tas::DtmFile
#include "../../../Utils/Log.h"
#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"

namespace fs = std::filesystem;

namespace savor::tasmovie {

    static constexpr const int PVersion = 3;

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

    std::string derive_save_path(const std::string& dtm_path)
    {
        const fs::path p(dtm_path);
        const std::string stem = p.stem().string();
        return (fs::path(dtm_path).parent_path() / (stem + ".sav")).string();
    }

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
    {
        out.clear();
        out.reserve(1 + 2 + 1 + 8 + 4 + spec.dtm_path.size());

        out.push_back(PK_TasMovie);                 // payload kind tag
        put_u16(out, PVersion);                            // version
        
        uint8_t flags = 0;
        out.push_back(flags);

        out.insert(out.end(), 8, uint8_t(0));

        put_u32(out, (uint32_t)spec.dtm_path.size());
        out.insert(out.end(), spec.dtm_path.begin(), spec.dtm_path.end());

        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx)
    {
        if (in.size() < 1 + 2 + 1 + 8 + 4) return false;
        size_t off = 0;
        const uint8_t pk = in[off++];         // ProgramKind tag
        if (pk != PK_TasMovie) return false;

        const uint16_t ver = rd_u16(in.data(), off, in.size());
        if (ver != PVersion) return false;

        const uint8_t flags = in[off++]; // bit0: save_on_fail

        off += 8; // reserved

        const uint32_t len_dtm = rd_u32(in.data(), off, in.size());
        if (off + len_dtm > in.size()) return false;

        const std::string dtm_path(reinterpret_cast<const char*>(in.data() + off), len_dtm);
        off += len_dtm;

        // Derive final save path from <dtm_dir>/<stem(dtm)>.sav
        const std::string save_path = derive_save_path(dtm_path);

        // Read DTM header to extract id6 and counts
        savor::tas::DtmFile df;
        std::string id6;
        if (df.load(dtm_path)) {
            const auto info = df.info();
            id6.assign(info.game_id.data(), 6);
        }
        else {
            // If we can't load it now, the program will still attempt playback; id6 left empty
            id6.assign("");
        }

        // Fill TAS program context keys
        out_ctx[savor::context::key::tas::DTM_PATH] = dtm_path;
        out_ctx[savor::context::key::tas::SAVE_PATH] = save_path;
        out_ctx[savor::context::key::tas::SAVE_ON_FAIL] = (uint32_t)0;

        savor::progress::ProgressDeets progress{};
        progress.set_flag(CoreProgressFlags::ViDelta);
        progress.set_flag(CoreProgressFlags::Filename);
        progress.set_flag(CoreProgressFlags::ScriptSection);

        out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = progress.flags;
        out_ctx[savor::context::key::tas::DISC_ID6] = id6;

        return true;
    }

} // namespace savor::tasmovie
