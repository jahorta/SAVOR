#include "ProgressTypes.h"

#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Core/Memory/Soa/SoaConstants.h"
#include "Core/Memory/Soa/SoaStructs.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <type_traits>

namespace savor::runtime::progress {
namespace {

void AppendField(std::string& output, std::string_view value)
{
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

template <typename Value>
void AppendNumber(std::string& output, Value value)
{
    AppendField(output, std::to_string(value));
}

[[nodiscard]] bool CompleteSha256(std::string_view value) noexcept
{
    return value.size() == 64 && std::ranges::all_of(
        value,
        [](char ch) {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

[[nodiscard]] std::string IdentityHash(
    std::string_view category,
    std::string_view canonical_id,
    std::uint32_t revision,
    std::string_view contract)
{
    std::string canonical;
    AppendField(canonical, category);
    AppendField(canonical, canonical_id);
    AppendNumber(canonical, revision);
    AppendField(canonical, contract);
    return hash::sha256(canonical.data(), canonical.size());
}

ProgressSchemaIdentity Schema(
    std::string canonical_id,
    std::string contract)
{
    ProgressSchemaIdentity result;
    result.canonical_id = std::move(canonical_id);
    result.revision = 1;
    result.sha256 = IdentityHash(
        "savor.progress.schema/v1",
        result.canonical_id,
        result.revision,
        contract);
    return result;
}

ProgressFormatterIdentity Formatter(
    std::string canonical_id,
    std::string contract)
{
    ProgressFormatterIdentity result;
    result.canonical_id = std::move(canonical_id);
    result.revision = 1;
    result.sha256 = IdentityHash(
        "savor.progress.formatter/v1",
        result.canonical_id,
        result.revision,
        contract);
    return result;
}

void FinishLibrary(ProgressLibraryDescriptor& library)
{
    std::string canonical;
    AppendField(canonical, "savor.progress.library/v1");
    AppendField(canonical, library.canonical_id);
    AppendNumber(canonical, library.revision);
    AppendField(canonical, library.provider_identity);
    AppendNumber(canonical, library.points.size());
    for (const ProgressPointDescriptor& point : library.points)
    {
        AppendField(canonical, point.canonical_id);
        AppendNumber(canonical, static_cast<std::uint8_t>(point.provider));
        AppendNumber(canonical, point.breakpoint_pc.has_value() ? 1 : 0);
        AppendNumber(canonical, point.breakpoint_pc.value_or(0));
        AppendField(canonical, point.schema.canonical_id);
        AppendNumber(canonical, point.schema.revision);
        AppendField(canonical, point.schema.sha256);
        AppendField(canonical, point.formatter.canonical_id);
        AppendNumber(canonical, point.formatter.revision);
        AppendField(canonical, point.formatter.sha256);
        AppendNumber(canonical, point.required_capture_fields.size());
        for (const std::string& field : point.required_capture_fields)
            AppendField(canonical, field);
    }
    library.canonical_sha256 =
        hash::sha256(canonical.data(), canonical.size());
}

ProgressLibraryDescriptor RuntimeViLibrary()
{
    ProgressLibraryDescriptor result{
        .canonical_id = "soa.progress.runtime.vi/1",
        .revision = 1,
        .provider_identity = "savor.runtime.execution_snapshot/1",
        .points = {{
            .canonical_id = "vi.current",
            .provider = ProgressProviderKind::RuntimeSample,
            .schema = Schema(
                "soa.progress.schema.vi/1",
                "record{vi_count:u64}"),
            .formatter = Formatter(
                "soa.progress.formatter.vi/1",
                "VI {vi_count}"),
            .display_name = "VI",
        }},
    };
    FinishLibrary(result);
    return result;
}

ProgressLibraryDescriptor ScriptLocationLibrary()
{
    ProgressLibraryDescriptor result{
        .canonical_id = "soa.progress.soa.script_location/1",
        .revision = 1,
        .provider_identity = "soa.runtime.script_location/1",
        .points = {{
            .canonical_id = "script_location.current",
            .provider = ProgressProviderKind::RuntimeSample,
            .schema = Schema(
                "soa.progress.schema.script_location/1",
                "record{file:text,section:text,pc:u32}"),
            .formatter = Formatter(
                "soa.progress.formatter.script_location/1",
                "{file}:{section} at {pc}"),
            .display_name = "Script location",
        }},
    };
    FinishLibrary(result);
    return result;
}

ProgressLibraryDescriptor SeedCallsLibrary()
{
    ProgressLibraryDescriptor result{
        .canonical_id = "soa.progress.soa.seed_calls/1",
        .revision = 1,
        .provider_identity = "soa.capture.seed_calls/1",
        .points = {{
            .canonical_id = "rng.srand",
            .provider = ProgressProviderKind::BreakpointCapture,
            .breakpoint_pc = 0x8025ecbcu,
            .schema = Schema(
                "soa.progress.schema.seed_call/1",
                "record{seed_argument:u32,previous_rng:u32,movie_input_count:u64,caller_stack:stack_trace}"),
            .formatter = Formatter(
                "soa.progress.formatter.seed_call/1",
                "srand seed, prior RNG, movie cursor, and caller witness"),
            .required_capture_fields = {
                "seed_argument",
                "previous_rng",
                "movie_input_count",
                "caller_stack",
            },
            .display_name = "RNG seed call",
        }},
    };
    FinishLibrary(result);
    return result;
}

ProgressPointDescriptor BattlePoint(
    std::string id,
    std::uint32_t pc,
    std::string display,
    std::string schema_contract,
    std::vector<std::string> required_fields)
{
    const std::string schema_id =
        "soa.progress.schema.battle." + id + "/1";
    const std::string formatter_id =
        "soa.progress.formatter.battle." + id + "/1";
    return {
        .canonical_id = std::move(id),
        .provider = ProgressProviderKind::BreakpointCapture,
        .breakpoint_pc = pc,
        .schema = Schema(
            schema_id,
            std::move(schema_contract)),
        .formatter = Formatter(
            formatter_id,
            "battle event with validated typed fields"),
        .required_capture_fields = std::move(required_fields),
        .display_name = std::move(display),
    };
}

std::vector<std::string> InstructionFields()
{
    std::vector<std::string> fields;
    fields.reserve(60);
    for (std::uint32_t slot = 0; slot < 12; ++slot)
    {
        const std::string prefix = "slot" + std::to_string(slot) + "_";
        fields.push_back(prefix + "id");
        fields.push_back(prefix + "status");
        fields.push_back(prefix + "inst");
        fields.push_back(prefix + "target");
        fields.push_back(prefix + "param");
    }
    return fields;
}

ProgressLibraryDescriptor BattleEventsLibrary()
{
    ProgressLibraryDescriptor result{
        .canonical_id = "soa.progress.battle.events/1",
        .revision = 1,
        .provider_identity = "soa.capture.battle_event_points/1",
        .points = {
            BattlePoint(
                "attack_damage",
                0x80081d04u,
                "Attack damage",
                "record{attacker_slot:u32,target_slot:u32,damage:u32,attacker_id:u16,target_id:u16}",
                {"attacker_slot", "target_slot", "damage", "attacker_id", "target_id"}),
            BattlePoint(
                "counterattack",
                0x80081d84u,
                "Counterattack",
                "record{target_slot:u32,target_id:u16}",
                {"target_slot", "target_id"}),
            BattlePoint(
                "death",
                0x8002bc84u,
                "Death",
                "record{slot:u32,combatant_id:u16}",
                {"slot", "combatant_id"}),
            BattlePoint(
                "item_drop",
                0x8002bb98u,
                "Item drop",
                "record{slot:u32,combatant_id:u16,drop_count:u16,item_id:u16}",
                {"slot", "combatant_id", "drop_count", "item_id"}),
            BattlePoint(
                "instruction_turn_state",
                0x800715e4u,
                "Battle instruction / turn state",
                "record{slots:array[12]{id:u16,status:u32,instruction:u32,target:u8,param:u16}}",
                InstructionFields()),
        },
    };
    FinishLibrary(result);
    return result;
}

ProgressLibraryDescriptor PredicateLibrary()
{
    ProgressLibraryDescriptor result{
        .canonical_id = "soa.progress.predicate.evaluations/1",
        .revision = 1,
        .provider_identity = "soa.program.predicate_emission/1",
        .points = {{
            .canonical_id = "predicate.evaluated",
            .provider = ProgressProviderKind::PhaseLibrary,
            .schema = Schema(
                "soa.progress.schema.predicate_evaluation/1",
                "registered predicate evaluation record"),
            .formatter = Formatter(
                "soa.progress.formatter.predicate_evaluation/1",
                "Predicate {predicate_id}: {status}"),
            .display_name = "Predicate evaluation",
        }},
    };
    FinishLibrary(result);
    return result;
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void WriteU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void WriteString(std::vector<std::uint8_t>& out, std::string_view value)
{
    WriteU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void WriteProgramU32(
    std::vector<std::uint8_t>& output,
    std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::vector<std::uint8_t> AbsoluteAddressProgram(std::uint32_t address)
{
    std::vector<std::uint8_t> program{0x07};
    WriteProgramU32(program, address);
    return program;
}

void AddProgramOffset(
    std::vector<std::uint8_t>& program,
    std::int32_t offset)
{
    program.push_back(0x03);
    WriteProgramU32(program, static_cast<std::uint32_t>(offset));
}

void LoadProgramPointer(std::vector<std::uint8_t>& program)
{
    program.push_back(0x02);
}

void AddProgramGprScaled(
    std::vector<std::uint8_t>& program,
    std::uint8_t reg,
    std::uint32_t stride)
{
    program.push_back(0x08);
    program.push_back(reg);
    WriteProgramU32(program, stride);
}

void FinishAddressProgram(std::vector<std::uint8_t>& program)
{
    program.push_back(0x00);
}

savor::probe::SampleDefinition GprSample(
    std::string name,
    std::uint8_t reg)
{
    savor::probe::SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = savor::probe::SampleKind::Gpr;
    sample.width = savor::probe::SampleWidth::U32;
    sample.base_register = reg;
    return sample;
}

savor::probe::SampleDefinition MemorySample(
    std::string name,
    std::uint32_t address,
    savor::probe::SampleWidth width)
{
    savor::probe::SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = savor::probe::SampleKind::Memory;
    sample.width = width;
    sample.address = address;
    return sample;
}

savor::probe::SampleDefinition RoutedSample(
    std::string name,
    std::uint32_t descriptor_id)
{
    savor::probe::SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = savor::probe::SampleKind::RoutedSample;
    sample.width = savor::probe::SampleWidth::U64;
    sample.routed_sample_descriptor_id = descriptor_id;
    return sample;
}

savor::probe::SampleDefinition StackTraceSample(
    std::string name,
    std::uint32_t max_frames)
{
    savor::probe::SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = savor::probe::SampleKind::StackTrace;
    sample.max_frames = max_frames;
    return sample;
}

savor::probe::SampleDefinition AddressProgramSample(
    std::string name,
    savor::probe::SampleWidth width,
    std::vector<std::uint8_t> program)
{
    savor::probe::SampleDefinition sample;
    sample.name = std::move(name);
    sample.kind = savor::probe::SampleKind::AddressProgram;
    sample.width = width;
    sample.address_program = std::move(program);
    return sample;
}

std::vector<std::uint8_t> CombatantIdProgram(std::uint8_t slot_register)
{
    auto program = AbsoluteAddressProgram(
        addr::AddrRegistry::base(addr::battle::CombatantIdTable));
    AddProgramGprScaled(program, slot_register, 2);
    FinishAddressProgram(program);
    return program;
}

std::optional<std::uint64_t> FieldValue(
    const savor::capture_format::Event& event,
    std::string_view name)
{
    const auto found = std::ranges::find_if(
        event.fields,
        [&](const savor::capture_format::Field& field) {
            return field.name == name &&
                field.status == savor::capture_format::FieldStatus::Present;
        });
    return found == event.fields.end()
        ? std::nullopt
        : std::optional<std::uint64_t>(found->value);
}

std::string CombatantName(std::uint32_t slot, std::uint16_t id)
{
    if (id == 0xffffu)
        return {};
    if (slot < 4)
    {
        if (id < soa::text::PCNames.size())
            return std::string(soa::text::PCNames[id]);
        return std::format("[{}]PC{}", slot, id);
    }
    return std::format("[{}]{}", slot, soa::text::get_enemy_name(id));
}

std::string InstructionDescription(
    std::uint32_t instruction,
    std::uint32_t parameter,
    std::uint32_t target,
    std::uint16_t target_id)
{
    const std::string_view name =
        instruction < soa::battle::InstructionNames.size()
        ? soa::battle::InstructionNames[instruction]
        : std::string_view("Unknown");
    std::string result(name);
    if ((instruction > 0 && instruction < 3) ||
        instruction == 5 || instruction == 12)
    {
        result = std::format("{}:{}", result, parameter);
    }
    if ((instruction > 0 && instruction < 4) ||
        instruction == 5 || instruction == 12)
    {
        result = std::format(
            "{}->{}",
            result,
            CombatantName(target, target_id));
    }
    return result;
}

} // namespace

ProgressSchemaIdentity::operator bool() const noexcept
{
    return !canonical_id.empty() && revision != 0 && CompleteSha256(sha256);
}

ProgressFormatterIdentity::operator bool() const noexcept
{
    return !canonical_id.empty() && revision != 0 && CompleteSha256(sha256);
}

ProgressLibraryDescriptor::operator bool() const noexcept
{
    return !canonical_id.empty() && revision != 0 &&
        !provider_identity.empty() && !points.empty() &&
        CompleteSha256(canonical_sha256);
}

ProgressPlanV1::ProgressPlanV1()
{
    content_sha256 = ComputeProgressPlanHashV1(*this);
}

ProgressPlanV1::operator bool() const noexcept
{
    return version == 1 && CompleteSha256(content_sha256);
}

ProgressLibraryRegistry::ProgressLibraryRegistry()
    : libraries_{
          RuntimeViLibrary(),
          ScriptLocationLibrary(),
          SeedCallsLibrary(),
          BattleEventsLibrary(),
          PredicateLibrary()}
{
    std::ranges::sort(
        libraries_,
        {},
        &ProgressLibraryDescriptor::canonical_id);
}

const ProgressLibraryDescriptor* ProgressLibraryRegistry::FindLibrary(
    std::string_view canonical_id,
    std::uint32_t revision) const noexcept
{
    const auto found = std::ranges::find_if(
        libraries_,
        [&](const ProgressLibraryDescriptor& library) {
            return library.canonical_id == canonical_id &&
                library.revision == revision;
        });
    return found == libraries_.end() ? nullptr : &*found;
}

const ProgressPointDescriptor* ProgressLibraryRegistry::FindPoint(
    std::string_view library_id,
    std::uint32_t library_revision,
    std::string_view point_id) const noexcept
{
    const ProgressLibraryDescriptor* library =
        FindLibrary(library_id, library_revision);
    if (library == nullptr)
        return nullptr;
    const auto found = std::ranges::find_if(
        library->points,
        [&](const ProgressPointDescriptor& point) {
            return point.canonical_id == point_id;
        });
    return found == library->points.end() ? nullptr : &*found;
}

const ProgressPointDescriptor* ProgressLibraryRegistry::FindFormatter(
    const ProgressFormatterIdentity& formatter) const noexcept
{
    for (const ProgressLibraryDescriptor& library : libraries_)
    {
        const auto found = std::ranges::find_if(
            library.points,
            [&](const ProgressPointDescriptor& point) {
                return point.formatter == formatter;
            });
        if (found != library.points.end())
            return &*found;
    }
    return nullptr;
}

std::string ProgressLibraryRegistry::canonical_sha256() const
{
    std::string canonical;
    AppendField(canonical, "savor.progress.registry/v1");
    for (const ProgressLibraryDescriptor& library : libraries_)
        AppendField(canonical, library.canonical_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

const ProgressLibraryRegistry& ProductionProgressRegistry()
{
    static const ProgressLibraryRegistry registry;
    return registry;
}

std::string ComputeProgressPlanHashV1(const ProgressPlanV1& plan)
{
    std::string canonical;
    AppendField(canonical, "savor.progress.plan/v1");
    AppendNumber(canonical, plan.version);
    AppendNumber(canonical, plan.points.size());
    for (const ProgressPointBindingV1& point : plan.points)
    {
        AppendField(canonical, point.library_id);
        AppendNumber(canonical, point.library_revision);
        AppendField(canonical, point.library_sha256);
        AppendField(canonical, point.point_id);
        AppendNumber(canonical, static_cast<std::uint8_t>(point.provider));
        AppendNumber(canonical, point.breakpoint_pc.has_value() ? 1 : 0);
        AppendNumber(canonical, point.breakpoint_pc.value_or(0));
        AppendField(canonical, point.schema.canonical_id);
        AppendNumber(canonical, point.schema.revision);
        AppendField(canonical, point.schema.sha256);
        AppendField(canonical, point.formatter.canonical_id);
        AppendNumber(canonical, point.formatter.revision);
        AppendField(canonical, point.formatter.sha256);
        AppendNumber(
            canonical,
            point.runtime_sample_trigger_pcs.size());
        for (const std::uint32_t pc :
             point.runtime_sample_trigger_pcs)
        {
            AppendNumber(canonical, pc);
        }
        AppendField(
            canonical,
            std::string_view(
                reinterpret_cast<const char*>(point.configuration.data()),
                point.configuration.size()));
    }
    return hash::sha256(canonical.data(), canonical.size());
}

ProgressValidationResult ValidateProgressPlanV1(
    const ProgressPlanV1& plan,
    const ProgressLibraryRegistry& registry)
{
    if (plan.version != 1 || !CompleteSha256(plan.content_sha256))
        return {false, "Progress plan identity is incomplete"};
    if (plan.points.size() > 256)
        return {false, "Progress plan exceeds 256 resolved points"};
    std::set<std::pair<std::string, std::string>> identities;
    for (const ProgressPointBindingV1& binding : plan.points)
    {
        const ProgressLibraryDescriptor* library =
            registry.FindLibrary(
                binding.library_id,
                binding.library_revision);
        const ProgressPointDescriptor* point =
            registry.FindPoint(
                binding.library_id,
                binding.library_revision,
                binding.point_id);
        if (library == nullptr || point == nullptr ||
            binding.library_sha256 != library->canonical_sha256 ||
            binding.provider != point->provider ||
            binding.breakpoint_pc != point->breakpoint_pc ||
            binding.schema != point->schema ||
            binding.formatter != point->formatter)
        {
            return {false, "Progress plan does not exactly link to the static registry"};
        }
        const bool runtime_sample = binding.provider ==
            ProgressProviderKind::RuntimeSample;
        if (runtime_sample !=
                !binding.runtime_sample_trigger_pcs.empty() ||
            binding.runtime_sample_trigger_pcs.size() > 128 ||
            std::ranges::any_of(
                binding.runtime_sample_trigger_pcs,
                [](std::uint32_t pc) { return pc == 0; }) ||
            !std::ranges::is_sorted(
                binding.runtime_sample_trigger_pcs) ||
            std::ranges::adjacent_find(
                binding.runtime_sample_trigger_pcs) !=
                binding.runtime_sample_trigger_pcs.end())
        {
            return {
                false,
                "Progress runtime sample point set is malformed"};
        }
        if (!identities.emplace(
                binding.library_id,
                binding.point_id).second)
        {
            return {false, "Progress plan contains a duplicate point"};
        }
    }
    if (ComputeProgressPlanHashV1(plan) != plan.content_sha256)
        return {false, "Progress plan content hash does not match"};
    return {true, {}};
}

ProgressPlanV1 ResolveProgressPlanV1(
    std::span<const std::string_view> library_ids,
    std::span<const std::uint32_t> runtime_sample_trigger_pcs,
    const ProgressLibraryRegistry& registry)
{
    ProgressPlanV1 result;
    std::set<std::string> requested;
    for (std::string_view id : library_ids)
        requested.emplace(id);
    std::vector<std::uint32_t> runtime_triggers(
        runtime_sample_trigger_pcs.begin(),
        runtime_sample_trigger_pcs.end());
    std::ranges::sort(runtime_triggers);
    runtime_triggers.erase(
        std::unique(runtime_triggers.begin(), runtime_triggers.end()),
        runtime_triggers.end());
    for (const ProgressLibraryDescriptor& library : registry.libraries())
    {
        if (!requested.contains(library.canonical_id))
            continue;
        for (const ProgressPointDescriptor& point : library.points)
        {
            result.points.push_back({
                .library_id = library.canonical_id,
                .library_revision = library.revision,
                .library_sha256 = library.canonical_sha256,
                .point_id = point.canonical_id,
                .provider = point.provider,
                .breakpoint_pc = point.breakpoint_pc,
                .schema = point.schema,
                .formatter = point.formatter,
                .runtime_sample_trigger_pcs =
                    point.provider == ProgressProviderKind::RuntimeSample
                    ? runtime_triggers
                    : std::vector<std::uint32_t>{},
            });
        }
    }
    result.content_sha256 = ComputeProgressPlanHashV1(result);
    return result;
}

ProgressPlanResolutionV1 ResolveProgressPlanSelectionV1(
    std::span<const std::string> default_library_ids,
    std::span<const std::uint32_t> runtime_sample_trigger_pcs,
    const ProgressPlanSelectionV1& selection,
    const ProgressLibraryRegistry& registry)
{
    const auto fail = [](std::string message) {
        return ProgressPlanResolutionV1{
            .plan = std::nullopt,
            .message = std::move(message),
        };
    };
    std::set<std::string> defaults;
    for (const std::string& id : default_library_ids)
    {
        if (id.empty() || !defaults.emplace(id).second ||
            registry.FindLibrary(id, 1) == nullptr)
        {
            return fail(
                "Program-kind default progress libraries are invalid");
        }
    }
    std::set<std::string> requested = defaults;
    std::set<std::string> disabled_libraries;
    for (const std::string& id :
         selection.disabled_default_library_ids)
    {
        if (!defaults.contains(id) ||
            !disabled_libraries.emplace(id).second)
        {
            return fail(
                "Progress selection disables a duplicate or non-default library");
        }
        requested.erase(id);
    }
    for (const std::string& id : selection.added_library_ids)
    {
        if (id.empty() || registry.FindLibrary(id, 1) == nullptr)
        {
            return fail(
                "Progress selection adds an unavailable library");
        }
        requested.emplace(id);
    }
    std::set<std::pair<std::string, std::string>> disabled_points;
    for (const ProgressPointSelectionV1& disabled :
         selection.disabled_points)
    {
        if (!requested.contains(disabled.library_id) ||
            registry.FindPoint(
                disabled.library_id,
                1,
                disabled.point_id) == nullptr ||
            !disabled_points.emplace(
                disabled.library_id,
                disabled.point_id).second)
        {
            return fail(
                "Progress selection disables a duplicate or unavailable requested point");
        }
    }

    std::vector<std::string_view> libraries;
    libraries.reserve(requested.size());
    for (const std::string& id : requested)
        libraries.push_back(id);
    ProgressPlanV1 plan = ResolveProgressPlanV1(
        libraries,
        runtime_sample_trigger_pcs,
        registry);
    std::erase_if(
        plan.points,
        [&](const ProgressPointBindingV1& point) {
            return disabled_points.contains(
                {point.library_id, point.point_id});
        });
    plan.content_sha256 = ComputeProgressPlanHashV1(plan);
    const ProgressValidationResult validated =
        ValidateProgressPlanV1(plan, registry);
    if (!validated)
        return fail(validated.message);
    return {
        .plan = std::move(plan),
        .message = {},
    };
}

ProgressValidationResult ValidateCaptureProgressFormatters(
    const savor::probe::Profile& profile,
    const ProgressPlanV1* plan,
    const ProgressLibraryRegistry& registry)
{
    for (const savor::probe::ProbeDefinition& probe : profile.probes)
    {
        if (!savor::probe::has_subscription(
                probe.subscriptions,
                savor::probe::Subscription::Progress))
        {
            continue;
        }
        if (probe.progress_formatter.empty())
        {
            return {
                false,
                "Progress probe '" + probe.id +
                    "' requires a registered formatter"};
        }
        const ProgressPointDescriptor* matched = nullptr;
        for (const ProgressLibraryDescriptor& library : registry.libraries())
        {
            const auto found = std::ranges::find_if(
                library.points,
                [&](const ProgressPointDescriptor& point) {
                    return point.formatter.canonical_id ==
                        probe.progress_formatter;
                });
            if (found != library.points.end())
            {
                matched = &*found;
                break;
            }
        }
        if (matched == nullptr)
        {
            return {
                false,
                "Progress probe '" + probe.id +
                    "' references an unregistered formatter"};
        }
        if (plan != nullptr && !std::ranges::any_of(
                plan->points,
                [&](const ProgressPointBindingV1& point) {
                    return point.formatter == matched->formatter;
                }))
        {
            return {
                false,
                "Progress probe '" + probe.id +
                    "' is not requested by the resolved progress plan"};
        }
        std::set<std::string> sample_names;
        for (const savor::probe::SampleDefinition& sample : probe.samples)
            sample_names.insert(sample.name);
        for (const std::string& required : matched->required_capture_fields)
        {
            if (!sample_names.contains(required))
            {
                return {
                    false,
                    "Progress probe '" + probe.id +
                        "' is missing formatter field '" + required + "'"};
            }
        }
    }
    return {true, {}};
}

std::optional<savor::probe::ProbeDefinition>
BuildBreakpointProgressProbeV1(
    const ProgressPointBindingV1& binding,
    const ProgressLibraryRegistry& registry)
{
    const ProgressPointDescriptor* point = registry.FindPoint(
        binding.library_id,
        binding.library_revision,
        binding.point_id);
    if (point == nullptr ||
        binding.provider != ProgressProviderKind::BreakpointCapture ||
        point->provider != binding.provider ||
        !binding.breakpoint_pc ||
        point->breakpoint_pc != binding.breakpoint_pc ||
        point->formatter != binding.formatter)
    {
        return std::nullopt;
    }

    savor::probe::ProbeDefinition probe;
    probe.id = "canonical-progress::" + binding.library_id +
        "::" + binding.point_id;
    probe.group = "canonical-progress";
    probe.kind = savor::probe::ProbeKind::Pc;
    probe.subscriptions = savor::probe::Subscription::Progress;
    probe.address = *binding.breakpoint_pc;
    probe.progress_record = true;
    probe.progress_formatter = binding.formatter.canonical_id;

    if (binding.library_id == "soa.progress.soa.seed_calls/1")
    {
        probe.samples.push_back(GprSample("seed_argument", 3));
        probe.samples.push_back(MemorySample(
            "previous_rng",
            0x803469a8u,
            savor::probe::SampleWidth::U32));
        probe.samples.push_back(RoutedSample(
            "movie_input_count",
            1397096450u));
        probe.samples.push_back(StackTraceSample("caller_stack", 4));
        return probe;
    }

    if (binding.library_id != "soa.progress.battle.events/1")
        return probe;

    using savor::probe::SampleWidth;
    if (binding.point_id == "attack_damage")
    {
        probe.samples.push_back(GprSample("attacker_slot", 31));
        probe.samples.push_back(GprSample("target_slot", 30));
        probe.samples.push_back(GprSample("damage", 0));
        probe.samples.push_back(AddressProgramSample(
            "attacker_id",
            SampleWidth::U16,
            CombatantIdProgram(31)));
        probe.samples.push_back(AddressProgramSample(
            "target_id",
            SampleWidth::U16,
            CombatantIdProgram(30)));
    }
    else if (binding.point_id == "counterattack")
    {
        probe.samples.push_back(GprSample("target_slot", 30));
        probe.samples.push_back(AddressProgramSample(
            "target_id",
            SampleWidth::U16,
            CombatantIdProgram(30)));
    }
    else if (binding.point_id == "death")
    {
        probe.samples.push_back(GprSample("slot", 31));
        probe.samples.push_back(AddressProgramSample(
            "combatant_id",
            SampleWidth::U16,
            CombatantIdProgram(31)));
    }
    else if (binding.point_id == "item_drop")
    {
        probe.samples.push_back(GprSample("slot", 29));
        probe.samples.push_back(AddressProgramSample(
            "combatant_id",
            SampleWidth::U16,
            CombatantIdProgram(29)));

        auto count = AbsoluteAddressProgram(
            addr::AddrRegistry::base(addr::battle::MainInstancePtr));
        LoadProgramPointer(count);
        AddProgramOffset(
            count,
            static_cast<std::int32_t>(offsetof(soa::BattleState, item_drops)) -
                4 * static_cast<std::int32_t>(sizeof(soa::BattleItemDropSlot)) +
                static_cast<std::int32_t>(offsetof(soa::BattleItemDropSlot, count)));
        AddProgramGprScaled(count, 29, sizeof(soa::BattleItemDropSlot));
        FinishAddressProgram(count);
        probe.samples.push_back(AddressProgramSample(
            "drop_count",
            SampleWidth::U16,
            std::move(count)));

        auto item = AbsoluteAddressProgram(
            addr::AddrRegistry::base(addr::battle::MainInstancePtr));
        LoadProgramPointer(item);
        AddProgramOffset(
            item,
            static_cast<std::int32_t>(offsetof(soa::BattleState, item_drops)) -
                4 * static_cast<std::int32_t>(sizeof(soa::BattleItemDropSlot)) +
                static_cast<std::int32_t>(offsetof(soa::BattleItemDropSlot, item_id)));
        AddProgramGprScaled(item, 29, sizeof(soa::BattleItemDropSlot));
        FinishAddressProgram(item);
        probe.samples.push_back(AddressProgramSample(
            "item_id",
            SampleWidth::U16,
            std::move(item)));
    }
    else if (binding.point_id == "instruction_turn_state")
    {
        for (std::uint32_t slot = 0; slot < 12; ++slot)
        {
            const std::string prefix =
                "slot" + std::to_string(slot) + "_";
            probe.samples.push_back(MemorySample(
                prefix + "id",
                addr::AddrRegistry::base(addr::battle::CombatantIdTable) +
                    slot * 2,
                SampleWidth::U16));

            auto status = AbsoluteAddressProgram(
                addr::AddrRegistry::base(
                    addr::battle::CombatantInstancesTable) +
                slot * 4);
            LoadProgramPointer(status);
            AddProgramOffset(
                status,
                static_cast<std::int32_t>(
                    offsetof(soa::CombatantInstance, status_flags)));
            FinishAddressProgram(status);
            probe.samples.push_back(AddressProgramSample(
                prefix + "status",
                SampleWidth::U32,
                std::move(status)));

            const std::uint32_t instruction =
                addr::AddrRegistry::base(addr::battle::Instructions) +
                slot * static_cast<std::uint32_t>(
                    sizeof(soa::InstructionSet));
            probe.samples.push_back(MemorySample(
                prefix + "inst",
                instruction +
                    static_cast<std::uint32_t>(
                        offsetof(soa::InstructionSet, current)) +
                    static_cast<std::uint32_t>(
                        offsetof(soa::Instruction, inst)),
                SampleWidth::U32));
            probe.samples.push_back(MemorySample(
                prefix + "target",
                instruction +
                    static_cast<std::uint32_t>(
                        offsetof(soa::InstructionSet, current)) +
                    static_cast<std::uint32_t>(
                        offsetof(soa::Instruction, target)),
                SampleWidth::U8));
            probe.samples.push_back(MemorySample(
                prefix + "param",
                instruction +
                    static_cast<std::uint32_t>(
                        offsetof(soa::InstructionSet, current)) +
                    static_cast<std::uint32_t>(
                        offsetof(soa::Instruction, instParam)),
                SampleWidth::U16));
        }
    }
    else
    {
        return std::nullopt;
    }
    return probe;
}

std::string ComputeResolvedObservationHashV1(
    const savor::probe::Profile& profile,
    const ProgressPlanV1& plan)
{
    const std::string normalized =
        savor::probe::serialize_profile_json(profile);
    std::string canonical;
    AppendField(canonical, "savor.workset.resolved-observation/v1");
    AppendField(canonical, normalized);
    AppendField(canonical, plan.content_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

std::vector<std::uint8_t> EncodeCaptureEventPayloadV1(
    const savor::capture_format::Event& event)
{
    std::vector<std::uint8_t> output;
    output.reserve(64 + event.fields.size() * 32);
    output.insert(output.end(), {'C', 'P', 'P', '1'});
    WriteU64(output, event.capture_sequence);
    WriteU64(output, event.frame_index);
    WriteU64(output, event.guest_workset_epoch);
    WriteU32(output, event.pc);
    WriteU32(output, event.address);
    WriteU64(output, event.value);
    WriteString(output, event.probe_id);
    WriteU32(output, static_cast<std::uint32_t>(event.fields.size()));
    for (const savor::capture_format::Field& field : event.fields)
    {
        WriteString(output, field.name);
        output.push_back(static_cast<std::uint8_t>(field.type));
        output.push_back(static_cast<std::uint8_t>(field.status));
        WriteU64(output, field.value);
        WriteU32(output, static_cast<std::uint32_t>(field.bytes.size()));
        output.insert(output.end(), field.bytes.begin(), field.bytes.end());
    }
    return output;
}

std::string FormatCaptureProgressText(
    const ProgressPointDescriptor& point,
    const savor::capture_format::Event& event)
{
    if (point.canonical_id == "rng.srand")
    {
        const auto seed = FieldValue(event, "seed_argument");
        const auto previous = FieldValue(event, "previous_rng");
        const auto cursor = FieldValue(event, "movie_input_count");
        const auto stack = std::ranges::find_if(
            event.fields,
            [](const savor::capture_format::Field& field) {
                return field.name == "caller_stack" &&
                    field.status == savor::capture_format::FieldStatus::Present &&
                    field.stack_trace.has_value();
            });
        if (seed && previous && cursor)
        {
            std::ostringstream text;
            text << "srand seed=" << *seed
                 << " previous_rng=" << *previous
                 << " movie_cursor=" << *cursor
                 << " pc=0x" << std::hex << event.pc
                 << std::dec << " vi=" << event.frame_index;
            if (stack != event.fields.end() &&
                !stack->stack_trace->frames.empty())
            {
                const auto& frame = stack->stack_trace->frames.front();
                text << " caller=0x" << std::hex << frame.callsite_pc
                     << " return=0x" << frame.return_pc << std::dec;
            }
            return text.str();
        }
    }
    else if (point.canonical_id == "attack_damage")
    {
        const auto attacker_slot = FieldValue(event, "attacker_slot");
        const auto target_slot = FieldValue(event, "target_slot");
        const auto damage = FieldValue(event, "damage");
        const auto attacker_id = FieldValue(event, "attacker_id");
        const auto target_id = FieldValue(event, "target_id");
        if (attacker_slot && target_slot && damage &&
            attacker_id && target_id)
        {
            return std::format(
                "{} attacks {} for {} damage",
                CombatantName(
                    static_cast<std::uint32_t>(*attacker_slot),
                    static_cast<std::uint16_t>(*attacker_id)),
                CombatantName(
                    static_cast<std::uint32_t>(*target_slot),
                    static_cast<std::uint16_t>(*target_id)),
                *damage);
        }
    }
    else if (point.canonical_id == "counterattack")
    {
        const auto slot = FieldValue(event, "target_slot");
        const auto id = FieldValue(event, "target_id");
        if (slot && id)
        {
            return std::format(
                "{} counter attacks...",
                CombatantName(
                    static_cast<std::uint32_t>(*slot),
                    static_cast<std::uint16_t>(*id)));
        }
    }
    else if (point.canonical_id == "death")
    {
        const auto slot = FieldValue(event, "slot");
        const auto id = FieldValue(event, "combatant_id");
        if (slot && id)
        {
            return std::format(
                "{} died...",
                CombatantName(
                    static_cast<std::uint32_t>(*slot),
                    static_cast<std::uint16_t>(*id)));
        }
    }
    else if (point.canonical_id == "item_drop")
    {
        const auto slot = FieldValue(event, "slot");
        const auto combatant_id = FieldValue(event, "combatant_id");
        const auto count = FieldValue(event, "drop_count");
        const auto item_id = FieldValue(event, "item_id");
        if (slot && combatant_id && count && item_id)
        {
            const std::string name = CombatantName(
                static_cast<std::uint32_t>(*slot),
                static_cast<std::uint16_t>(*combatant_id));
            if (static_cast<std::uint16_t>(*item_id) == 0xffffu)
                return std::format("{} dropped nothing", name);
            return std::format(
                "{} dropped [{}]{} x{}",
                name,
                *item_id,
                soa::text::get_item_name(
                    static_cast<std::size_t>(*item_id)),
                *count);
        }
    }
    else if (point.canonical_id == "instruction_turn_state")
    {
        std::ostringstream output;
        bool first = true;
        for (std::uint32_t slot = 0; slot < 12; ++slot)
        {
            const std::string prefix =
                "slot" + std::to_string(slot) + "_";
            const auto id = FieldValue(event, prefix + "id");
            const auto status = FieldValue(event, prefix + "status");
            const auto instruction = FieldValue(event, prefix + "inst");
            const auto target = FieldValue(event, prefix + "target");
            const auto parameter = FieldValue(event, prefix + "param");
            if (!id || !status || !instruction || !target || !parameter ||
                (*status & (soa::battle::ctx::Fled |
                    soa::battle::ctx::Dead)) != 0)
            {
                continue;
            }
            const auto target_id = FieldValue(
                event,
                "slot" + std::to_string(*target) + "_id")
                    .value_or(0xffffu);
            if (!first)
                output << '\n';
            first = false;
            output << CombatantName(
                slot,
                static_cast<std::uint16_t>(*id))
                   << ": "
                   << InstructionDescription(
                          static_cast<std::uint32_t>(*instruction),
                          static_cast<std::uint32_t>(*parameter),
                          static_cast<std::uint32_t>(*target),
                          static_cast<std::uint16_t>(target_id));
        }
        if (!first)
            return output.str();
    }

    std::ostringstream text;
    text << point.display_name << " at 0x" << std::hex << event.pc
         << std::dec << " (VI/frame " << event.frame_index << ')';
    if (!event.fields.empty())
    {
        text << ": ";
        for (std::size_t index = 0; index < event.fields.size(); ++index)
        {
            if (index != 0)
                text << ", ";
            text << event.fields[index].name << '='
                 << event.fields[index].value;
        }
    }
    return text.str();
}

std::vector<std::uint8_t> EncodeViProgressPayloadV1(
    std::uint64_t vi_count)
{
    std::vector<std::uint8_t> output{'V', 'I', 'P', '1'};
    WriteU64(output, vi_count);
    return output;
}

std::vector<std::uint8_t> EncodeScriptLocationProgressPayloadV1(
    std::string_view file,
    std::string_view section,
    std::uint32_t pc)
{
    std::vector<std::uint8_t> output{'S', 'L', 'P', '1'};
    WriteString(output, file);
    WriteString(output, section);
    WriteU32(output, pc);
    return output;
}

} // namespace savor::runtime::progress
