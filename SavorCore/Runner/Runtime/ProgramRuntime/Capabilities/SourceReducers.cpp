#include "SourceReducers.h"

#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Core/Input/SoaBattle/ActionLibrary.h"

#include <algorithm>
#include <cstring>
#include <map>
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
    if (!DecodeBytes(reader, context->fields[1], decoded.state) ||
        turn_type == nullptr ||
        turn_type->schema != SchemaIdentityFor("soa.battle.TurnType") ||
        turn_type->value < 0 || turn_type->value > 2 ||
        !Scalar(reader, context->fields[3], decoded.turn_count) ||
        !Scalar(reader, context->fields[4], decoded.battle_phase))
    {
        return false;
    }
    decoded.turn_type =
        static_cast<soa::battle::TurnType>(turn_type->value);
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
        if (!Scalar(reader, record->fields[0], command.actor_slot) ||
            action == nullptr ||
            action->schema != SchemaIdentityFor("soa.battle.BattleAction") ||
            action->value < 0 || action->value > 4 ||
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
        command.macro =
            static_cast<soa::battle::actions::BattleAction>(
                action->value);
        output.commands.push_back(command);
    }
    return true;
}

ProgramValueGraph EncodeMaterialization(
    bool success,
    const savor::ControllerInputSequence& sequence,
    std::string diagnostic)
{
    GraphBuilder builder;
    const auto success_id = builder.Add(
        TypeRef::Builtin(BuiltinType::Bool),
        success);
    std::vector<ProgramValueId> frame_ids;
    frame_ids.reserve(sequence.size());
    for (const auto& frame : sequence)
    {
        std::vector<ProgramValueId> fields;
        fields.reserve(7);
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U16),
            frame.buttons));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.main_x));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.main_y));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.c_x));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.c_y));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.trig_l));
        fields.push_back(builder.Add(
            TypeRef::Builtin(BuiltinType::U8),
            frame.trig_r));
        frame_ids.push_back(builder.AddRecord(
            SchemaIdentityFor("runtime.input.GCInputFrame"),
            std::move(fields)));
    }
    const auto sequence_id = builder.AddList(
        SchemaIdentityFor("runtime.input.GCInputSequence"),
        std::move(frame_ids));
    const auto diagnostic_id = builder.Add(
        TypeRef::Named(SchemaIdentityFor("runtime.DiagnosticText")),
        std::move(diagnostic));
    const auto root = builder.AddRecord(
        SchemaIdentityFor("soa.battle.TurnInputMaterialization"),
        {success_id, sequence_id, diagnostic_id});
    return std::move(builder).Finish(root);
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

bool DecodeTurnInputMaterializationValue(
    const ProgramValueGraph& graph,
    bool& success,
    savor::ControllerInputSequence& sequence,
    std::string* domain_diagnostic)
{
    GraphReader reader(graph);
    if (!reader.valid()) return false;
    const auto* result = reader.RootRecord(
        SchemaIdentityFor("soa.battle.TurnInputMaterialization"),
        3);
    if (result == nullptr ||
        !Scalar(reader, result->fields[0], success))
    {
        return false;
    }
    const auto* frames = reader.PayloadOf<ListValue>(result->fields[1]);
    const auto* diagnostic =
        reader.PayloadOf<std::string>(result->fields[2]);
    if (frames == nullptr || diagnostic == nullptr)
        return false;

    sequence.clear();
    sequence.reserve(frames->elements.size());
    for (const auto id : frames->elements)
    {
        const auto* frame = reader.PayloadOf<RecordValue>(id);
        if (frame == nullptr || frame->fields.size() != 7)
            return false;
        savor::GCInputFrame value{};
        if (!Scalar(reader, frame->fields[0], value.buttons) ||
            !Scalar(reader, frame->fields[1], value.main_x) ||
            !Scalar(reader, frame->fields[2], value.main_y) ||
            !Scalar(reader, frame->fields[3], value.c_x) ||
            !Scalar(reader, frame->fields[4], value.c_y) ||
            !Scalar(reader, frame->fields[5], value.trig_l) ||
            !Scalar(reader, frame->fields[6], value.trig_r))
        {
            return false;
        }
        sequence.push_back(value);
    }
    if (domain_diagnostic != nullptr)
        *domain_diagnostic = *diagnostic;
    return true;
}

std::optional<ProgramValueGraph> InvokeSourceReducer(
    const ExactDependencyIdentity& identity,
    std::span<const ProgramValueGraph> inputs,
    std::string* diagnostic)
{
    if (diagnostic != nullptr) diagnostic->clear();
    if (identity != CanonicalReducerIdentity(
        CanonicalReducer::BattleMaterializeTurnInput))
    {
        if (diagnostic != nullptr)
            *diagnostic = "unknown source reducer identity";
        return std::nullopt;
    }
    if (inputs.size() != 2)
    {
        if (diagnostic != nullptr)
            *diagnostic = "materialize_turn_input requires two typed inputs";
        return std::nullopt;
    }

    soa::battle::ctx::BattleContext context{};
    soa::battle::actions::BattleTurnExecutionSpec specification{};
    if (!DecodeBattleContext(inputs[0], context) ||
        !DecodeTurnSpec(inputs[1], specification))
    {
        if (diagnostic != nullptr)
            *diagnostic = "materialize_turn_input received malformed typed input";
        return std::nullopt;
    }

    savor::ControllerInputSequence sequence;
    soa::battle::actions::MaterializeErr error =
        soa::battle::actions::MaterializeErr::OK;
    const bool success =
        soa::battle::actions::MaterializeBattleTurnInputs(
            context,
            specification,
            sequence,
            error);
    return EncodeMaterialization(
        success,
        sequence,
        success
            ? std::string{}
            : soa::battle::actions::get_materialize_err_string(error));
}

} // namespace savor::runtime::program::capabilities
