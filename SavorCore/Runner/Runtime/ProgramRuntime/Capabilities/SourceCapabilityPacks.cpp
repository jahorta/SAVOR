#include "SourceCapabilityPacks.h"

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
constexpr std::string_view kNavigationPackId = "soa.navigation";

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
        {
            {"BackAttack", 0},
            {"Normal", 1},
            {"Advantage", 2},
        },
        "enum{BackAttack=0,Normal=1,Advantage=2}"));
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
            {"stop_sequence", TypeRef::Builtin(BuiltinType::U64)},
            {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"expected_pc", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record CaptureContextRequest/1(stop_sequence,workset_epoch,expected_pc)"));

    schemas.push_back(EnumSchema(
        "soa.battle.BattleAction",
        {
            {"Attack", 0},
            {"Defend", 1},
            {"Focus", 2},
            {"FakeAttack", 3},
            {"UseItem", 4},
        },
        "enum BattleAction/1"));
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

    schemas.push_back(RecordSchema(
        "runtime.input.GCInputFrame",
        {
            {"buttons", TypeRef::Builtin(BuiltinType::U16)},
            {"main_x", TypeRef::Builtin(BuiltinType::U8)},
            {"main_y", TypeRef::Builtin(BuiltinType::U8)},
            {"c_x", TypeRef::Builtin(BuiltinType::U8)},
            {"c_y", TypeRef::Builtin(BuiltinType::U8)},
            {"trigger_l", TypeRef::Builtin(BuiltinType::U8)},
            {"trigger_r", TypeRef::Builtin(BuiltinType::U8)},
        },
        "record GCInputFrame/1"));
    const auto input_frame = schemas.back().identity;
    schemas.push_back(ListSchema(
        "runtime.input.GCInputSequence",
        Named(input_frame),
        65536,
        "list<GCInputFrame>(max=65536)"));
    const auto input_sequence = schemas.back().identity;
    schemas.push_back(RecordSchema(
        "soa.battle.TurnInputMaterialization",
        {
            {"success", TypeRef::Builtin(BuiltinType::Bool)},
            {"sequence", Named(input_sequence)},
            {"diagnostic", Named(diagnostic)},
        },
        "record TurnInputMaterialization/1"));

    schemas.push_back(RecordSchema(
        "soa.navigation.CaptureContextRequest",
        {
            {"stop_sequence", TypeRef::Builtin(BuiltinType::U64)},
            {"workset_epoch", TypeRef::Builtin(BuiltinType::U64)},
            {"expected_pc", TypeRef::Builtin(BuiltinType::U32)},
        },
        "record NavigationCaptureContextRequest/1"));

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
            point.key == bp::battle::BattleEndFieldReturnReseedComplete;
        if ((!prebattle && !field_return) || point.pc == 0)
            continue;
        points.push_back({
            .canonical_id = prebattle
                ? std::format("soa.field.point.prebattle.{}", point.name)
                : "soa.field.point.field_return.RandSeedCommitted",
            .kind = SemanticPointPhysicalKind::ProgramCounter,
            .pc = point.pc,
            .legacy_key = point.stable_id ? point.stable_id : "",
        });
    }
    return points;
}

std::vector<SemanticPointDescriptor> BuildBattlePoints()
{
    std::vector<SemanticPointDescriptor> points;
    for (const auto& point : bp::BpRegistry::AllRuntime())
    {
        if (bp::domain_of(point.key) != bp::BPDomain::Battle ||
            point.pc == 0)
        {
            continue;
        }
        points.push_back({
            .canonical_id = LogicalPointId(kBattlePackId, point),
            .kind = SemanticPointPhysicalKind::ProgramCounter,
            .pc = point.pc,
            .legacy_key = point.stable_id ? point.stable_id : "",
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
            .kind = SemanticPointPhysicalKind::ProgramCounter,
            .pc = point.pc,
            .legacy_key = point.stable_id ? point.stable_id : "",
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
        .cpu_evaluators = {{
            .canonical_id = "soa.field.sample.RngSeed",
            .operations = {CpuEvaluatorOperation::ReadU32},
            .maximum_reads = 1,
            .maximum_output_bytes = 4,
        }},
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
            return !schema.identity.canonical_id.starts_with("soa.battle.") &&
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
        .cpu_evaluators = {{
            .canonical_id = "soa.battle.sample.CurrentTurn",
            .operations = {CpuEvaluatorOperation::ReadU8},
            .maximum_reads = 1,
            .maximum_output_bytes = 1,
        }},
        .actions = {QueryActionIdentity(
            "soa.battle.capture_context",
            std::string(kBattlePackId),
            Named(battle_request.identity),
            Named(battle_context.identity))},
        .reducers = {CanonicalReducerIdentity(
            CanonicalReducer::BattleMaterializeTurnInput)},
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

ExactDependencyIdentity BattleMaterializeTurnInputReducerIdentity()
{
    const auto schemas = BuildSchemas();
    ReducerDescriptor descriptor{
        .identity = {
            .canonical_id =
                "soa.battle.materialize_turn_input",
            .version = 1,
        },
        .providing_pack = UnsignedPackIdentity(
            std::string(kBattlePackId)),
        .input_types = {
            Named(RequireSchema(
                schemas,
                "soa.battle.BattleContext").identity),
            Named(RequireSchema(
                schemas,
                "soa.battle.BattleTurnExecutionSpec").identity),
        },
        .output_type = Named(RequireSchema(
            schemas,
            "soa.battle.TurnInputMaterialization").identity),
        .permitted_actions = {},
        .permitted_subprograms = {},
        .maximum_steps = 100000,
        .maximum_value_bytes = 4 * 1024 * 1024,
    };
    descriptor.identity.signature_hash =
        ComputeReducerDescriptorContractHash(descriptor);
    return descriptor.identity;
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
    const auto& turn_spec = RequireSchema(
        catalog.schemas,
        "soa.battle.BattleTurnExecutionSpec");
    const auto& materialization = RequireSchema(
        catalog.schemas,
        "soa.battle.TurnInputMaterialization");
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
        if (schema.identity.canonical_id.starts_with("soa.battle.") ||
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
    catalog.reducers = {{
        .identity = CanonicalReducerIdentity(
            CanonicalReducer::BattleMaterializeTurnInput),
        .providing_pack = BattlePackIdentity(),
        .input_types = {
            Named(battle_context.identity),
            Named(turn_spec.identity),
        },
        .output_type = Named(materialization.identity),
        .permitted_actions = {},
        .permitted_subprograms = {},
        .maximum_steps = 100000,
        .maximum_value_bytes = 4 * 1024 * 1024,
    }};

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
        .cpu_evaluators = {{
            .canonical_id = "soa.field.sample.RngSeed",
            .operations = {CpuEvaluatorOperation::ReadU32},
            .maximum_reads = 1,
            .maximum_output_bytes = 4,
        }},
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
        .cpu_evaluators = {{
            .canonical_id = "soa.battle.sample.CurrentTurn",
            .operations = {CpuEvaluatorOperation::ReadU8},
            .maximum_reads = 1,
            .maximum_output_bytes = 1,
        }},
        .actions = {BattleCaptureContextActionIdentity()},
        .reducers = {CanonicalReducerIdentity(
            CanonicalReducer::BattleMaterializeTurnInput)},
    };

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
    runtime.actions.reserve(
        static_cast<std::size_t>(CanonicalAction::TelemetryEmit) + 1u);
    for (std::size_t index = 0;
         index <= static_cast<std::size_t>(CanonicalAction::TelemetryEmit);
         ++index)
    {
        runtime.actions.push_back(CanonicalActionIdentity(
            static_cast<CanonicalAction>(index)));
    }

    catalog.manifests = {
        std::move(runtime),
        std::move(field),
        std::move(battle),
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

ExactDependencyIdentity CanonicalReducerIdentity(
    CanonicalReducer reducer)
{
    if (reducer != CanonicalReducer::BattleMaterializeTurnInput)
        throw std::out_of_range("Unknown canonical reducer");
    return capabilities::BattleMaterializeTurnInputReducerIdentity();
}

} // namespace savor::runtime::program
