// Runner/Breakpoints/Predicate.cpp
#include "Predicate.h"

#include <unordered_map>
#include <span>
#include <cstring>

#include "../../Utils/Hash.h"

namespace {
    // FNV-1a 64-bit
    inline uint64_t hash64(std::span<const uint8_t> s) {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : s) { h ^= b; h *= 1099511628211ull; }
        return h;
    }

    struct BlobDeduper {
        std::vector<uint8_t> blob;
        struct Entry { uint32_t off; uint32_t len; };
        std::unordered_map<uint64_t, std::vector<Entry>> map;

        uint32_t intern(std::span<const uint8_t> s) {
            if (s.empty()) return 0;
            const uint64_t h = hash64(s);
            auto& vec = map[h];
            for (const auto& e : vec) {
                if (e.len == s.size() && std::memcmp(blob.data() + e.off, s.data(), s.size()) == 0)
                    return e.off;
            }
            const uint32_t off = (uint32_t)blob.size();
            blob.insert(blob.end(), s.begin(), s.end());
            vec.push_back({ off, (uint32_t)s.size() });
            return off;
        }
    };
}

namespace simcore::pred {

    static inline void push_u8(std::vector<uint8_t>& b, uint8_t  v) { b.push_back(v); }
    static inline void push_u16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v & 0xFF)); b.push_back(uint8_t((v >> 8) & 0xFF)); }
    static inline void push_u32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF)); }
    static inline void push_u64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF)); }
    static inline void push_blob(std::vector<uint8_t>& b, const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }

    std::string fingerprint(const Spec& s) {
        std::vector<uint8_t> buf;
        buf.reserve(128 + s.lhs_prog.size() + s.rhs_prog.size());

        const uint8_t width = s.width ? s.width : 4;
        const uint32_t tmask = s.turn_mask ? s.turn_mask : 0xFFFFFFFFu;

        // Required core fields
        push_u16(buf, s.required_bp);
        push_u8(buf, (uint8_t)s.kind);
        push_u8(buf, width);
        push_u8(buf, (uint8_t)s.cmp);
        push_u32(buf, s.flags);
        push_u32(buf, tmask);

        // LHS
        push_u32(buf, s.lhs_addr);
        if (s.lhs_key.has_value()) { push_u8(buf, 1); push_u16(buf, (uint16_t)s.lhs_key.value()); }
        else { push_u8(buf, 0); }

        // RHS: choose one of key | prog | imm
        if ((s.flags & uint32_t(PredFlag::RhsIsKey)) && s.rhs_key.has_value()) {
            push_u8(buf, 1); push_u16(buf, (uint16_t)s.rhs_key.value());
        }
        else if ((s.flags & uint32_t(PredFlag::RhsIsProg)) && !s.rhs_prog.empty()) {
            push_u8(buf, 2); push_u32(buf, (uint32_t)s.rhs_prog.size()); push_blob(buf, s.rhs_prog);
        }
        else {
            push_u8(buf, 0); push_u64(buf, s.rhs_value);
        }

        // LHS program (only if flagged)
        if ((s.flags & uint32_t(PredFlag::LhsIsProg)) && !s.lhs_prog.empty()) {
            push_u8(buf, 1); push_u32(buf, (uint32_t)s.lhs_prog.size()); push_blob(buf, s.lhs_prog);
        }
        else {
            push_u8(buf, 0);
        }

        // Exclude cosmetic fields (desc, id)

        return hash::sha256(buf.data(), buf.size());
    }

    bool BuildTable(const std::vector<Spec>& in,
        std::vector<PredicateRecord>& out_records,
        std::vector<uint8_t>& out_blob)
    {
        out_records.clear();
        out_blob.clear();
        out_records.reserve(in.size());

        // First pass: write records w/ zero offsets
        for (const auto& s : in) {
            PredicateRecord r{};
            r.id = s.id; r.required_bp = s.required_bp;
            r.kind = static_cast<uint8_t>(s.kind);
            r.cmp = static_cast<uint8_t>(s.cmp);

            const uint8_t width = s.width ? s.width : 4; // explicit-at-read-time rule
            r.width = width;

            r.flags = s.flags;
            r.turn_mask = s.turn_mask ? s.turn_mask : 0xFFFFFFFFu;

            // LHS
            r.lhs_addr = s.lhs_addr;
            r.lhs_addr_key = s.lhs_key.has_value() ? static_cast<uint16_t>(*s.lhs_key) : 0;
            r.lhs_addrprog_offset = 0; // fill after packing

            // RHS
            const bool rhs_is_key = s.rhs_key.has_value();
            if (rhs_is_key) r.flags |= uint8_t(PredFlag::RhsIsKey);

            r.rhs_addr_key = rhs_is_key ? static_cast<uint16_t>(*s.rhs_key) : 0;
            r.rhs_imm = rhs_is_key ? 0ull : s.rhs_value;
            r.rhs_addrprog_offset = 0; // fill after packing

            std::string name = s.name;
            while (name.size() > (sizeof(r.name) - 1))
                name.pop_back();
            snprintf(r.name, sizeof(r.name)-1, "%s", s.name.c_str());

            out_records.push_back(r);
        }

        if (out_records.empty()) return true;

        // Second pass: dedupe and lay out programs into a single blob
        BlobDeduper dedupe;
        const uint32_t base = (uint32_t)(out_records.size() * sizeof(PredicateRecord));
        for (size_t i = 0; i < in.size(); ++i) {
            auto& r = out_records[i];
            const auto& s = in[i];

            if (!s.lhs_prog.empty() && s.has_flag(PredFlag::LhsIsProg)) {
                const uint32_t off = dedupe.intern(std::span<const uint8_t>(s.lhs_prog.data(), s.lhs_prog.size()));
                r.lhs_addrprog_offset =base + off;
            }
            if (!s.rhs_prog.empty() && s.has_flag(PredFlag::RhsIsProg)) {
                const uint32_t off = dedupe.intern(std::span<const uint8_t>(s.rhs_prog.data(), s.rhs_prog.size()));
                r.rhs_addrprog_offset = base + off;
            }
        }

        out_blob = std::move(dedupe.blob);
        return true;
    }

} // namespace simcore::pred
