#include "ScriptProgress.h"

#include <cstddef>
#include <format>
#include <sstream>
#include <string_view>

#include "../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../Core/Memory/Soa/SoaConstants.h"
#include "../../Core/Memory/Soa/SoaStructs.h"

namespace savor::progress {
namespace {

using probe::MemoryAccess;
using probe::ProbeDefinition;
using probe::ProbeKind;
using probe::SampleDefinition;
using probe::SampleKind;
using probe::SampleWidth;
using probe::Subscription;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::vector<std::uint8_t> absolute_program(std::uint32_t address)
{
    std::vector<std::uint8_t> program{ 0x07 };
    append_u32(program, address);
    return program;
}

void add_offset(std::vector<std::uint8_t>& program, std::int32_t offset)
{
    program.push_back(0x03);
    append_u32(program, static_cast<std::uint32_t>(offset));
}

void load_pointer(std::vector<std::uint8_t>& program)
{
    program.push_back(0x02);
}

void add_gpr_scaled(std::vector<std::uint8_t>& program, std::uint8_t reg, std::uint32_t stride)
{
    program.push_back(0x08);
    program.push_back(reg);
    append_u32(program, stride);
}

void finish(std::vector<std::uint8_t>& program)
{
    program.push_back(0x00);
}

SampleDefinition gpr(std::string name, std::uint8_t reg)
{
    SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = SampleKind::Gpr;
    sample.width = SampleWidth::U32;
    sample.base_register = reg;
    return sample;
}

SampleDefinition memory(std::string name, std::uint32_t address, SampleWidth width)
{
    SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = SampleKind::Memory;
    sample.width = width;
    sample.address = address;
    return sample;
}

SampleDefinition address_program(
    std::string name,
    SampleWidth width,
    std::vector<std::uint8_t> program)
{
    SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = SampleKind::AddressProgram;
    sample.width = width;
    sample.address_program = std::move(program);
    return sample;
}

std::vector<std::uint8_t> combatant_id_program(std::uint8_t slot_register)
{
    auto program = absolute_program(addr::AddrRegistry::base(addr::battle::CombatantIdTable));
    add_gpr_scaled(program, slot_register, 2);
    finish(program);
    return program;
}

ProbeDefinition progress_probe(std::string id, std::uint32_t pc, std::string formatter)
{
    ProbeDefinition probe;
    probe.id = std::move(id);
    probe.group = "battle_progress";
    probe.kind = ProbeKind::Pc;
    probe.subscriptions = Subscription::Progress;
    probe.address = pc;
    probe.progress_formatter = std::move(formatter);
    probe.progress_record = true;
    return probe;
}

std::optional<std::uint64_t> field_value(
    const capture_format::Event& event,
    std::string_view name)
{
    for (const auto& field : event.fields) {
        if (field.name == name && field.status == capture_format::FieldStatus::Present)
            return field.value;
    }
    return std::nullopt;
}

std::string combatant_name(std::uint32_t slot, std::uint16_t id)
{
    if (id == 0xffffu)
        return {};
    if (slot < 4) {
        if (id < soa::text::PCNames.size())
            return std::string(soa::text::PCNames[id]);
        return std::format("[{}]PC{}", slot, id);
    }
    return std::format("[{}]{}", slot, soa::text::get_enemy_name(id));
}

std::string instruction_description(
    std::uint32_t inst,
    std::uint32_t param,
    std::uint32_t target,
    std::uint16_t target_id)
{
    const auto name = inst < soa::battle::InstructionNames.size()
        ? soa::battle::InstructionNames[inst]
        : std::string_view("Unknown");
    std::string result(name);
    if ((inst > 0 && inst < 3) || inst == 5 || inst == 12)
        result = std::format("{}:{}", result, param);
    if ((inst > 0 && inst < 4) || inst == 5 || inst == 12)
        result = std::format("{}->{}", result, combatant_name(target, target_id));
    return result;
}

} // namespace

void append_battle_progress_probes(probe::Profile& profile)
{
    {
        auto probe = progress_probe("battle_progress.attack_damage", 0x80081d04, "attack_damage");
        probe.samples.push_back(gpr("attacker_slot", 31));
        probe.samples.push_back(gpr("target_slot", 30));
        probe.samples.push_back(gpr("damage", 0));
        probe.samples.push_back(address_program("attacker_id", SampleWidth::U16, combatant_id_program(31)));
        probe.samples.push_back(address_program("target_id", SampleWidth::U16, combatant_id_program(30)));
        profile.probes.push_back(std::move(probe));
    }
    {
        auto probe = progress_probe("battle_progress.counterattack", 0x80081d84, "counterattack");
        probe.samples.push_back(gpr("target_slot", 30));
        probe.samples.push_back(address_program("target_id", SampleWidth::U16, combatant_id_program(30)));
        profile.probes.push_back(std::move(probe));
    }
    {
        auto probe = progress_probe("battle_progress.death", 0x8002bc84, "death");
        probe.samples.push_back(gpr("slot", 31));
        probe.samples.push_back(address_program("combatant_id", SampleWidth::U16, combatant_id_program(31)));
        profile.probes.push_back(std::move(probe));
    }
    {
        auto probe = progress_probe("battle_progress.item_drop", 0x8002bb98, "item_drop");
        probe.samples.push_back(gpr("slot", 29));
        probe.samples.push_back(address_program("combatant_id", SampleWidth::U16, combatant_id_program(29)));

        auto count = absolute_program(addr::AddrRegistry::base(addr::battle::MainInstancePtr));
        load_pointer(count);
        add_offset(count, static_cast<std::int32_t>(offsetof(soa::BattleState, item_drops))
            - 4 * static_cast<std::int32_t>(sizeof(soa::BattleItemDropSlot))
            + static_cast<std::int32_t>(offsetof(soa::BattleItemDropSlot, count)));
        add_gpr_scaled(count, 29, sizeof(soa::BattleItemDropSlot));
        finish(count);
        probe.samples.push_back(address_program("drop_count", SampleWidth::U16, std::move(count)));

        auto item = absolute_program(addr::AddrRegistry::base(addr::battle::MainInstancePtr));
        load_pointer(item);
        add_offset(item, static_cast<std::int32_t>(offsetof(soa::BattleState, item_drops))
            - 4 * static_cast<std::int32_t>(sizeof(soa::BattleItemDropSlot))
            + static_cast<std::int32_t>(offsetof(soa::BattleItemDropSlot, item_id)));
        add_gpr_scaled(item, 29, sizeof(soa::BattleItemDropSlot));
        finish(item);
        probe.samples.push_back(address_program("item_id", SampleWidth::U16, std::move(item)));
        profile.probes.push_back(std::move(probe));
    }
    {
        auto probe = progress_probe("battle_progress.instructions", 0x800715e4, "instructions");
        for (std::uint32_t slot = 0; slot < 12; ++slot) {
            const auto prefix = "slot" + std::to_string(slot) + "_";
            probe.samples.push_back(memory(
                prefix + "id",
                addr::AddrRegistry::base(addr::battle::CombatantIdTable) + slot * 2,
                SampleWidth::U16));

            auto status = absolute_program(
                addr::AddrRegistry::base(addr::battle::CombatantInstancesTable) + slot * 4);
            load_pointer(status);
            add_offset(status, offsetof(soa::CombatantInstance, status_flags));
            finish(status);
            probe.samples.push_back(address_program(prefix + "status", SampleWidth::U32, std::move(status)));

            const auto instruction = addr::AddrRegistry::base(addr::battle::Instructions)
                + slot * static_cast<std::uint32_t>(sizeof(soa::InstructionSet));
            probe.samples.push_back(memory(prefix + "inst", instruction
                + static_cast<std::uint32_t>(offsetof(soa::InstructionSet, current))
                + static_cast<std::uint32_t>(offsetof(soa::Instruction, inst)), SampleWidth::U32));
            probe.samples.push_back(memory(prefix + "target", instruction
                + static_cast<std::uint32_t>(offsetof(soa::InstructionSet, current))
                + static_cast<std::uint32_t>(offsetof(soa::Instruction, target)), SampleWidth::U8));
            probe.samples.push_back(memory(prefix + "param", instruction
                + static_cast<std::uint32_t>(offsetof(soa::InstructionSet, current))
                + static_cast<std::uint32_t>(offsetof(soa::Instruction, instParam)), SampleWidth::U16));
        }
        profile.probes.push_back(std::move(probe));
    }
}

std::optional<FormattedProgress> format_battle_progress(
    const capture_format::Event& event,
    bool record_progress)
{
    if (event.probe_id == "battle_progress.attack_damage") {
        const auto attacker_slot = field_value(event, "attacker_slot");
        const auto target_slot = field_value(event, "target_slot");
        const auto damage = field_value(event, "damage");
        const auto attacker_id = field_value(event, "attacker_id");
        const auto target_id = field_value(event, "target_id");
        if (!attacker_slot || !target_slot || !damage || !attacker_id || !target_id)
            return std::nullopt;
        return FormattedProgress{
            std::format("{} attacks {} for {} damage",
                combatant_name(static_cast<std::uint32_t>(*attacker_slot), static_cast<std::uint16_t>(*attacker_id)),
                combatant_name(static_cast<std::uint32_t>(*target_slot), static_cast<std::uint16_t>(*target_id)),
                *damage),
            record_progress,
        };
    }
    if (event.probe_id == "battle_progress.counterattack") {
        const auto slot = field_value(event, "target_slot");
        const auto id = field_value(event, "target_id");
        if (!slot || !id)
            return std::nullopt;
        return FormattedProgress{
            std::format("{} counter attacks...",
                combatant_name(static_cast<std::uint32_t>(*slot), static_cast<std::uint16_t>(*id))),
            record_progress,
        };
    }
    if (event.probe_id == "battle_progress.death") {
        const auto slot = field_value(event, "slot");
        const auto id = field_value(event, "combatant_id");
        if (!slot || !id)
            return std::nullopt;
        return FormattedProgress{
            std::format("{} died...",
                combatant_name(static_cast<std::uint32_t>(*slot), static_cast<std::uint16_t>(*id))),
            record_progress,
        };
    }
    if (event.probe_id == "battle_progress.item_drop") {
        const auto slot = field_value(event, "slot");
        const auto combatant_id = field_value(event, "combatant_id");
        const auto count = field_value(event, "drop_count");
        const auto item_id = field_value(event, "item_id");
        if (!slot || !combatant_id || !count || !item_id)
            return std::nullopt;
        const auto name = combatant_name(
            static_cast<std::uint32_t>(*slot), static_cast<std::uint16_t>(*combatant_id));
        if (static_cast<std::uint16_t>(*item_id) == 0xffffu)
            return FormattedProgress{ std::format("{} dropped nothing", name), record_progress };
        return FormattedProgress{
            std::format("{} dropped [{}]{} x{}", name, *item_id,
                soa::text::get_item_name(static_cast<std::size_t>(*item_id)), *count),
            record_progress,
        };
    }
    if (event.probe_id == "battle_progress.instructions") {
        std::ostringstream out;
        bool first = true;
        for (std::uint32_t slot = 0; slot < 12; ++slot) {
            const auto prefix = "slot" + std::to_string(slot) + "_";
            const auto id = field_value(event, prefix + "id");
            const auto status = field_value(event, prefix + "status");
            const auto inst = field_value(event, prefix + "inst");
            const auto target = field_value(event, prefix + "target");
            const auto param = field_value(event, prefix + "param");
            if (!id || !status || !inst || !target || !param)
                continue;
            if ((*status & (soa::battle::ctx::Fled | soa::battle::ctx::Dead)) != 0)
                continue;
            const auto target_id = field_value(event, "slot" + std::to_string(*target) + "_id")
                .value_or(0xffffu);
            if (!first)
                out << '\n';
            first = false;
            out << combatant_name(slot, static_cast<std::uint16_t>(*id)) << ": "
                << instruction_description(
                    static_cast<std::uint32_t>(*inst),
                    static_cast<std::uint32_t>(*param),
                    static_cast<std::uint32_t>(*target),
                    static_cast<std::uint16_t>(target_id));
        }
        return FormattedProgress{ out.str(), record_progress };
    }
    return std::nullopt;
}

} // namespace savor::progress
