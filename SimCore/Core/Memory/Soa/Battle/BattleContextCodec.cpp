#include "BattleContextCodec.h"
#include "../SoaAddrRegistry.h"
#include "../SoaStructReaders.h"
#include "../SoaConstants.h"
#include <cstring>

namespace soa::battle::ctx::codec {

    bool extract_from_mem1(const simcore::MemView& view, BattleContext& out)
    {
        if (!view.valid()) return false;

        for (int i = 0; i < 12; ++i) {
            out.slots_[i] = {};
            out.slots_[i].is_player = (i < 4) ? 1 : 0;
        }

        for (int i = 0; i < 12; ++i) {
            uint32_t p = 0;
            if (!view.read_u32(addr::Registry::spec(addr::battle::CombatantInstancesTable).base + i * 4, p)) return false;
            if (p && view.in_mem1(p)) {
                out.slots_[i].instance_addr = p;
                (void)soa::readers::read(view, p, out.slots_[i].instance);
                out.slots_[i].present = !(out.slots_[i].instance.status_flags & StatusFlags::Fled);
                out.slots_[i].is_alive = !(out.slots_[i].instance.status_flags & StatusFlags::Dead);
            }
        }

        for (int i = 0; i < 12; ++i) {
            uint16_t id = 0;
            if (!view.read_u16(addr::Registry::spec(addr::battle::CombatantIdTable).base + i * 2, id)) return false;
            out.slots_[i].id = id;
        }

        for (int i = 4; i < 12; ++i) {
            auto& s = out.slots_[i];
            if (!s.present) continue;

            uint32_t ed_va = s.instance.Enemy_Definition;
            if (!view.in_mem1(ed_va)) continue;

            s.enemy_def_addr = ed_va;
            s.has_enemy_def = 1;
            (void)soa::readers::read(view, ed_va, s.enemy_def);
        }

        uint32_t p = 0;
        if (!view.read_u32(addr::Registry::base(addr::battle::MainInstancePtr), p)) return false;
        if (p && view.in_mem1(p)) {
            (void)soa::readers::read(view, p, out.state);
        }

        p = addr::Registry::base(addr::battle::TurnType);
        if (p && view.in_mem1(p)) {
            uint32_t v;
            (void)soa::readers::read(view, p, v);
            out.turn_type = soa::battle::TurnType(v);
        }

        uint32_t bp;
        if (!view.read_u32(addr::Registry::base(addr::battle::BattlePhase), bp)) return false;
        out.battle_phase = bp;

        uint8_t ct;
        if (!view.read_u8(addr::Registry::base(addr::battle::CurrentTurn), ct)) return false;
        out.turn_count = ct;

        return true;
    }

    // Wire format is now native-endian.
    // Per-slot: present(1), is_player(1), id(u16 native), has_enemy_def(1), instance_addr(u32 native),
    //           if present: raw sizeof(CombatantInstance),
    //           enemy_def_addr(u32 native),
    //           if has_enemy_def: raw sizeof(EnemyDefinition).

    static inline void append_u16_native(std::string& s, uint16_t v) {
        const char* p = reinterpret_cast<const char*>(&v);
        s.append(p, p + sizeof(uint16_t));
    }
    static inline void append_u32_native(std::string& s, uint32_t v) {
        const char* p = reinterpret_cast<const char*>(&v);
        s.append(p, p + sizeof(uint32_t));
    }
    static inline bool read_u16_native(const char*& p, const char* end, uint16_t& v) {
        if (p + sizeof(uint16_t) > end) return false;
        std::memcpy(&v, p, sizeof(uint16_t)); p += sizeof(uint16_t); return true;
    }
    static inline bool read_u32_native(const char*& p, const char* end, uint32_t& v) {
        if (p + sizeof(uint32_t) > end) return false;
        std::memcpy(&v, p, sizeof(uint32_t)); p += sizeof(uint32_t); return true;
    }

    bool encode(const BattleContext& in, std::string& out)
    {
        out.clear();
        out.reserve(12 * (2 + 2 + 1 + 4 + sizeof(soa::CombatantInstance) + 4 + sizeof(soa::EnemyDefinition)));

        for (int i = 0; i < 12; ++i) {
            const auto& s = in.slots_[i];
            out.push_back(char(s.present));
            out.push_back(char(s.is_alive));
            out.push_back(char(s.is_player));
            append_u16_native(out, s.id);
            out.push_back(char(s.has_enemy_def));
            append_u32_native(out, s.instance_addr);
            if (s.present) {
                const char* bytes = reinterpret_cast<const char*>(&s.instance);
                out.append(bytes, bytes + sizeof(soa::CombatantInstance));
            }
            append_u32_native(out, s.enemy_def_addr);
            if (s.has_enemy_def) {
                const char* bytes = reinterpret_cast<const char*>(&s.enemy_def);
                out.append(bytes, bytes + sizeof(soa::EnemyDefinition));
            }
            
        }

        append_u32_native(out, (uint32_t)in.turn_type);
        append_u32_native(out, in.turn_count);
        append_u32_native(out, in.battle_phase);

        return true;
    }

    bool decode(std::string_view in, BattleContext& out)
    {
        const char* p = in.data();
        const char* const end = in.data() + in.size();

        for (int i = 0; i < 12; ++i) {
            auto& s = out.slots_[i];
            s = {};

            if (p + 1 > end) return false; s.present = static_cast<uint8_t>(*p++);
            if (p + 1 > end) return false; s.is_alive = static_cast<uint8_t>(*p++);
            if (p + 1 > end) return false; s.is_player = static_cast<uint8_t>(*p++);
            if (!read_u16_native(p, end, s.id)) return false;
            if (p + 1 > end) return false; s.has_enemy_def = static_cast<uint8_t>(*p++);
            if (!read_u32_native(p, end, s.instance_addr)) return false;

            if (s.present) {
                if (p + sizeof(soa::CombatantInstance) > end) return false;
                std::memcpy(&s.instance, p, sizeof(soa::CombatantInstance));
                p += sizeof(soa::CombatantInstance);
            }

            if (!read_u32_native(p, end, s.enemy_def_addr)) return false;

            if (s.has_enemy_def) {
                if (p + sizeof(soa::EnemyDefinition) > end) return false;
                std::memcpy(&s.enemy_def, p, sizeof(soa::EnemyDefinition));
                p += sizeof(soa::EnemyDefinition);
            }
        }
        
        uint32_t tt; read_u32_native(p, end, tt);
        out.turn_type = (soa::battle::TurnType)tt;
        read_u32_native(p, end, out.turn_count);
        read_u32_native(p, end, out.battle_phase);

        return p == end;
    }

    bool resolve(const simcore::MemView& view, const addr::DolphinAddr& a, uint32_t& out_va) {
        if (!view.in_mem1(a.base)) return false;
        out_va = a.base;
        return true;
    }

    bool readU8(const simcore::MemView& v, addr::AddrKey k, uint8_t& out) { uint32_t va = 0; if (!resolve(v, addr::Registry::spec(k), va)) return false; return v.read_u8(va, out); }
    bool readU16(const simcore::MemView& v, addr::AddrKey k, uint16_t& out) { uint32_t va = 0; if (!resolve(v, addr::Registry::spec(k), va)) return false; return v.read_u16(va, out); }
    bool readU32(const simcore::MemView& v, addr::AddrKey k, uint32_t& out) { uint32_t va = 0; if (!resolve(v, addr::Registry::spec(k), va)) return false; return v.read_u32(va, out); }
    bool readU64(const simcore::MemView& v, addr::AddrKey k, uint64_t& out) { uint32_t va = 0; if (!resolve(v, addr::Registry::spec(k), va)) return false; return v.read_u64(va, out); }

} // namespace simcore::battlectx::codec
