#include "SourceCapabilityPacks.h"

#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/SoaConstants.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace savor::runtime::program::capabilities {
namespace {

using composition::ExactSchema;

constexpr std::string_view kFieldPackId = "soa.field";
constexpr std::string_view kBattlePackId = "soa.battle";
constexpr std::string_view kBattleCommandPackId = "soa.battle.command";
constexpr std::string_view kNavigationPackId = "soa.navigation";
constexpr std::uint32_t kFieldRngSeedSampleDescriptorId = 0x53460001u;
constexpr std::uint32_t kBattleCurrentTurnSampleDescriptorId = 0x53420001u;

CpuEvaluatorDescriptor FieldRngSeedEvaluator()
{
    return {
        .canonical_id = "soa.field.sample.RngSeed",
        .routed_sample_descriptor_id = kFieldRngSeedSampleDescriptorId,
        .address_dependency = "soa.field.address.RNG_SEED",
        .result_type = TypeRef::Builtin(BuiltinType::U32),
        .operations = {CpuEvaluatorOperation::ReadU32},
        .maximum_reads = 1,
        .maximum_output_bytes = 4,
    };
}

CpuEvaluatorDescriptor BattleCurrentTurnEvaluator()
{
    return {
        .canonical_id = "soa.battle.sample.CurrentTurn",
        .routed_sample_descriptor_id = kBattleCurrentTurnSampleDescriptorId,
        .address_dependency = "soa.battle.address.CurrentTurn",
        .result_type = TypeRef::Builtin(BuiltinType::U8),
        .operations = {CpuEvaluatorOperation::ReadU8},
        .maximum_reads = 1,
        .maximum_output_bytes = 1,
    };
}


SchemaIdentity Schema(
    std::string canonical_id,
    std::string_view contract)
{
    return ExactSchema(std::move(canonical_id), 1, contract);
}

TypeRef Named(const SchemaIdentity& schema)
{
    return TypeRef::Named(schema);
}

TypeSchemaDefinition BytesSchema(
    std::string id,
    std::uint64_t maximum_size,
    std::string_view semantic_contract)
{
    return {
        .identity = Schema(std::move(id), semantic_contract),
        .kind = TypeSchemaKind::BoundedBytes,
        .maximum_size = maximum_size,
    };
}

TypeSchemaDefinition StringSchema(
    std::string id,
    std::uint64_t maximum_size,
    std::string_view semantic_contract)
{
    return {
        .identity = Schema(std::move(id), semantic_contract),
        .kind = TypeSchemaKind::BoundedUtf8String,
        .maximum_size = maximum_size,
    };
}

TypeSchemaDefinition EnumSchema(
    std::string id,
    std::vector<EnumMemberDefinition> members,
    std::string_view semantic_contract)
{
    return {
        .identity = Schema(std::move(id), semantic_contract),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = std::move(members),
    };
}

std::vector<EnumMemberDefinition> TurnTypeEnumMembers()
{
    std::vector<EnumMemberDefinition> members;
    members.reserve(soa::battle::TurnTypeDefinitions.size());
    for (const auto& definition : soa::battle::TurnTypeDefinitions)
    {
        members.push_back({
            std::string(definition.name),
            static_cast<std::int64_t>(definition.type)});
    }
    return members;
}

std::string TurnTypeEnumContract()
{
    std::string contract = "enum soa.battle.TurnType/1{";
    for (std::size_t index = 0;
         index < soa::battle::TurnTypeDefinitions.size(); ++index)
    {
        if (index != 0) contract += ',';
        const auto& definition = soa::battle::TurnTypeDefinitions[index];
        contract += std::format(
            "{}={}",
            definition.name,
            static_cast<std::int64_t>(definition.type));
    }
    contract += '}';
    return contract;
}

std::vector<EnumMemberDefinition> BattleActionEnumMembers()
{
    std::vector<EnumMemberDefinition> members;
    members.reserve(
        soa::battle::actions::BattleActionDefinitions.size());
    for (const auto& definition :
         soa::battle::actions::BattleActionDefinitions)
    {
        members.push_back({
            std::string(definition.name),
            static_cast<std::int64_t>(definition.action)});
    }
    return members;
}

std::string BattleActionEnumContract()
{
    std::string contract = "enum soa.battle.BattleAction/1{";
    for (std::size_t index = 0;
         index < soa::battle::actions::BattleActionDefinitions.size(); ++index)
    {
        if (index != 0) contract += ',';
        const auto& definition =
            soa::battle::actions::BattleActionDefinitions[index];
        contract += std::format(
            "{}={}",
            definition.name,
            static_cast<std::int64_t>(definition.action));
    }
    contract += '}';
    return contract;
}

TypeSchemaDefinition RecordSchema(
    std::string id,
    std::vector<RecordFieldDefinition> fields,
    std::string_view semantic_contract)
{
    return {
        .identity = Schema(std::move(id), semantic_contract),
        .kind = TypeSchemaKind::Record,
        .record_fields = std::move(fields),
    };
}

TypeSchemaDefinition ListSchema(
    std::string id,
    TypeRef element,
    std::uint64_t maximum_size,
    std::string_view semantic_contract)
{
    return {
        .identity = Schema(std::move(id), semantic_contract),
        .kind = TypeSchemaKind::BoundedList,
        .maximum_size = maximum_size,
        .element_type = std::move(element),
    };
}

std::vector<TypeSchemaDefinition> BuildSchemas()
{
    std::vector<TypeSchemaDefinition> schemas;
    schemas.reserve(18);

    schemas.push_back(StringSchema(
        "runtime.DiagnosticText",
        4096,
        "utf8(max=4096)"));
    const auto diagnostic = schemas.back().identity;

    schemas.push_back(BytesSchema(
        "soa.battle.CombatantInstanceSnapshot",
        2048,
        "bytes(max=2048;source=soa::CombatantInstance)"));
    const auto combatant = schemas.back().identity;
    schemas.push_back(BytesSchema(
        "soa.battle.EnemyDefinitionSnapshot",
        2048,
        "bytes(max=2048;source=soa::EnemyDefinition)"));
    const auto enemy = schemas.back().identity;
    schemas.push_back(BytesSchema(
        "soa.battle.BattleStateSnapshot",
        0x178,
        "bytes(max=376;source=soa::BattleState)"));
    const auto battle_state = schemas.back().identity;

    schemas.push_back(EnumSchema(
        "soa.battle.TurnType",
        TurnTypeEnumMembers(),
        TurnTypeEnumContract()));
    const auto turn_type = schemas.back().identity;

    schemas.push_back(RecordSchema(
        "soa.battle.BattleSlot",
        {
            {"present", TypeRef::Builtin(BuiltinType::Bool)},
            {"is_player", TypeRef::Builtin(BuiltinType::Bool)},
            {"id", TypeRef::Builtin(BuiltinType::U16)},
            {"is_alive", TypeRef::Builtin(BuiltinType::Bool)},
            {"has_enemy_definition", TypeRef::Builtin(BuiltinType::Bool)},
            {"instance", Named(combatant)},
            {"enemy_definition", Named(enemy)},
            {"instance_address", TypeRef::Builtin(BuiltinType::U32)},
            {"enemy_definition_address", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record BattleSlot/1"));
    const auto battle_slot = schemas.back().identity;
    schemas.push_back(ListSchema(
        "soa.battle.BattleSlotList",
        Named(battle_slot),
        12,
        "list<BattleSlot>(max=12)"));
    const auto battle_slots = schemas.back().identity;

    schemas.push_back(RecordSchema(
        "soa.battle.BattleContext",
        {
            {"slots", Named(battle_slots)},
            {"state", Named(battle_state)},
            {"turn_type", Named(turn_type)},
            {"turn_count", TypeRef::Builtin(BuiltinType::U32)},
            {"battle_phase", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record BattleContext/1(materialized slots,state,turn_type,turn_count,battle_phase)"));
    const auto battle_context = schemas.back().identity;

    schemas.push_back(RecordSchema(
        "soa.battle.CaptureContextRequest",
        {
            {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"expected_pc", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record CaptureContextRequest/1(workset_epoch,expected_pc)"));

    schemas.push_back(EnumSchema(
        "soa.battle.BattleAction",
        BattleActionEnumMembers(),
        BattleActionEnumContract()));
    const auto battle_action = schemas.back().identity;
    schemas.push_back(RecordSchema(
        "soa.battle.BattleCommand",
        {
            {"actor_slot", TypeRef::Builtin(BuiltinType::U8)},
            {"action", Named(battle_action)},
            {"target_slot", TypeRef::Builtin(BuiltinType::U8)},
            {"item_id", TypeRef::Builtin(BuiltinType::U16)},
        },
        "record BattleCommand/1"));
    const auto battle_command = schemas.back().identity;
    schemas.push_back(ListSchema(
        "soa.battle.BattleCommandList",
        Named(battle_command),
        4,
        "list<BattleCommand>(max=4)"));
    const auto battle_commands = schemas.back().identity;
    schemas.push_back(RecordSchema(
        "soa.battle.BattleTurnExecutionSpec",
        {
            {"fake_attack_count", TypeRef::Builtin(BuiltinType::U32)},
            {"commands", Named(battle_commands)},
        },
        "record BattleTurnExecutionSpec/1"));
    const auto turn_spec = schemas.back().identity;

    std::vector<EnumMemberDefinition> command_segment_members;
    command_segment_members.reserve(kBattleCommandSegments.size());
    std::string command_segment_contract = "enum BattleCommandSegment/1{";
    for (const auto& segment : kBattleCommandSegments)
    {
        if (command_segment_contract.back() != '{')
            command_segment_contract.push_back(',');
        command_segment_contract.append(segment.name);
        command_segment_contract.push_back('=');
        command_segment_contract.append(std::to_string(
            BattleCommandSegmentValue(segment.segment)));
        command_segment_members.push_back({
            std::string(segment.name),
            BattleCommandSegmentValue(segment.segment),
        });
    }
    command_segment_contract.push_back('}');
    schemas.push_back(EnumSchema(
        "soa.battle.command.Segment",
        std::move(command_segment_members),
        command_segment_contract));
    const auto command_segment = schemas.back().identity;
    schemas.push_back(RecordSchema(
        "soa.battle.command.State",
        {
            {"context", Named(battle_context)},
            {"plan", Named(turn_spec)},
            {"segment", Named(command_segment)},
            {"fake_remaining", TypeRef::Builtin(BuiltinType::U32)},
            {"command_index", TypeRef::Builtin(BuiltinType::U32)},
            {"moves_remaining", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record BattleCommandInteractionState/1(context,plan,segment,fake_remaining,command_index,moves_remaining)"));
    const auto command_state = schemas.back().identity;
    schemas.push_back(RecordSchema(
        "soa.battle.command.Preparation",
        {
            {"success", TypeRef::Builtin(BuiltinType::Bool)},
            {"state", Named(command_state)},
            {"diagnostic", Named(diagnostic)},
        },
        "record BattleCommandInteractionPreparation/1(success,state,diagnostic)"));
    schemas.push_back(RecordSchema(
        "soa.battle.command.Transition",
        {
            {"state", Named(command_state)},
            {"segment", Named(command_segment)},
        },
        "record BattleCommandInteractionTransition/1(state,segment)"));

    schemas.push_back(RecordSchema(
        "soa.navigation.CaptureContextRequest",
        {
            {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"expected_pc", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record NavigationCaptureContextRequest/1(workset_epoch,expected_pc)"));

    schemas.push_back(RecordSchema(
        "soa.navigation.NavigationContext",
        {
            {"capture_pc", TypeRef::Builtin(BuiltinType::U32)},
            {"player_worksheet", TypeRef::Builtin(BuiltinType::U32)},
            {"area", TypeRef::Builtin(BuiltinType::U32)},
            {"subarea", TypeRef::Builtin(BuiltinType::U8)},
            {"motion_state", TypeRef::Builtin(BuiltinType::U16)},
            {"motion_substate", TypeRef::Builtin(BuiltinType::U16)},
            {"post_input_movement_suppress", TypeRef::Builtin(BuiltinType::U32)},
            {"position_x", TypeRef::Builtin(BuiltinType::F32)},
            {"position_y", TypeRef::Builtin(BuiltinType::F32)},
            {"position_z", TypeRef::Builtin(BuiltinType::F32)},
            {"rotation_x_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"rotation_y_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"rotation_z_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"previous_position_x", TypeRef::Builtin(BuiltinType::F32)},
            {"previous_position_y", TypeRef::Builtin(BuiltinType::F32)},
            {"previous_position_z", TypeRef::Builtin(BuiltinType::F32)},
            {"previous_rotation_x_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"previous_rotation_y_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"previous_rotation_z_raw", TypeRef::Builtin(BuiltinType::U32)},
            {"step_distance_carry_in", TypeRef::Builtin(BuiltinType::F32)},
            {"has_ground", TypeRef::Builtin(BuiltinType::Bool)},
            {"ground_tbl_id", TypeRef::Builtin(BuiltinType::U16)},
        },
        "record NavigationContext/1"));

    return schemas;
}

const TypeSchemaDefinition& RequireSchema(
    const std::vector<TypeSchemaDefinition>& schemas,
    std::string_view id)
{
    return *std::ranges::find(
        schemas,
        id,
        [](const TypeSchemaDefinition& value)
        {
            return std::string_view(value.identity.canonical_id);
        });
}

std::string LogicalPointId(
    std::string_view pack,
    const BPAddr& point)
{
    return std::format("{}.point.{}", pack, point.name);
}

std::vector<SemanticPointDescriptor> BuildFieldPoints()
{
    std::vector<SemanticPointDescriptor> points;
    for (const auto& point : bp::BpRegistry::AllRuntime())
    {
        const bool prebattle =
            bp::domain_of(point.key) == bp::BPDomain::PreBattle;
        const bool field_return =
            point.key == bp::battle::FieldReturnRandSeedCommitted;
        if ((!prebattle && !field_return) || point.pc == 0)
            continue;
        points.push_back({
            .canonical_id = prebattle
                ? std::format("soa.field.point.prebattle.{}", point.name)
                : "soa.field.point.field_return.RandSeedCommitted",
            .kind = SemanticPointKind::ProgramCounter,
            .pc = point.pc,
        });
    }
    return points;
}

std::vector<SemanticPointDescriptor> BuildBattlePoints()
{
    std::vector<SemanticPointDescriptor> points;
    for (const auto& point :
         bp::BpRegistry::ForConsumer(BreakpointConsumer::Predicate))
    {
        if (bp::domain_of(point.key) != bp::BPDomain::Battle ||
            point.pc == 0 ||
            point.key == bp::battle::StartAction)
        {
            continue;
        }
        points.push_back({
            .canonical_id = LogicalPointId(kBattlePackId, point),
            .kind = SemanticPointKind::ProgramCounter,
            .pc = point.pc,
        });
    }
    return points;
}

std::vector<SemanticPointDescriptor> BuildNavigationPoints()
{
    std::vector<SemanticPointDescriptor> points;
    for (const auto& point : bp::BpRegistry::AllRuntime())
    {
        if (bp::domain_of(point.key) != bp::BPDomain::Navigation ||
            point.pc == 0)
        {
            continue;
        }
        points.push_back({
            .canonical_id = LogicalPointId(kNavigationPackId, point),
            .kind = SemanticPointKind::ProgramCounter,
            .pc = point.pc,
        });
    }
    return points;
}

TypeRef AddressValueType(std::string_view name)
{
    if (name == "core.SCT_FILE_LTTR" ||
        name == "battle.CurrentTurn" ||
        name == "battle.TurnOrderTable")
    {
        return TypeRef::Builtin(BuiltinType::U8);
    }
    return TypeRef::Builtin(BuiltinType::U32);
}

std::vector<AddressSymbolDescriptor> BuildFieldAddresses()
{
    std::vector<AddressSymbolDescriptor> addresses;
    for (const auto& address : addr::AddrRegistry::all())
    {
        const std::string_view name(address.name);
        if (!name.starts_with("core.") || address.spec.base == 0)
            continue;
        addresses.push_back({
            .canonical_id = "soa.field.address." +
                std::string(name.substr(5)),
            .address = address.spec.base,
            .value_type = AddressValueType(name),
        });
    }
    return addresses;
}

std::vector<AddressSymbolDescriptor> BuildBattleAddresses()
{
    std::vector<AddressSymbolDescriptor> addresses;
    for (const auto& address : addr::AddrRegistry::all())
    {
        const std::string_view name(address.name);
        // DerivedBattleBuffer entries are offsets into a host-owned buffer,
        // not compatibility-pinned guest addresses.
        if (!name.starts_with("battle.") || address.spec.base == 0)
            continue;
        addresses.push_back({
            .canonical_id = "soa.battle.address." +
                std::string(name.substr(7)),
            .address = address.spec.base,
            .value_type = AddressValueType(name),
        });
    }
    return addresses;
}

std::vector<AddressSymbolDescriptor> BuildNavigationAddresses()
{
    using namespace soa::navigation::ctx;
    return {
        {
            .canonical_id = "soa.navigation.address.PlayerWorksheetPointer",
            .address = PlayerWorksheetPointerAddress,
            .value_type = TypeRef::Builtin(BuiltinType::U32),
        },
        {
            .canonical_id = "soa.navigation.address.Area",
            .address = AreaAddress,
            .value_type = TypeRef::Builtin(BuiltinType::U32),
        },
        {
            .canonical_id = "soa.navigation.address.Subarea",
            .address = SubareaAddress,
            .value_type = TypeRef::Builtin(BuiltinType::U8),
        },
        {
            .canonical_id =
                "soa.navigation.address.PostInputMovementSuppress",
            .address = PostInputMovementSuppressAddress,
            .value_type = TypeRef::Builtin(BuiltinType::U32),
        },
        {
            .canonical_id = "soa.navigation.address.StepDistanceCarryIn",
            .address = StepDistanceCarryInAddress,
            .value_type = TypeRef::Builtin(BuiltinType::F32),
        },
    };
}

std::vector<std::string> AddressIds(
    const std::vector<AddressSymbolDescriptor>& addresses)
{
    std::vector<std::string> ids;
    ids.reserve(addresses.size());
    for (const auto& address : addresses)
        ids.push_back(address.canonical_id);
    return ids;
}

CapabilityPackIdentity UnsignedPackIdentity(
    std::string canonical_id)
{
    return {
        .canonical_id = std::move(canonical_id),
        .version = 1,
    };
}

ActionDescriptor QueryAction(
    ExactDependencyIdentity identity,
    CapabilityPackIdentity pack,
    TypeRef input,
    TypeRef output)
{
    return {
        .identity = std::move(identity),
        .providing_pack = std::move(pack),
        .input_type = std::move(input),
        .output_type = output,
        .domain_observation_type = output,
        .required_services = ServiceMask(
            SessionServiceCapability::GuestMemory),
        .effects = EffectMask(ActionEffect::ReadGuest),
        .epoch_policy = ActionEpochPolicy::RequiresCurrentEpoch,
        .replay_class = ActionReplayClass::RecordedEvidence,
        .cancellation = ActionCancellationMode::BeforeMutationOnly,
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
        .resource_behavior = ActionResourceBehavior::None,
        .cleanup = ActionCleanupGuarantee::None,
        .idempotency = ActionIdempotency::NaturallyIdempotent,
        .diagnostic_categories = {
            "unavailable",
            "stale_epoch",
            "invalid_guest_state",
        },
    };
}

ExactDependencyIdentity QueryActionIdentity(
    std::string canonical_id,
    std::string pack_id,
    TypeRef input,
    TypeRef output)
{
    ActionDescriptor descriptor = QueryAction(
        {
            .canonical_id = std::move(canonical_id),
            .version = 1,
        },
        UnsignedPackIdentity(std::move(pack_id)),
        std::move(input),
        std::move(output));
    descriptor.identity.signature_hash =
        ComputeActionDescriptorContractHash(descriptor);
    return descriptor.identity;
}

} // namespace

std::vector<SemanticPointDescriptor> BuildBattleCommandPoints();

RuntimeCompatibility SupportedSoaUsaCompatibility()
{
    return CanonicalRuntimeCompatibility();
}

CapabilityPackIdentity FieldPackIdentity()
{
    CapabilityPackManifest manifest{
        .identity = UnsignedPackIdentity(
            std::string(kFieldPackId)),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .semantic_points = BuildFieldPoints(),
        .address_symbols = BuildFieldAddresses(),
        .cpu_evaluators = {FieldRngSeedEvaluator()},
    };
    manifest.identity.manifest_hash =
        ComputeCapabilityPackManifestContractHash(manifest);
    return manifest.identity;
}

CapabilityPackIdentity BattlePackIdentity()
{
    const auto points = BuildBattlePoints();
    const auto addresses = BuildBattleAddresses();
    auto schemas = BuildSchemas();
    std::erase_if(
        schemas,
        [](const TypeSchemaDefinition& schema)
        {
            return (!schema.identity.canonical_id.starts_with("soa.battle.") ||
                    schema.identity.canonical_id.starts_with("soa.battle.command.")) &&
                !schema.identity.canonical_id.starts_with("runtime.input.") &&
                schema.identity.canonical_id != "runtime.DiagnosticText";
        });
    std::vector<SchemaIdentity> schema_identities;
    schema_identities.reserve(schemas.size());
    for (const TypeSchemaDefinition& schema : schemas)
        schema_identities.push_back(schema.identity);
    const auto& battle_request = RequireSchema(
        schemas,
        "soa.battle.CaptureContextRequest");
    const auto& battle_context = RequireSchema(
        schemas,
        "soa.battle.BattleContext");

    CapabilityPackManifest manifest{
        .identity = UnsignedPackIdentity(
            std::string(kBattlePackId)),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .schemas = std::move(schema_identities),
        .semantic_points = std::move(points),
        .address_symbols = addresses,
        .coherent_queries = {{
            .canonical_id = "soa.battle.query.BattleContext",
            .result_schema = battle_context.identity,
            .address_dependencies = AddressIds(addresses),
            .maximum_guest_reads = 64,
        }},
        .cpu_evaluators = {BattleCurrentTurnEvaluator()},
        .actions = {QueryActionIdentity(
            "soa.battle.capture_context",
            std::string(kBattlePackId),
            Named(battle_request.identity),
            Named(battle_context.identity))},
    };
    manifest.identity.manifest_hash =
        ComputeCapabilityPackManifestContractHash(manifest);
    return manifest.identity;
}

CapabilityPackIdentity NavigationPackIdentity()
{
    const auto points = BuildNavigationPoints();
    const auto addresses = BuildNavigationAddresses();
    auto schemas = BuildSchemas();
    std::erase_if(
        schemas,
        [](const TypeSchemaDefinition& schema)
        {
            return !schema.identity.canonical_id.starts_with(
                "soa.navigation.");
        });
    std::vector<SchemaIdentity> schema_identities;
    schema_identities.reserve(schemas.size());
    for (const TypeSchemaDefinition& schema : schemas)
        schema_identities.push_back(schema.identity);
    const auto& navigation_request = RequireSchema(
        schemas,
        "soa.navigation.CaptureContextRequest");
    const auto& navigation_context = RequireSchema(
        schemas,
        "soa.navigation.NavigationContext");

    CapabilityPackManifest manifest{
        .identity = UnsignedPackIdentity(
            std::string(kNavigationPackId)),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .schemas = std::move(schema_identities),
        .semantic_points = std::move(points),
        .address_symbols = addresses,
        .coherent_queries = {{
            .canonical_id =
                "soa.navigation.query.NavigationContext",
            .result_schema = navigation_context.identity,
            .address_dependencies = AddressIds(addresses),
            .maximum_guest_reads = 21,
        }},
        .actions = {QueryActionIdentity(
            "soa.navigation.capture_context",
            std::string(kNavigationPackId),
            Named(navigation_request.identity),
            Named(navigation_context.identity))},
    };
    manifest.identity.manifest_hash =
        ComputeCapabilityPackManifestContractHash(manifest);
    return manifest.identity;
}

std::vector<SemanticPointDescriptor> BuildBattleCommandPoints()
{
    std::vector<SemanticPointDescriptor> points;
    for (const auto& point :
         bp::BpRegistry::ForConsumer(BreakpointConsumer::InteractionControl))
    {
        if (bp::domain_of(point.key) != bp::BPDomain::Battle ||
            point.pc == 0 ||
            point.visibility != BreakpointVisibility::Internal ||
            point.owner != BreakpointOwner::Interaction)
        {
            continue;
        }
        points.push_back({
            .canonical_id = LogicalPointId(kBattleCommandPackId, point),
            .kind = SemanticPointKind::ProgramCounter,
            .pc = point.pc,
        });
    }
    return points;
}

SchemaIdentity BattleContextSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(
        schemas,
        "soa.battle.BattleContext").identity;
}

SchemaIdentity BattleCaptureContextRequestSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(
        schemas,
        "soa.battle.CaptureContextRequest").identity;
}

SchemaIdentity BattleTurnExecutionSpecSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(schemas, "soa.battle.BattleTurnExecutionSpec").identity;
}

SchemaIdentity BattleCommandStateSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(schemas, "soa.battle.command.State").identity;
}

SchemaIdentity BattleCommandPreparationSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(schemas, "soa.battle.command.Preparation").identity;
}

SchemaIdentity BattleCommandTransitionSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(schemas, "soa.battle.command.Transition").identity;
}

SchemaIdentity BattleCommandSegmentSchemaIdentity()
{
    const auto schemas = BuildSchemas();
    return RequireSchema(schemas, "soa.battle.command.Segment").identity;
}

ExactDependencyIdentity BattleCaptureContextActionIdentity()
{
    const auto schemas = BuildSchemas();
    return QueryActionIdentity(
        "soa.battle.capture_context",
        std::string(kBattlePackId),
        Named(RequireSchema(
            schemas,
            "soa.battle.CaptureContextRequest").identity),
        Named(RequireSchema(
            schemas,
            "soa.battle.BattleContext").identity));
}

ExactDependencyIdentity NavigationCaptureContextActionIdentity()
{
    const auto schemas = BuildSchemas();
    return QueryActionIdentity(
        "soa.navigation.capture_context",
        std::string(kNavigationPackId),
        Named(RequireSchema(
            schemas,
            "soa.navigation.CaptureContextRequest").identity),
        Named(RequireSchema(
            schemas,
            "soa.navigation.NavigationContext").identity));
}

namespace {

struct BattleCommandReducerEntry
{
    CanonicalReducer reducer;
    ReducerDescriptor descriptor;
};

struct BattleCommandCapabilityCatalog
{
    std::vector<BattleCommandReducerEntry> reducers;
    CapabilityPackManifest manifest;
};

BattleCommandCapabilityCatalog BuildBattleCommandCapabilityCatalog()
{
    const auto schemas = BuildSchemas();
    const TypeRef battle_context = Named(RequireSchema(
        schemas, "soa.battle.BattleContext").identity);
    const TypeRef turn_spec = Named(RequireSchema(
        schemas, "soa.battle.BattleTurnExecutionSpec").identity);
    const TypeRef command_state = Named(RequireSchema(
        schemas, "soa.battle.command.State").identity);
    const TypeRef command_preparation = Named(RequireSchema(
        schemas, "soa.battle.command.Preparation").identity);
    const TypeRef command_transition = Named(RequireSchema(
        schemas, "soa.battle.command.Transition").identity);
    const TypeRef input_frame = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
    const TypeRef continue_result = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    const CapabilityPackIdentity unsigned_pack = UnsignedPackIdentity(
        std::string(kBattleCommandPackId));

    BattleCommandCapabilityCatalog catalog;
    catalog.reducers = {
        {
            .reducer = CanonicalReducer::BattlePrepareCommandInteraction,
            .descriptor = {
                .identity = {
                    .canonical_id =
                        "soa.battle.command_interaction.prepare",
                    .version = 1,
                },
                .providing_pack = unsigned_pack,
                .input_types = {battle_context, turn_spec},
                .output_type = command_preparation,
                .maximum_steps = 100000,
                .maximum_value_bytes = 4 * 1024 * 1024,
            },
        },
        {
            .reducer = CanonicalReducer::BattleCommandInteractionInitialize,
            .descriptor = {
                .identity = {
                    .canonical_id =
                        "soa.battle.command_interaction.initialize",
                    .version = 1,
                },
                .providing_pack = unsigned_pack,
                .input_types = {
                    command_state,
                    input_frame,
                    input_frame,
                    input_frame,
                    input_frame,
                },
                .output_type = command_state,
                .maximum_steps = 1,
                .maximum_value_bytes = 4 * 1024 * 1024,
            },
        },
        {
            .reducer = CanonicalReducer::BattleCommandInteractionAdvance,
            .descriptor = {
                .identity = {
                    .canonical_id =
                        "soa.battle.command_interaction.advance",
                    .version = 1,
                },
                .providing_pack = unsigned_pack,
                .input_types = {command_state, continue_result},
                .output_type = command_transition,
                .maximum_steps = 1000,
                .maximum_value_bytes = 4 * 1024 * 1024,
            },
        },
        {
            .reducer = CanonicalReducer::BattleCommandInteractionCompleteSegment,
            .descriptor = {
                .identity = {
                    .canonical_id =
                        "soa.battle.command_interaction.complete_segment",
                    .version = 1,
                },
                .providing_pack = unsigned_pack,
                .input_types = {command_state, continue_result},
                .output_type = continue_result,
                .maximum_steps = 1,
                .maximum_value_bytes = 64 * 1024,
            },
        },
        {
            .reducer = CanonicalReducer::BattleCommandInteractionFinalize,
            .descriptor = {
                .identity = {
                    .canonical_id =
                        "soa.battle.command_interaction.finalize",
                    .version = 1,
                },
                .providing_pack = unsigned_pack,
                .input_types = {command_state, continue_result},
                .output_type = continue_result,
                .maximum_steps = 1,
                .maximum_value_bytes = 16,
            },
        },
    };

    constexpr auto expected_reducer_count = static_cast<std::size_t>(
        CanonicalReducer::BattleCommandInteractionFinalize) + 1u;
    std::array<bool, expected_reducer_count> seen_reducers{};
    if (catalog.reducers.size() != expected_reducer_count)
        throw std::logic_error("Battle command reducer catalog is incomplete");
    for (const BattleCommandReducerEntry& entry : catalog.reducers)
    {
        const auto key = static_cast<std::size_t>(entry.reducer);
        if (key >= expected_reducer_count || seen_reducers[key] ||
            entry.descriptor.identity.canonical_id.empty())
        {
            throw std::logic_error(
                "Battle command reducer catalog has duplicate or invalid keys");
        }
        seen_reducers[key] = true;
    }
    std::ranges::sort(
        catalog.reducers,
        {},
        [](const BattleCommandReducerEntry& entry)
        {
            return std::pair{
                entry.descriptor.identity.canonical_id,
                entry.descriptor.identity.version};
        });

    for (BattleCommandReducerEntry& entry : catalog.reducers)
    {
        entry.descriptor.identity.signature_hash =
            ComputeReducerDescriptorContractHash(entry.descriptor);
    }

    std::vector<SchemaIdentity> schema_identities;
    for (const TypeSchemaDefinition& schema : schemas)
    {
        if (schema.identity.canonical_id.starts_with(
                "soa.battle.command."))
        {
            schema_identities.push_back(schema.identity);
        }
    }

    catalog.manifest = {
        .identity = unsigned_pack,
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {
            CanonicalRuntimePackIdentity(),
            BattlePackIdentity(),
        },
        .schemas = std::move(schema_identities),
        .semantic_points = BuildBattleCommandPoints(),
    };
    catalog.manifest.reducers.reserve(catalog.reducers.size());
    for (const BattleCommandReducerEntry& entry : catalog.reducers)
        catalog.manifest.reducers.push_back(entry.descriptor.identity);
    catalog.manifest.identity.manifest_hash =
        ComputeCapabilityPackManifestContractHash(catalog.manifest);

    for (BattleCommandReducerEntry& entry : catalog.reducers)
        entry.descriptor.providing_pack = catalog.manifest.identity;
    return catalog;
}

const BattleCommandCapabilityCatalog& CanonicalBattleCommandCapabilityCatalog()
{
    static const BattleCommandCapabilityCatalog catalog =
        BuildBattleCommandCapabilityCatalog();
    return catalog;
}

} // namespace

const ReducerDescriptor& CanonicalBattleCommandReducerDescriptor(
    CanonicalReducer reducer)
{
    const auto& entries =
        CanonicalBattleCommandCapabilityCatalog().reducers;
    const auto found = std::ranges::find(
        entries,
        reducer,
        &BattleCommandReducerEntry::reducer);
    if (found == entries.end())
        throw std::out_of_range("Unknown canonical reducer");
    return found->descriptor;
}

CapabilityPackIdentity BattleCommandPackIdentity()
{
    return CanonicalBattleCommandCapabilityCatalog().manifest.identity;
}

SourceCapabilityPackCatalog BuildSourceCapabilityPackCatalog()
{
    SourceCapabilityPackCatalog catalog{};
    auto runtime_schemas = BuildCanonicalRuntimeActionSchemas();
    std::vector<SchemaIdentity> runtime_schema_identities;
    runtime_schema_identities.reserve(runtime_schemas.size());
    for (const TypeSchemaDefinition& schema : runtime_schemas)
        runtime_schema_identities.push_back(schema.identity);
    catalog.schemas.insert(
        catalog.schemas.end(),
        std::make_move_iterator(runtime_schemas.begin()),
        std::make_move_iterator(runtime_schemas.end()));
    auto game_schemas = BuildSchemas();
    catalog.schemas.insert(
        catalog.schemas.end(),
        std::make_move_iterator(game_schemas.begin()),
        std::make_move_iterator(game_schemas.end()));

    const auto& battle_request = RequireSchema(
        catalog.schemas,
        "soa.battle.CaptureContextRequest");
    const auto& battle_context = RequireSchema(
        catalog.schemas,
        "soa.battle.BattleContext");
    const auto& navigation_request = RequireSchema(
        catalog.schemas,
        "soa.navigation.CaptureContextRequest");
    const auto& navigation_context = RequireSchema(
        catalog.schemas,
        "soa.navigation.NavigationContext");

    std::vector<SchemaIdentity> battle_schemas;
    std::vector<SchemaIdentity> navigation_schemas;
    for (const auto& schema : catalog.schemas)
    {
        if (std::ranges::find(
                runtime_schema_identities,
                schema.identity) !=
            runtime_schema_identities.end())
        {
            continue;
        }
        if ((schema.identity.canonical_id.starts_with("soa.battle.") &&
             !schema.identity.canonical_id.starts_with("soa.battle.command.")) ||
            schema.identity.canonical_id.starts_with("runtime.input.") ||
            schema.identity.canonical_id == "runtime.DiagnosticText")
        {
            battle_schemas.push_back(schema.identity);
        }
        if (schema.identity.canonical_id.starts_with("soa.navigation."))
            navigation_schemas.push_back(schema.identity);
    }

    catalog.actions = BuildCanonicalRuntimeActionDescriptors();
    catalog.actions.push_back(QueryAction(
        BattleCaptureContextActionIdentity(),
        BattlePackIdentity(),
        Named(battle_request.identity),
        Named(battle_context.identity)));
    catalog.actions.push_back(QueryAction(
        NavigationCaptureContextActionIdentity(),
        NavigationPackIdentity(),
        Named(navigation_request.identity),
        Named(navigation_context.identity)));
    const auto& battle_command_catalog =
        CanonicalBattleCommandCapabilityCatalog();
    catalog.reducers.reserve(battle_command_catalog.reducers.size());
    for (const BattleCommandReducerEntry& entry :
         battle_command_catalog.reducers)
    {
        catalog.reducers.push_back(entry.descriptor);
    }

    auto field_addresses = BuildFieldAddresses();
    auto battle_addresses = BuildBattleAddresses();
    auto navigation_addresses = BuildNavigationAddresses();

    CapabilityPackManifest field{
        .identity = FieldPackIdentity(),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .schemas = {},
        .semantic_points = BuildFieldPoints(),
        .address_symbols = field_addresses,
        .cpu_evaluators = {FieldRngSeedEvaluator()},
    };

    CapabilityPackManifest battle{
        .identity = BattlePackIdentity(),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .schemas = std::move(battle_schemas),
        .semantic_points = BuildBattlePoints(),
        .address_symbols = battle_addresses,
        .coherent_queries = {{
            .canonical_id = "soa.battle.query.BattleContext",
            .result_schema = battle_context.identity,
            .address_dependencies = AddressIds(battle_addresses),
            .maximum_guest_reads = 64,
        }},
        .cpu_evaluators = {BattleCurrentTurnEvaluator()},
        .actions = {BattleCaptureContextActionIdentity()},
    };

    CapabilityPackManifest battle_command =
        battle_command_catalog.manifest;

    CapabilityPackManifest navigation{
        .identity = NavigationPackIdentity(),
        .compatibility = SupportedSoaUsaCompatibility(),
        .dependencies = {CanonicalRuntimePackIdentity()},
        .schemas = std::move(navigation_schemas),
        .semantic_points = BuildNavigationPoints(),
        .address_symbols = navigation_addresses,
        .coherent_queries = {{
            .canonical_id = "soa.navigation.query.NavigationContext",
            .result_schema = navigation_context.identity,
            .address_dependencies = AddressIds(navigation_addresses),
            .maximum_guest_reads = 21,
        }},
        .actions = {NavigationCaptureContextActionIdentity()},
    };

    CapabilityPackManifest runtime{
        .identity = CanonicalRuntimePackIdentity(),
        .compatibility = SupportedSoaUsaCompatibility(),
        .schemas = std::move(runtime_schema_identities),
    };
    runtime.actions.reserve(CanonicalActionDefinitions().size());
    for (const CanonicalActionDefinition& definition :
         CanonicalActionDefinitions())
    {
        runtime.actions.push_back(
            CanonicalActionIdentity(definition.action));
    }
    std::ranges::sort(runtime.actions);

    catalog.manifests = {
        std::move(runtime),
        std::move(field),
        std::move(battle),
        std::move(battle_command),
        std::move(navigation),
    };
    return catalog;
}

RegistryResult RegisterSourceCapabilityPacks(
    TypeSchemaRegistry& schemas,
    ActionRegistry& actions,
    CapabilityPackRegistry& packs)
{
    SourceCapabilityPackCatalog catalog =
        BuildSourceCapabilityPackCatalog();
    auto result = schemas.RegisterBatch(std::move(catalog.schemas));
    if (!result.success) return result;
    result = actions.RegisterCatalog(
        std::move(catalog.actions),
        std::move(catalog.reducers));
    if (!result.success) return result;
    return packs.RegisterBatch(std::move(catalog.manifests));
}

} // namespace savor::runtime::program::capabilities

namespace savor::runtime::program {

std::string_view CanonicalReducerName(
    CanonicalReducer reducer) noexcept
{
    try
    {
        return capabilities::CanonicalBattleCommandReducerDescriptor(
            reducer).identity.canonical_id;
    }
    catch (const std::out_of_range&)
    {
        return {};
    }
}

ExactDependencyIdentity CanonicalReducerIdentity(
    CanonicalReducer reducer)
{
    return capabilities::CanonicalBattleCommandReducerDescriptor(
        reducer).identity;
}

} // namespace savor::runtime::program
