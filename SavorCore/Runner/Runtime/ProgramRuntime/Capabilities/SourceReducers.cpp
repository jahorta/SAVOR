#include "SourceReducers.h"

#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Core/Input/SoaBattle/BattlePlanValidation.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
#include <stdexcept>
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
