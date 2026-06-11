#pragma once
#include <cstdint>
#include <string>
#include <cstring>
#include <array>
#include <initializer_list>
#include "../../DerivedBase.h"
#include "../SoaAddrRegistry.h"
#include "../../../../Runner/Breakpoints/BpRegistry.h"
#include "../SoaStructs.h"

using namespace addr::derived::battle;

namespace savor {

    class DolphinWrapper;

    class DerivedBattleBuffer final : public IDerivedBuffer {
    public:
        static constexpr uint32_t DBUF_SIZE = 0x0440;
        static constexpr uint32_t DBUF_MAGIC = 0x44425546u;
        static constexpr uint16_t DBUF_VERSION = 1;
        static constexpr uint16_t DBUF_MAX_ITEMID = 512;
        static constexpr uint32_t DBUF_FLAG_VALID = 1u << 0;

        DerivedBattleBuffer() { bytes_.resize(DBUF_SIZE); }

        bool can_serve(addr::AddrKey k) const override {
            return addr::AddrRegistry::region(k) == addr::Region::DERIVED;
        }

        bool read_key(addr::AddrKey k, uint8_t width, uint64_t& out_bits) const override {
            const auto off = addr::AddrRegistry::base(k);
            if (off + width > bytes_.size()) return false;
            switch (width) {
            case 1: out_bits = *(const uint8_t*)(bytes_.data() + off);  return true;
            case 2: out_bits = *(const uint16_t*)(bytes_.data() + off); return true;
            case 4: out_bits = *(const uint32_t*)(bytes_.data() + off); return true;
            case 8: out_bits = *(const uint64_t*)(bytes_.data() + off); return true;
            default: return false;
            }
        }

        bool read_raw(uint32_t off, uint8_t width, uint64_t& out_bits) const override {
            if (off + width > bytes_.size()) return false;
            switch (width) {
            case 1: out_bits = *(const uint8_t*)(bytes_.data() + off);  return true;
            case 2: out_bits = *(const uint16_t*)(bytes_.data() + off); return true;
            case 4: out_bits = *(const uint32_t*)(bytes_.data() + off); return true;
            case 8: out_bits = *(const uint64_t*)(bytes_.data() + off); return true;
            default: return false;
            }
        }

        void on_init(const PSContext& ctx) override {
            (void)ctx;
            std::memset(bytes_.data(), 0, bytes_.size());
            write_u32(HeaderMagic, DBUF_MAGIC);
            write_u16(HeaderVersion, DBUF_VERSION);
            write_u16(HeaderMaxItemId, DBUF_MAX_ITEMID);
        }

        void update_turn_order_indices(savor::DolphinWrapper& host) {
            size_t idx_base = addr::AddrRegistry::base(TurnOrderIdx_base);
            for (int i = 0; i < 12; i++) write_u8(idx_base + i, 0xffu);

            uint8_t min_pc = 12, max_pc = 0, min_ec = 12, max_ec = 0;

            int cur_idx; uint8_t slot;
            for (cur_idx = 0; cur_idx < 12; cur_idx++) {
                host.readU8(addr::AddrRegistry::base(addr::battle::TurnOrderTable) + cur_idx, slot);
                if (slot == 0xff) break;
                write_u8(idx_base + slot, cur_idx);
                if (slot < 4) { if (cur_idx < min_pc) min_pc = cur_idx; else if (cur_idx > max_pc) max_pc = cur_idx; }
                else { if (cur_idx < min_ec) min_ec = cur_idx; else if (cur_idx > max_ec) max_ec = cur_idx; }
            }

            write_u32(TurnOrderSize, cur_idx);
            write_u8(TurnOrderPcMin, min_pc);
            write_u8(TurnOrderPcMax, max_pc);
            write_u8(TurnOrderEcMin, min_ec);
            write_u8(TurnOrderEcMax, max_ec);
        }

        void clear_item_tables() {
            std::memset(bytes_.data() + addr::AddrRegistry::base(DropsByItem_base), 0, DBUF_MAX_ITEMID);
            std::memset(bytes_.data() + addr::AddrRegistry::base(InventoryByItem_base), 0, DBUF_MAX_ITEMID);
        }

        void rebuild_inventory_table(savor::DolphinWrapper& host) {
            // Zero out the whole table
            std::memset(bytes_.data() + addr::AddrRegistry::base(InventoryByItem_base), 0, DBUF_MAX_ITEMID);

            uint32_t main_ptr = 0;
            host.readU32(addr::AddrRegistry::base(addr::battle::MainInstancePtr), main_ptr);
            if (!main_ptr) return;

            // Each entry is 4 bytes (ItemSlot), 80 entries
            for (int i = 0; i < 80; i++) {
                uint16_t item_id = 0;
                uint8_t count = 0;
                host.readU16(main_ptr + offsetof(soa::BattleState, useable_items) + i * sizeof(soa::ItemSlot) + offsetof(soa::ItemSlot, item_id), item_id);
                host.readU8(main_ptr + offsetof(soa::BattleState, useable_items) + i * sizeof(soa::ItemSlot) + offsetof(soa::ItemSlot, count), count);

                if (item_id < DBUF_MAX_ITEMID) {
                    uint8_t clamped = (count > 99) ? 99 : count;
                    size_t off = addr::AddrRegistry::base(InventoryByItem_base) + item_id;
                    bytes_[off] = clamped;
                }
            }
        }

        void fold_enemy_drops_into_table(savor::DolphinWrapper& host) {
            // Zero out the whole table
            std::memset(bytes_.data() + addr::AddrRegistry::base(DropsByItem_base), 0, DBUF_MAX_ITEMID);

            uint32_t main_ptr = 0;
            host.readU32(addr::AddrRegistry::base(addr::battle::MainInstancePtr), main_ptr);
            if (!main_ptr) return;

            for (int i = 0; i < 8; i++) {
                uint16_t count = 0;
                uint16_t item_id = 0;
                host.readU16(main_ptr + offsetof(soa::BattleState, item_drops) + i * sizeof(soa::BattleItemDropSlot) + offsetof(soa::BattleItemDropSlot, count), count);
                host.readU16(main_ptr + offsetof(soa::BattleState, item_drops) + i * sizeof(soa::BattleItemDropSlot) + offsetof(soa::BattleItemDropSlot, item_id), item_id);

                if (static_cast<int16_t>(item_id) >= 0 && static_cast<int16_t>(item_id) < DBUF_MAX_ITEMID && static_cast<int16_t>(count) > 0) {
                    size_t off = addr::AddrRegistry::base(DropsByItem_base) + (size_t)item_id;
                    uint8_t cur = bytes_[off];
                    uint16_t sum = static_cast<uint16_t>(cur) + count;
                    if (sum > 99) sum = 99;
                    bytes_[off] = static_cast<uint8_t>(sum);
                }
            }
        }

        void update_on_bp(uint32_t hit_bp, const PSContext& ctx, savor::DolphinWrapper& host) override {
            write_u16(HeaderLastUpdateBp, (uint16_t)hit_bp);
            uint8_t cur; host.readU8(addr::AddrRegistry::base(addr::battle::CurrentTurn), cur);
            write_u16(CurrentTurn, cur);

            uint64_t flags = 0; read_key(HeaderFlags, 4, flags);
            flags |= DBUF_FLAG_VALID; write_u32(HeaderFlags, (uint32_t)flags);
            
            if (hit_bp == (uint32_t)bp::battle::TurnIsReady) 
            {
                update_turn_order_indices(host);
                rebuild_inventory_table(host);

                uint64_t cur; read_key(CurrentTurn, 4, cur);
                write_u16(HeaderLastUpdateTurn, (uint16_t)cur);
            }
            if (hit_bp == (uint32_t)bp::battle::EndTurn 
                || hit_bp == (uint32_t)bp::battle::EndBattleVictory)
            {
                fold_enemy_drops_into_table(host);
                uint64_t cur; read_key(CurrentTurn, 4, cur);
                write_u16(HeaderLastUpdateTurn, (uint16_t)cur);
            }
        }

    private:
        void write_u8(size_t off, uint8_t v) {
            if (off + 1 > bytes_.size()) bytes_.resize(off + 1);
            bytes_[off] = v;
        }

        void write_u8(addr::AddrKey k, uint8_t v) {
            const auto off = addr::AddrRegistry::base(k);
            if (off + 1 > bytes_.size()) bytes_.resize(off + 1);
            bytes_[off] = v;
        }

        void write_u16(size_t off, uint16_t v) {
            if (off + 2 > bytes_.size()) bytes_.resize(off + 2);
            std::memcpy(bytes_.data() + off, &v, 2);
        }

        void write_u16(addr::AddrKey k, uint16_t v) {
            const auto off = addr::AddrRegistry::base(k);
            if (off + 2 > bytes_.size()) bytes_.resize(off + 2);
            std::memcpy(bytes_.data() + off, &v, 2);
        }

        void write_u32(size_t off, uint32_t v) {
            if (off + 4 > bytes_.size()) bytes_.resize(off + 4);
            std::memcpy(bytes_.data() + off, &v, 4);
        }

        void write_u32(addr::AddrKey k, uint32_t v) {
            const auto off = addr::AddrRegistry::base(k);
            if (off + 4 > bytes_.size()) bytes_.resize(off + 4);
            std::memcpy(bytes_.data() + off, &v, 4);
        }

        std::string bytes_;
    };

} // namespace savor
