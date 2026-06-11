#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <format>
#include <unordered_set>
#include "../../Core/DolphinWrapper.h"
#include "../../Core/Memory/Soa/SoaStructs.h"
#include "../../Core/Memory/Soa/SoaConstants.h"
#include "../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../Core/Memory/Soa/SoaStructReaders.h"
#include "../../Core/Memory/Soa/Battle/BattleContext.h"

enum class CoreProgressFlags : uint32_t {
	ViDelta = 1 << 0,
	WarnViStall = 1 << 1,
	Filename = 1 << 2,
	ScriptSection = 1 << 3,
	BattleProgress = 1 << 4,
	PredicateProgress = 1 << 5,

	DontRecordHeartbeat = 1u << 31
};

using soa::battle::ctx::StatusFlags;

namespace savor::progress {
	struct ProgressDeets {
		uint32_t poll_rate = 0;
		uint32_t flags = 0;

		inline void clear_flags() { flags = 0; }
		inline void set_flag(CoreProgressFlags f) { flags |= uint32_t(f); }
		inline void clear_flag(CoreProgressFlags f) { flags &= ~uint32_t(f); }
		inline bool has_flag(CoreProgressFlags f) const { return (flags & uint32_t(f)) != 0; }
	};

	using BattleProgressFxn = std::function<std::string(savor::DolphinWrapper&)>;

	struct BattleProgressEntry {
		uint32_t key;
		BattleProgressFxn fxn;
	};

	static inline std::string get_combatant_name(savor::DolphinWrapper& dw, uint32_t slot) {
		uint16_t id = 0xffffu;
		dw.readU16(addr::AddrRegistry::base(addr::battle::CombatantIdTable) + (slot * 2), id);
		if (id == 0xffffu) return "";
		if (slot < 4) {
			return std::string(soa::text::PCNames[id]);
		}
		else {
			return std::format("[{}]{}", slot, soa::text::get_enemy_name(id));
		}
	}

	static inline std::string describe_instruction(savor::DolphinWrapper& dw, soa::InstructionSet inst) {
		std::string inst_str = std::format("{}", soa::battle::InstructionNames[inst.current.inst]);
		if ((0 < inst.current.inst && inst.current.inst < 3) || inst.current.inst == 5 || inst.current.inst == 12) inst_str = std::format("{}:{}", inst_str, inst.current.instParam);
		if ((0 < inst.current.inst && inst.current.inst < 4) || inst.current.inst == 5 || inst.current.inst == 12) inst_str = std::format("{}->{}", inst_str, get_combatant_name(dw, inst.current.target));
		return inst_str;
	}

	static auto BattleProgress = std::to_array<BattleProgressEntry>({
		// Attack Damage: Displays damage taken (setupTurnAction -> performAttack)
		{
			0x80081d04,
			[](savor::DolphinWrapper& dw)->std::string
			{
				uint32_t atk_slot = dw.getRegister(31);
				std::string atk_name = get_combatant_name(dw, atk_slot);

				uint32_t tgt_slot = dw.getRegister(30);
				std::string tgt_name = get_combatant_name(dw, tgt_slot);

				uint32_t atk_dmg = dw.getRegister(0);

				

				return std::format("{} attacks {} for {} damage", atk_name, tgt_name, atk_dmg);
			}
		},

		// Counter attack: Displays if target counters (setupTurnAction -> performAttack)
		{
			0x80081d84,
			[](savor::DolphinWrapper& dw)->std::string
			{
				uint32_t tgt_slot = dw.getRegister(30);
				std::string tgt_name = get_combatant_name(dw, tgt_slot);

				return std::format("{} counter attacks...", tgt_name);
			}
		},

		// Combatant death: Displays if a combatant dies (HandleCombatantDeath)
		{
			0x8002bc84,
			[](savor::DolphinWrapper& dw)->std::string
			{
				uint32_t slot = dw.getRegister(31);
				std::string name = get_combatant_name(dw, slot);

				return std::format("{} died...", name);
			}
		},

		// Item Drop: Displays if an enemmy combatant dies and drops an item (HandleCombatantDeath -> enemyDropItem)
		{
			0x8002bb98,
			[](savor::DolphinWrapper& dw)->std::string
			{
				uint32_t slot = dw.getRegister(29);
				std::string name = get_combatant_name(dw, slot);

				// get ItemDropSlot for battle state slot

				uint32_t main_ptr = 0;
				dw.readU32(addr::AddrRegistry::base(addr::battle::MainInstancePtr), main_ptr);
				if (!main_ptr) return {};

				uint16_t count = 0;
				uint16_t item_id = 0;
				dw.readU16(main_ptr + offsetof(soa::BattleState, item_drops) + (slot - 4) * sizeof(soa::BattleItemDropSlot) + offsetof(soa::BattleItemDropSlot, count), count);
				dw.readU16(main_ptr + offsetof(soa::BattleState, item_drops) + (slot - 4) * sizeof(soa::BattleItemDropSlot) + offsetof(soa::BattleItemDropSlot, item_id), item_id);

				if (item_id == 0xffff) return std::format("{} dropped nothing", name);

				std::string item_name{ soa::text::get_item_name(item_id) };

				return std::format("{} dropped [{}]{} x{}", name, item_id, item_name, count);
			}
		},

		// Combatant Instructions: Displays combatant instructions (Setup Turn at return)
		{
			0x800715e4u,
			[](savor::DolphinWrapper& dw)->std::string
			{
				std::vector<std::string> insts;

				for (int i = 0; i < 12; i++) {
					uint32_t p = 0;
					if (!dw.readU32(addr::AddrRegistry::spec(addr::battle::CombatantInstancesTable).base + i * 4, p)) return "";
					soa::CombatantInstance instance{};
					std::string instance_raw{};
					if (!dw.getMem1RangeRaw(instance_raw, p, sizeof(instance))) return "";
					(void)soa::readers::read(instance_raw, instance);
					if ((instance.status_flags & StatusFlags::Fled) || instance.status_flags & StatusFlags::Dead) continue;
					
					soa::InstructionSet instructionset{};
					std::string instructionset_raw{};

					if (!dw.getMem1RangeRaw(instructionset_raw, addr::AddrRegistry::base(addr::battle::Instructions) + (i * sizeof(instructionset)), sizeof(instructionset))) return "";
					(void)soa::readers::read(instructionset_raw, instructionset);

					std::string name = get_combatant_name(dw, i);
					std::string inst = describe_instruction(dw, instructionset);

					insts.push_back(std::format("{}: {}", name, inst));
				}
			
				std::string insts_str_out{};
				bool first = true;
				for (auto s : insts) {
					if (first) first = false;
					else insts_str_out += "\n";
					insts_str_out += s;
				}

				return insts_str_out;
			}
		}
	});

	static std::unordered_set<uint32_t> bpbps;

	static inline std::unordered_set<uint32_t> BattleProgressBPs() {
		if (bpbps.size() != 0) return bpbps;
		for (auto [bp, fxn] : BattleProgress) {
			bpbps.insert(bp);
		}
		return bpbps;
	}
}

