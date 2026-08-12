#include "SourceReducers.h"

#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include "Core/Input/SoaBattle/BattlePlanValidation.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
#include <stdexcept>
#include <set>
#include <type_traits>
#include <utility>

namespace savor::runtime::program::capabilities {
namespace {

const SchemaIdentity& SchemaIdentityFor(std::string_view id)
{
    static const SourceCapabilityPackCatalog catalog =
        BuildSourceCapabilityPackCatalog();
    const auto found = std::ranges::find(
        catalog.schemas,
        id,
        [](const TypeSchemaDefinition& schema)
        {
            return std::string_view(schema.identity.canonical_id);
        });
    if (found == catalog.schemas.end())
        throw std::logic_error("source reducer schema is missing");
    return found->identity;
}

class GraphBuilder final
{
public:
    template <typename Payload>
    ProgramValueId Add(TypeRef type, Payload payload)
    {
        const auto id = ProgramValueId(next_++);
        values_.push_back({
            .id = id,
            .type = std::move(type),
            .payload = std::move(payload),
        });
        return id;
    }

    ProgramValueId AddRecord(
        const SchemaIdentity& schema,
        std::vector<ProgramValueId> fields)
    {
        return Add(
            TypeRef::Named(schema),
            RecordValue{.fields = std::move(fields)});
    }

    ProgramValueId AddList(
        const SchemaIdentity& schema,
        std::vector<ProgramValueId> elements)
    {
        return Add(
            TypeRef::Named(schema),
            ListValue{.elements = std::move(elements)});
    }

    ProgramValueId Import(const ProgramValueGraph& graph)
    {
        std::map<ProgramValueId, ProgramValueId> remap;
        for (const auto& value : graph.values)
            remap.emplace(value.id, ProgramValueId(next_++));
        for (const auto& value : graph.values)
        {
            ProgramValue imported = value;
            imported.id = remap.at(value.id);
            if (auto* optional = std::get_if<OptionalValue>(&imported.payload);
                optional && optional->value)
            {
                optional->value = remap.at(*optional->value);
            }
            if (auto* record = std::get_if<RecordValue>(&imported.payload))
                for (auto& field : record->fields) field = remap.at(field);
            if (auto* list = std::get_if<ListValue>(&imported.payload))
                for (auto& element : list->elements) element = remap.at(element);
            values_.push_back(std::move(imported));
        }
        return remap.at(graph.root);
    }

    ProgramValueGraph Finish(ProgramValueId root) &&
    {
        return {
            .root = root,
            .values = std::move(values_),
        };
    }

private:
    std::uint64_t next_ = 1;
    std::vector<ProgramValue> values_;
};

class GraphReader final
{
public:
    explicit GraphReader(const ProgramValueGraph& graph)
        : graph_(graph)
    {
        valid_ = static_cast<bool>(graph_.root);
        for (const auto& value : graph_.values)
        {
            if (!value.id || !values_.emplace(value.id, &value).second)
            {
                valid_ = false;
                break;
            }
        }
        if (!Find(graph_.root)) valid_ = false;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] const ProgramValue* Find(ProgramValueId id) const
    {
        const auto found = values_.find(id);
        return found == values_.end() ? nullptr : found->second;
    }

    template <typename Payload>
    [[nodiscard]] const Payload* PayloadOf(ProgramValueId id) const
    {
        const auto* value = Find(id);
        return value == nullptr
            ? nullptr
            : std::get_if<Payload>(&value->payload);
    }

    [[nodiscard]] const RecordValue* RootRecord(
        const SchemaIdentity& schema,
        std::size_t field_count) const
    {
        const auto* root = Find(graph_.root);
        if (root == nullptr || root->type != TypeRef::Named(schema))
            return nullptr;
        const auto* record = std::get_if<RecordValue>(&root->payload);
        return record != nullptr && record->fields.size() == field_count
            ? record
            : nullptr;
    }

private:
    const ProgramValueGraph& graph_;
    bool valid_ = false;
    std::map<ProgramValueId, const ProgramValue*> values_;
};

template <typename T>
std::vector<Byte> BytesOf(const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<Byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
bool DecodeBytes(
    const GraphReader& reader,
    ProgramValueId id,
    T& output)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reader.PayloadOf<std::vector<Byte>>(id);
    if (bytes == nullptr || bytes->size() != sizeof(T))
        return false;
    std::memcpy(&output, bytes->data(), sizeof(T));
    return true;
}

template <typename T>
bool Scalar(
    const GraphReader& reader,
    ProgramValueId id,
    T& output)
{
    const auto* value = reader.PayloadOf<T>(id);
    if (value == nullptr) return false;
    output = *value;
    return true;
}

bool DecodeBattleContext(
    const ProgramValueGraph& graph,
    soa::battle::ctx::BattleContext& output)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* context = reader.RootRecord(
        SchemaIdentityFor("soa.battle.BattleContext"),
        5);
    if (context == nullptr) return false;
    const auto* slots = reader.PayloadOf<ListValue>(context->fields[0]);
    if (slots == nullptr ||
        slots->elements.size() !=
            static_cast<std::size_t>(soa::battle::ctx::SLOT_COUNT))
    {
        return false;
    }

    soa::battle::ctx::BattleContext decoded{};
    for (std::size_t index = 0; index < slots->elements.size(); ++index)
    {
        const auto* record =
            reader.PayloadOf<RecordValue>(slots->elements[index]);
        if (record == nullptr || record->fields.size() != 9)
            return false;
        auto& slot = decoded.slots_[index];
        bool present = false;
        bool is_player = false;
        bool is_alive = false;
        bool has_enemy = false;
        if (!Scalar(reader, record->fields[0], present) ||
            !Scalar(reader, record->fields[1], is_player) ||
            !Scalar(reader, record->fields[2], slot.id) ||
            !Scalar(reader, record->fields[3], is_alive) ||
            !Scalar(reader, record->fields[4], has_enemy) ||
            !DecodeBytes(reader, record->fields[5], slot.instance) ||
            !DecodeBytes(reader, record->fields[6], slot.enemy_def) ||
            !Scalar(reader, record->fields[7], slot.instance_addr) ||
            !Scalar(reader, record->fields[8], slot.enemy_def_addr))
        {
            return false;
        }
        slot.present = present ? 1 : 0;
        slot.is_player = is_player ? 1 : 0;
        slot.is_alive = is_alive ? 1 : 0;
        slot.has_enemy_def = has_enemy ? 1 : 0;
    }

    const auto* turn_type = reader.PayloadOf<EnumValue>(
        context->fields[2]);
    const auto* turn_type_definition = turn_type == nullptr
        ? nullptr
        : soa::battle::find_turn_type_definition(turn_type->value);
    if (!DecodeBytes(reader, context->fields[1], decoded.state) ||
        turn_type == nullptr ||
        turn_type->schema != SchemaIdentityFor("soa.battle.TurnType") ||
        turn_type_definition == nullptr ||
        !Scalar(reader, context->fields[3], decoded.turn_count) ||
        !Scalar(reader, context->fields[4], decoded.battle_phase))
    {
        return false;
    }
    decoded.turn_type = turn_type_definition->type;
    output = std::move(decoded);
    return true;
}

bool DecodeTurnSpec(
    const ProgramValueGraph& graph,
    soa::battle::actions::BattleTurnExecutionSpec& output)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* specification = reader.RootRecord(
        SchemaIdentityFor("soa.battle.BattleTurnExecutionSpec"),
        2);
    if (specification == nullptr ||
        !Scalar(
            reader,
            specification->fields[0],
            output.fake_attack_count))
    {
        return false;
    }
    const auto* commands =
        reader.PayloadOf<ListValue>(specification->fields[1]);
    if (commands == nullptr || commands->elements.size() > 4)
        return false;
    output.commands.clear();
    for (const auto id : commands->elements)
    {
        const auto* record = reader.PayloadOf<RecordValue>(id);
        if (record == nullptr || record->fields.size() != 4)
            return false;
        soa::battle::actions::BattleCommand command{};
        const auto* action = reader.PayloadOf<EnumValue>(
            record->fields[1]);
        const auto* action_definition = action == nullptr
            ? nullptr
            : soa::battle::actions::find_battle_action_definition(
                  action->value);
        if (!Scalar(reader, record->fields[0], command.actor_slot) ||
            action == nullptr ||
            action->schema != SchemaIdentityFor("soa.battle.BattleAction") ||
            action_definition == nullptr ||
            !Scalar(
                reader,
                record->fields[2],
                command.params.target_slot) ||
            !Scalar(
                reader,
                record->fields[3],
                command.params.item_id))
        {
            return false;
        }
        command.macro = action_definition->action;
        output.commands.push_back(command);
    }
    return true;
}

struct BattleCommandState
{
    soa::battle::ctx::BattleContext context;
    soa::battle::actions::BattleTurnExecutionSpec plan;
    BattleCommandSegment segment = BattleCommandSegment::AwaitInputReady;
    std::uint32_t fake_remaining = 0;
    std::uint32_t command_index = 0;
    std::uint32_t moves_remaining = 0;
};

ProgramValueGraph EncodeBattleCommandState(const BattleCommandState& state);

bool DecodeBattleCommandState(
    const ProgramValueGraph& graph,
    BattleCommandState& output)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* record = reader.RootRecord(
        SchemaIdentityFor("soa.battle.command.State"), 6);
    if (!record) return false;
    const auto* segment = reader.PayloadOf<EnumValue>(record->fields[2]);
    if (!segment ||
        segment->schema != SchemaIdentityFor("soa.battle.command.Segment") ||
        segment->value < 0 ||
        segment->value > BattleCommandSegmentValue(
            BattleCommandSegment::Complete))
    {
        return false;
    }
    ProgramValueGraph context_graph = graph;
    context_graph.root = record->fields[0];
    ProgramValueGraph plan_graph = graph;
    plan_graph.root = record->fields[1];
    BattleCommandState decoded;
    if (!DecodeBattleContext(context_graph, decoded.context) ||
        !DecodeTurnSpec(plan_graph, decoded.plan) ||
        !Scalar(reader, record->fields[3], decoded.fake_remaining) ||
        !Scalar(reader, record->fields[4], decoded.command_index) ||
        !Scalar(reader, record->fields[5], decoded.moves_remaining))
    {
        return false;
    }
    decoded.segment = static_cast<BattleCommandSegment>(segment->value);
    output = std::move(decoded);
    return true;
}

enum class BattleDerivedGroup : std::int64_t
{
    TurnEntry = 1,
    TurnOrder = 2,
    Rewards = 3,
};

std::optional<BattleDerivedGroup> DerivedGroup(
    std::string_view group_id)
{
    if (group_id == derived::kBattleTurnEntryGroupId)
        return BattleDerivedGroup::TurnEntry;
    if (group_id == derived::kBattleTurnOrderGroupId)
        return BattleDerivedGroup::TurnOrder;
    if (group_id == derived::kBattleRewardsGroupId)
        return BattleDerivedGroup::Rewards;
    return std::nullopt;
}

ProgramValueId EncodeDerivedProvenance(
    GraphBuilder& builder,
    const derived::DerivedStateSnapshotProvenanceV1& provenance)
{
    const auto group = DerivedGroup(provenance.group_id);
    if (!provenance || !group)
        throw std::invalid_argument("Battle derived-state provenance is invalid");
    const auto group_value = builder.Add(
        TypeRef::Named(SchemaIdentityFor("soa.battle.derived.Group")),
        EnumValue{
            .schema = SchemaIdentityFor("soa.battle.derived.Group"),
            .value = static_cast<std::int64_t>(*group),
        });
    return builder.AddRecord(
        SchemaIdentityFor("soa.battle.derived.Provenance"),
        {
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.workset_epoch.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.item_id.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U32), provenance.block.revision),
            group_value,
            builder.Add(TypeRef::Builtin(BuiltinType::U32), provenance.group_revision),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.generation),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.routed_stop.sequence.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.routed_stop.sample_snapshot.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.routed_stop.dispatch_generation.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U64), provenance.routed_stop.physical_generation.value()),
            builder.Add(TypeRef::Builtin(BuiltinType::U32), provenance.trigger_pc),
        });
}

ProgramValueId EncodeItemTotals(
    GraphBuilder& builder,
    std::span<const derived::BattleItemTotalV1> totals)
{
    std::vector<ProgramValueId> elements;
    elements.reserve(totals.size());
    for (const auto& total : totals)
    {
        elements.push_back(builder.AddRecord(
            SchemaIdentityFor("soa.battle.derived.ItemTotal"),
            {
                builder.Add(TypeRef::Builtin(BuiltinType::U16), total.item_id),
                builder.Add(TypeRef::Builtin(BuiltinType::U32), total.count),
            }));
    }
    return builder.AddList(
        SchemaIdentityFor("soa.battle.derived.ItemTotals"),
        std::move(elements));
}

ProgramValueId EncodeTurnOrder(
    GraphBuilder& builder,
    std::span<const std::uint8_t> slots)
{
    std::vector<ProgramValueId> elements;
    elements.reserve(slots.size());
    for (const auto slot : slots)
        elements.push_back(builder.Add(TypeRef::Builtin(BuiltinType::U8), slot));
    return builder.AddList(
        SchemaIdentityFor("soa.battle.derived.TurnOrderSlots"),
        std::move(elements));
}

ProgramValueGraph EncodeBattleDerivedSnapshot(
    const derived::DerivedStateSnapshotProvenanceV1& provenance,
    std::uint32_t current_turn,
    std::span<const derived::BattleItemTotalV1> inventory,
    std::span<const std::uint8_t> turn_order,
    std::span<const derived::BattleItemTotalV1> drops)
{
    GraphBuilder builder;
    const auto encoded_provenance =
        EncodeDerivedProvenance(builder, provenance);
    const auto encoded_turn = builder.Add(
        TypeRef::Builtin(BuiltinType::U32), current_turn);
    const auto encoded_inventory = EncodeItemTotals(builder, inventory);
    const auto encoded_order = EncodeTurnOrder(builder, turn_order);
    const auto encoded_drops = EncodeItemTotals(builder, drops);
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.derived.Snapshot"),
        {encoded_provenance, encoded_turn, encoded_inventory, encoded_order, encoded_drops});
    return std::move(builder).Finish(root);
}

struct DecodedBattleDerivedSnapshot
{
    BattleDerivedGroup group = BattleDerivedGroup::TurnEntry;
    std::uint32_t current_turn = 0;
    std::vector<derived::BattleItemTotalV1> inventory;
    std::vector<std::uint8_t> turn_order;
    std::vector<derived::BattleItemTotalV1> drops;
};

bool DecodeItemTotals(
    const GraphReader& reader,
    ProgramValueId id,
    std::vector<derived::BattleItemTotalV1>& output)
{
    const auto* list = reader.PayloadOf<ListValue>(id);
    if (!list || list->elements.size() > 80)
        return false;
    std::vector<derived::BattleItemTotalV1> decoded;
    decoded.reserve(list->elements.size());
    std::optional<std::uint16_t> previous;
    for (const auto element : list->elements)
    {
        const auto* record = reader.PayloadOf<RecordValue>(element);
        derived::BattleItemTotalV1 total;
        if (!record || record->fields.size() != 2 ||
            !Scalar(reader, record->fields[0], total.item_id) ||
            !Scalar(reader, record->fields[1], total.count) ||
            total.count == 0 || (previous && total.item_id <= *previous))
        {
            return false;
        }
        previous = total.item_id;
        decoded.push_back(total);
    }
    output = std::move(decoded);
    return true;
}

bool DecodeBattleDerivedSnapshot(
    const ProgramValueGraph& graph,
    DecodedBattleDerivedSnapshot& output)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* snapshot = reader.RootRecord(
        SchemaIdentityFor("soa.battle.derived.Snapshot"), 5);
    if (!snapshot) return false;
    const auto* provenance = reader.PayloadOf<RecordValue>(snapshot->fields[0]);
    const auto* group = provenance && provenance->fields.size() == 11
        ? reader.PayloadOf<EnumValue>(provenance->fields[3])
        : nullptr;
    if (!group || group->schema != SchemaIdentityFor("soa.battle.derived.Group") ||
        group->value < 1 || group->value > 3)
    {
        return false;
    }
    DecodedBattleDerivedSnapshot decoded;
    decoded.group = static_cast<BattleDerivedGroup>(group->value);
    if (!Scalar(reader, snapshot->fields[1], decoded.current_turn) ||
        !DecodeItemTotals(reader, snapshot->fields[2], decoded.inventory) ||
        !DecodeItemTotals(reader, snapshot->fields[4], decoded.drops))
    {
        return false;
    }
    const auto* order = reader.PayloadOf<ListValue>(snapshot->fields[3]);
    std::set<std::uint8_t> slots;
    if (!order || order->elements.size() > 12)
        return false;
    for (const auto element : order->elements)
    {
        std::uint8_t slot = 0;
        if (!Scalar(reader, element, slot) || slot > 11 ||
            !slots.insert(slot).second)
        {
            return false;
        }
        decoded.turn_order.push_back(slot);
    }
    const bool shape_ok =
        (decoded.group == BattleDerivedGroup::TurnEntry &&
         decoded.turn_order.empty() && decoded.drops.empty()) ||
        (decoded.group == BattleDerivedGroup::TurnOrder &&
         decoded.inventory.empty() && decoded.drops.empty()) ||
        (decoded.group == BattleDerivedGroup::Rewards &&
         decoded.inventory.empty() && decoded.turn_order.empty());
    if (!shape_ok)
        return false;
    output = std::move(decoded);
    return true;
}

template <typename T>
std::optional<T> RootScalar(const ProgramValueGraph& graph)
{
    GraphReader reader(graph);
    if (!reader.valid()) return std::nullopt;
    const auto* value = reader.PayloadOf<T>(graph.root);
    return value ? std::optional<T>(*value) : std::nullopt;
}

ProgramValueGraph ScalarU32(std::uint32_t value)
{
    GraphBuilder builder;
    const auto root = builder.Add(TypeRef::Builtin(BuiltinType::U32), value);
    return std::move(builder).Finish(root);
}

std::uint32_t SelectableEnemyIndex(
    const soa::battle::ctx::BattleContext& context,
    std::uint8_t requested_slot)
{
    std::uint32_t index = 0;
    for (std::uint32_t slot = 4; slot < soa::battle::ctx::SLOT_COUNT; ++slot)
    {
        const auto& candidate = context.slots_[slot];
        if (!candidate.present || candidate.is_player || !candidate.is_alive)
            continue;
        if (requested_slot == 0xffu || requested_slot == slot)
            return index;
        ++index;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

void SelectCommandSegment(BattleCommandState& state)
{
    const auto& command = state.plan.commands.at(state.command_index);
    using soa::battle::actions::BattleAction;
    switch (command.macro)
    {
    case BattleAction::Attack:
        state.moves_remaining = SelectableEnemyIndex(
            state.context, command.params.target_slot);
        state.segment = BattleCommandSegment::AttackAccept;
        break;
    case BattleAction::Defend:
        state.moves_remaining = 1;
        state.segment = BattleCommandSegment::MainMenuMoveUp;
        break;
    case BattleAction::Focus:
        state.moves_remaining = 3;
        state.segment = BattleCommandSegment::MainMenuMoveDown;
        break;
    default:
        state.segment = BattleCommandSegment::Complete;
        break;
    }
}

} // namespace

ProgramValueGraph EncodeBattleContextValue(
    const soa::battle::ctx::BattleContext& context)
{
    GraphBuilder builder;
    std::vector<ProgramValueId> slot_ids;
    slot_ids.reserve(soa::battle::ctx::SLOT_COUNT);
    for (const auto& slot : context.slots_)
    {
        std::vector<ProgramValueId> fields;
        fields.reserve(9);
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::Bool),
            slot.present != 0));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::Bool),
            slot.is_player != 0));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U16),
            slot.id));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::Bool),
            slot.is_alive != 0));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::Bool),
            slot.has_enemy_def != 0));
        fields.push_back(builder.Add(
            TypeRef::Named(
                SchemaIdentityFor(
                    "soa.battle.CombatantInstanceSnapshot")),
            BytesOf(slot.instance)));
        fields.push_back(builder.Add(
            TypeRef::Named(
                SchemaIdentityFor(
                    "soa.battle.EnemyDefinitionSnapshot")),
            BytesOf(slot.enemy_def)));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U32),
            slot.instance_addr));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U32),
            slot.enemy_def_addr));
        slot_ids.push_back(builder.AddRecord(
            SchemaIdentityFor("soa.battle.BattleSlot"),
            std::move(fields)));
    }
    const auto slots = builder.AddList(
        SchemaIdentityFor("soa.battle.BattleSlotList"),
        std::move(slot_ids));
    const auto state = builder.Add(
        TypeRef::Named(
            SchemaIdentityFor("soa.battle.BattleStateSnapshot")),
        BytesOf(context.state));
    const auto turn_type = builder.Add(
        TypeRef::Named(SchemaIdentityFor("soa.battle.TurnType")),
        EnumValue{
            .schema = SchemaIdentityFor("soa.battle.TurnType"),
            .value = static_cast<std::int64_t>(context.turn_type),
        });
    const auto turn_count = builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.turn_count);
    const auto battle_phase = builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.battle_phase);
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.BattleContext"),
        {slots, state, turn_type, turn_count, battle_phase});
    return std::move(builder).Finish(root);
}

bool DecodeBattleContextValue(
    const ProgramValueGraph& graph,
    soa::battle::ctx::BattleContext& context)
{
    return DecodeBattleContext(graph, context);
}

bool DecodeBattleDerivedQueryValue(
    const ProgramValueGraph& graph,
    derived::DerivedStateQueryV1& query)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* request = reader.RootRecord(
        SchemaIdentityFor("soa.battle.derived.QueryRequest"), 2);
    const auto* freshness = request
        ? reader.PayloadOf<EnumValue>(request->fields[0])
        : nullptr;
    if (!freshness ||
        freshness->schema != SchemaIdentityFor("soa.battle.derived.Freshness") ||
        (freshness->value != 1 && freshness->value != 2))
    {
        return false;
    }
    const auto* optional_value = reader.Find(request->fields[1]);
    const auto* optional = optional_value
        ? std::get_if<OptionalValue>(&optional_value->payload)
        : nullptr;
    if (!optional_value ||
        optional_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalContinueUntilResult) ||
        !optional)
    {
        return false;
    }
    const auto mode = static_cast<derived::DerivedStateFreshness>(
        freshness->value);
    if (mode == derived::DerivedStateFreshness::LatestInItem)
    {
        if (optional->value) return false;
        query = {.freshness = mode};
        return true;
    }
    if (mode != derived::DerivedStateFreshness::SameRoutedEvent ||
        !optional->value)
    {
        return false;
    }

    const auto* result_value = reader.Find(*optional->value);
    const auto* result = result_value
        ? std::get_if<RecordValue>(&result_value->payload)
        : nullptr;
    if (!result_value ||
        result_value->type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil) ||
        !result || result->fields.size() != 5)
    {
        return false;
    }
    const auto* reason = reader.PayloadOf<EnumValue>(result->fields[0]);
    const auto* routed_optional_value = reader.Find(result->fields[1]);
    const auto* routed_optional = routed_optional_value
        ? std::get_if<OptionalValue>(&routed_optional_value->payload)
        : nullptr;
    if (!reason ||
        reason->schema != CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason) ||
        reason->value != static_cast<std::int64_t>(
            ContinueUntilCompletionReasonV1::Breakpoint) ||
        !routed_optional_value ||
        routed_optional_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt) ||
        !routed_optional || !routed_optional->value)
    {
        return false;
    }
    const auto* routed_value = reader.Find(*routed_optional->value);
    const auto* routed = routed_value
        ? std::get_if<RecordValue>(&routed_value->payload)
        : nullptr;
    if (!routed_value ||
        routed_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt) ||
        !routed || routed->fields.size() != 5)
    {
        return false;
    }
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint32_t pc = 0;
    std::uint64_t sample = 0;
    if (!Scalar(reader, routed->fields[0], sequence) ||
        !Scalar(reader, routed->fields[1], epoch) ||
        !Scalar(reader, routed->fields[2], pc) ||
        !Scalar(reader, routed->fields[3], sample) ||
        sequence == 0 || epoch == 0 || pc == 0 || sample == 0)
    {
        return false;
    }
    const auto* evidence_value = reader.Find(routed->fields[4]);
    const auto* evidence = evidence_value
        ? std::get_if<std::vector<Byte>>(&evidence_value->payload)
        : nullptr;
    if (!evidence_value ||
        evidence_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload) ||
        !evidence || evidence->size() < 6 ||
        (*evidence)[0] != static_cast<Byte>('R') ||
        (*evidence)[1] != static_cast<Byte>('S') ||
        (*evidence)[2] != static_cast<Byte>('E') ||
        (*evidence)[3] != static_cast<Byte>('1'))
    {
        return false;
    }
    std::size_t offset = 5; // magic plus route path
    const auto point_kind = (*evidence)[offset++];
    if (point_kind == 0)
        offset += 4;
    else if (point_kind == 1)
        offset += 9;
    else if (point_kind == 2)
        offset += 8;
    else
        return false;
    // hit PC, sampled value, and post-write flag precede the internal routing
    // generations in the canonical RSE1 payload.
    offset += 4 + 8 + 1;
    if (offset > evidence->size() || evidence->size() - offset < 16)
        return false;
    const auto read_u64 = [&](std::size_t at)
    {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= static_cast<std::uint64_t>((*evidence)[at++]) << shift;
        return value;
    };
    const auto dispatch = read_u64(offset);
    const auto physical = read_u64(offset + 8);
    if (dispatch == 0 || physical == 0)
        return false;
    query = {
        .freshness = mode,
        .workset_epoch = WorksetEpoch(epoch),
        .routed_stop = {
            .sequence = RoutedStopSequence(sequence),
            .sample_snapshot = StopSampleSnapshotId(sample),
            .workset_epoch = WorksetEpoch(epoch),
            .dispatch_generation = StopDispatchGeneration(dispatch),
            .physical_generation = PhysicalPlanGeneration(physical),
        },
    };
    return true;
}

ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleTurnEntrySnapshotV1& snapshot)
{
    return EncodeBattleDerivedSnapshot(
        snapshot.provenance,
        snapshot.current_turn,
        snapshot.inventory,
        {},
        {});
}

ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleTurnOrderSnapshotV1& snapshot)
{
    return EncodeBattleDerivedSnapshot(
        snapshot.provenance,
        snapshot.current_turn,
        {},
        snapshot.active_slots,
        {});
}

ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleRewardsSnapshotV1& snapshot)
{
    return EncodeBattleDerivedSnapshot(
        snapshot.provenance,
        snapshot.current_turn,
        {},
        {},
        snapshot.drops);
}

ProgramValueGraph EncodeNavigationContextValue(
    const soa::navigation::ctx::NavigationContext& context)
{
    GraphBuilder builder;
    std::vector<ProgramValueId> fields;
    fields.reserve(22);
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.capture_pc));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.player_worksheet));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.area));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U8),
        context.subarea));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U16),
        context.motion_state));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U16),
        context.motion_substate));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.post_input_movement_suppress));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.position_x));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.position_y));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.position_z));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.rotation_x_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.rotation_y_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.rotation_z_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.previous_position_x));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.previous_position_y));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.previous_position_z));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.previous_rotation_x_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.previous_rotation_y_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        context.previous_rotation_z_raw));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::F32),
        context.step_distance_carry_in));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::Bool),
        context.has_ground));
    fields.push_back(builder.Add(
        TypeRef::Builtin(BuiltinType::U16),
        context.ground_tbl_id));
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.navigation.NavigationContext"),
        std::move(fields));
    return std::move(builder).Finish(root);
}

ProgramValueGraph EncodeBattleTurnExecutionSpecValue(
    const soa::battle::actions::BattleTurnExecutionSpec& specification)
{
    GraphBuilder builder;
    const auto fake_attack_count = builder.Add(
        TypeRef::Builtin(BuiltinType::U32),
        specification.fake_attack_count);
    std::vector<ProgramValueId> commands;
    commands.reserve(specification.commands.size());
    for (const auto& command : specification.commands)
    {
        const auto actor = builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            command.actor_slot);
        const auto action = builder.Add(
            TypeRef::Named(
                SchemaIdentityFor("soa.battle.BattleAction")),
            EnumValue{
                .schema = SchemaIdentityFor("soa.battle.BattleAction"),
                .value = static_cast<std::int64_t>(command.macro),
            });
        const auto target = builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            command.params.target_slot);
        const auto item = builder.Add(
            TypeRef::Builtin(BuiltinType::U16),
            command.params.item_id);
        commands.push_back(builder.AddRecord(
            SchemaIdentityFor("soa.battle.BattleCommand"),
            {actor, action, target, item}));
    }
    const auto command_list = builder.AddList(
        SchemaIdentityFor("soa.battle.BattleCommandList"),
        std::move(commands));
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.BattleTurnExecutionSpec"),
        {fake_attack_count, command_list});
    return std::move(builder).Finish(root);
}

namespace {

ProgramValueGraph EncodeBattleCommandState(const BattleCommandState& state)
{
    GraphBuilder builder;
    const auto context = builder.Import(EncodeBattleContextValue(state.context));
    const auto plan = builder.Import(EncodeBattleTurnExecutionSpecValue(state.plan));
    const auto segment = builder.Add(
        TypeRef::Named(SchemaIdentityFor("soa.battle.command.Segment")),
        EnumValue{
            .schema = SchemaIdentityFor("soa.battle.command.Segment"),
            .value = static_cast<std::int64_t>(state.segment),
        });
    const auto fake_remaining = builder.Add(
        TypeRef::Builtin(BuiltinType::U32), state.fake_remaining);
    const auto command_index = builder.Add(
        TypeRef::Builtin(BuiltinType::U32), state.command_index);
    const auto moves_remaining = builder.Add(
        TypeRef::Builtin(BuiltinType::U32), state.moves_remaining);
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.command.State"),
        {context, plan, segment, fake_remaining, command_index, moves_remaining});
    return std::move(builder).Finish(root);
}

ProgramValueGraph EncodePreparation(
    bool success,
    const BattleCommandState& state,
    std::string diagnostic)
{
    GraphBuilder builder;
    const auto ok = builder.Add(TypeRef::Builtin(BuiltinType::Bool), success);
    const auto encoded_state = builder.Import(EncodeBattleCommandState(state));
    const auto message = builder.Add(
        TypeRef::Named(SchemaIdentityFor("runtime.DiagnosticText")),
        std::move(diagnostic));
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.command.Preparation"),
        {ok, encoded_state, message});
    return std::move(builder).Finish(root);
}

ProgramValueGraph EncodeTransition(const BattleCommandState& state)
{
    GraphBuilder builder;
    const auto encoded_state = builder.Import(EncodeBattleCommandState(state));
    const auto segment = builder.Add(
        TypeRef::Named(SchemaIdentityFor("soa.battle.command.Segment")),
        EnumValue{
            .schema = SchemaIdentityFor("soa.battle.command.Segment"),
            .value = static_cast<std::int64_t>(state.segment),
        });
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.command.Transition"),
        {encoded_state, segment});
    return std::move(builder).Finish(root);
}

void AdvanceBattleCommandState(BattleCommandState& state)
{
    switch (state.segment)
    {
    case BattleCommandSegment::AwaitInputReady:
        if (state.fake_remaining != 0)
            state.segment = BattleCommandSegment::FakeAccept;
        else
            SelectCommandSegment(state);
        break;
    case BattleCommandSegment::FakeAccept:
        state.segment = BattleCommandSegment::FakeBack;
        break;
    case BattleCommandSegment::FakeBack:
        if (state.fake_remaining != 0) --state.fake_remaining;
        if (state.fake_remaining != 0)
            state.segment = BattleCommandSegment::FakeAccept;
        else
            SelectCommandSegment(state);
        break;
    case BattleCommandSegment::AttackAccept:
        state.segment = BattleCommandSegment::AttackTargetReady;
        break;
    case BattleCommandSegment::AttackTargetReady:
        state.segment = BattleCommandSegment::AttackTargetReadyConfirm;
        break;
    case BattleCommandSegment::AttackTargetReadyConfirm:
        state.segment = state.moves_remaining == 0
            ? BattleCommandSegment::AttackTargetAccept
            : BattleCommandSegment::AttackTargetDown;
        break;
    case BattleCommandSegment::AttackTargetDown:
        if (state.moves_remaining != 0) --state.moves_remaining;
        state.segment = state.moves_remaining == 0
            ? BattleCommandSegment::AttackTargetAccept
            : BattleCommandSegment::AttackTargetReadyBetween;
        break;
    case BattleCommandSegment::AttackTargetReadyBetween:
        state.segment = BattleCommandSegment::AttackTargetDown;
        break;
    case BattleCommandSegment::MainMenuMoveUp:
    case BattleCommandSegment::MainMenuMoveDown:
        if (state.moves_remaining != 0) --state.moves_remaining;
        state.segment = BattleCommandSegment::MainMenuTransition;
        break;
    case BattleCommandSegment::MainMenuTransition:
        if (state.moves_remaining == 0)
            state.segment = BattleCommandSegment::DirectCommandAccept;
        else if (state.plan.commands[state.command_index].macro ==
                 soa::battle::actions::BattleAction::Defend)
            state.segment = BattleCommandSegment::MainMenuMoveUp;
        else
            state.segment = BattleCommandSegment::MainMenuMoveDown;
        break;
    case BattleCommandSegment::AttackTargetAccept:
    case BattleCommandSegment::DirectCommandAccept:
        ++state.command_index;
        state.segment = state.command_index < state.plan.commands.size()
            ? BattleCommandSegment::AwaitNextInputReady
            : BattleCommandSegment::AwaitTurnReady;
        break;
    case BattleCommandSegment::AwaitNextInputReady:
        SelectCommandSegment(state);
        break;
    case BattleCommandSegment::AwaitTurnReady:
    case BattleCommandSegment::Complete:
        state.segment = BattleCommandSegment::Complete;
        break;
    }
}

} // namespace

std::optional<ProgramValueGraph> InvokeSourceReducer(
    const ExactDependencyIdentity& identity,
    std::span<const ProgramValueGraph> inputs,
    std::string* diagnostic)
{
    if (diagnostic != nullptr) diagnostic->clear();
    const auto derived_reducer = [&](std::string_view canonical_id) {
        return identity == BattleDerivedReducerIdentity(canonical_id);
    };
    const bool any_derived =
        derived_reducer("soa.battle.derived.current_turn") ||
        derived_reducer("soa.battle.derived.inventory_count") ||
        derived_reducer("soa.battle.derived.drop_count") ||
        derived_reducer("soa.battle.derived.player_count") ||
        derived_reducer("soa.battle.derived.enemy_count") ||
        derived_reducer("soa.battle.derived.player_min_position") ||
        derived_reducer("soa.battle.derived.player_max_position") ||
        derived_reducer("soa.battle.derived.enemy_min_position") ||
        derived_reducer("soa.battle.derived.enemy_max_position");
    if (any_derived)
    {
        if (inputs.empty())
            return std::nullopt;
        DecodedBattleDerivedSnapshot snapshot;
        if (!DecodeBattleDerivedSnapshot(inputs[0], snapshot))
        {
            if (diagnostic) *diagnostic =
                "Battle derived reducer received a malformed snapshot";
            return std::nullopt;
        }
        if (derived_reducer("soa.battle.derived.current_turn"))
            return inputs.size() == 1
                ? std::optional(ScalarU32(snapshot.current_turn))
                : std::nullopt;

        if (derived_reducer("soa.battle.derived.inventory_count") ||
            derived_reducer("soa.battle.derived.drop_count"))
        {
            if (inputs.size() != 2)
                return std::nullopt;
            const auto item_id = RootScalar<std::uint16_t>(inputs[1]);
            const bool inventory = derived_reducer(
                "soa.battle.derived.inventory_count");
            if (!item_id ||
                (inventory && snapshot.group != BattleDerivedGroup::TurnEntry) ||
                (!inventory && snapshot.group != BattleDerivedGroup::Rewards))
            {
                if (diagnostic) *diagnostic =
                    "Battle item reducer requires the matching refresh group";
                return std::nullopt;
            }
            const auto& totals = inventory
                ? snapshot.inventory
                : snapshot.drops;
            const auto found = std::ranges::find(
                totals, *item_id, &derived::BattleItemTotalV1::item_id);
            return ScalarU32(found == totals.end() ? 0u : found->count);
        }

        if (inputs.size() != 1 ||
            snapshot.group != BattleDerivedGroup::TurnOrder)
        {
            if (diagnostic) *diagnostic =
                "Battle turn-order reducer requires TurnIsReady evidence";
            return std::nullopt;
        }
        const bool players =
            derived_reducer("soa.battle.derived.player_count") ||
            derived_reducer("soa.battle.derived.player_min_position") ||
            derived_reducer("soa.battle.derived.player_max_position");
        std::vector<std::uint32_t> positions;
        for (std::uint32_t position = 0;
             position < snapshot.turn_order.size(); ++position)
        {
            const bool player_slot = snapshot.turn_order[position] < 4;
            if (player_slot == players)
                positions.push_back(position);
        }
        if (derived_reducer("soa.battle.derived.player_count") ||
            derived_reducer("soa.battle.derived.enemy_count"))
        {
            return ScalarU32(static_cast<std::uint32_t>(positions.size()));
        }
        if (positions.empty())
        {
            if (diagnostic) *diagnostic =
                "Battle turn-order cohort is empty";
            return std::nullopt;
        }
        const bool minimum =
            derived_reducer("soa.battle.derived.player_min_position") ||
            derived_reducer("soa.battle.derived.enemy_min_position");
        return ScalarU32(minimum ? positions.front() : positions.back());
    }
    if (identity == CanonicalReducerIdentity(
            CanonicalReducer::BattlePrepareCommandInteraction))
    {
        if (inputs.size() != 2) return std::nullopt;
        BattleCommandState state;
        if (!DecodeBattleContext(inputs[0], state.context) ||
            !DecodeTurnSpec(inputs[1], state.plan))
        {
            if (diagnostic) *diagnostic = "battle command preparation received malformed typed input";
            return std::nullopt;
        }
        state.fake_remaining = state.plan.fake_attack_count;
        const auto validation = soa::battle::actions::ValidateBattleTurnPlan(
            state.context, state.plan);
        return EncodePreparation(
            static_cast<bool>(validation),
            state,
            validation ? std::string{} : validation.diagnostic);
    }
    if (identity == CanonicalReducerIdentity(
            CanonicalReducer::BattleCommandInteractionInitialize))
    {
        if (inputs.size() != 5) return std::nullopt;
        BattleCommandState state;
        return DecodeBattleCommandState(inputs[0], state)
            ? std::optional(inputs[0])
            : std::nullopt;
    }
    if (identity == CanonicalReducerIdentity(
            CanonicalReducer::BattleCommandInteractionAdvance))
    {
        if (inputs.size() != 2) return std::nullopt;
        BattleCommandState state;
        if (!DecodeBattleCommandState(inputs[0], state)) return std::nullopt;
        AdvanceBattleCommandState(state);
        return EncodeTransition(state);
    }
    if (identity == CanonicalReducerIdentity(
            CanonicalReducer::BattleCommandInteractionCompleteSegment))
    {
        return inputs.size() == 2 ? std::optional(inputs[1]) : std::nullopt;
    }
    if (identity == CanonicalReducerIdentity(
            CanonicalReducer::BattleCommandInteractionFinalize))
    {
        if (inputs.size() != 2) return std::nullopt;
        return inputs[1];
    }
    if (diagnostic != nullptr)
        *diagnostic = "unknown source reducer identity";
    return std::nullopt;
}

} // namespace savor::runtime::program::capabilities
