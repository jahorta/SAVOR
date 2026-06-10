// Runner/Script/PSContextCodec.cpp
#include "PSContextCodec.h"
#include <cstring>
#include <type_traits>
#include "KeyRegistry.h"

namespace savor::psctx {

    template <typename T> static inline void push(std::vector<uint8_t>& v, const T& x) {
        static_assert(std::is_trivially_copyable_v<T>, "POD only");
        const auto* p = reinterpret_cast<const uint8_t*>(&x);
        v.insert(v.end(), p, p + sizeof(T));
    }

    static inline void push_bytes(std::vector<uint8_t>& v, const void* p, size_t n) {
        v.insert(v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }

    bool encode_numeric(const PSContext& ctx, std::vector<uint8_t>& out) {
        std::vector<uint8_t> entries;
        uint32_t count = 0;

        for (const auto& [kid, v] : ctx) {
            EntryPrefix ep{};
            ep.key_id = static_cast<uint16_t>(kid);
            ep.reserved = 0;

            // Encode value
            if (const auto p = std::get_if<uint8_t>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::U8);  ep.vlen = 1;
                push(entries, ep); entries.push_back(*p);
                count++;
            }
            else if (const auto p16 = std::get_if<uint16_t>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::U16); ep.vlen = 2;
                push(entries, ep); push(entries, *p16);
                count++;
            }
            else if (const auto p32 = std::get_if<uint32_t>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::U32); ep.vlen = 4;
                push(entries, ep); push(entries, *p32);
                count++;
            }
            else if (const auto pf = std::get_if<float>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::F32); ep.vlen = 4;
                push(entries, ep); push(entries, *pf);
                count++;
            }
            else if (const auto pd = std::get_if<double>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::F64); ep.vlen = 8;
                push(entries, ep); push(entries, *pd);
                count++;
            }
            else if (const auto ps = std::get_if<std::string>(&v)) {
                ep.type = static_cast<uint8_t>(TypeCode::STR);
                ep.vlen = static_cast<uint32_t>(ps->size());
                push(entries, ep);
                if (!ps->empty()) push_bytes(entries, ps->data(), ps->size());
                count++;
            }
            else {
                // GCInputFrame or any unsupported type -> skip
                continue;
            }
        }

        Header h{ CTX_MAGIC, CTX_VERSION, 0u, count };
        out.clear();
        out.reserve(sizeof(Header) + entries.size());
        push(out, h);
        push_bytes(out, entries.data(), entries.size());
        return true;
    }

    static inline bool pull(const uint8_t*& p, const uint8_t* end, void* dst, size_t n) {
        if (static_cast<size_t>(end - p) < n) return false;
        std::memcpy(dst, p, n); p += n; return true;
    }

    bool decode_numeric(const uint8_t* data, size_t size, PSContext& out) {
        out.clear();
        const uint8_t* p = data;
        const uint8_t* end = data + size;

        Header h{};
        if (!pull(p, end, &h, sizeof(h))) return false;
        if (h.magic != CTX_MAGIC || h.version != CTX_VERSION) return false;

        for (uint32_t i = 0; i < h.count; ++i) {
            EntryPrefix ep{};
            if (!pull(p, end, &ep, sizeof(ep))) return false;
            if (static_cast<size_t>(end - p) < ep.vlen) return false;

            switch (static_cast<TypeCode>(ep.type)) {
            case TypeCode::U8: {
                if (ep.vlen != 1) return false;
                uint8_t v = *p; p += 1;
                out.emplace(ep.key_id, v);
                break;
            }
            case TypeCode::U16: {
                if (ep.vlen != 2) return false;
                uint16_t v; std::memcpy(&v, p, 2); p += 2;
                out.emplace(ep.key_id, v);
                break;
            }
            case TypeCode::U32: {
                if (ep.vlen != 4) return false;
                uint32_t v; std::memcpy(&v, p, 4); p += 4;
                out.emplace(ep.key_id, v);
                break;
            }
            case TypeCode::F32: {
                if (ep.vlen != 4) return false;
                float v; std::memcpy(&v, p, 4); p += 4;
                out.emplace(ep.key_id, v);
                break;
            }
            case TypeCode::F64: {
                if (ep.vlen != 8) return false;
                double v; std::memcpy(&v, p, 8); p += 8;
                out.emplace(ep.key_id, v);
                break;
            }
            case TypeCode::STR: {
                std::string s;
                if (ep.vlen) { s.assign(reinterpret_cast<const char*>(p), ep.vlen); p += ep.vlen; }
                out.emplace(ep.key_id, std::move(s));
                break;
            }
            default:
                // unknown type -> skip
                p += ep.vlen;
                break;
            }
        }

        return true;
    }

} // namespace savor::psctx
