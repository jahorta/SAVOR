#include "SessionProgramActionHost.h"

#include "CanonicalActionPayload.h"
#include "../Capabilities/SourceCapabilityPacks.h"
#include "../Capabilities/SourceReducers.h"
#include "../Model/ProgramValueArena.h"
#include "../Registry/CanonicalActionCatalog.h"
#include "../../EmulationSession.h"
#include "Core/Memory/MemView.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Core/Memory/Soa/SoaStructReaders.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace savor::runtime::program {
namespace {

using Field = CanonicalActionPayloadField;

constexpr ResourceServiceId kStateService{1};
constexpr ResourceServiceId kExecutionService{2};
constexpr ResourceServiceId kStopPointService{3};
constexpr ResourceServiceId kInputService{4};
constexpr ResourceServiceId kMovieService{5};
constexpr ResourceServiceId kMutationService{6};
constexpr ResourceServiceId kCaptureService{7};

constexpr std::uint32_t kMaximumStopAlternatives = 128;
constexpr std::uint32_t kMaximumInputFrames = 65536;

[[nodiscard]] bool CompleteSha256(std::string_view value) noexcept
{
    return value.size() == 64 &&
        std::ranges::all_of(
            value,
            [](char ch)
            {
                return (ch >= '0' && ch <= '9') ||
                    (ch >= 'a' && ch <= 'f');
            });
}

struct EnumFieldBound
{
    Field field;
    std::uint64_t exclusive_upper_bound = 0;
};

constexpr std::array kEnumFieldBounds{
    EnumFieldBound{
        Field::EpochPolicy,
        static_cast<std::uint64_t>(
            StopEpochPolicy::RebindAfterRestore) +
            1},
    EnumFieldBound{
        Field::Delivery,
        static_cast<std::uint64_t>(
            StopDeliveryMode::Wake) +
            1},
    EnumFieldBound{
        Field::RoutingPolicy,
        static_cast<std::uint64_t>(
            StopRoutingPolicy::Fail) +
            1},
    EnumFieldBound{
        Field::Lifetime,
        static_cast<std::uint64_t>(
            StopSubscriptionLifetime::OneShot) +
            1},
    EnumFieldBound{
        Field::MovieEndedPolicy,
        static_cast<std::uint64_t>(
            MovieEndedPolicy::Fail) +
            1},
    EnumFieldBound{
        Field::ThrottlePolicy,
        static_cast<std::uint64_t>(
            ExecutionThrottlePolicy::RequireDisabled) +
            1},
    EnumFieldBound{
        Field::CurrentPointPolicy,
        static_cast<std::uint64_t>(
            ExecutionCurrentPointPolicy::Require) +
            1},
    EnumFieldBound{
        Field::InterruptionPolicy,
        static_cast<std::uint64_t>(
            ExecutionInterruptionPolicy::AllowKnown) +
            1},
    EnumFieldBound{
        Field::TelemetrySeverity,
        static_cast<std::uint64_t>(
            TelemetrySeverity::Error) +
            1},
};

std::optional<Field> InvalidEnumField(
    const CanonicalActionPayload& payload)
{
    for (const EnumFieldBound& bound : kEnumFieldBounds)
    {
        if (!payload.Contains(bound.field))
            continue;
        const auto value = payload.Unsigned(bound.field);
        if (!value || *value >= bound.exclusive_upper_bound)
            return bound.field;
    }
    return std::nullopt;
}

struct ContextRequestEvidence
{
    std::uint64_t stop_sequence = 0;
    StateEpoch epoch;
    std::uint32_t expected_pc = 0;
};

const ProgramValue* FindValue(
    const ProgramValueGraph& graph,
    ProgramValueId id)
{
    const auto found = std::ranges::find(
        graph.values,
        id,
        &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

const ProgramValue* RootValue(const ProgramValueGraph& graph)
{
    return FindValue(graph, graph.root);
}

template <typename T>
bool ScalarValue(
    const ProgramValueGraph& graph,
    ProgramValueId id,
    T& output)
{
    const ProgramValue* value = FindValue(graph, id);
    const T* scalar = value
        ? std::get_if<T>(&value->payload)
        : nullptr;
    if (!scalar)
        return false;
    output = *scalar;
    return true;
}

bool DecodeContextRequest(
    const ProgramValueGraph& graph,
    std::string_view expected_schema,
    ContextRequestEvidence& output)
{
    const ProgramValue* root = RootValue(graph);
    const auto* record = root
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    if (!root || !root->type.named ||
        root->type.named->canonical_id != expected_schema ||
        !record || record->fields.size() != 3)
    {
        return false;
    }

    std::uint64_t epoch = 0;
    return ScalarValue(
               graph,
               record->fields[0],
               output.stop_sequence) &&
        ScalarValue(graph, record->fields[1], epoch) &&
        ScalarValue(
            graph,
            record->fields[2],
            output.expected_pc) &&
        output.stop_sequence != 0 &&
        epoch != 0 &&
        (output.epoch = StateEpoch(epoch), true);
}

bool IsMem1Range(std::uint32_t address, std::size_t size)
{
    if (address < savor::MemView::kMem1Base)
        return false;
    const std::uint64_t offset =
        static_cast<std::uint64_t>(address) -
        savor::MemView::kMem1Base;
    return offset + size <= savor::MemView::kMem1Size;
}

template <typename T>
bool ReadGuestStruct(
    GuestMemory& memory,
    std::uint32_t address,
    StateEpoch epoch,
    T& output)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!IsMem1Range(address, sizeof(T)))
        return false;
    GuestBytesResult bytes =
        memory.ReadBytes(address, sizeof(T), epoch);
    if (!bytes.result.ok || bytes.bytes.size() != sizeof(T))
        return false;
    const std::string image(
        reinterpret_cast<const char*>(bytes.bytes.data()),
        bytes.bytes.size());
    return soa::readers::read(image, output);
}

bool ReadGuestScalar(
    GuestMemory& memory,
    std::uint32_t address,
    GuestScalarWidth width,
    StateEpoch epoch,
    std::uint64_t& output)
{
    const GuestReadReceipt read =
        memory.ReadScalar(address, width, epoch);
    if (!read.ok)
        return false;
    output = read.value;
    return true;
}

bool ReadBattleContext(
    GuestMemory& memory,
    StateEpoch epoch,
    soa::battle::ctx::BattleContext& output,
    std::string& diagnostic)
{
    using namespace soa::battle::ctx;
    BattleContext candidate{};
    for (int index = 0; index < SLOT_COUNT; ++index)
    {
        auto& slot = candidate.slots_[index];
        slot = {};
        slot.is_player = index < 4 ? 1 : 0;

        std::uint64_t instance_address = 0;
        if (!ReadGuestScalar(
                memory,
                addr::AddrRegistry::base(
                    addr::battle::CombatantInstancesTable) +
                    static_cast<std::uint32_t>(index * 4),
                GuestScalarWidth::U32,
                epoch,
                instance_address))
        {
            diagnostic = "battle combatant pointer table is unavailable";
            return false;
        }
        if (instance_address != 0 &&
            IsMem1Range(
                static_cast<std::uint32_t>(instance_address),
                sizeof(slot.instance)))
        {
            slot.instance_addr =
                static_cast<std::uint32_t>(instance_address);
            if (!ReadGuestStruct(
                    memory,
                    slot.instance_addr,
                    epoch,
                    slot.instance))
            {
                diagnostic = "battle combatant snapshot is unavailable";
                return false;
            }
            slot.present =
                (slot.instance.status_flags & StatusFlags::Fled) == 0;
            slot.is_alive =
                (slot.instance.status_flags & StatusFlags::Dead) == 0;
        }

        std::uint64_t combatant_id = 0;
        if (!ReadGuestScalar(
                memory,
                addr::AddrRegistry::base(
                    addr::battle::CombatantIdTable) +
                    static_cast<std::uint32_t>(index * 2),
                GuestScalarWidth::U16,
                epoch,
                combatant_id))
        {
            diagnostic = "battle combatant ID table is unavailable";
            return false;
        }
        slot.id = static_cast<std::uint16_t>(combatant_id);

        if (index >= 4 && slot.present &&
            IsMem1Range(
                slot.instance.Enemy_Definition,
                sizeof(slot.enemy_def)))
        {
            slot.enemy_def_addr = slot.instance.Enemy_Definition;
            if (!ReadGuestStruct(
                    memory,
                    slot.enemy_def_addr,
                    epoch,
                    slot.enemy_def))
            {
                diagnostic = "battle enemy definition is unavailable";
                return false;
            }
            slot.has_enemy_def = 1;
        }
    }

    std::uint64_t state_address = 0;
    if (!ReadGuestScalar(
            memory,
            addr::AddrRegistry::base(addr::battle::MainInstancePtr),
            GuestScalarWidth::U32,
            epoch,
            state_address))
    {
        diagnostic = "battle state pointer is unavailable";
        return false;
    }
    if (state_address != 0 &&
        IsMem1Range(
            static_cast<std::uint32_t>(state_address),
            sizeof(candidate.state)) &&
        !ReadGuestStruct(
            memory,
            static_cast<std::uint32_t>(state_address),
            epoch,
            candidate.state))
    {
        diagnostic = "battle state snapshot is unavailable";
        return false;
    }

    std::uint64_t turn_type = 0;
    std::uint64_t battle_phase = 0;
    std::uint64_t current_turn = 0;
    if (!ReadGuestScalar(
            memory,
            addr::AddrRegistry::base(addr::battle::TurnType),
            GuestScalarWidth::U32,
            epoch,
            turn_type) ||
        !ReadGuestScalar(
            memory,
            addr::AddrRegistry::base(addr::battle::BattlePhase),
            GuestScalarWidth::U32,
            epoch,
            battle_phase) ||
        !ReadGuestScalar(
            memory,
            addr::AddrRegistry::base(addr::battle::CurrentTurn),
            GuestScalarWidth::U8,
            epoch,
            current_turn))
    {
        diagnostic = "battle phase fields are unavailable";
        return false;
    }
    candidate.turn_type =
        static_cast<soa::battle::TurnType>(turn_type);
    candidate.battle_phase =
        static_cast<std::uint32_t>(battle_phase);
    candidate.turn_count =
        static_cast<std::uint32_t>(current_turn);
    output = std::move(candidate);
    return true;
}

bool ReadNavigationContext(
    GuestMemory& memory,
    StateEpoch epoch,
    std::uint32_t capture_pc,
    soa::navigation::ctx::NavigationContext& output,
    std::string& diagnostic)
{
    using namespace soa::navigation::ctx;
    if (capture_pc != CapturePc)
    {
        diagnostic = "navigation capture PC does not match";
        return false;
    }

    auto read = [&](std::uint32_t address,
                    GuestScalarWidth width,
                    std::uint64_t& value) {
        return ReadGuestScalar(
            memory,
            address,
            width,
            epoch,
            value);
    };
    auto read_u32 = [&](std::uint32_t address,
                        std::uint32_t& value) {
        std::uint64_t scalar = 0;
        if (!read(address, GuestScalarWidth::U32, scalar))
            return false;
        value = static_cast<std::uint32_t>(scalar);
        return true;
    };
    auto read_u16 = [&](std::uint32_t address,
                        std::uint16_t& value) {
        std::uint64_t scalar = 0;
        if (!read(address, GuestScalarWidth::U16, scalar))
            return false;
        value = static_cast<std::uint16_t>(scalar);
        return true;
    };
    auto read_u8 = [&](std::uint32_t address,
                       std::uint8_t& value) {
        std::uint64_t scalar = 0;
        if (!read(address, GuestScalarWidth::U8, scalar))
            return false;
        value = static_cast<std::uint8_t>(scalar);
        return true;
    };
    auto read_f32 = [&](std::uint32_t address, float& value) {
        std::uint32_t bits = 0;
        if (!read_u32(address, bits))
            return false;
        value = std::bit_cast<float>(bits);
        return true;
    };

    NavigationContext candidate{};
    candidate.capture_pc = capture_pc;
    if (!read_u32(
            PlayerWorksheetPointerAddress,
            candidate.player_worksheet) ||
        !read_u32(AreaAddress, candidate.area) ||
        !read_u8(SubareaAddress, candidate.subarea) ||
        !read_u32(
            PostInputMovementSuppressAddress,
            candidate.post_input_movement_suppress) ||
        !read_f32(
            StepDistanceCarryInAddress,
            candidate.step_distance_carry_in))
    {
        diagnostic = "navigation fixed fields are unavailable";
        return false;
    }
    const std::uint32_t worksheet = candidate.player_worksheet;
    if (!IsMem1Range(worksheet, 0x1bcu))
    {
        diagnostic = "navigation player worksheet is invalid";
        return false;
    }

    std::uint32_t ground_selector = 0;
    if (!read_u16(worksheet + 0x170u, candidate.motion_state) ||
        !read_u16(
            worksheet + 0x172u,
            candidate.motion_substate) ||
        !read_f32(worksheet + 0x38u, candidate.position_x) ||
        !read_f32(worksheet + 0x3cu, candidate.position_y) ||
        !read_f32(worksheet + 0x40u, candidate.position_z) ||
        !read_u32(
            worksheet + 0x44u,
            candidate.rotation_x_raw) ||
        !read_u32(
            worksheet + 0x48u,
            candidate.rotation_y_raw) ||
        !read_u32(
            worksheet + 0x4cu,
            candidate.rotation_z_raw) ||
        !read_f32(
            worksheet + 0x104u,
            candidate.previous_position_x) ||
        !read_f32(
            worksheet + 0x108u,
            candidate.previous_position_y) ||
        !read_f32(
            worksheet + 0x10cu,
            candidate.previous_position_z) ||
        !read_u32(
            worksheet + 0x110u,
            candidate.previous_rotation_x_raw) ||
        !read_u32(
            worksheet + 0x114u,
            candidate.previous_rotation_y_raw) ||
        !read_u32(
            worksheet + 0x118u,
            candidate.previous_rotation_z_raw) ||
        !read_u32(
            worksheet + GroundSelectorPointerOffset,
            ground_selector))
    {
        diagnostic = "navigation player worksheet fields are unavailable";
        return false;
    }
    if (candidate.motion_state != 1u)
    {
        diagnostic = "navigation motion state is not ordinary state 1";
        return false;
    }
    if (ground_selector != 0)
    {
        if (!IsMem1Range(
                ground_selector,
                GroundSelectorRecordSize) ||
            !read_u16(
                ground_selector + GroundTblIdOffset,
                candidate.ground_tbl_id))
        {
            diagnostic = "navigation ground selector is invalid";
            return false;
        }
        candidate.has_ground = true;
    }
    output = std::move(candidate);
    return true;
}

std::optional<CanonicalAction> ResolveCanonicalAction(
    const ExactDependencyIdentity& identity)
{
    for (std::size_t index = 0;
         index <= static_cast<std::size_t>(
             CanonicalAction::TelemetryEmit);
         ++index)
    {
        const auto action = static_cast<CanonicalAction>(index);
        if (CanonicalActionIdentity(action) == identity)
            return action;
    }
    return std::nullopt;
}

std::optional<ActionEffectMask> ResolveActionEffects(
    const ExactDependencyIdentity& identity)
{
    static const std::vector<ActionDescriptor> canonical =
        BuildCanonicalRuntimeActionDescriptors();
    const auto canonical_action = std::ranges::find(
        canonical,
        identity,
        &ActionDescriptor::identity);
    if (canonical_action != canonical.end())
        return canonical_action->effects;

    static const std::vector<ActionDescriptor> source =
        capabilities::BuildSourceCapabilityPackCatalog().actions;
    const auto source_action = std::ranges::find(
        source,
        identity,
        &ActionDescriptor::identity);
    return source_action == source.end()
        ? std::nullopt
        : std::optional<ActionEffectMask>(
              source_action->effects);
}

std::uint64_t UnsignedOr(
    const CanonicalActionPayload& payload,
    Field field,
    std::uint64_t fallback)
{
    const auto value = payload.Unsigned(field);
    return value ? *value : fallback;
}

bool BooleanOr(
    const CanonicalActionPayload& payload,
    Field field,
    bool fallback)
{
    const auto value = payload.Boolean(field);
    return value ? *value : fallback;
}

std::vector<Byte> EncodeInputFrame(
    const savor::GCInputFrame& frame)
{
    return {
        static_cast<Byte>(frame.buttons),
        static_cast<Byte>(frame.buttons >> 8u),
        frame.main_x,
        frame.main_y,
        frame.c_x,
        frame.c_y,
        frame.trig_l,
        frame.trig_r};
}

bool DecodeInputFrame(
    std::span<const Byte> bytes,
    savor::GCInputFrame& frame)
{
    if (bytes.size() != 8)
        return false;
    frame.buttons = static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8u);
    frame.main_x = bytes[2];
    frame.main_y = bytes[3];
    frame.c_x = bytes[4];
    frame.c_y = bytes[5];
    frame.trig_l = bytes[6];
    frame.trig_r = bytes[7];
    return true;
}

bool DecodeInputFrames(
    std::span<const Byte> bytes,
    std::vector<savor::GCInputFrame>& frames)
{
    if (bytes.empty() || bytes.size() % 8 != 0 ||
        bytes.size() / 8 > kMaximumInputFrames)
    {
        return false;
    }
    frames.clear();
    frames.reserve(bytes.size() / 8);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8)
    {
        savor::GCInputFrame frame{};
        if (!DecodeInputFrame(bytes.subspan(offset, 8), frame))
            return false;
        frames.push_back(frame);
    }
    return true;
}

bool AddInputPublicationResult(
    CanonicalActionPayload& payload,
    const InputPublicationEvidence& publication)
{
    return publication.lease &&
        publication.publication &&
        publication.epoch &&
        payload.AddUnsigned(
            Field::Handle,
            publication.lease.value()) &&
        payload.AddUnsigned(
            Field::Publication,
            publication.publication.value()) &&
        payload.AddUnsigned(
            Field::ResultEpoch,
            publication.epoch.value()) &&
        payload.AddBytes(
            Field::ResultFrame,
            EncodeInputFrame(publication.frame));
}

std::optional<InputPublicationEvidence>
InputPublicationEvidenceFromPayload(
    const CanonicalActionPayload& payload)
{
    const auto lease = payload.Unsigned(Field::ParentHandle);
    const auto publication =
        payload.Unsigned(Field::Publication);
    const auto epoch = payload.Unsigned(Field::ResultEpoch);
    const auto encoded = payload.Bytes(Field::ResultFrame);
    savor::GCInputFrame frame{};
    if (!lease || !publication || !epoch || !encoded ||
        !DecodeInputFrame(*encoded, frame))
    {
        return std::nullopt;
    }
    return InputPublicationEvidence{
        InputLeaseId(*lease),
        InputPublicationToken(*publication),
        StateEpoch(*epoch),
        frame};
}

bool UsesTypedRequestRecord(CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
    case CanonicalAction::ExecutionStepFrames:
    case CanonicalAction::StopPointsSubscribeGroup:
    case CanonicalAction::InputAcquireLease:
    case CanonicalAction::InputPublishHeld:
    case CanonicalAction::InputPublishPulse:
    case CanonicalAction::InputNeutralize:
    case CanonicalAction::InputPublishSequence:
    case CanonicalAction::InputAwaitGuestPoll:
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
    case CanonicalAction::GuestRunCoherentQuery:
        return true;
    default:
        return false;
    }
}

class StaticConfigReader final
{
public:
    StaticConfigReader(
        std::span<const Byte> bytes,
        std::array<char, 4> expected_magic)
        : bytes_(bytes)
    {
        if (bytes_.size() < expected_magic.size())
            return;
        for (std::size_t index = 0;
             index < expected_magic.size();
             ++index)
        {
            if (bytes_[index] !=
                static_cast<Byte>(expected_magic[index]))
            {
                return;
            }
        }
        offset_ = expected_magic.size();
        valid_ = true;
    }

    [[nodiscard]] bool U8(std::uint8_t& value)
    {
        if (!Take(1))
            return false;
        value = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool Bool(bool& value)
    {
        std::uint8_t encoded = 0;
        if (!U8(encoded) || encoded > 1)
            return false;
        value = encoded != 0;
        return true;
    }

    [[nodiscard]] bool U32(std::uint32_t& value)
    {
        if (!Take(4))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_++])
                << shift;
        }
        return true;
    }

    [[nodiscard]] bool U64(std::uint64_t& value)
    {
        if (!Take(8))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
        {
            value |= static_cast<std::uint64_t>(
                         bytes_[offset_++])
                << shift;
        }
        return true;
    }

    [[nodiscard]] bool String(
        std::string& value,
        std::size_t maximum = 4096)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > maximum || !Take(size))
            return false;
        value.assign(
            reinterpret_cast<const char*>(
                bytes_.data() + offset_),
            size);
        offset_ += size;
        return !value.empty() &&
            std::ranges::none_of(
                value,
                [](unsigned char character) {
                    return character == 0;
                });
    }

    [[nodiscard]] bool Hash(ContentHash256& value)
    {
        if (!Take(value.bytes.size()))
            return false;
        std::ranges::copy(
            bytes_.subspan(offset_, value.bytes.size()),
            value.bytes.begin());
        offset_ += value.bytes.size();
        return !value.empty();
    }

    [[nodiscard]] bool Type(TypeRef& value)
    {
        bool named = false;
        if (!Bool(named))
            return false;
        if (!named)
        {
            std::uint8_t builtin = 0;
            if (!U8(builtin) ||
                builtin >
                    static_cast<std::uint8_t>(
                        BuiltinType::F64))
            {
                return false;
            }
            value = TypeRef::Builtin(
                static_cast<BuiltinType>(builtin));
            return true;
        }
        SchemaIdentity identity;
        if (!String(identity.canonical_id) ||
            !U32(identity.version) ||
            identity.version == 0 ||
            !Hash(identity.schema_hash))
        {
            return false;
        }
        value = TypeRef::Named(std::move(identity));
        return true;
    }

    [[nodiscard]] bool done() const noexcept
    {
        return valid_ && offset_ == bytes_.size();
    }

private:
    [[nodiscard]] bool Take(std::size_t count) const noexcept
    {
        return valid_ && count <= bytes_.size() - offset_;
    }

    std::span<const Byte> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = false;
};

const ProgramValue* RequireTypedValue(
    const ProgramValueGraph& graph,
    ProgramValueId id,
    const TypeRef& expected)
{
    const ProgramValue* value = FindValue(graph, id);
    return value && value->type == expected ? value : nullptr;
}

const std::vector<Byte>* NamedBytes(
    const ProgramValueGraph& graph,
    ProgramValueId id,
    CanonicalRuntimeSchema schema)
{
    const ProgramValue* value = RequireTypedValue(
        graph,
        id,
        CanonicalRuntimeType(schema));
    return value
        ? std::get_if<std::vector<Byte>>(&value->payload)
        : nullptr;
}

bool OptionalElement(
    const ProgramValueGraph& graph,
    ProgramValueId id,
    CanonicalRuntimeSchema schema,
    const ProgramValue*& element)
{
    const ProgramValue* value = RequireTypedValue(
        graph,
        id,
        CanonicalRuntimeType(schema));
    const auto* optional = value
        ? std::get_if<OptionalValue>(&value->payload)
        : nullptr;
    if (!optional)
        return false;
    element = optional->value
        ? FindValue(graph, *optional->value)
        : nullptr;
    return !optional->value || element != nullptr;
}

bool DecodeReceiptPayload(
    const ProgramValue& value,
    CanonicalAction producer,
    CanonicalActionPayload& payload)
{
    if (value.type != CanonicalActionOutputType(producer))
        return false;
    ProgramValueGraph leaf{value.id, {value}};
    const auto schema =
        CanonicalActionOutputSchemaIdentity(producer);
    return schema &&
        DecodeCanonicalActionPayload(
            leaf,
            *schema,
            payload);
}

bool AddResourceHandle(
    const ProgramValue& value,
    CanonicalAction producer,
    CanonicalActionPayload& payload)
{
    if (value.type != CanonicalActionOutputType(producer))
        return false;
    const auto* handle =
        std::get_if<ResourceHandleValue>(&value.payload);
    return handle && handle->handle_id &&
        payload.AddUnsigned(
            Field::Handle,
            handle->handle_id.value());
}

bool DecodeStopReceipt(
    const ProgramValueGraph& graph,
    const ProgramValue& value,
    StateEpoch current_epoch,
    CanonicalActionPayload& payload)
{
    if (value.type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))
    {
        return false;
    }
    const auto* record =
        std::get_if<RecordValue>(&value.payload);
    if (!record || record->fields.size() != 5)
        return false;
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint32_t pc = 0;
    std::uint64_t sample = 0;
    const ProgramValue* evidence = RequireTypedValue(
        graph,
        record->fields[4],
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload));
    const auto* evidence_bytes = evidence
        ? std::get_if<std::vector<Byte>>(
              &evidence->payload)
        : nullptr;
    return ScalarValue(
               graph,
               record->fields[0],
               sequence) &&
        ScalarValue(graph, record->fields[1], epoch) &&
        ScalarValue(graph, record->fields[2], pc) &&
        ScalarValue(graph, record->fields[3], sample) &&
        evidence_bytes && evidence_bytes->size() >= 4 &&
        (*evidence_bytes)[0] == static_cast<Byte>('R') &&
        (*evidence_bytes)[1] == static_cast<Byte>('S') &&
        (*evidence_bytes)[2] == static_cast<Byte>('E') &&
        (*evidence_bytes)[3] == static_cast<Byte>('1') &&
        sequence != 0 && sample != 0 && pc != 0 &&
        epoch == current_epoch.value() &&
        payload.AddUnsigned(
            Field::ResultStopSequence,
            sequence) &&
        payload.AddUnsigned(Field::ResultEpoch, epoch) &&
        payload.AddUnsigned(Field::ResultPc, pc);
}

const SemanticPointDescriptor* FindSourcePoint(
    const CapabilityPackIdentity& pack,
    std::string_view canonical_id)
{
    static const capabilities::SourceCapabilityPackCatalog catalog =
        capabilities::BuildSourceCapabilityPackCatalog();
    const auto manifest = std::ranges::find(
        catalog.manifests,
        pack,
        &CapabilityPackManifest::identity);
    if (manifest == catalog.manifests.end())
        return nullptr;
    const auto point = std::ranges::find(
        manifest->semantic_points,
        canonical_id,
        &SemanticPointDescriptor::canonical_id);
    return point == manifest->semantic_points.end()
        ? nullptr
        : &*point;
}

bool SourceEvaluatorExists(
    std::string_view canonical_id,
    std::uint32_t maximum_bytes)
{
    static const capabilities::SourceCapabilityPackCatalog catalog =
        capabilities::BuildSourceCapabilityPackCatalog();
    for (const CapabilityPackManifest& manifest :
         catalog.manifests)
    {
        const auto found = std::ranges::find(
            manifest.cpu_evaluators,
            canonical_id,
            &CpuEvaluatorDescriptor::canonical_id);
        if (found != manifest.cpu_evaluators.end())
        {
            return maximum_bytes != 0 &&
                maximum_bytes <= found->maximum_output_bytes;
        }
    }
    return false;
}

bool DecodeStopGroupConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'S', 'G', 'C', '1'});
    std::uint32_t alternative_count = 0;
    if (!reader.U32(alternative_count) ||
        alternative_count == 0 ||
        alternative_count > kMaximumStopAlternatives)
    {
        diagnostic = "SGC1 has an invalid alternative count";
        return false;
    }

    std::vector<Byte> pcs;
    pcs.reserve(
        static_cast<std::size_t>(alternative_count) * 4);
    for (std::uint32_t index = 0;
         index < alternative_count;
         ++index)
    {
        CapabilityPackIdentity pack;
        std::string point_id;
        std::uint8_t kind = 0;
        std::uint32_t pc = 0;
        if (!reader.String(pack.canonical_id) ||
            !reader.U32(pack.version) ||
            pack.version == 0 ||
            !reader.Hash(pack.manifest_hash) ||
            !reader.String(point_id) ||
            !reader.U8(kind) ||
            !reader.U32(pc))
        {
            diagnostic = "SGC1 contains a malformed semantic point";
            return false;
        }
        const SemanticPointDescriptor* point =
            FindSourcePoint(pack, point_id);
        if (!point ||
            kind != static_cast<std::uint8_t>(
                        SemanticPointPhysicalKind::
                            ProgramCounter) ||
            point->kind !=
                SemanticPointPhysicalKind::ProgramCounter ||
            point->pc == 0 || point->pc != pc)
        {
            diagnostic =
                "SGC1 semantic point is not an exact registered PC point";
            return false;
        }
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            pcs.push_back(
                static_cast<Byte>((pc >> shift) & 0xffu));
        }
    }

    std::uint32_t sample_count = 0;
    if (!reader.U32(sample_count) ||
        sample_count > kMaxRoutedHitSamples)
    {
        diagnostic = "SGC1 has an invalid hit-time sample count";
        return false;
    }
    for (std::uint32_t index = 0; index < sample_count; ++index)
    {
        std::string evaluator;
        TypeRef type;
        std::uint32_t maximum_bytes = 0;
        bool required = false;
        if (!reader.String(evaluator) ||
            !reader.Type(type) ||
            !reader.U32(maximum_bytes) ||
            !reader.Bool(required) ||
            !SourceEvaluatorExists(evaluator, maximum_bytes))
        {
            diagnostic =
                "SGC1 contains an unregistered or malformed hit-time sampler";
            return false;
        }
        (void)type;
        (void)required;
    }

    std::uint8_t delivery = 0;
    std::uint8_t routing = 0;
    std::uint8_t epoch = 0;
    std::uint8_t lifetime = 0;
    if (!reader.U8(delivery) ||
        !reader.U8(routing) ||
        !reader.U8(epoch) ||
        !reader.U8(lifetime) ||
        !reader.done())
    {
        diagnostic = "SGC1 is truncated or has trailing data";
        return false;
    }
    if (delivery !=
            static_cast<std::uint8_t>(
                StopDeliveryMode::Observe) ||
        routing !=
            static_cast<std::uint8_t>(
                StopRoutingPolicy::Pass) ||
        epoch >
            static_cast<std::uint8_t>(
                StopEpochPolicy::RebindAfterRestore) ||
        lifetime !=
            static_cast<std::uint8_t>(
                StopSubscriptionLifetime::Scoped))
    {
        diagnostic =
            "SGC1 must describe a passive scoped Observe/Pass group";
        return false;
    }
    if (sample_count != 0)
    {
        // Source manifests identify bounded evaluator contracts, but the
        // session does not yet own the numeric descriptor/evaluator binding
        // required by the native router. Refuse to silently omit samples.
        diagnostic =
            "SGC1 hit-time samples require a registered session CPU evaluator binding";
        return false;
    }
    return payload.AddBytes(Field::PcAlternatives, std::move(pcs)) &&
        payload.AddUnsigned(Field::Delivery, delivery) &&
        payload.AddUnsigned(Field::RoutingPolicy, routing) &&
        payload.AddUnsigned(Field::EpochPolicy, epoch) &&
        payload.AddUnsigned(Field::Lifetime, lifetime);
}

bool DecodeContinueConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'C', 'U', 'C', '1'});
    std::uint8_t current = 0;
    bool suppress = false;
    std::uint8_t movie = 0;
    std::uint8_t throttle = 0;
    std::uint8_t interruption = 0;
    if (!reader.U8(current) || !reader.Bool(suppress) ||
        !reader.U8(movie) || !reader.U8(throttle) ||
        !reader.U8(interruption) || !reader.done() ||
        current > 1 || movie > 1 ||
        throttle >
            static_cast<std::uint8_t>(
                ExecutionThrottlePolicy::RequireDisabled) ||
        interruption >
            static_cast<std::uint8_t>(
                ExecutionInterruptionPolicy::AllowKnown))
    {
        diagnostic = "CUC1 contains an invalid execution policy";
        return false;
    }
    return payload.AddUnsigned(
               Field::CurrentPointPolicy,
               current == 0
                   ? static_cast<std::uint64_t>(
                         ExecutionCurrentPointPolicy::
                             AcceptIfAvailable)
                   : static_cast<std::uint64_t>(
                         ExecutionCurrentPointPolicy::Ignore)) &&
        payload.AddUnsigned(
            Field::MovieEndedPolicy,
            movie == 0
                ? static_cast<std::uint64_t>(
                      MovieEndedPolicy::Ignore)
                : static_cast<std::uint64_t>(
                      MovieEndedPolicy::Fail)) &&
        payload.AddUnsigned(Field::ThrottlePolicy, throttle) &&
        payload.AddUnsigned(
            Field::InterruptionPolicy,
            interruption) &&
        payload.AddUnsigned(
            Field::Flags,
            suppress ? 1u : 0u);
}

bool DecodeAdvanceConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'E', 'A', 'C', '1'});
    std::uint8_t kind = 0;
    bool movie = false;
    std::uint8_t throttle = 0;
    std::uint8_t interruption = 0;
    constexpr std::uint8_t expected = 2u;
    if (!reader.U8(kind) || !reader.Bool(movie) ||
        !reader.U8(throttle) ||
        !reader.U8(interruption) || !reader.done() ||
        kind != expected ||
        throttle >
            static_cast<std::uint8_t>(
                ExecutionThrottlePolicy::RequireDisabled) ||
        interruption >
            static_cast<std::uint8_t>(
                ExecutionInterruptionPolicy::AllowKnown))
    {
        diagnostic = "EAC1 contains an invalid advance policy";
        return false;
    }
    return payload.AddUnsigned(
               Field::MovieEndedPolicy,
               movie
                   ? static_cast<std::uint64_t>(
                         MovieEndedPolicy::Fail)
                   : static_cast<std::uint64_t>(
                         MovieEndedPolicy::Ignore)) &&
        payload.AddUnsigned(Field::ThrottlePolicy, throttle) &&
        payload.AddUnsigned(
            Field::InterruptionPolicy,
            interruption);
}

bool DecodeLeaseConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'I', 'L', 'C', '1'});
    std::uint32_t port = 0;
    std::uint32_t priority = 0;
    bool suspendable = false;
    bool borrowable = false;
    bool neutral_ack = false;
    bool movie_exclusive = false;
    if (!reader.U32(port) || !reader.U32(priority) ||
        !reader.Bool(suspendable) ||
        !reader.Bool(borrowable) ||
        !reader.Bool(neutral_ack) ||
        !reader.Bool(movie_exclusive) || !reader.done() ||
        port > std::numeric_limits<std::uint8_t>::max() ||
        priority >
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max()))
    {
        diagnostic = "ILC1 contains an invalid input lease policy";
        return false;
    }
    return payload.AddUnsigned(Field::Port, port) &&
        payload.AddSigned(
            Field::Priority,
            static_cast<std::int32_t>(priority)) &&
        payload.AddBoolean(Field::Suspendable, suspendable) &&
        payload.AddBoolean(
            Field::InterruptionBorrowable,
            borrowable) &&
        payload.AddBoolean(
            Field::RequireNeutralAcknowledgement,
            neutral_ack) &&
        payload.AddBoolean(
            Field::MovieExclusive,
            movie_exclusive);
}

bool DecodePublicationConfig(
    CanonicalAction action,
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'I', 'P', 'C', '1'});
    std::uint8_t kind = 0;
    std::uint8_t acknowledgement = 0;
    std::uint8_t expected = 0;
    switch (action)
    {
    case CanonicalAction::InputPublishHeld:
        expected = 0;
        break;
    case CanonicalAction::InputPublishPulse:
        expected = 1;
        break;
    case CanonicalAction::InputPublishSequence:
        expected = 3;
        break;
    default:
        diagnostic = "IPC1 was supplied to a non-publication action";
        return false;
    }
    if (!reader.U8(kind) || !reader.U8(acknowledgement) ||
        !reader.done() || kind != expected ||
        acknowledgement > 2)
    {
        diagnostic = "IPC1 contains an invalid publication policy";
        return false;
    }
    return payload.AddUnsigned(
        Field::Flags,
        acknowledgement);
}

bool DecodeNeutralConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'I', 'N', 'C', '1'});
    bool acknowledgement = false;
    bool cleanup_safe = false;
    if (!reader.Bool(acknowledgement) ||
        !reader.Bool(cleanup_safe) || !reader.done() ||
        !cleanup_safe)
    {
        diagnostic = "INC1 contains an invalid neutral policy";
        return false;
    }
    return payload.AddBoolean(
        Field::RequireNeutralAcknowledgement,
        acknowledgement);
}

bool DecodePollConfig(
    std::span<const Byte> bytes,
    bool& release,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'I', 'G', 'P', '1'});
    std::string witness;
    std::uint32_t retry_limit = 0;
    bool neutral_between_retries = false;
    if (!reader.Bool(release) ||
        !reader.String(witness) ||
        !reader.U32(retry_limit) ||
        !reader.Bool(neutral_between_retries) ||
        !reader.done() || retry_limit == 0 ||
        retry_limit > 65536 || !neutral_between_retries)
    {
        diagnostic = "IGP1 contains an invalid bounded poll policy";
        return false;
    }
    return payload.AddUtf8(Field::Label, std::move(witness)) &&
        payload.AddUnsigned(Field::RetryLimit, retry_limit) &&
        payload.AddBoolean(Field::Flags, release);
}

bool DecodeObservationConfig(
    CanonicalAction action,
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    StaticConfigReader reader(bytes, {'O', 'S', 'C', '1'});
    std::string observation;
    std::uint8_t requirement = 0;
    bool coherent = false;
    std::uint8_t dereference_depth = 0;
    if (!reader.String(observation) ||
        !reader.U8(requirement) ||
        !reader.Bool(coherent) ||
        !reader.U8(dereference_depth) || !reader.done() ||
        requirement > 1 || dereference_depth > 8 ||
        (action ==
             CanonicalAction::GuestRunCoherentQuery) !=
            coherent)
    {
        diagnostic = "OSC1 contains an invalid observation policy";
        return false;
    }
    return payload.AddUtf8(
               Field::Label,
               std::move(observation)) &&
        payload.AddBoolean(
            Field::Required,
            requirement == 0) &&
        payload.AddUnsigned(
            Field::Flags,
            dereference_depth);
}

bool DecodeTypedCanonicalRequest(
    CanonicalAction action,
    const ProgramValueGraph& graph,
    StateEpoch current_epoch,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    const std::vector<TypeSchemaDefinition> schemas =
        BuildCanonicalRuntimeActionSchemas();
    const ProgramValueArenaStatus validated =
        ValidateProgramValueGraph(
            graph,
            CanonicalActionInputType(action),
            schemas,
            {
                .maximum_values = 4096,
                .maximum_value_bytes = 1024u * 1024u,
            },
            current_epoch);
    if (!validated)
    {
        diagnostic = validated.message;
        return false;
    }
    const ProgramValue* root = RootValue(graph);
    const auto* record = root
        ? std::get_if<RecordValue>(&root->payload)
        : nullptr;
    if (!record)
    {
        diagnostic = "Canonical typed request root is not a record";
        return false;
    }

    const auto u64 = [&](std::size_t index,
                         std::uint64_t& value) {
        return index < record->fields.size() &&
            ScalarValue(
                graph,
                record->fields[index],
                value);
    };
    const auto bytes = [&](std::size_t index,
                           CanonicalRuntimeSchema schema) {
        return index < record->fields.size()
            ? NamedBytes(
                  graph,
                  record->fields[index],
                  schema)
            : nullptr;
    };
    const auto handle = [&](std::size_t index,
                            CanonicalAction producer) {
        const ProgramValue* value =
            index < record->fields.size()
            ? FindValue(graph, record->fields[index])
            : nullptr;
        return value &&
            AddResourceHandle(*value, producer, payload);
    };
    const auto optional_receipt =
        [&](std::size_t index,
            CanonicalRuntimeSchema optional_schema,
            CanonicalAction producer,
            bool require_publication,
            bool& present) {
            const ProgramValue* element = nullptr;
            if (index >= record->fields.size() ||
                !OptionalElement(
                    graph,
                    record->fields[index],
                    optional_schema,
                    element))
            {
                return false;
            }
            present = element != nullptr;
            if (!element)
                return true;
            CanonicalActionPayload receipt;
            if (!DecodeReceiptPayload(
                    *element,
                    producer,
                    receipt))
            {
                return false;
            }
            const auto publication =
                receipt.Unsigned(Field::Publication);
            if (optional_schema ==
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt)
            {
                const auto lease =
                    receipt.Unsigned(Field::Handle);
                const auto epoch =
                    receipt.Unsigned(Field::ResultEpoch);
                const auto frame =
                    receipt.Bytes(Field::ResultFrame);
                if (!lease || !publication ||
                    !epoch || !frame)
                    return false;
                return payload.AddUnsigned(
                           Field::ParentHandle,
                           *lease) &&
                    payload.AddUnsigned(
                        Field::Publication,
                        *publication) &&
                    payload.AddUnsigned(
                        Field::ResultEpoch,
                        *epoch) &&
                    payload.AddBytes(
                        Field::ResultFrame,
                        std::vector<Byte>(
                            frame->begin(),
                            frame->end()));
            }
            if (!publication)
                return !require_publication;
            return payload.AddUnsigned(
                Field::Publication,
                *publication);
        };

    switch (action)
    {
    case CanonicalAction::StopPointsSubscribeGroup:
    {
        const auto* config = bytes(
            0,
            CanonicalRuntimeSchema::StopGroupStaticConfig);
        return record->fields.size() == 1 && config &&
            DecodeStopGroupConfig(
                *config,
                payload,
                diagnostic);
    }
    case CanonicalAction::ExecutionContinueUntil:
    {
        bool publication = false;
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::
                ContinueUntilStaticConfig);
        if (record->fields.size() != 3 ||
            !handle(
                0,
                CanonicalAction::StopPointsSubscribeGroup) ||
            !optional_receipt(
                1,
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt,
                CanonicalAction::InputPublishHeld,
                false,
                publication) ||
            !config ||
            !DecodeContinueConfig(
                *config,
                payload,
                diagnostic))
        {
            if (diagnostic.empty())
                diagnostic = "ContinueUntilRequest is malformed";
            return false;
        }
        (void)publication;
        return true;
    }
    case CanonicalAction::ExecutionStepFrames:
    {
        std::uint64_t count = 0;
        bool receipt = false;
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::
                ExecutionAdvanceStaticConfig);
        if (record->fields.size() != 3 ||
            !u64(0, count) || count == 0 ||
            !payload.AddUnsigned(Field::Count, count) ||
            !optional_receipt(
                1,
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness,
                CanonicalAction::InputNeutralize,
                false,
                receipt) ||
            !config ||
            !DecodeAdvanceConfig(
                *config,
                payload,
                diagnostic))
        {
            if (diagnostic.empty())
                diagnostic = "Execution advance request is malformed";
            return false;
        }
        (void)receipt;
        return true;
    }
    case CanonicalAction::InputAcquireLease:
    {
        const auto* config = bytes(
            0,
            CanonicalRuntimeSchema::InputLeaseStaticConfig);
        return record->fields.size() == 1 && config &&
            DecodeLeaseConfig(*config, payload, diagnostic);
    }
    case CanonicalAction::InputPublishHeld:
    case CanonicalAction::InputPublishPulse:
    {
        const auto* input = bytes(
            1,
            CanonicalRuntimeSchema::InputFramePayload);
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::
                InputPublicationStaticConfig);
        return record->fields.size() == 3 &&
            handle(0, CanonicalAction::InputAcquireLease) &&
            input && payload.AddBytes(Field::InputFrame, *input) &&
            config &&
            DecodePublicationConfig(
                action,
                *config,
                payload,
                diagnostic);
    }
    case CanonicalAction::InputPublishSequence:
    {
        const auto* inputs = bytes(
            1,
            CanonicalRuntimeSchema::InputSequencePayload);
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::
                InputPublicationStaticConfig);
        return record->fields.size() == 3 &&
            handle(0, CanonicalAction::InputAcquireLease) &&
            inputs &&
            payload.AddBytes(Field::InputFrames, *inputs) &&
            config &&
            DecodePublicationConfig(
                action,
                *config,
                payload,
                diagnostic);
    }
    case CanonicalAction::InputNeutralize:
    {
        const auto* config = bytes(
            1,
            CanonicalRuntimeSchema::InputNeutralStaticConfig);
        return record->fields.size() == 2 &&
            handle(0, CanonicalAction::InputAcquireLease) &&
            config &&
            DecodeNeutralConfig(
                *config,
                payload,
                diagnostic);
    }
    case CanonicalAction::InputAwaitGuestPoll:
    {
        bool publication = false;
        bool witness = false;
        bool release = false;
        const auto* config = bytes(
            3,
            CanonicalRuntimeSchema::InputPollStaticConfig);
        if (record->fields.size() != 4 ||
            !handle(0, CanonicalAction::InputAcquireLease) ||
            !optional_receipt(
                1,
                CanonicalRuntimeSchema::
                    OptionalInputPublicationReceipt,
                CanonicalAction::InputPublishHeld,
                true,
                publication) ||
            !optional_receipt(
                2,
                CanonicalRuntimeSchema::
                    OptionalInputNeutralWitness,
                CanonicalAction::InputNeutralize,
                true,
                witness) ||
            !config ||
            !DecodePollConfig(
                *config,
                release,
                payload,
                diagnostic) ||
            (release ? !witness : !publication))
        {
            if (diagnostic.empty())
            {
                diagnostic =
                    "InputPollRequest lacks its exact request or release witness";
            }
            return false;
        }
        return true;
    }
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
    case CanonicalAction::GuestRunCoherentQuery:
    {
        const ProgramValue* stop = nullptr;
        if (!OptionalElement(
                graph,
                record->fields[0],
                CanonicalRuntimeSchema::
                    OptionalContinueUntilResult,
                stop) ||
            (stop &&
             !DecodeStopReceipt(
                 graph,
                 *stop,
                 current_epoch,
                 payload)))
        {
            diagnostic = "Observation request has a malformed stop receipt";
            return false;
        }
        const std::size_t config_index =
            action == CanonicalAction::GuestRunCoherentQuery
            ? 1
            : 2;
        if (action !=
            CanonicalAction::GuestRunCoherentQuery)
        {
            std::uint64_t address = 0;
            if (record->fields.size() != 3 ||
                !u64(1, address) ||
                address >
                    std::numeric_limits<std::uint32_t>::max() ||
                !payload.AddUnsigned(Field::Address, address))
            {
                diagnostic =
                    "Guest scalar observation has an invalid address";
                return false;
            }
        }
        else if (record->fields.size() != 2)
        {
            diagnostic =
                "Coherent observation request has the wrong shape";
            return false;
        }
        const auto* config = bytes(
            config_index,
            CanonicalRuntimeSchema::ObservationStaticConfig);
        return config &&
            DecodeObservationConfig(
                action,
                *config,
                payload,
                diagnostic);
    }
    default:
        diagnostic =
            "Action does not use a canonical typed request record";
        return false;
    }
}

bool DecodePcAlternatives(
    std::span<const Byte> bytes,
    std::vector<std::uint32_t>& pcs)
{
    if (bytes.empty() || bytes.size() % 4 != 0 ||
        bytes.size() / 4 > kMaximumStopAlternatives)
    {
        return false;
    }
    pcs.clear();
    pcs.reserve(bytes.size() / 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
    {
        const std::uint32_t pc =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
        if (pc == 0)
            return false;
        pcs.push_back(pc);
    }
    return true;
}

class ProgramStopConsumer final : public IStopPointConsumer
{
public:
    void OnStopPoint(const StopDelivery&) override {}
};

std::uint64_t ProgramStopIdentitySeed(
    InvocationId invocation,
    ProgramActionRequestId request) noexcept
{
    // Reserve one byte per request so the group/source tags and all 128
    // bounded subscription alternatives cannot alias an adjacent request.
    return (
        (invocation.value() << 16u) ^
        request.value()) << 8u;
}

StopSubscriptionGroupDefinition BuildPcGroup(
    const CanonicalActionPayload& payload,
    InvocationId invocation,
    ProgramActionRequestId request,
    StopDeliveryMode fallback_delivery)
{
    std::vector<std::uint32_t> pcs;
    if (const auto encoded = payload.Bytes(Field::PcAlternatives))
        (void)DecodePcAlternatives(*encoded, pcs);
    else if (const auto address = payload.Unsigned(Field::Address);
             address && *address <=
                 std::numeric_limits<std::uint32_t>::max())
        pcs.push_back(static_cast<std::uint32_t>(*address));

    const std::uint64_t seed =
        ProgramStopIdentitySeed(invocation, request);
    StopSubscriptionGroupDefinition definition;
    definition.id = StopSubscriptionGroupId(
        UnsignedOr(payload, Field::GroupId, seed | 1u));
    definition.source = {
        StopSourceId(
            UnsignedOr(payload, Field::SourceId, seed | 2u)),
        "program.action." + std::to_string(invocation.value()) +
            "." + std::to_string(request.value()),
        "canonical program action"};
    definition.epoch_policy = static_cast<StopEpochPolicy>(
        UnsignedOr(
            payload,
            Field::EpochPolicy,
            static_cast<std::uint64_t>(
                StopEpochPolicy::EndOnEpochChange)));

    const StopDeliveryMode delivery =
        static_cast<StopDeliveryMode>(UnsignedOr(
            payload,
            Field::Delivery,
            static_cast<std::uint64_t>(fallback_delivery)));
    const StopRoutingPolicy routing =
        static_cast<StopRoutingPolicy>(UnsignedOr(
            payload,
            Field::RoutingPolicy,
            static_cast<std::uint64_t>(
                StopRoutingPolicy::Pass)));
    const StopSubscriptionLifetime lifetime =
        static_cast<StopSubscriptionLifetime>(UnsignedOr(
            payload,
            Field::Lifetime,
            static_cast<std::uint64_t>(
                StopSubscriptionLifetime::Scoped)));
    const auto priority = static_cast<std::int32_t>(
        payload.Signed(Field::Priority).value_or(0));
    const std::uint64_t subscription_seed =
        UnsignedOr(payload, Field::SubscriptionId, seed | 0x80u);
    for (std::size_t index = 0; index < pcs.size(); ++index)
    {
        definition.subscriptions.push_back({
            .id = StopSubscriptionId(
                subscription_seed + index),
            .point = PcStopPointSpec{pcs[index]},
            .delivery = delivery,
            .policy = routing,
            .lifetime = lifetime,
            .priority = priority,
            .lossless = BooleanOr(
                payload,
                Field::Lossless,
                delivery != StopDeliveryMode::Observe &&
                    delivery != StopDeliveryMode::Progress),
            .suppress_immediate_reentry =
                delivery == StopDeliveryMode::Wake,
        });
    }
    return definition;
}

ProgramValueGraph UnitGraph()
{
    ProgramValue value{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::Unit),
        UnitValue{}};
    return {value.id, {std::move(value)}};
}

ProgramCleanupStatus CleanupStatusOf(
    ResourceCleanupDisposition disposition) noexcept
{
    switch (disposition)
    {
    case ResourceCleanupDisposition::Clean:
        return ProgramCleanupStatus::Clean;
    case ResourceCleanupDisposition::CleanWithDiagnostics:
        return ProgramCleanupStatus::CleanWithDiagnostics;
    case ResourceCleanupDisposition::TaintRequired:
        return ProgramCleanupStatus::Tainted;
    }
    return ProgramCleanupStatus::Tainted;
}

bool ExecutionSucceeded(const ExecutionTerminalResult& terminal) noexcept
{
    return terminal.status ==
            ExecutionTerminalStatus::RequestedCompletion ||
        terminal.status == ExecutionTerminalStatus::StepsCompleted ||
        terminal.status == ExecutionTerminalStatus::Paused;
}

ProgramActionCompletionStatus ExecutionCompletionStatus(
    const ExecutionTerminalResult& terminal) noexcept
{
    switch (terminal.status)
    {
    case ExecutionTerminalStatus::Cancelled:
        return ProgramActionCompletionStatus::Cancelled;
    case ExecutionTerminalStatus::TimedOut:
        return ProgramActionCompletionStatus::TimedOut;
    case ExecutionTerminalStatus::StateEpochMismatch:
        return ProgramActionCompletionStatus::StaleEpoch;
    case ExecutionTerminalStatus::Unsupported:
        return ProgramActionCompletionStatus::Unsupported;
    default:
        return ExecutionSucceeded(terminal)
            ? ProgramActionCompletionStatus::Completed
            : ProgramActionCompletionStatus::Failed;
    }
}

ProgramValueGraph ResourceHandleGraph(
    CanonicalAction action,
    ProgramResourceHandleId handle,
    std::optional<StateEpoch> epoch)
{
    const auto contract =
        CanonicalActionResourceContractSchemaIdentity(action);
    if (!contract)
        return {};
    ProgramValue value;
    value.id = ProgramValueId(1);
    value.type = CanonicalActionOutputType(action);
    value.payload = ResourceHandleValue{
        handle,
        *contract,
        epoch};
    return {value.id, {std::move(value)}};
}

ProgramValueGraph ContinueUntilResultGraph(
    const ExecutionTerminalResult& terminal)
{
    if (!terminal.stop || !terminal.stop->event)
        return {};
    const StopRouteReceipt& stop = *terminal.stop;
    const RoutedStopEvent& event = *stop.event;
    if (!stop.identity.sequence ||
        !stop.identity.sample_snapshot ||
        !stop.identity.state_epoch)
    {
        return {};
    }

    std::vector<Byte> evidence{
        static_cast<Byte>('R'),
        static_cast<Byte>('S'),
        static_cast<Byte>('E'),
        static_cast<Byte>('1')};
    const auto u8 = [&evidence](std::uint8_t value) {
        evidence.push_back(value);
    };
    const auto u32 = [&evidence](std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            evidence.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    };
    const auto u64 = [&evidence](std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            evidence.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    };

    u8(static_cast<std::uint8_t>(event.evidence.path));
    if (const auto* pc =
            std::get_if<PcStopPointSpec>(
                &event.evidence.point))
    {
        u8(0);
        u32(pc->pc);
    }
    else if (const auto* memory =
                 std::get_if<MemoryStopPointSpec>(
                     &event.evidence.point))
    {
        u8(1);
        u32(memory->address);
        u32(memory->size);
        u8(static_cast<std::uint8_t>(memory->access));
    }
    else
    {
        const auto* synthetic =
            std::get_if<SyntheticStopPointSpec>(
                &event.evidence.point);
        if (!synthetic)
            return {};
        u8(2);
        u64(synthetic->identity);
    }
    u32(event.evidence.hit_pc);
    u64(event.evidence.value);
    u8(event.evidence.post_write ? 1u : 0u);
    u64(stop.identity.dispatch_generation.value());
    u64(stop.identity.physical_generation.value());
    u8(event.sample_count);
    for (std::size_t index = 0;
         index < event.sample_count;
         ++index)
    {
        u32(event.samples[index].descriptor_id);
        u64(event.samples[index].value);
        u8(event.samples[index].available ? 1u : 0u);
    }

    ProgramValue sequence{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U64),
        stop.identity.sequence.value()};
    ProgramValue epoch{
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U64),
        stop.identity.state_epoch.value()};
    ProgramValue pc{
        ProgramValueId(3),
        TypeRef::Builtin(BuiltinType::U32),
        event.evidence.hit_pc};
    ProgramValue sample{
        ProgramValueId(4),
        TypeRef::Builtin(BuiltinType::U64),
        stop.identity.sample_snapshot.value()};
    ProgramValue evidence_value{
        ProgramValueId(5),
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload),
        std::move(evidence)};
    ProgramValue root{
        ProgramValueId(6),
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        RecordValue{{
            sequence.id,
            epoch.id,
            pc.id,
            sample.id,
            evidence_value.id,
        }}};
    return {
        root.id,
        {
            std::move(sequence),
            std::move(epoch),
            std::move(pc),
            std::move(sample),
            std::move(evidence_value),
            std::move(root),
        }};
}

template <typename T>
ProgramValueGraph ScalarResultGraph(
    CanonicalAction action,
    T scalar)
{
    ProgramValue value;
    value.id = ProgramValueId(1);
    value.type = CanonicalActionOutputType(action);
    value.payload = scalar;
    return {value.id, {std::move(value)}};
}

ProgramValueGraph ArtifactResultGraph(
    CanonicalAction action,
    std::string artifact_id,
    std::string storage_reference,
    std::string sha256)
{
    const auto schema =
        CanonicalActionArtifactPayloadSchemaIdentity(action);
    const auto hash = ContentHash256::FromHex(sha256);
    if (!schema || !hash)
        return {};
    ProgramValue value;
    value.id = ProgramValueId(1);
    value.type = CanonicalActionOutputType(action);
    value.payload = ArtifactReferenceValue{
        std::move(artifact_id),
        *schema,
        *hash,
        std::move(storage_reference),
        true};
    return {value.id, {std::move(value)}};
}

ProgramValueGraph ArtifactListResultGraph(
    CanonicalAction action,
    std::string artifact_id,
    std::string storage_reference,
    std::string sha256,
    bool complete)
{
    const auto output_schema =
        CanonicalActionOutputSchemaIdentity(action);
    const auto reference_schema =
        CanonicalActionArtifactReferenceSchemaIdentity(action);
    const auto payload_schema =
        CanonicalActionArtifactPayloadSchemaIdentity(action);
    const auto hash = ContentHash256::FromHex(sha256);
    if (!output_schema || !reference_schema || !payload_schema || !hash)
        return {};

    ProgramValue artifact;
    artifact.id = ProgramValueId(1);
    artifact.type = TypeRef::Named(*reference_schema);
    artifact.payload = ArtifactReferenceValue{
        std::move(artifact_id),
        *payload_schema,
        *hash,
        std::move(storage_reference),
        complete};

    ProgramValue list;
    list.id = ProgramValueId(2);
    list.type = TypeRef::Named(*output_schema);
    list.payload = ListValue{{artifact.id}};
    return {list.id, {std::move(artifact), std::move(list)}};
}

} // namespace

struct SessionProgramActionHost::Impl
{
    struct ActiveInvocation
    {
        InvocationId invocation;
        AttemptId attempt;
        ResourceOwnerId owner;
        ResourceScopeId root_scope;
        CancellationSource cancellation;

        ActiveInvocation(
            InvocationId invocation_id,
            AttemptId attempt_id)
            : invocation(invocation_id),
              attempt(attempt_id),
              owner(invocation_id.value()),
              cancellation(invocation_id)
        {
        }
    };

    struct ResourceMapping
    {
        ProgramResourceHandleId handle;
        ResourceReceiptId receipt;
        ResourceKind kind = ResourceKind::HostResource;
        StateEpoch epoch;
        std::optional<StateEpoch> origin_epoch;
        std::uint64_t concrete_id = 0;
        std::shared_ptr<StopSubscriptionGroupHandle> stop_group;
        std::optional<StopSubscriptionGroupDefinition>
            stop_group_definition;
        std::filesystem::path artifact_path;
        std::shared_ptr<bool> finalized;
    };

    struct BaselineMapping
    {
        StateHandleId handle;
        ResourceReceiptId receipt;
    };

    struct SavedArtifact
    {
        StateArtifactId artifact;
        std::filesystem::path path;
        std::string sha256;
    };

    enum class PendingKind : std::uint8_t
    {
        Action,
        InputAdvance,
        Cleanup,
    };

    struct PendingExecution
    {
        PendingKind kind = PendingKind::Action;
        ProgramActionRequest request;
        ExecutionOperationId operation;
        std::optional<InputAdvanceBindingId> input_binding;
        std::optional<ResourceCleanupContinuationId>
            cleanup_continuation;
        std::optional<CancellationSource> cleanup_cancellation;
        std::uint32_t cleanup_advances = 0;
        bool closes_scope = false;
        bool finishes_invocation = false;
        ProgramResourceHandleId releasing_resource;
    };

    explicit Impl(
        EmulationSession& emulation_session,
        SessionProgramActionHostConfig host_config)
        : session(emulation_session),
          config(std::move(host_config))
    {
        config.maximum_retained_completions =
            std::max<std::size_t>(
                config.maximum_retained_completions,
                1);
    }

    [[nodiscard]] bool BindOrCheckOwner() noexcept
    {
        if (owner_thread == std::thread::id{})
            owner_thread = std::this_thread::get_id();
        return owner_thread == std::this_thread::get_id();
    }

    [[nodiscard]] ProgramActionCompletion Completion(
        const ProgramActionRequest& request,
        ProgramActionCompletionStatus status,
        std::string code = {},
        std::string message = {}) const
    {
        const SessionSnapshot current = session.snapshot();
        ProgramActionCompletion completion;
        completion.request_id = request.request_id;
        completion.invocation_id = request.invocation_id;
        completion.attempt_id = request.attempt_id;
        completion.operation = request.operation;
        completion.status = status;
        completion.origin_epoch = request.expected_epoch;
        completion.resulting_epoch = current.state_epoch;
        completion.output =
            request.operation == ProgramHostOperation::InvokeAction
            ? ProgramValueGraph{}
            : UnitGraph();
        completion.session_disposition = current.disposition;
        completion.cleanup =
            current.disposition == SessionDisposition::Tainted
            ? ProgramCleanupStatus::Tainted
            : current.disposition ==
                    SessionDisposition::CleanWithDiagnostics
            ? ProgramCleanupStatus::CleanWithDiagnostics
            : ProgramCleanupStatus::Clean;
        completion.code = std::move(code);
        completion.message = std::move(message);
        return completion;
    }

    [[nodiscard]] ProgramActionDispatchResult Immediate(
        ProgramActionCompletion completion)
    {
        return {true, std::move(completion), {}};
    }

    [[nodiscard]] ProgramActionDispatchResult Reject(
        const ProgramActionRequest& request,
        ProgramActionCompletionStatus status,
        std::string code,
        std::string message)
    {
        ProgramActionCompletion completion = Completion(
            request,
            status,
            std::move(code),
            std::move(message));
        return {false, std::move(completion), completion.message};
    }

    [[nodiscard]] ResourceScopeId Scope(
        ProgramScopeId program_scope) const noexcept
    {
        const auto found = scopes.find(program_scope.value());
        return found == scopes.end() ? ResourceScopeId{}
                                     : found->second;
    }

    [[nodiscard]] ResourceMapping* Resource(
        ProgramResourceHandleId handle) noexcept
    {
        const auto found = resources.find(handle.value());
        return found == resources.end() ? nullptr : &found->second;
    }

    void PruneReleasedResources()
    {
        SessionResourceLedger* ledger = session.resources();
        if (!ledger)
            return;
        std::erase_if(
            resources,
            [ledger](const auto& item) {
                const auto receipt =
                    ledger->FindResource(item.second.receipt);
                return !receipt ||
                    receipt->status != ResourceRecordStatus::Active;
            });
    }

    [[nodiscard]] std::vector<CleanupReceipt>
    MakeCleanupReceipts(
        const std::vector<ResourceUnwindStep>& steps) const
    {
        std::vector<CleanupReceipt> result;
        for (const ResourceUnwindStep& step : steps)
        {
            if (step.result.status ==
                ResourceReleaseStatus::
                    CleanupExecutionRequired)
            {
                continue;
            }
            const auto mapping = std::ranges::find_if(
                resources,
                [&step](const auto& item) {
                    return item.second.receipt ==
                        step.request.receipt.id;
                });
            if (mapping == resources.end())
                continue;

            ProgramCleanupStatus status =
                ProgramCleanupStatus::Clean;
            if (step.result.status ==
                ResourceReleaseStatus::Failed)
            {
                status = ProgramCleanupStatus::Tainted;
            }
            else if (!step.result.diagnostic.empty())
            {
                status =
                    ProgramCleanupStatus::
                        CleanWithDiagnostics;
            }
            const auto existing = std::ranges::find(
                result,
                mapping->second.handle,
                &CleanupReceipt::resource);
            CleanupReceipt receipt{
                mapping->second.handle,
                status,
                step.result.diagnostic};
            if (existing == result.end())
                result.push_back(std::move(receipt));
            else
                *existing = std::move(receipt);
        }
        return result;
    }

    [[nodiscard]] std::optional<ProgramActionResource>
    RegisterResource(
        ProgramScopeId program_scope,
        ResourceKind kind,
        ResourceServiceId service_id,
        StateEpoch epoch,
        SessionResourceReleaseCallback release,
        std::uint64_t concrete_id,
        std::shared_ptr<StopSubscriptionGroupHandle> stop_group,
        std::string label,
        std::string& diagnostic,
        ResourceEpochPolicy epoch_policy =
            ResourceEpochPolicy::EndOnEpochChange)
    {
        SessionResourceLedger* ledger = session.resources();
        SessionResourceBindingTable* bindings =
            session.resource_bindings();
        const ResourceScopeId scope = Scope(program_scope);
        if (!active || !ledger || !bindings || !scope)
        {
            diagnostic =
                "Program resource scope or session ledger is unavailable";
            return std::nullopt;
        }

        SessionResourceBindingReceipt bound = bindings->Bind({
            .kind = kind,
            .acquisition_epoch = epoch,
            .release = std::move(release),
            .diagnostic_label = label,
        });
        if (!bound.success)
        {
            diagnostic = std::move(bound.diagnostic);
            return std::nullopt;
        }

        ResourceAcquisitionDefinition definition;
        definition.owner = active->owner;
        definition.service = service_id;
        definition.release = {kind, bound.external_id};
        definition.epoch_policy = epoch_policy;
        definition.promotion =
            ResourcePromotionPolicy::AnyAncestor;
        definition.cleanup =
            ResourceCleanupRequirement::Mandatory;
        definition.diagnostic_label = std::move(label);
        ResourceAcquisitionResult acquired =
            ledger->Acquire(scope, {definition});
        if (!acquired.success || acquired.receipts.size() != 1)
        {
            ResourceReceipt temporary;
            temporary.release = definition.release;
            temporary.acquisition_epoch = epoch;
            temporary.cleanup = definition.cleanup;
            const ResourceReleaseResult compensated =
                bindings->Release({
                    temporary,
                    ResourceReleaseReason::Explicit,
                    session.snapshot().state_epoch,
                    true});
            if (compensated.status !=
                    ResourceReleaseStatus::Released &&
                compensated.status !=
                    ResourceReleaseStatus::
                        SupersededByStateReplacement)
            {
                session.MarkTainted(
                    "Program resource registration compensation could not prove cleanup");
            }
            diagnostic = acquired.error.message.empty()
                ? "Program resource ledger acquisition failed"
                : std::move(acquired.error.message);
            return std::nullopt;
        }
        if (next_resource_handle == 0 ||
            next_resource_handle ==
                std::numeric_limits<std::uint64_t>::max())
        {
            const ResourceUnwindResult compensated =
                ledger->Release(
                acquired.receipts.front().id,
                *bindings);
            if (!compensated.completed())
            {
                session.MarkTainted(
                    "Program resource identity exhaustion compensation could not prove cleanup");
            }
            diagnostic =
                "Program resource handle identity space is exhausted";
            return std::nullopt;
        }

        ResourceMapping mapping;
        mapping.handle =
            ProgramResourceHandleId(next_resource_handle++);
        mapping.receipt = acquired.receipts.front().id;
        mapping.kind = kind;
        mapping.epoch = epoch;
        mapping.origin_epoch =
            epoch_policy == ResourceEpochPolicy::EpochAgnostic
            ? std::nullopt
            : std::optional<StateEpoch>(epoch);
        mapping.concrete_id = concrete_id;
        mapping.stop_group = std::move(stop_group);
        const ProgramActionResource resource{
            mapping.handle,
            mapping.receipt,
            mapping.kind,
            mapping.epoch,
            mapping.origin_epoch};
        resources.emplace(mapping.handle.value(), std::move(mapping));
        return resource;
    }

    [[nodiscard]] bool RegisterBaseline(
        std::string key,
        const StateHandleReceipt& captured,
        std::string& diagnostic)
    {
        StateService* states = session.state_service();
        SessionResourceLedger* ledger = session.resources();
        SessionResourceBindingTable* bindings =
            session.resource_bindings();
        if (key.empty() || !states || !ledger || !bindings)
        {
            diagnostic =
                "Session baseline registration dependencies are unavailable";
            return false;
        }
        const ResourceLedgerSnapshot snapshot = ledger->snapshot();
        const auto root = ledger->FindScope(snapshot.session_root);
        if (!root || !root->open)
        {
            diagnostic = "Session resource root is unavailable";
            return false;
        }

        SessionResourceBindingReceipt bound = bindings->Bind({
            .kind = ResourceKind::StateHandle,
            .acquisition_epoch = captured.captured_epoch,
            .release =
                [states, handle = captured.handle](
                    const ResourceReleaseRequest&) {
                    const StateServiceResult released =
                        states->ReleaseMemoryHandle(handle);
                    return ResourceReleaseResult{
                        released.ok
                            ? ResourceReleaseStatus::Released
                            : ResourceReleaseStatus::Failed,
                        released.message};
                },
            .diagnostic_label =
                "program session baseline " + key,
        });
        if (!bound.success)
        {
            diagnostic = std::move(bound.diagnostic);
            return false;
        }

        ResourceAcquisitionDefinition definition;
        definition.owner = root->owner;
        definition.service = kStateService;
        definition.release = {
            ResourceKind::StateHandle,
            bound.external_id};
        definition.epoch_policy =
            ResourceEpochPolicy::EpochAgnostic;
        definition.promotion =
            ResourcePromotionPolicy::Forbidden;
        definition.cleanup =
            ResourceCleanupRequirement::Mandatory;
        definition.diagnostic_label =
            "program session baseline " + key;
        ResourceAcquisitionResult acquired =
            ledger->Acquire(snapshot.session_root, {definition});
        if (!acquired.success || acquired.receipts.size() != 1)
        {
            ResourceReceipt temporary;
            temporary.release = definition.release;
            temporary.acquisition_epoch =
                captured.captured_epoch;
            temporary.cleanup = definition.cleanup;
            const ResourceReleaseResult compensated =
                bindings->Release({
                    temporary,
                    ResourceReleaseReason::Explicit,
                    session.snapshot().state_epoch,
                    true});
            if (compensated.status !=
                    ResourceReleaseStatus::Released &&
                compensated.status !=
                    ResourceReleaseStatus::
                        SupersededByStateReplacement)
            {
                session.MarkTainted(
                    "Session baseline registration compensation could not prove cleanup");
            }
            diagnostic = acquired.error.message.empty()
                ? "Session baseline ledger acquisition failed"
                : std::move(acquired.error.message);
            return false;
        }

        if (const auto previous = baselines.find(key);
            previous != baselines.end())
        {
            ResourceUnwindResult released =
                ledger->Release(previous->second.receipt, *bindings);
            if (!released.completed())
            {
                const ResourceUnwindResult compensated =
                    ledger->Release(
                    acquired.receipts.front().id,
                    *bindings);
                if (!compensated.completed())
                {
                    session.MarkTainted(
                        "Replacement session baseline compensation could not prove cleanup");
                }
                diagnostic =
                    "Prior session baseline could not be released cleanly";
                if (released.disposition ==
                        ResourceCleanupDisposition::TaintRequired ||
                    released.outcome ==
                        ResourceUnwindOutcome::CleanupFailed)
                {
                    session.MarkTainted(diagnostic);
                }
                return false;
            }
            baselines.erase(previous);
        }
        baselines.emplace(
            std::move(key),
            BaselineMapping{
                captured.handle,
                acquired.receipts.front().id});
        PruneReleasedResources();
        return true;
    }

    [[nodiscard]] bool HasCompletionCapacity() const noexcept
    {
        return completions.size() <
            config.maximum_retained_completions;
    }

    [[nodiscard]] bool Queue(ProgramActionCompletion completion)
    {
        if (!HasCompletionCapacity())
        {
            session.MarkTainted(
                "Program action completion queue exceeded its bound");
            // This is an invariant failure: accepted asynchronous work is
            // admitted only while one completion slot is available. Surface
            // the terminal anyway so its waiter cannot hang.
            completion.status =
                ProgramActionCompletionStatus::CleanupFailed;
            completion.cleanup = ProgramCleanupStatus::Tainted;
            completion.session_disposition =
                SessionDisposition::Tainted;
            completion.code = "completion_capacity_invariant";
            completion.message =
                "Accepted program action completed without its reserved completion slot";
            completions.push_back(std::move(completion));
            return false;
        }
        completions.push_back(std::move(completion));
        return true;
    }

    [[nodiscard]] std::optional<
        std::chrono::steady_clock::time_point>
    Deadline(const ProgramActionRequest& request) const noexcept
    {
        return request.timing ==
                ActionTimingClass::BoundedHostOperation
            ? request.bounded_host_deadline
            : std::nullopt;
    }

    [[nodiscard]] std::chrono::milliseconds Timeout(
        const ProgramActionRequest& request,
        const CanonicalActionPayload& payload) const
    {
        const std::uint64_t requested = UnsignedOr(
            payload,
            Field::TimeoutMilliseconds,
            static_cast<std::uint64_t>(
                config.default_action_timeout.count()));
        const auto maximum = static_cast<std::uint64_t>(
            config.maximum_action_timeout.count());
        std::uint64_t bounded = std::min(requested, maximum);
        if (const auto deadline = Deadline(request))
        {
            const auto now = std::chrono::steady_clock::now();
            if (*deadline <= now)
                return std::chrono::milliseconds(0);
            const auto remaining =
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    *deadline - now);
            bounded = std::min(
                bounded,
                static_cast<std::uint64_t>(
                    std::max<std::int64_t>(
                        remaining.count(),
                        1)));
        }
        return std::chrono::milliseconds(bounded);
    }

    [[nodiscard]] ExecutionRequestPolicy ExecutionPolicy(
        const ProgramActionRequest& request,
        const CanonicalActionPayload& payload) const
    {
        ExecutionRequestPolicy policy;
        policy.expected_epoch = request.expected_epoch;
        policy.movie_ended =
            static_cast<MovieEndedPolicy>(UnsignedOr(
                payload,
                Field::MovieEndedPolicy,
                static_cast<std::uint64_t>(
                    MovieEndedPolicy::Ignore)));
        policy.throttle =
            static_cast<ExecutionThrottlePolicy>(UnsignedOr(
                payload,
                Field::ThrottlePolicy,
                static_cast<std::uint64_t>(
                    ExecutionThrottlePolicy::Preserve)));
        policy.current_point =
            static_cast<ExecutionCurrentPointPolicy>(UnsignedOr(
                payload,
                Field::CurrentPointPolicy,
                static_cast<std::uint64_t>(
                    ExecutionCurrentPointPolicy::Ignore)));
        policy.interruptions =
            static_cast<ExecutionInterruptionPolicy>(UnsignedOr(
                payload,
                Field::InterruptionPolicy,
                static_cast<std::uint64_t>(
                    ExecutionInterruptionPolicy::Reject)));
        if (active)
            policy.cancellation = active->cancellation.token();
        return policy;
    }

    [[nodiscard]] ProgramActionDispatchResult SubmitExecutionAction(
        ProgramActionRequest request,
        ExecutionRequest execution,
        PendingKind kind = PendingKind::Action,
        std::optional<InputAdvanceBindingId> binding = {})
    {
        if (!HasCompletionCapacity())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "completion_capacity_exhausted",
                "Program action completion capacity must be drained before accepting asynchronous work");
        }
        if (pending)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "execution_busy",
                "Another program action owns ExecutionEngine");
        }
        const ExecutionSubmissionReceipt submitted =
            session.SubmitExecution(std::move(execution));
        if (!submitted.accepted)
        {
            if (binding)
            {
                InputArbiter* input = session.input_arbiter();
                const InputArbiterOperationReceipt removed = input
                    ? input->RemoveAdvanceBinding(*binding)
                    : InputArbiterOperationReceipt{
                          false,
                          InputArbiterErrorCode::Stopped,
                          "InputArbiter is unavailable"};
                if (!removed.ok)
                {
                    session.MarkTainted(
                        "Rejected program execution left its input advance binding live");
                    return Reject(
                        request,
                        ProgramActionCompletionStatus::CleanupFailed,
                        "input_binding_compensation_failed",
                        std::string(removed.message));
                }
            }
            const auto status =
                submitted.error.code ==
                        ExecutionErrorCode::StateEpochMismatch
                ? ProgramActionCompletionStatus::StaleEpoch
                : submitted.error.code ==
                            ExecutionErrorCode::Unsupported
                ? ProgramActionCompletionStatus::Unsupported
                : ProgramActionCompletionStatus::Failed;
            return Reject(
                request,
                status,
                "execution_rejected",
                submitted.error.message.empty()
                    ? "ExecutionEngine rejected the action"
                    : submitted.error.message);
        }
        pending = PendingExecution{
            kind,
            std::move(request),
            submitted.operation_id,
            binding,
            {},
            {},
            0,
            false,
            false,
            {}};
        return {true, {}, {}};
    }

    [[nodiscard]] ProgramActionDispatchResult PrepareState(
        ProgramActionRequest request)
    {
        if (active || !request.state_request || !request.scope)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_state_request",
                "Invocation state preparation is malformed or overlaps another invocation");
        }
        const SessionSnapshot current = session.snapshot();
        if (!current.open ||
            current.state_epoch != request.expected_epoch)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::StaleEpoch,
                "stale_epoch",
                "Invocation state preparation expected another session epoch");
        }
        const InvocationStateRequest& state = *request.state_request;
        if ((state.expected_session &&
             state.expected_session != current.session_id) ||
            (state.expected_epoch &&
             state.expected_epoch != current.state_epoch))
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "state_identity_mismatch",
                "Invocation state policy identifies another session or epoch");
        }
        if (state.session_lineage.empty())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "lineage_required",
                "Invocation state policy requires an explicit session lineage");
        }
        if (request.state_already_prepared &&
            (!CompleteSha256(request.prepared_baseline_sha256) ||
             !state.expected_session || !state.expected_epoch ||
             state.expected_session != current.session_id ||
             state.expected_epoch != current.state_epoch))
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "prepared_baseline_mismatch",
                "Prepared baseline must identify the exact current session and epoch");
        }

        switch (state.policy)
        {
        case InvocationStatePolicy::Boot:
            if (state.state_artifact)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "boot_artifact_forbidden",
                    "Boot cannot consume prior emulation state");
            }
            break;
        case InvocationStatePolicy::LoadArtifact:
            if (!state.state_artifact)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "state_artifact_required",
                    "LoadArtifact requires one explicit immutable state artifact");
            }
            break;
        case InvocationStatePolicy::RestoreBaseline:
            if (state.state_artifact)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "restore_baseline_artifact_forbidden",
                    "RestoreBaseline resolves its registered immutable baseline by lineage and cannot carry a state artifact");
            }
            break;
        case InvocationStatePolicy::ContinueSession:
            if (!state.expected_session || !state.expected_epoch ||
                state.expected_session != current.session_id ||
                state.expected_epoch != current.state_epoch ||
                current_lineage.empty() ||
                state.session_lineage != current_lineage ||
                state.state_artifact.has_value())
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "continue_session_mismatch",
                    "ContinueSession requires the exact current session, epoch, and established lineage");
            }
            break;
        default:
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_state_policy",
                "Invocation state preparation contains an unknown state policy");
        }

        if (state.state_artifact)
        {
            const auto expected_schema =
                CanonicalActionArtifactPayloadSchemaIdentity(
                    CanonicalAction::
                        StateSaveImmutableArtifact);
            const ArtifactReferenceValue& artifact =
                *state.state_artifact;
            if (!expected_schema ||
                artifact.schema != *expected_schema ||
                artifact.artifact_id.empty() ||
                !artifact.complete ||
                artifact.storage_reference.empty() ||
                artifact.content_hash.empty())
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "invalid_state_artifact",
                    "LoadArtifact requires the exact complete immutable state-artifact reference");
            }
        }

        SessionResourceLedger* ledger = session.resources();
        if (!ledger)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Failed,
                "resource_ledger_unavailable",
                "Session resource ledger is unavailable");
        }
        const ResourceLedgerSnapshot ledger_snapshot =
            ledger->snapshot();
        active.emplace(
            request.invocation_id,
            request.attempt_id);
        ResourceScopeResult invocation_scope =
            ledger->OpenSyntheticScope(
                ledger_snapshot.session_root,
                active->owner,
                "program invocation " +
                    std::to_string(request.invocation_id.value()));
        if (!invocation_scope.success)
        {
            active.reset();
            return Reject(
                request,
                ProgramActionCompletionStatus::Failed,
                "scope_open_failed",
                invocation_scope.error.message);
        }
        scopes.emplace(
            request.scope.value(),
            invocation_scope.scope.id);
        active->root_scope = invocation_scope.scope.id;

        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        if (request.state_already_prepared)
        {
            current_lineage = state.session_lineage;
            return Immediate(std::move(completion));
        }
        if (state.policy == InvocationStatePolicy::ContinueSession)
        {
            return Immediate(std::move(completion));
        }

        StateService* states = session.state_service();
        if (!states)
        {
            return FailPreparedState(
                request,
                "state_service_unavailable",
                "StateService is unavailable");
        }

        if (state.policy == InvocationStatePolicy::Boot)
        {
            const SessionOperationReceipt rebooted = session.Reboot();
            if (!rebooted.ok)
            {
                return FailPreparedState(
                    request,
                    "session_boot_failed",
                    rebooted.backend.message.empty()
                        ? "Fresh session boot failed"
                        : rebooted.backend.message);
            }

            const StateHandleReceipt baseline =
                session.CaptureStateHandle();
            if (!baseline.result.ok)
            {
                return FailPreparedState(
                    request,
                    "baseline_capture_failed",
                    baseline.result.message.empty()
                        ? "Fresh session baseline capture failed"
                        : baseline.result.message);
            }
            std::string diagnostic;
            if (!RegisterBaseline(
                    state.session_lineage,
                    baseline,
                    diagnostic))
            {
                (void)states->ReleaseMemoryHandle(
                    baseline.handle);
                return FailPreparedState(
                    request,
                    "baseline_registration_failed",
                    std::move(diagnostic));
            }
            current_lineage = state.session_lineage;
            completion.resulting_epoch =
                rebooted.resulting_epoch;
            PruneReleasedResources();
            return Immediate(std::move(completion));
        }

        StateOperationReceipt restored;
        if (state.state_artifact)
        {
            const ArtifactReferenceValue& artifact =
                *state.state_artifact;
            const auto same_session =
                saved_artifacts.find(artifact.artifact_id);
            if (same_session != saved_artifacts.end() &&
                same_session->second.path ==
                    std::filesystem::path(
                        artifact.storage_reference) &&
                same_session->second.sha256 ==
                    artifact.content_hash.ToHex())
            {
                restored = session.RestoreStateArtifact(
                    same_session->second.artifact);
            }
            else
            {
                const std::filesystem::path state_path(
                    artifact.storage_reference);
                const std::filesystem::path movie_sidecar(
                    state_path.string() + ".dtm");
                if (std::filesystem::exists(movie_sidecar))
                {
                    return FailPreparedState(
                        request,
                        "external_movie_metadata_required",
                        "External movie-backed state requires an exact DTM hash that InvocationStateRequest does not provide");
                }

                StateFileImportRequest import;
                import.path = state_path;
                import.expected_sha256 =
                    artifact.content_hash.ToHex();
                import.compatibility =
                    states->compatibility();
                // Absence of the exact <state>.dtm sidecar is the only
                // supported external no-movie declaration in this envelope.
                import.movie_mode =
                    ExternalMovieImportMode::NoMovie;
                import.lineage.edge =
                    state.session_lineage;
                import.lineage.producer =
                    "ProgramRuntime";
                const StateFileArtifactReceipt imported =
                    session.ImportStateArtifact(import);
                if (!imported.result.ok)
                {
                    return FailPreparedState(
                        request,
                        "state_import_failed",
                        imported.result.message);
                }
                restored = session.RestoreStateArtifact(
                    imported.artifact);
                (void)states->ReleaseFileArtifact(
                    imported.artifact);
            }
        }
        else if (state.policy ==
                     InvocationStatePolicy::RestoreBaseline &&
                 !state.session_lineage.empty())
        {
            const auto found =
                baselines.find(state.session_lineage);
            if (found == baselines.end())
            {
                return FailPreparedState(
                    request,
                    "baseline_unavailable",
                    "Requested invocation baseline is unavailable");
            }
            restored = session.RestoreStateHandle(
                found->second.handle);
        }
        else
        {
            return FailPreparedState(
                request,
                "state_artifact_required",
                "This invocation state policy requires an artifact or registered baseline");
        }
        if (!restored.result.ok)
        {
            return FailPreparedState(
                request,
                "state_restore_failed",
                restored.result.message);
        }
        completion.resulting_epoch = restored.resulting_epoch;
        current_lineage = state.session_lineage;
        PruneReleasedResources();
        return Immediate(std::move(completion));
    }

    [[nodiscard]] ProgramActionDispatchResult FailPreparedState(
        const ProgramActionRequest& request,
        std::string code,
        std::string message)
    {
        SessionResourceLedger* ledger = session.resources();
        SessionResourceBindingTable* bindings =
            session.resource_bindings();
        if (ledger && bindings)
        {
            const ResourceScopeId scope = Scope(request.scope);
            if (scope)
                (void)ledger->CloseScope(scope, *bindings);
        }
        scopes.clear();
        resources.clear();
        active.reset();
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            std::move(code),
            std::move(message));
    }

    [[nodiscard]] ProgramActionDispatchResult Dispatch(
        ProgramActionRequest request);
    [[nodiscard]] ProgramActionDispatchResult OpenScope(
        ProgramActionRequest request);
    [[nodiscard]] ProgramActionDispatchResult CloseScope(
        ProgramActionRequest request,
        bool finish);
    [[nodiscard]] ProgramActionDispatchResult Promote(
        ProgramActionRequest request);
    [[nodiscard]] ProgramActionDispatchResult Invoke(
        ProgramActionRequest request);
    [[nodiscard]] ProgramActionDispatchResult InvokeCanonical(
        ProgramActionRequest request,
        CanonicalAction action,
        CanonicalActionPayload payload);
    [[nodiscard]] ProgramActionDispatchResult InvokeSourceQuery(
        ProgramActionRequest request);
    [[nodiscard]] ProgramActionDispatchResult BeginUnwind(
        ProgramActionRequest request,
        ResourceUnwindResult unwind,
        bool finish);
    [[nodiscard]] bool SubmitCleanupAdvance(
        PendingExecution& continuation);
    void CompletePendingExecution(
        ExecutionTerminalResult terminal);
    void CompleteCleanup(
        ExecutionTerminalResult terminal);
    [[nodiscard]] ProgramActionCompletion ExecutionCompletion(
        const PendingExecution& operation,
        const ExecutionTerminalResult& terminal);
    [[nodiscard]] ProgramActionDispatchResult CompleteWithPayload(
        ProgramActionRequest request,
        CanonicalActionPayload payload,
        std::vector<ProgramActionResource> acquired = {});
    [[nodiscard]] ProgramActionDispatchResult ResourceFailure(
        ProgramActionRequest request,
        std::string diagnostic);
    [[nodiscard]] ProgramActionDispatchResult ReleaseMappedResource(
        ProgramActionRequest request,
        ResourceMapping& mapping,
        CanonicalActionPayload output = {});
    void ReleaseSavedArtifactRecords() noexcept;

    EmulationSession& session;
    SessionProgramActionHostConfig config;
    ProgramStopConsumer stop_consumer;
    std::thread::id owner_thread;
    std::optional<ActiveInvocation> active;
    std::unordered_map<std::uint64_t, ResourceScopeId> scopes;
    std::unordered_map<std::uint64_t, ResourceMapping> resources;
    std::unordered_map<std::string, BaselineMapping> baselines;
    std::unordered_map<std::string, SavedArtifact> saved_artifacts;
    std::string current_lineage;
    std::optional<PendingExecution> pending;
    std::vector<ProgramActionCompletion> completions;
    std::uint64_t next_resource_handle = 1;
    bool stopped = false;
};

ProgramActionDispatchResult
SessionProgramActionHost::Impl::CompleteWithPayload(
    ProgramActionRequest request,
    CanonicalActionPayload payload,
    std::vector<ProgramActionResource> acquired)
{
    if (!request.action)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "result_encoding_failed",
            "Canonical action completion has no action identity");
    }
    const auto action = ResolveCanonicalAction(*request.action);
    const auto schema = action
        ? CanonicalActionOutputSchemaIdentity(*action)
        : std::nullopt;
    if (!schema)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "result_encoding_failed",
            "Canonical action output is not an SAP1 receipt");
    }
    CanonicalActionPayloadResult encoded =
        EncodeCanonicalActionPayload(payload, *schema);
    if (!encoded.ok)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "result_encoding_failed",
            encoded.diagnostic);
    }
    ProgramActionCompletion completion = Completion(
        request,
        ProgramActionCompletionStatus::Completed);
    completion.output = std::move(encoded.graph);
    completion.resources = std::move(acquired);
    return Immediate(std::move(completion));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::ResourceFailure(
    ProgramActionRequest request,
    std::string diagnostic)
{
    return Reject(
        request,
        ProgramActionCompletionStatus::Failed,
        "resource_registration_failed",
        diagnostic.empty()
            ? "Program resource registration failed"
            : std::move(diagnostic));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::Dispatch(
    ProgramActionRequest request)
{
    if (!BindOrCheckOwner())
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "wrong_thread",
            "Program action host dispatch ran outside its actor thread");
    }
    if (stopped)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "runtime_stopping",
            "Program action host is shut down");
    }
    if (!request.request_id || !request.invocation_id ||
        !request.attempt_id || !request.expected_epoch)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_request",
            "Program action request identities must be nonzero");
    }
    if (request.operation !=
            ProgramHostOperation::PrepareInvocationState &&
        (!active ||
         active->invocation != request.invocation_id ||
         active->attempt != request.attempt_id))
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invocation_mismatch",
            "Program action does not identify the active invocation");
    }
    if (request.operation !=
            ProgramHostOperation::PrepareInvocationState &&
        request.expected_epoch != session.snapshot().state_epoch)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::StaleEpoch,
            "stale_epoch",
            "Program action expected a stale StateEpoch");
    }
    if (pending)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "action_pending",
            "A program action is already awaiting service completion");
    }
    if (!HasCompletionCapacity())
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "completion_capacity_exhausted",
            "Program action completions must be drained before accepting more work");
    }
    if (const auto deadline = Deadline(request);
        deadline && *deadline <= std::chrono::steady_clock::now())
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::TimedOut,
            "action_deadline",
            "Program action request reached its effective deadline before dispatch");
    }
    if (request.operation == ProgramHostOperation::InvokeAction &&
        request.action)
    {
        const auto effects =
            ResolveActionEffects(*request.action);
        if (!effects ||
            (*effects & ~request.allowed_effects) != 0)
        {
            return Reject(
                request,
                effects
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                effects
                    ? "action_effect_not_authorized"
                    : "unsupported_action",
                effects
                    ? "Invocation policy does not authorize every declared action effect"
                    : "The exact action identity has no registered effect contract");
        }
    }

    switch (request.operation)
    {
    case ProgramHostOperation::PrepareInvocationState:
        return PrepareState(std::move(request));
    case ProgramHostOperation::InvokeAction:
        return Invoke(std::move(request));
    case ProgramHostOperation::OpenScope:
        return OpenScope(std::move(request));
    case ProgramHostOperation::CloseScope:
        return CloseScope(std::move(request), false);
    case ProgramHostOperation::PromoteResource:
        return Promote(std::move(request));
    case ProgramHostOperation::FinishInvocation:
        return CloseScope(std::move(request), true);
    }
    return Reject(
        request,
        ProgramActionCompletionStatus::Rejected,
        "unknown_host_operation",
        "Program action host operation is unknown");
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::OpenScope(
    ProgramActionRequest request)
{
    if (!request.scope || !request.parent_scope ||
        Scope(request.scope) || !Scope(request.parent_scope))
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_scope",
            "Program lexical scope mapping is malformed");
    }
    SessionResourceLedger* ledger = session.resources();
    if (!ledger || !active)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "resource_ledger_unavailable",
            "Session resource ledger is unavailable");
    }
    ResourceScopeResult opened = ledger->OpenSyntheticScope(
        Scope(request.parent_scope),
        active->owner,
        "program lexical scope " +
            std::to_string(request.scope.value()));
    if (!opened.success)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "scope_open_failed",
            opened.error.message);
    }
    scopes.emplace(request.scope.value(), opened.scope.id);
    return Immediate(Completion(
        request,
        ProgramActionCompletionStatus::Completed));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::BeginUnwind(
    ProgramActionRequest request,
    ResourceUnwindResult unwind,
    bool finish)
{
    if (unwind.completed())
    {
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.cleanup_receipts =
            MakeCleanupReceipts(unwind.steps);
        completion.cleanup = CleanupStatusOf(unwind.disposition);
        PruneReleasedResources();
        if (finish)
        {
            scopes.clear();
            resources.clear();
            active.reset();
        }
        else
        {
            scopes.erase(request.scope.value());
        }
        return Immediate(std::move(completion));
    }
    if (unwind.outcome ==
            ResourceUnwindOutcome::CleanupExecutionRequired &&
        unwind.cleanup_execution_request)
    {
        PendingExecution continuation;
        continuation.kind = PendingKind::Cleanup;
        continuation.request = std::move(request);
        continuation.cleanup_continuation =
            unwind.cleanup_execution_request->continuation;
        continuation.cleanup_cancellation.emplace(
            continuation.request.invocation_id);
        continuation.closes_scope = true;
        continuation.finishes_invocation = finish;
        pending.emplace(std::move(continuation));
        if (SubmitCleanupAdvance(*pending))
            return {true, {}, {}};

        ProgramActionRequest failed_request =
            std::move(pending->request);
        pending.reset();
        session.MarkTainted(
            "Program resource cleanup could not advance the guest");
        return Reject(
            failed_request,
            ProgramActionCompletionStatus::CleanupFailed,
            "cleanup_execution_failed",
            "Program resource cleanup could not submit its bounded frame advance");
    }

    const bool taint =
        unwind.disposition ==
            ResourceCleanupDisposition::TaintRequired ||
        unwind.outcome == ResourceUnwindOutcome::CleanupFailed;
    if (taint)
    {
        session.MarkTainted(
            unwind.error.message.empty()
                ? "Program resource unwind could not prove cleanup"
                : unwind.error.message);
    }
    ProgramActionDispatchResult failed = Reject(
        request,
        taint
            ? ProgramActionCompletionStatus::CleanupFailed
            : ProgramActionCompletionStatus::Failed,
        taint ? "cleanup_failed" : "scope_close_failed",
        unwind.error.message.empty()
            ? "Program resource unwind failed"
            : unwind.error.message);
    if (failed.immediate_completion)
    {
        failed.immediate_completion->cleanup_receipts =
            MakeCleanupReceipts(unwind.steps);
    }
    return failed;
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::CloseScope(
    ProgramActionRequest request,
    bool finish)
{
    const ResourceScopeId scope = Scope(request.scope);
    SessionResourceLedger* ledger = session.resources();
    SessionResourceBindingTable* bindings =
        session.resource_bindings();
    if (!scope || !ledger || !bindings)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "scope_unavailable",
            "Program scope is not mapped to the session ledger");
    }
    return BeginUnwind(
        std::move(request),
        ledger->CloseScope(scope, *bindings),
        finish);
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::Promote(
    ProgramActionRequest request)
{
    ResourceMapping* mapping = Resource(request.resource);
    const ResourceScopeId destination = Scope(request.scope);
    SessionResourceLedger* ledger = session.resources();
    if (!mapping || !destination || !ledger)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "resource_unavailable",
            "Program resource or promotion scope is unavailable");
    }
    const ResourcePromotionResult promoted =
        ledger->Promote(mapping->receipt, destination);
    if (!promoted.success)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "promotion_rejected",
            promoted.error.message);
    }
    return Immediate(Completion(
        request,
        ProgramActionCompletionStatus::Completed));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::Invoke(
    ProgramActionRequest request)
{
    if (!request.action)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "missing_action",
            "Program action request has no exact action identity");
    }
    if (*request.action ==
            capabilities::BattleCaptureContextActionIdentity() ||
        *request.action ==
            capabilities::NavigationCaptureContextActionIdentity())
    {
        return InvokeSourceQuery(std::move(request));
    }
    const auto canonical = ResolveCanonicalAction(*request.action);
    if (!canonical)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Unsupported,
            "unsupported_action",
            "The exact action identity has no session host implementation");
    }
    CanonicalActionPayload payload;
    const TypeRef input_type =
        CanonicalActionInputType(*canonical);
    if (input_type.named &&
        input_type.named->canonical_id.ends_with(".Result"))
    {
        const ProgramValue* root = RootValue(request.input);
        const auto* handle = root
            ? std::get_if<ResourceHandleValue>(&root->payload)
            : nullptr;
        if (!root || root->type != input_type || !handle ||
            !payload.AddUnsigned(
                Field::Handle,
                handle->handle_id.value()))
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_resource_handle",
                "Canonical action requires its exact typed resource handle");
        }
        return InvokeCanonical(
            std::move(request),
            *canonical,
            std::move(payload));
    }
    std::string diagnostic;
    const auto input_schema =
        CanonicalActionInputSchemaIdentity(*canonical);
    if (!input_schema)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_action_payload",
            "Canonical action has no nominal request schema");
    }
    const bool decoded = UsesTypedRequestRecord(*canonical)
        ? DecodeTypedCanonicalRequest(
              *canonical,
              request.input,
              request.expected_epoch,
              payload,
              diagnostic)
        : DecodeCanonicalActionPayload(
              request.input,
              *input_schema,
              payload,
              &diagnostic);
    if (!decoded)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_action_payload",
            diagnostic.empty()
                ? "Canonical action payload is malformed"
                : std::move(diagnostic));
    }
    return InvokeCanonical(
        std::move(request),
        *canonical,
        std::move(payload));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::InvokeSourceQuery(
    ProgramActionRequest request)
{
    ContextRequestEvidence evidence;
    const bool battle = request.action ==
        capabilities::BattleCaptureContextActionIdentity();
    const std::string_view input_schema = battle
        ? "soa.battle.CaptureContextRequest"
        : "soa.navigation.CaptureContextRequest";
    if (!DecodeContextRequest(
            request.input,
            input_schema,
            evidence) ||
        evidence.epoch != request.expected_epoch)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_context_request",
            "Coherent context request has malformed or stale routed evidence");
    }
    const ExecutionSnapshot execution =
        session.execution_snapshot();
    if (execution.activity != ExecutionActivity::IdlePaused ||
        !execution.evidence.pause_confirmed ||
        execution.evidence.pc != evidence.expected_pc)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "observation_point_unavailable",
            "Coherent context acquisition is not paused at its bound receipt PC");
    }
    GuestMemory* memory = session.guest_memory();
    if (!memory)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Unsupported,
            "guest_memory_unavailable",
            "GuestMemory is unavailable");
    }

    ProgramActionCompletion completion = Completion(
        request,
        ProgramActionCompletionStatus::Completed);
    std::string diagnostic;
    if (battle)
    {
        soa::battle::ctx::BattleContext context{};
        if (!ReadBattleContext(
                *memory,
                request.expected_epoch,
                context,
                diagnostic))
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Failed,
                "battle_context_unavailable",
                std::move(diagnostic));
        }
        completion.output =
            capabilities::EncodeBattleContextValue(context);
    }
    else
    {
        soa::navigation::ctx::NavigationContext context{};
        if (!ReadNavigationContext(
                *memory,
                request.expected_epoch,
                evidence.expected_pc,
                context,
                diagnostic))
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Failed,
                "navigation_context_unavailable",
                std::move(diagnostic));
        }
        completion.output =
            capabilities::EncodeNavigationContextValue(context);
    }
    return Immediate(std::move(completion));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::ReleaseMappedResource(
    ProgramActionRequest request,
    ResourceMapping& mapping,
    CanonicalActionPayload output)
{
    SessionResourceLedger* ledger = session.resources();
    SessionResourceBindingTable* bindings =
        session.resource_bindings();
    if (!ledger || !bindings)
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            "resource_ledger_unavailable",
            "Session resource ledger is unavailable");
    }
    const ProgramResourceHandleId handle = mapping.handle;
    ResourceUnwindResult unwind =
        ledger->Release(mapping.receipt, *bindings);
    if (unwind.completed())
    {
        std::vector<CleanupReceipt> cleanup_receipts =
            MakeCleanupReceipts(unwind.steps);
        resources.erase(handle.value());
        if (request.action)
        {
            const auto action =
                ResolveCanonicalAction(*request.action);
            if (action &&
                CanonicalActionOutputSchemaIdentity(*action))
            {
                ProgramActionDispatchResult completed =
                    CompleteWithPayload(
                    std::move(request),
                    std::move(output));
                if (completed.immediate_completion)
                {
                    completed.immediate_completion->
                        cleanup_receipts =
                            std::move(cleanup_receipts);
                }
                return completed;
            }
        }
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.cleanup_receipts =
            std::move(cleanup_receipts);
        return Immediate(std::move(completion));
    }
    if (unwind.outcome ==
            ResourceUnwindOutcome::CleanupExecutionRequired &&
        unwind.cleanup_execution_request)
    {
        PendingExecution continuation;
        continuation.kind = PendingKind::Cleanup;
        continuation.request = std::move(request);
        continuation.cleanup_continuation =
            unwind.cleanup_execution_request->continuation;
        continuation.cleanup_cancellation.emplace(
            continuation.request.invocation_id);
        continuation.releasing_resource = handle;
        pending.emplace(std::move(continuation));
        if (SubmitCleanupAdvance(*pending))
            return {true, {}, {}};
        ProgramActionRequest failed =
            std::move(pending->request);
        pending.reset();
        session.MarkTainted(
            "Program resource release could not advance the guest");
        return Reject(
            failed,
            ProgramActionCompletionStatus::CleanupFailed,
            "cleanup_execution_failed",
            "Program resource release could not submit its bounded frame advance");
    }
    const bool taint = unwind.disposition ==
            ResourceCleanupDisposition::TaintRequired ||
        unwind.outcome == ResourceUnwindOutcome::CleanupFailed;
    if (taint)
        session.MarkTainted("Program resource release failed");
    ProgramActionDispatchResult failed = Reject(
        request,
        taint
            ? ProgramActionCompletionStatus::CleanupFailed
            : ProgramActionCompletionStatus::Failed,
        "resource_release_failed",
        unwind.error.message.empty()
            ? "Program resource release failed"
            : unwind.error.message);
    if (failed.immediate_completion)
    {
        failed.immediate_completion->cleanup_receipts =
            MakeCleanupReceipts(unwind.steps);
    }
    return failed;
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::InvokeCanonical(
    ProgramActionRequest request,
    CanonicalAction action,
    CanonicalActionPayload payload)
{
    if (const auto invalid = InvalidEnumField(payload))
    {
        return Reject(
            request,
            ProgramActionCompletionStatus::Rejected,
            "invalid_enum_value",
            "Canonical action payload contains an unknown numeric policy value in field " +
                std::to_string(
                    static_cast<std::uint16_t>(*invalid)));
    }

    const auto require_handle = [&]()
        -> ResourceMapping* {
        const auto handle = payload.Unsigned(Field::Handle);
        return handle
            ? Resource(ProgramResourceHandleId(*handle))
            : nullptr;
    };
    const auto complete_resource =
        [&](ProgramActionRequest completed_request,
            const ProgramActionResource& resource)
            -> ProgramActionDispatchResult {
        ProgramActionCompletion completion = Completion(
            completed_request,
            ProgramActionCompletionStatus::Completed);
        completion.output = ResourceHandleGraph(
            action,
            resource.handle,
            resource.origin_epoch);
        if (completion.output.values.empty())
        {
            return Reject(
                completed_request,
                ProgramActionCompletionStatus::Failed,
                "result_encoding_failed",
                "Resource handle result schema is unavailable");
        }
        completion.resources.push_back(resource);
        return Immediate(std::move(completion));
    };
    const auto service_failure =
        [&](std::string code, std::string message) {
        return Reject(
            request,
            ProgramActionCompletionStatus::Failed,
            std::move(code),
            std::move(message));
    };

    switch (action)
    {
    case CanonicalAction::StateCapture:
    {
        StateService* states = session.state_service();
        if (!states)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Unsupported,
                "state_service_unavailable",
                "StateService is unavailable");
        }
        const StateHandleReceipt captured =
            session.CaptureStateHandle();
        if (!captured.result.ok)
        {
            return service_failure(
                "state_capture_failed",
                captured.result.message);
        }
        std::string diagnostic;
        std::optional<ProgramActionResource> resource;
        if (const auto key = payload.Utf8(Field::BaselineKey);
            key && !key->empty())
        {
            const std::string baseline_key(*key);
            if (!RegisterBaseline(
                    baseline_key,
                    captured,
                    diagnostic))
            {
                (void)states->ReleaseMemoryHandle(
                    captured.handle);
                return ResourceFailure(
                    std::move(request),
                    diagnostic);
            }
            if (next_resource_handle == 0 ||
                next_resource_handle ==
                    std::numeric_limits<std::uint64_t>::max())
            {
                session.MarkTainted(
                    "Program resource handle identity space is exhausted after baseline registration");
                return ResourceFailure(
                    std::move(request),
                    "Program resource handle identity space is exhausted");
            }
            const BaselineMapping& baseline =
                baselines.at(baseline_key);
            ResourceMapping mapping;
            mapping.handle = ProgramResourceHandleId(
                next_resource_handle++);
            mapping.receipt = baseline.receipt;
            mapping.kind = ResourceKind::StateHandle;
            mapping.epoch = captured.captured_epoch;
            mapping.origin_epoch = std::nullopt;
            mapping.concrete_id =
                captured.handle.value();
            resource = ProgramActionResource{
                mapping.handle,
                mapping.receipt,
                mapping.kind,
                mapping.epoch,
                mapping.origin_epoch};
            resources.emplace(
                mapping.handle.value(),
                std::move(mapping));
        }
        else
        {
            resource = RegisterResource(
                request.scope,
                ResourceKind::StateHandle,
                kStateService,
                captured.captured_epoch,
                [states, handle = captured.handle](
                    const ResourceReleaseRequest&) {
                    const StateServiceResult released =
                        states->ReleaseMemoryHandle(handle);
                    return ResourceReleaseResult{
                        released.ok
                            ? ResourceReleaseStatus::Released
                            : ResourceReleaseStatus::Failed,
                        released.message};
                },
                captured.handle.value(),
                {},
                "program state handle",
                diagnostic,
                ResourceEpochPolicy::EpochAgnostic);
        }
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::StateRestore:
    case CanonicalAction::StateRestoreBaseline:
    {
        StateService* states = session.state_service();
        if (!states)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Unsupported,
                "state_service_unavailable",
                "StateService is unavailable");
        }
        StateHandleId handle;
        if (ResourceMapping* mapping = require_handle();
            mapping && mapping->kind == ResourceKind::StateHandle)
        {
            handle = StateHandleId(mapping->concrete_id);
        }
        else if (const auto key = payload.Utf8(Field::BaselineKey);
                 key && baselines.contains(std::string(*key)))
        {
            handle = baselines.at(std::string(*key)).handle;
        }
        if (!handle)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "state_handle_unavailable",
                "State restore requires a current typed state handle or baseline");
        }
        const StateOperationReceipt restored =
            session.RestoreStateHandle(handle);
        if (!restored.result.ok)
        {
            return service_failure(
                "state_restore_failed",
                restored.result.message);
        }
        PruneReleasedResources();
        CanonicalActionPayload result;
        (void)result.AddUnsigned(
            Field::ResultEpoch,
            restored.resulting_epoch.value());
        ProgramActionDispatchResult completed =
            CompleteWithPayload(
                std::move(request),
                std::move(result));
        if (completed.immediate_completion)
        {
            completed.immediate_completion->resulting_epoch =
                restored.resulting_epoch;
        }
        return completed;
    }
    case CanonicalAction::StateSaveImmutableArtifact:
    {
        StateService* states = session.state_service();
        const auto path = payload.Utf8(Field::Path);
        if (!states || !path || path->empty())
        {
            return Reject(
                request,
                states
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "invalid_artifact_request",
                "State artifact capture requires StateService and a caller-declared path");
        }
        StateFileCaptureRequest capture;
        capture.path = std::filesystem::path(*path);
        capture.lineage.edge = std::string(
            payload.Utf8(Field::Label).value_or(
                "program state artifact"));
        capture.lineage.producer = "ProgramRuntime";
        ImmutableStateArtifactCaptureReceipt artifact =
            session.CaptureImmutableStateArtifact(capture);
        if (!artifact.result.ok)
        {
            return service_failure(
                "state_artifact_failed",
                artifact.result.message);
        }
        const std::string artifact_id =
            "state:" +
            std::to_string(artifact.artifact.value());
        CanonicalActionPayload result;
        if (!result.AddUnsigned(
                Field::ResultArtifactId,
                artifact.artifact.value()) ||
            !result.AddUnsigned(
                Field::ResultSize,
                artifact.resident_bytes()) ||
            !result.AddUtf8(
                Field::StorageReference,
                artifact.final_path.string()) ||
            !result.AddUtf8(
                Field::Publication,
                "pending"))
        {
            (void)session.AbandonImmutableStateArtifact(
                artifact.artifact);
            return service_failure(
                "result_encoding_failed",
                "Pending state-artifact receipt could not be encoded");
        }
        ProgramActionDispatchResult completed =
            CompleteWithPayload(
                std::move(request),
                std::move(result));
        if (!completed.immediate_completion)
        {
            (void)session.AbandonImmutableStateArtifact(
                artifact.artifact);
            return service_failure(
                "result_encoding_failed",
                "Pending state-artifact receipt was not completed");
        }
        completed.immediate_completion
            ->pending_state_artifacts.push_back({
            artifact_id,
            std::move(artifact)});
        return completed;
    }
    case CanonicalAction::ExecutionContinueUntil:
    {
        ResourceMapping* mapping = require_handle();
        if (!mapping ||
            mapping->kind != ResourceKind::StopPointGroup ||
            !mapping->stop_group ||
            !mapping->stop_group_definition)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "stop_group_unavailable",
                "ContinueUntil requires its exact passive stop-group handle");
        }
        StopSubscriptionGroupDefinition wake =
            *mapping->stop_group_definition;
        const std::uint64_t seed =
            ProgramStopIdentitySeed(
                request.invocation_id,
                request.request_id);
        wake.id = StopSubscriptionGroupId(seed | 1u);
        wake.source = {
            StopSourceId(seed | 2u),
            "program.execution." +
                std::to_string(request.invocation_id.value()) +
                "." +
                std::to_string(request.request_id.value()),
            "temporary canonical ContinueUntil wake"};
        const bool suppress =
            UnsignedOr(payload, Field::Flags, 0) != 0;
        for (std::size_t index = 0;
             index < wake.subscriptions.size();
             ++index)
        {
            StopSubscriptionDefinition& subscription =
                wake.subscriptions[index];
            if (subscription.delivery !=
                    StopDeliveryMode::Observe ||
                subscription.policy != StopRoutingPolicy::Pass)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "stop_group_not_passive",
                    "ContinueUntil can promote only a passive Observe/Pass stop group");
            }
            subscription.id =
                StopSubscriptionId(
                    (seed | 0x80u) + index);
            subscription.delivery = StopDeliveryMode::Wake;
            subscription.policy = StopRoutingPolicy::Pass;
            subscription.lifetime =
                StopSubscriptionLifetime::Scoped;
            subscription.lossless = true;
            subscription.suppress_immediate_reentry = suppress;
            subscription.consumer = &stop_consumer;
        }
        if (wake.subscriptions.empty())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_stop_alternatives",
                "ContinueUntil passive group has no bounded alternatives");
        }
        std::optional<InputAdvanceBindingId>
            input_relationship;
        if (payload.Contains(Field::Publication))
        {
            InputArbiter* input = session.input_arbiter();
            const auto publication =
                InputPublicationEvidenceFromPayload(payload);
            if (!input || !publication)
            {
                return Reject(
                    request,
                    input
                        ? ProgramActionCompletionStatus::Rejected
                        : ProgramActionCompletionStatus::Unsupported,
                    "input_relationship_invalid",
                    "ContinueUntil requires a complete typed input publication receipt");
            }
            const InputAdvanceBindingReceipt relationship =
                input->CreatePublicationRelationship(
                    *publication);
            if (!relationship.ok)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "input_relationship_invalid",
                    relationship.message);
            }
            input_relationship = relationship.binding;
        }
        ExecutionRequestPolicy policy =
            ExecutionPolicy(request, payload);
        policy.input_relationship = input_relationship;
        ContinueUntilRequest execution{
            std::move(policy),
            std::move(wake)};
        return SubmitExecutionAction(
            std::move(request),
            std::move(execution),
            PendingKind::Action,
            input_relationship);
    }
    case CanonicalAction::ExecutionStepFrames:
    {
        const std::uint64_t count =
            UnsignedOr(payload, Field::Count, 1);
        if (count == 0 ||
            count > std::numeric_limits<std::uint32_t>::max())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_step_count",
                "Frame step count is outside its bounded range");
        }
        return SubmitExecutionAction(
            std::move(request),
            StepFramesRequest{
                ExecutionPolicy(request, payload),
                static_cast<std::uint32_t>(count)});
    }
    case CanonicalAction::StopPointsSubscribeGroup:
    {
        StopPointRouter* router = session.stop_points();
        if (!router)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Unsupported,
                "stop_router_unavailable",
                "StopPointRouter is unavailable");
        }
        StopSubscriptionGroupDefinition definition = BuildPcGroup(
            payload,
            request.invocation_id,
            request.request_id,
            StopDeliveryMode::Observe);
        if (definition.subscriptions.empty())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_stop_group",
                "Stop group requires bounded PC alternatives");
        }
        for (StopSubscriptionDefinition& subscription :
             definition.subscriptions)
        {
            subscription.consumer = &stop_consumer;
        }
        const ResourceEpochPolicy resource_epoch_policy =
            definition.epoch_policy ==
                StopEpochPolicy::EndOnEpochChange
            ? ResourceEpochPolicy::EndOnEpochChange
            : ResourceEpochPolicy::EpochAgnostic;
        StopGroupRegistrationOptions options;
        options.current_point =
            static_cast<StopCurrentPointPolicy>(UnsignedOr(
                payload,
                Field::CurrentPointPolicy,
                static_cast<std::uint64_t>(
                    StopCurrentPointPolicy::Ignore)));
        const StopSubscriptionGroupDefinition retained_definition =
            definition;
        StopGroupRegistrationResult registered =
            router->RegisterGroup(std::move(definition), options);
        if (!registered.receipt.ok)
        {
            return service_failure(
                "stop_group_registration_failed",
                registered.receipt.error.message);
        }
        auto group =
            std::make_shared<StopSubscriptionGroupHandle>(
                std::move(registered.handle));
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::StopPointGroup,
            kStopPointService,
            registered.receipt.lease.acquisition_epoch,
            [group](const ResourceReleaseRequest&) {
                const StopReleaseReceipt released =
                    group->Release();
                return ResourceReleaseResult{
                    released.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    released.error.message};
            },
            registered.receipt.lease.group_id.value(),
            group,
            "program stop-point group",
            diagnostic,
            resource_epoch_policy);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        if (ResourceMapping* mapping =
                Resource(resource->handle))
        {
            mapping->stop_group_definition =
                retained_definition;
        }
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::StopPointsReplaceGroup:
    {
        ResourceMapping* mapping = require_handle();
        if (!mapping ||
            mapping->kind != ResourceKind::StopPointGroup ||
            !mapping->stop_group)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "stop_group_unavailable",
                "Stop group replacement requires its typed handle");
        }
        StopSubscriptionGroupDefinition definition = BuildPcGroup(
            payload,
            request.invocation_id,
            request.request_id,
            StopDeliveryMode::Observe);
        definition.id =
            mapping->stop_group->lease().group_id;
        definition.source.id =
            mapping->stop_group->lease().source_id;
        const bool replacement_epoch_agnostic =
            definition.epoch_policy !=
                StopEpochPolicy::EndOnEpochChange;
        if (replacement_epoch_agnostic !=
            !mapping->origin_epoch.has_value())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "stop_group_epoch_policy_changed",
                "Stop group replacement cannot change the resource handle epoch policy");
        }
        if (definition.subscriptions.empty())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "invalid_stop_group",
                "Replacement stop group requires bounded PC alternatives");
        }
        for (StopSubscriptionDefinition& subscription :
             definition.subscriptions)
        {
            subscription.consumer = &stop_consumer;
        }
        const StopSubscriptionGroupDefinition retained_definition =
            definition;
        const StopGroupReceipt replaced =
            mapping->stop_group->Replace(std::move(definition));
        if (!replaced.ok)
        {
            return service_failure(
                "stop_group_replacement_failed",
                replaced.error.message);
        }
        mapping->stop_group_definition = retained_definition;
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.output = ResourceHandleGraph(
            action,
            mapping->handle,
            mapping->origin_epoch);
        return Immediate(std::move(completion));
    }
    case CanonicalAction::InputAcquireLease:
    {
        InputArbiter* input = session.input_arbiter();
        if (!input)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Unsupported,
                "input_unavailable",
                "InputArbiter is unavailable");
        }
        InputLeaseRequest acquire;
        acquire.owner =
            InputOwnerId(request.invocation_id.value());
        acquire.port = static_cast<std::uint8_t>(
            UnsignedOr(payload, Field::Port, 0));
        acquire.priority = static_cast<std::int32_t>(
            payload.Signed(Field::Priority).value_or(0));
        acquire.suspendable =
            BooleanOr(payload, Field::Suspendable, true);
        acquire.interruption_borrowable = BooleanOr(
            payload,
            Field::InterruptionBorrowable,
            false);
        acquire.require_neutral_acknowledgement =
            BooleanOr(
                payload,
                Field::RequireNeutralAcknowledgement,
                true);
        acquire.movie_exclusive =
            BooleanOr(payload, Field::MovieExclusive, false);
        const InputLeaseReceipt leased =
            input->Acquire(acquire, request.expected_epoch);
        if (!leased.ok)
        {
            return service_failure(
                "input_lease_failed",
                leased.message);
        }
        auto neutral = std::make_shared<
            std::optional<InputPublicationToken>>();
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::InputLease,
            kInputService,
            leased.epoch,
            [input, lease = leased.lease, neutral](
                const ResourceReleaseRequest& release) mutable {
                if (release.reason ==
                        ResourceReleaseReason::StateEpochChanged)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::
                            SupersededByStateReplacement,
                        {}};
                }
                if (!*neutral)
                {
                    const InputReleaseReceipt begun =
                        input->BeginRelease(
                            lease,
                            release.current_epoch);
                    if (!begun.ok)
                    {
                        return ResourceReleaseResult{
                            ResourceReleaseStatus::Failed,
                            begun.message};
                    }
                    if (begun.status ==
                        InputLeaseStatus::Released)
                    {
                        return ResourceReleaseResult{
                            ResourceReleaseStatus::Released,
                            {}};
                    }
                    *neutral = begun.neutral_publication;
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::
                            CleanupExecutionRequired,
                        begun.message};
                }
                const InputReleaseReceipt completed =
                    input->CompleteRelease(
                        lease,
                        **neutral,
                        release.current_epoch);
                if (completed.ok &&
                    completed.status ==
                        InputLeaseStatus::Released)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                return ResourceReleaseResult{
                    completed.ok
                        ? ResourceReleaseStatus::
                              CleanupExecutionRequired
                        : ResourceReleaseStatus::Failed,
                    completed.message};
            },
            leased.lease.value(),
            {},
            "program input lease",
            diagnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::InputPublishHeld:
    case CanonicalAction::InputNeutralize:
    case CanonicalAction::InputAwaitGuestPoll:
    {
        InputArbiter* input = session.input_arbiter();
        ResourceMapping* mapping = require_handle();
        if (!input || !mapping ||
            mapping->kind != ResourceKind::InputLease)
        {
            return Reject(
                request,
                input
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "input_lease_unavailable",
                "Input action requires a current typed input lease");
        }
        const InputLeaseId lease(mapping->concrete_id);
        if (action ==
            CanonicalAction::InputAwaitGuestPoll)
        {
            const auto publication =
                payload.Unsigned(Field::Publication);
            if (!publication)
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "publication_required",
                    "Input poll requires a publication token");
            }
            if (payload.Contains(Field::ParentHandle))
            {
                const auto exact =
                    InputPublicationEvidenceFromPayload(payload);
                if (!exact || exact->lease != lease ||
                    exact->epoch != request.expected_epoch)
                {
                    return Reject(
                        request,
                        ProgramActionCompletionStatus::Rejected,
                        "input_publication_mismatch",
                        "Input poll receipt does not match its exact lease and epoch");
                }
                const InputArbiterOperationReceipt validated =
                    input->ValidatePublication(*exact);
                if (!validated.ok)
                {
                    return Reject(
                        request,
                        ProgramActionCompletionStatus::Rejected,
                        "input_publication_mismatch",
                        std::string(validated.message));
                }
            }
            const InputAcknowledgementReceipt observed =
                input->Observe(
                    lease,
                    InputPublicationToken(*publication),
                    request.expected_epoch);
            if (!observed.ok)
            {
                return service_failure(
                    "input_poll_failed",
                    observed.message);
            }
            CanonicalActionPayload result;
            (void)result.AddBoolean(
                Field::ResultAcknowledged,
                observed.acknowledged);
            (void)result.AddUnsigned(
                Field::ResultSequence,
                observed.receipt.value());
            (void)result.AddUnsigned(
                Field::Publication,
                observed.publication.value());
            (void)result.AddUnsigned(
                Field::ResultEpoch,
                observed.epoch.value());
            return CompleteWithPayload(
                std::move(request),
                std::move(result));
        }

        savor::GCInputFrame frame{};
        if (action ==
            CanonicalAction::InputPublishHeld)
        {
            const auto encoded =
                payload.Bytes(Field::InputFrame);
            if (!encoded ||
                !DecodeInputFrame(*encoded, frame))
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "invalid_input_frame",
                    "Held input requires one canonical GC frame");
            }
        }
        const InputPublicationReceipt published =
            input->Publish(
                lease,
                frame,
                request.expected_epoch);
        if (!published.ok)
        {
            return service_failure(
                "input_publish_failed",
                published.message);
        }
        CanonicalActionPayload result;
        if (action == CanonicalAction::InputPublishHeld)
        {
            (void)result.AddUnsigned(
                Field::Handle,
                published.lease.value());
        }
        (void)result.AddUnsigned(
            Field::Publication,
            published.publication.value());
        (void)result.AddBytes(
            Field::ResultFrame,
            EncodeInputFrame(published.frame));
        (void)result.AddUnsigned(
            Field::ResultEpoch,
            published.epoch.value());
        if (action == CanonicalAction::InputNeutralize &&
            BooleanOr(
                payload,
                Field::RequireNeutralAcknowledgement,
                false))
        {
            const InputNeutralWitnessReceipt witness =
                input->ProveNeutralWitness(
                    lease,
                    published.publication,
                    request.expected_epoch);
            if (!witness.ok)
            {
                return service_failure(
                    "neutral_witness_failed",
                    witness.message);
            }
            (void)result.AddUnsigned(
                Field::ResultSequence,
                witness.witness.value());
            (void)result.AddBoolean(
                Field::ResultAcknowledged,
                static_cast<bool>(
                    witness.acknowledgement));
        }
        return CompleteWithPayload(
            std::move(request),
            std::move(result));
    }
    case CanonicalAction::InputPublishPulse:
    case CanonicalAction::InputPublishSequence:
    {
        InputArbiter* input = session.input_arbiter();
        ResourceMapping* mapping = require_handle();
        if (!input || !mapping ||
            mapping->kind != ResourceKind::InputLease)
        {
            return Reject(
                request,
                input
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "input_lease_unavailable",
                "Input sequence requires a current typed input lease");
        }
        std::vector<savor::GCInputFrame> frames;
        if (action == CanonicalAction::InputPublishPulse)
        {
            savor::GCInputFrame held{};
            const auto encoded =
                payload.Bytes(Field::InputFrame);
            if (!encoded ||
                !DecodeInputFrame(*encoded, held))
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "invalid_input_frame",
                    "Input pulse requires one canonical GC frame");
            }
            frames = {held, savor::GCInputFrame{}};
        }
        else
        {
            const auto encoded =
                payload.Bytes(Field::InputFrames);
            if (!encoded ||
                !DecodeInputFrames(*encoded, frames))
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "invalid_input_sequence",
                    "Input sequence is empty, malformed, or over its bound");
            }
        }
        const auto binding = input->CreateAdvanceBinding(
            InputLeaseId(mapping->concrete_id),
            frames,
            request.expected_epoch,
            static_cast<std::uint32_t>(UnsignedOr(
                payload,
                Field::RetryLimit,
                1)));
        if (!binding.ok)
        {
            return service_failure(
                "input_binding_failed",
                binding.message);
        }
        ExecutionRequestPolicy policy =
            ExecutionPolicy(request, payload);
        policy.input_relationship = binding.binding;
        const std::uint32_t maximum =
            static_cast<std::uint32_t>(frames.size());
        return SubmitExecutionAction(
            std::move(request),
            InputSynchronizedAdvanceRequest{
                std::move(policy),
                binding.binding,
                maximum},
            PendingKind::InputAdvance,
            binding.binding);
    }
    case CanonicalAction::MovieStartPlayback:
    case CanonicalAction::MovieStartRecording:
    {
        MovieService* movies = session.movie_service();
        if (!movies)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Unsupported,
                "movie_service_unavailable",
                "MovieService is unavailable");
        }
        MovieOperationReceipt started;
        std::filesystem::path declared_artifact;
        if (action ==
            CanonicalAction::MovieStartPlayback)
        {
            const auto path = payload.Utf8(Field::Path);
            if (!path || path->empty())
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "movie_path_required",
                    "Movie playback requires a caller-declared DTM path");
            }
            MoviePlaybackRequest playback;
            playback.dtm_path = std::filesystem::path(*path);
            playback.boot.diagnostic_label = std::string(
                payload.Utf8(Field::Label).value_or(
                    "program movie playback"));
            started =
                movies->StartReadOnlyPlayback(playback);
        }
        else
        {
            MovieRecordingRequest recording;
            recording.diagnostic_label = std::string(
                payload.Utf8(Field::Label).value_or(
                    "program movie recording"));
            if (const auto path = payload.Utf8(Field::Path))
                declared_artifact = std::filesystem::path(*path);
            if (declared_artifact.empty())
            {
                return Reject(
                    request,
                    ProgramActionCompletionStatus::Rejected,
                    "movie_artifact_path_required",
                    "Movie recording start must declare its immutable DTM output path");
            }
            started = movies->StartRecording(recording);
        }
        if (!started.result.ok)
        {
            return service_failure(
                "movie_start_failed",
                started.result.message);
        }
        PruneReleasedResources();
        const MovieActivity activity = started.activity;
        auto finalized = std::make_shared<bool>(false);
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::MovieSession,
            kMovieService,
            started.state_epoch,
            [movies,
             activity,
             acquisition_epoch = started.state_epoch,
             finalized](
                const ResourceReleaseRequest& release) {
                if (*finalized)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                const MovieActivity current =
                    movies->activity();
                if (current == MovieActivity::Inactive)
                {
                    *finalized = true;
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        release.current_epoch != acquisition_epoch
                            ? "movie session was reconciled inactive by state replacement"
                            : std::string{}};
                }
                if (current != activity)
                {
                    if (release.current_epoch != acquisition_epoch)
                    {
                        // The restored state owns the new movie mode. A stale
                        // epoch-agnostic handle must never stop that activity.
                        *finalized = true;
                        return ResourceReleaseResult{
                            ResourceReleaseStatus::Released,
                            "movie session was superseded by state replacement"};
                    }
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Failed,
                        "movie service activity no longer matches its live program handle"};
                }
                const MovieOperationReceipt stopped =
                    activity == MovieActivity::Recording
                    ? movies->CancelRecording()
                    : movies->StopPlayback();
                if (stopped.result.ok)
                    *finalized = true;
                return ResourceReleaseResult{
                    stopped.result.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    stopped.result.message};
            },
            static_cast<std::uint64_t>(activity),
            {},
            "program movie session",
            diagnostic,
            ResourceEpochPolicy::EpochAgnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        ResourceMapping* mapping = Resource(resource->handle);
        if (mapping)
        {
            mapping->artifact_path = std::move(declared_artifact);
            mapping->finalized = std::move(finalized);
        }
        ProgramActionDispatchResult completed =
            complete_resource(std::move(request), *resource);
        if (completed.immediate_completion)
        {
            completed.immediate_completion->resulting_epoch =
                started.state_epoch;
        }
        return completed;
    }
    case CanonicalAction::MovieStopPlayback:
    {
        ResourceMapping* mapping = require_handle();
        if (!mapping ||
            mapping->kind != ResourceKind::MovieSession)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "movie_session_unavailable",
                "Movie stop requires its typed playback handle");
        }
        return ReleaseMappedResource(
            std::move(request),
            *mapping);
    }
    case CanonicalAction::MovieStopRecording:
    {
        MovieService* movies = session.movie_service();
        ResourceMapping* mapping = require_handle();
        if (!movies || !mapping ||
            mapping->kind != ResourceKind::MovieSession ||
            mapping->artifact_path.empty())
        {
            return Reject(
                request,
                movies
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "movie_session_unavailable",
                "Movie finalization requires its typed recording handle");
        }
        const MovieOperationReceipt finalized =
            movies->FinalizeRecording(
                MovieFinalizeRequest{mapping->artifact_path});
        if (!finalized.result.ok)
        {
            return service_failure(
                "movie_finalize_failed",
                finalized.result.message);
        }
        if (mapping->finalized)
            *mapping->finalized = true;
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.output = ArtifactResultGraph(
            action,
            "movie:" + std::to_string(
                request.request_id.value()),
            finalized.artifact_path.string(),
            finalized.dtm_sha256);
        if (completion.output.values.empty())
        {
            session.MarkTainted(
                "Finalized movie artifact could not be projected");
            return service_failure(
                "result_encoding_failed",
                "Finalized movie artifact reference could not be encoded");
        }
        SessionResourceLedger* ledger = session.resources();
        SessionResourceBindingTable* bindings =
            session.resource_bindings();
        if (!ledger || !bindings ||
            !ledger->Release(mapping->receipt, *bindings).completed())
        {
            session.MarkTainted(
                "Finalized movie resource could not be released");
            return Reject(
                request,
                ProgramActionCompletionStatus::CleanupFailed,
                "movie_cleanup_failed",
                "Finalized movie resource could not be released");
        }
        resources.erase(mapping->handle.value());
        return Immediate(std::move(completion));
    }
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
    {
        GuestMemory* memory = session.guest_memory();
        const auto address = payload.Unsigned(Field::Address);
        if (!memory || !address ||
            *address > std::numeric_limits<std::uint32_t>::max())
        {
            return Reject(
                request,
                memory
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "invalid_guest_read",
                "Guest scalar read requires GuestMemory and a 32-bit address");
        }
        GuestScalarWidth width = GuestScalarWidth::U8;
        if (action == CanonicalAction::GuestReadU16)
            width = GuestScalarWidth::U16;
        else if (action == CanonicalAction::GuestReadU32)
            width = GuestScalarWidth::U32;
        else if (action == CanonicalAction::GuestReadU64)
            width = GuestScalarWidth::U64;
        const GuestReadReceipt read = memory->ReadScalar(
            static_cast<std::uint32_t>(*address),
            width,
            request.expected_epoch);
        if (!read.ok)
            return service_failure("guest_read_failed", read.message);
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        switch (action)
        {
        case CanonicalAction::GuestReadU8:
            completion.output = ScalarResultGraph(
                action,
                static_cast<std::uint8_t>(read.value));
            break;
        case CanonicalAction::GuestReadU16:
            completion.output = ScalarResultGraph(
                action,
                static_cast<std::uint16_t>(read.value));
            break;
        case CanonicalAction::GuestReadU32:
            completion.output = ScalarResultGraph(
                action,
                static_cast<std::uint32_t>(read.value));
            break;
        default:
            completion.output =
                ScalarResultGraph(action, read.value);
            break;
        }
        return Immediate(std::move(completion));
    }
    case CanonicalAction::GuestRunCoherentQuery:
        return Reject(
            request,
            ProgramActionCompletionStatus::Unsupported,
            "query_identity_required",
            "Generic coherent-query dispatch has no registered query identity; use an exact capability-pack action");
    case CanonicalAction::GuestWriteData:
    case CanonicalAction::GuestPatchExecutable:
    {
        GuestMutationService* mutations =
            session.guest_mutations();
        const auto address = payload.Unsigned(Field::Address);
        const auto expected = payload.Unsigned(Field::Expected);
        const auto replacement =
            payload.Unsigned(Field::Replacement);
        const auto width_value =
            payload.Unsigned(Field::Width);
        if (!mutations || !address || !expected ||
            !replacement || !width_value ||
            *address >
                std::numeric_limits<std::uint32_t>::max() ||
            (*width_value != 1 && *width_value != 2 &&
             *width_value != 4))
        {
            return Reject(
                request,
                mutations
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "invalid_mutation_request",
                "Guest mutation request is incomplete or outside supported widths");
        }
        GuestMutationRequest mutation;
        mutation.owner =
            MutationOwnerId(request.invocation_id.value());
        mutation.scope = MutationScopeId(
            Scope(request.scope).value());
        mutation.epoch = request.expected_epoch;
        mutation.address =
            static_cast<std::uint32_t>(*address);
        mutation.width =
            static_cast<GuestScalarWidth>(*width_value);
        mutation.expected = *expected;
        mutation.replacement = *replacement;
        mutation.mask =
            UnsignedOr(payload, Field::Mask, ~std::uint64_t{0});
        mutation.kind =
            action == CanonicalAction::GuestPatchExecutable
            ? GuestMutationKind::ExecutablePatch
            : GuestMutationKind::Data;
        const GuestMutationReceipt applied =
            mutations->Apply(mutation);
        if (!applied.ok)
        {
            return service_failure(
                "guest_mutation_failed",
                applied.message);
        }
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::GuestMutation,
            kMutationService,
            applied.epoch,
            [mutations, mutation_id = applied.mutation](
                const ResourceReleaseRequest& release) {
                if (release.reason ==
                        ResourceReleaseReason::StateEpochChanged)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::
                            SupersededByStateReplacement,
                        {}};
                }
                const GuestMutationReceipt restored =
                    mutations->Restore(
                        mutation_id,
                        release.current_epoch);
                return ResourceReleaseResult{
                    restored.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    restored.message};
            },
            applied.mutation.value(),
            {},
            "program guest mutation",
            diagnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::CaptureAttach:
    {
        CaptureService* capture = session.capture_service();
        const auto profile = payload.Utf8(Field::ProfileJson);
        const auto path = payload.Utf8(Field::Path);
        if (!capture || !profile || !path || path->empty())
        {
            return Reject(
                request,
                capture
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "capture_unavailable",
                "Capture attachment requires CaptureService, an opaque profile, and a caller-declared path");
        }
        CaptureAttachmentRequest attach;
        attach.profile_json = std::string(*profile);
        attach.options.capture_path =
            std::filesystem::path(*path);
        attach.expected_epoch = request.expected_epoch;
        const CaptureServiceReceipt attached =
            capture->Attach(std::move(attach));
        if (!attached.ok)
        {
            return service_failure(
                "capture_attach_failed",
                attached.error.message);
        }
        auto finalized = std::make_shared<bool>(false);
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::CaptureAttachment,
            kCaptureService,
            attached.epoch,
            [capture,
             attachment = attached.attachment,
             finalized](
                const ResourceReleaseRequest&) {
                if (*finalized)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                const CaptureServiceReceipt detached =
                    capture->Detach(attachment);
                if (detached.ok)
                    *finalized = true;
                return ResourceReleaseResult{
                    detached.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    detached.error.message};
            },
            attached.attachment.value(),
            {},
            "program capture attachment",
            diagnostic,
            ResourceEpochPolicy::EpochAgnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        ResourceMapping* mapping = Resource(resource->handle);
        if (mapping)
        {
            mapping->artifact_path =
                std::filesystem::path(*path);
            mapping->finalized = std::move(finalized);
        }
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::CaptureMark:
    {
        CaptureService* capture = session.capture_service();
        ResourceMapping* mapping = require_handle();
        const auto marker = payload.Utf8(Field::MarkerId);
        if (!capture || !mapping ||
            mapping->kind != ResourceKind::CaptureAttachment ||
            !marker)
        {
            return Reject(
                request,
                capture
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "capture_attachment_unavailable",
                "Capture marker requires a current attachment and marker ID");
        }
        const CaptureServiceReceipt marked = capture->Mark(
            CaptureAttachmentId(mapping->concrete_id),
            *marker,
            UnsignedOr(payload, Field::MarkerValue, 0));
        if (!marked.ok)
        {
            return service_failure(
                "capture_mark_failed",
                marked.error.message);
        }
        CanonicalActionPayload result;
        (void)result.AddUnsigned(
            Field::ResultSequence,
            marked.dispatch_generation.value());
        return CompleteWithPayload(
            std::move(request),
            std::move(result));
    }
    case CanonicalAction::CaptureFinalize:
    {
        CaptureService* capture = session.capture_service();
        ResourceMapping* mapping = require_handle();
        if (!capture || !mapping ||
            mapping->kind != ResourceKind::CaptureAttachment)
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "capture_attachment_unavailable",
                "Capture finalization requires its typed attachment handle");
        }
        const ProgramResourceHandleId handle =
            mapping->handle;
        const std::filesystem::path artifact_path =
            mapping->artifact_path;
        CaptureServiceReceipt finalized =
            capture->Detach(
                CaptureAttachmentId(mapping->concrete_id));
        if (!finalized.ok)
        {
            if (finalized.requires_session_taint)
            {
                session.MarkTainted(
                    finalized.error.message.empty()
                        ? "Capture finalization left session integrity unproven"
                        : finalized.error.message);
            }
            return service_failure(
                "capture_finalize_failed",
                finalized.error.message);
        }
        if (mapping->finalized)
            *mapping->finalized = true;

        std::string digest;
        try
        {
            if (artifact_path.empty() ||
                !std::filesystem::is_regular_file(
                    artifact_path))
            {
                return service_failure(
                    "capture_artifact_missing",
                    "Capture finalization did not publish its declared artifact");
            }
            digest = hash::sha256_of_file(
                artifact_path.string());
        }
        catch (const std::exception& ex)
        {
            return service_failure(
                "capture_artifact_hash_failed",
                ex.what());
        }

        SessionResourceLedger* ledger =
            session.resources();
        SessionResourceBindingTable* bindings =
            session.resource_bindings();
        if (!ledger || !bindings)
        {
            return service_failure(
                "resource_ledger_unavailable",
                "Capture resource ledger is unavailable after finalization");
        }
        ResourceUnwindResult released =
            ledger->Release(mapping->receipt, *bindings);
        if (!released.completed())
        {
            session.MarkTainted(
                "Finalized capture resource could not be retired");
            return service_failure(
                "capture_resource_release_failed",
                released.error.message.empty()
                    ? "Finalized capture resource could not be retired"
                    : released.error.message);
        }
        std::vector<CleanupReceipt> cleanup_receipts =
            MakeCleanupReceipts(released.steps);
        resources.erase(handle.value());

        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.output = ArtifactListResultGraph(
            action,
            "capture:" +
                std::to_string(
                    request.request_id.value()),
            artifact_path.string(),
            std::move(digest),
            finalized.capture_complete &&
                finalized.artifacts_finalized);
        completion.cleanup_receipts =
            std::move(cleanup_receipts);
        if (completion.output.values.empty())
        {
            return service_failure(
                "result_encoding_failed",
                "Capture artifact references could not be encoded");
        }
        return Immediate(std::move(completion));
    }
    case CanonicalAction::ScreenshotCapture:
    {
        const auto path = payload.Utf8(Field::Path);
        if (!path || path->empty())
        {
            return Reject(
                request,
                ProgramActionCompletionStatus::Rejected,
                "screenshot_path_required",
                "Screenshot capture requires a caller-declared path");
        }
        const auto timeout = Timeout(request, payload);
        const SessionOperationReceipt captured =
            session.CaptureScreenshot(
                std::filesystem::path(*path),
                timeout);
        if (!captured.ok)
        {
            return service_failure(
                "screenshot_failed",
                captured.backend.message);
        }
        std::string digest;
        try
        {
            digest = hash::sha256_of_file(std::string(*path));
        }
        catch (const std::exception& ex)
        {
            return service_failure(
                "screenshot_hash_failed",
                ex.what());
        }
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::Completed);
        completion.output = ArtifactResultGraph(
            action,
            "screenshot:" +
                std::to_string(request.request_id.value()),
            std::string(*path),
            std::move(digest));
        if (completion.output.values.empty())
        {
            return service_failure(
                "result_encoding_failed",
                "Screenshot artifact reference could not be encoded");
        }
        return Immediate(std::move(completion));
    }
    case CanonicalAction::TelemetryEmit:
    {
        TelemetryBus* telemetry = session.telemetry();
        const auto source = payload.Utf8(Field::TelemetrySource);
        const auto kind = payload.Utf8(Field::TelemetryKind);
        const auto text = payload.Utf8(Field::TelemetryPayload);
        if (!telemetry || !source || !kind || !text)
        {
            return Reject(
                request,
                telemetry
                    ? ProgramActionCompletionStatus::Rejected
                    : ProgramActionCompletionStatus::Unsupported,
                "invalid_telemetry",
                "Telemetry action requires source, kind, and bounded payload");
        }
        TelemetryEvent event;
        event.source = std::string(*source);
        event.kind = std::string(*kind);
        event.severity =
            static_cast<TelemetrySeverity>(UnsignedOr(
                payload,
                Field::TelemetrySeverity,
                static_cast<std::uint64_t>(
                    TelemetrySeverity::Info)));
        event.epoch = request.expected_epoch;
        event.payload = std::string(*text);
        event.record_progress =
            BooleanOr(payload, Field::RecordProgress, false);
        event.loss_policy =
            BooleanOr(payload, Field::Required, false)
            ? TelemetryLossPolicy::Required
            : TelemetryLossPolicy::LossyCoalescing;
        const TelemetryEnqueueReceipt enqueued =
            telemetry->Enqueue(std::move(event));
        if (!enqueued.ok)
        {
            if (enqueued.authoritative_overflow)
            {
                session.MarkTainted(
                    "Required program telemetry overflowed");
            }
            return service_failure(
                "telemetry_failed",
                enqueued.message);
        }
        CanonicalActionPayload result;
        (void)result.AddUnsigned(
            Field::ResultSequence,
            enqueued.sequence.value());
        (void)result.AddBoolean(
            Field::ResultAcknowledged,
            !enqueued.dropped);
        return CompleteWithPayload(
            std::move(request),
            std::move(result));
    }
    }
    return Reject(
        request,
        ProgramActionCompletionStatus::Unsupported,
        "unsupported_action",
        "Canonical action is not implemented by the session host");
}

bool SessionProgramActionHost::Impl::SubmitCleanupAdvance(
    PendingExecution& continuation)
{
    if (!continuation.cleanup_continuation ||
        !continuation.cleanup_cancellation ||
        continuation.cleanup_advances >=
            config.maximum_cleanup_advances)
    {
        return false;
    }

    const SessionSnapshot current = session.snapshot();
    ExecutionRequestPolicy policy;
    policy.expected_epoch = current.state_epoch;
    policy.movie_ended = MovieEndedPolicy::Ignore;
    policy.throttle = ExecutionThrottlePolicy::Preserve;
    policy.current_point =
        ExecutionCurrentPointPolicy::Ignore;
    policy.interruptions =
        ExecutionInterruptionPolicy::Reject;
    policy.cancellation =
        continuation.cleanup_cancellation->token();

    const ExecutionSubmissionReceipt submitted =
        session.SubmitExecution(
            StepFramesRequest{std::move(policy), 1});
    if (!submitted.accepted)
        return false;
    continuation.operation = submitted.operation_id;
    ++continuation.cleanup_advances;
    return true;
}

ProgramActionCompletion
SessionProgramActionHost::Impl::ExecutionCompletion(
    const PendingExecution& operation,
    const ExecutionTerminalResult& terminal)
{
    ProgramActionCompletion completion = Completion(
        operation.request,
        ExecutionCompletionStatus(terminal));
    completion.resulting_epoch = terminal.state_epoch
        ? terminal.state_epoch
        : session.snapshot().state_epoch;
    completion.message = terminal.error.message;
    completion.code =
        terminal.status == ExecutionTerminalStatus::CoreStalled
        ? "core_stalled"
        : "execution_terminal_" +
              std::to_string(
                  static_cast<std::uint32_t>(terminal.status));

    if (!ExecutionSucceeded(terminal) ||
        !operation.request.action)
    {
        return completion;
    }

    const auto action =
        ResolveCanonicalAction(*operation.request.action);
    const auto schema = action
        ? CanonicalActionOutputSchemaIdentity(*action)
        : std::nullopt;
    if (!schema)
    {
        completion.status =
            ProgramActionCompletionStatus::Failed;
        completion.code = "result_encoding_failed";
        completion.message =
            "Execution action has no canonical result schema";
        return completion;
    }

    if (*action == CanonicalAction::ExecutionContinueUntil)
    {
        completion.output =
            ContinueUntilResultGraph(terminal);
        if (completion.output.values.empty())
        {
            completion.status =
                ProgramActionCompletionStatus::Failed;
            completion.code = "result_encoding_failed";
            completion.message =
                "ContinueUntil completed without exact routed stop evidence";
            return completion;
        }
        completion.code.clear();
        completion.message.clear();
        return completion;
    }

    if (*action == CanonicalAction::InputPublishPulse ||
        *action == CanonicalAction::InputPublishSequence)
    {
        CanonicalActionPayload publication;
        if (!terminal.input_publication ||
            !AddInputPublicationResult(
                publication,
                *terminal.input_publication))
        {
            completion.status =
                ProgramActionCompletionStatus::Failed;
            completion.code = "result_encoding_failed";
            completion.message =
                "Input publication completed without exact lease, frame, token, and epoch evidence";
            return completion;
        }
        CanonicalActionPayloadResult encoded =
            EncodeCanonicalActionPayload(
                publication,
                *schema);
        if (!encoded.ok)
        {
            completion.status =
                ProgramActionCompletionStatus::Failed;
            completion.code = "result_encoding_failed";
            completion.message =
                std::move(encoded.diagnostic);
            return completion;
        }
        completion.output = std::move(encoded.graph);
        completion.code.clear();
        completion.message.clear();
        return completion;
    }

    CanonicalActionPayload payload;
    (void)payload.AddUnsigned(
        Field::ResultStatus,
        static_cast<std::uint64_t>(terminal.status));
    (void)payload.AddUnsigned(
        Field::CompletedCount,
        terminal.completed_count);
    (void)payload.AddUnsigned(
        Field::ResultEpoch,
        completion.resulting_epoch.value());
    (void)payload.AddUnsigned(
        Field::ResultPc,
        terminal.evidence.pc);
    if (terminal.stop)
    {
        (void)payload.AddUnsigned(
            Field::ResultStopSequence,
            terminal.stop->identity.sequence.value());
    }
    CanonicalActionPayloadResult encoded =
        EncodeCanonicalActionPayload(payload, *schema);
    if (!encoded.ok)
    {
        completion.status =
            ProgramActionCompletionStatus::Failed;
        completion.code = "result_encoding_failed";
        completion.message = std::move(encoded.diagnostic);
        return completion;
    }
    completion.output = std::move(encoded.graph);
    completion.code.clear();
    completion.message.clear();
    return completion;
}

void SessionProgramActionHost::Impl::CompletePendingExecution(
    ExecutionTerminalResult terminal)
{
    if (!pending || pending->operation != terminal.operation_id)
        return;
    if (pending->kind == PendingKind::Cleanup)
    {
        CompleteCleanup(std::move(terminal));
        return;
    }

    PendingExecution completed = std::move(*pending);
    pending.reset();
    ProgramActionCompletion completion =
        ExecutionCompletion(completed, terminal);

    if (completed.input_binding)
    {
        InputArbiter* input = session.input_arbiter();
        const InputArbiterOperationReceipt removed = input
            ? input->RemoveAdvanceBinding(
                  *completed.input_binding)
            : InputArbiterOperationReceipt{
                  false,
                  InputArbiterErrorCode::Stopped,
                  "InputArbiter is unavailable"};
        if (!removed.ok)
        {
            session.MarkTainted(
                "Program input advance binding could not be retired");
            completion.status =
                ProgramActionCompletionStatus::CleanupFailed;
            completion.cleanup =
                ProgramCleanupStatus::Tainted;
            completion.code =
                "input_binding_cleanup_failed";
            completion.message =
                std::string(removed.message);
            completion.output = {};
        }
    }
    if (terminal.integrity == BackendIntegrity::Unknown ||
        terminal.status ==
            ExecutionTerminalStatus::CleanupFailure)
    {
        session.MarkTainted(
            terminal.error.message.empty()
                ? "Program execution completion left session integrity unproven"
                : terminal.error.message);
        completion.cleanup = ProgramCleanupStatus::Tainted;
        completion.session_disposition =
            SessionDisposition::Tainted;
    }
    (void)Queue(std::move(completion));
}

void SessionProgramActionHost::Impl::CompleteCleanup(
    ExecutionTerminalResult terminal)
{
    if (!pending || pending->kind != PendingKind::Cleanup ||
        pending->operation != terminal.operation_id)
    {
        return;
    }
    PendingExecution continuation = std::move(*pending);
    pending.reset();

    const auto fail = [&](std::string code, std::string message) {
        session.MarkTainted(message);
        ProgramActionCompletion completion = Completion(
            continuation.request,
            ProgramActionCompletionStatus::CleanupFailed,
            std::move(code),
            std::move(message));
        completion.cleanup = ProgramCleanupStatus::Tainted;
        completion.session_disposition =
            SessionDisposition::Tainted;
        (void)Queue(std::move(completion));
    };

    if (!ExecutionSucceeded(terminal))
    {
        fail(
            "cleanup_execution_failed",
            terminal.error.message.empty()
                ? "Bounded cleanup frame advance failed"
                : terminal.error.message);
        return;
    }

    SessionResourceLedger* ledger = session.resources();
    SessionResourceBindingTable* bindings =
        session.resource_bindings();
    if (!ledger || !bindings ||
        !continuation.cleanup_continuation)
    {
        fail(
            "cleanup_ledger_unavailable",
            "Program resource cleanup continuation is unavailable");
        return;
    }

    ResourceUnwindResult unwind = ledger->ContinueCleanup(
        *continuation.cleanup_continuation,
        *bindings);
    if (unwind.outcome ==
            ResourceUnwindOutcome::CleanupExecutionRequired &&
        unwind.cleanup_execution_request)
    {
        continuation.cleanup_continuation =
            unwind.cleanup_execution_request->continuation;
        pending.emplace(std::move(continuation));
        if (SubmitCleanupAdvance(*pending))
            return;
        ProgramActionRequest request =
            std::move(pending->request);
        pending.reset();
        session.MarkTainted(
            "Program resource cleanup exceeded or could not submit its bounded advance");
        ProgramActionCompletion completion = Completion(
            request,
            ProgramActionCompletionStatus::CleanupFailed,
            "cleanup_advance_exhausted",
            "Program resource cleanup exceeded or could not submit its bounded advance");
        completion.cleanup = ProgramCleanupStatus::Tainted;
        completion.session_disposition =
            SessionDisposition::Tainted;
        (void)Queue(std::move(completion));
        return;
    }
    if (!unwind.completed())
    {
        fail(
            "cleanup_failed",
            unwind.error.message.empty()
                ? "Program resource cleanup could not be proven"
                : unwind.error.message);
        return;
    }

    std::vector<CleanupReceipt> cleanup_receipts =
        MakeCleanupReceipts(unwind.steps);
    PruneReleasedResources();
    if (continuation.releasing_resource)
    {
        resources.erase(
            continuation.releasing_resource.value());
    }
    if (continuation.closes_scope)
    {
        if (continuation.finishes_invocation)
        {
            scopes.clear();
            resources.clear();
            active.reset();
        }
        else
        {
            scopes.erase(
                continuation.request.scope.value());
        }
    }

    ProgramActionCompletion completion = Completion(
        continuation.request,
        ProgramActionCompletionStatus::Completed);
    completion.cleanup_receipts =
        std::move(cleanup_receipts);
    completion.cleanup = CleanupStatusOf(unwind.disposition);
    if (unwind.disposition ==
        ResourceCleanupDisposition::TaintRequired)
    {
        session.MarkTainted(
            "Program resource cleanup requires session taint");
        completion.status =
            ProgramActionCompletionStatus::CleanupFailed;
        completion.session_disposition =
            SessionDisposition::Tainted;
    }
    (void)Queue(std::move(completion));
}

void SessionProgramActionHost::Impl::
ReleaseSavedArtifactRecords() noexcept
{
    StateService* states = session.state_service();
    if (!states)
    {
        saved_artifacts.clear();
        return;
    }
    for (const auto& [identity, artifact] : saved_artifacts)
    {
        (void)identity;
        const StateServiceResult released =
            states->ReleaseFileArtifact(artifact.artifact);
        if (!released.ok &&
            released.code != StateServiceErrorCode::NotFound)
        {
            session.MarkCleanWithDiagnostics(
                released.message.empty()
                    ? "Program state artifact record could not be released"
                    : released.message);
        }
    }
    saved_artifacts.clear();
}

SessionProgramActionHost::SessionProgramActionHost(
    EmulationSession& session,
    SessionProgramActionHostConfig config)
    : impl_(std::make_unique<Impl>(
          session,
          std::move(config)))
{
}

SessionProgramActionHost::~SessionProgramActionHost() = default;

ProgramActionDispatchResult SessionProgramActionHost::Dispatch(
    ProgramActionRequest request)
{
    return impl_->Dispatch(std::move(request));
}

void SessionProgramActionHost::RequestCancellation(
    InvocationId invocation_id,
    CancellationReason reason) noexcept
{
    if (!impl_ || !impl_->BindOrCheckOwner() ||
        !impl_->active ||
        impl_->active->invocation != invocation_id)
    {
        return;
    }
    (void)impl_->active->cancellation.request_cancellation(
        reason);
    if (impl_->pending)
        (void)impl_->session.CancelExecution(reason);
}

void SessionProgramActionHost::HandleExecutionEvent(
    ExecutionEvent event)
{
    if (!impl_ || !impl_->BindOrCheckOwner() ||
        event.kind != ExecutionEventKind::Terminal ||
        !event.terminal)
    {
        return;
    }
    impl_->CompletePendingExecution(
        std::move(*event.terminal));
}

void SessionProgramActionHost::Pump()
{
    if (!impl_)
        return;
    (void)impl_->BindOrCheckOwner();
}

std::vector<ProgramActionCompletion>
SessionProgramActionHost::DrainCompletions()
{
    if (!impl_ || !impl_->BindOrCheckOwner())
        return {};
    std::vector<ProgramActionCompletion> result;
    result.swap(impl_->completions);
    return result;
}

void SessionProgramActionHost::Shutdown() noexcept
{
    if (!impl_ || !impl_->BindOrCheckOwner() ||
        impl_->stopped)
    {
        return;
    }
    impl_->stopped = true;
    if (impl_->active)
    {
        (void)impl_->active->cancellation.request_cancellation(
            CancellationReason::Shutdown);
    }
    if (impl_->pending)
        (void)impl_->session.CancelExecution(
            CancellationReason::Shutdown);

    SessionResourceLedger* ledger =
        impl_->session.resources();
    SessionResourceBindingTable* bindings =
        impl_->session.resource_bindings();
    if (impl_->active && ledger && bindings &&
        impl_->active->root_scope)
    {
        ResourceUnwindResult unwind =
            ledger->CloseScope(
                impl_->active->root_scope,
                *bindings);
        if (!unwind.completed())
        {
            impl_->session.MarkTainted(
                "Program action host shutdown could not prove invocation resource cleanup");
        }
    }
    impl_->pending.reset();
    impl_->scopes.clear();
    impl_->resources.clear();
    impl_->active.reset();
    impl_->ReleaseSavedArtifactRecords();
}

SessionProgramActionHostSnapshot
SessionProgramActionHost::snapshot() const noexcept
{
    SessionProgramActionHostSnapshot result;
    if (!impl_)
    {
        result.shutdown = true;
        return result;
    }
    result.shutdown = impl_->stopped;
    result.invocation_active = impl_->active.has_value();
    if (impl_->active)
    {
        result.invocation_id = impl_->active->invocation;
        result.attempt_id = impl_->active->attempt;
    }
    result.epoch = impl_->session.snapshot().state_epoch;
    result.mapped_scope_count = impl_->scopes.size();
    result.mapped_resource_count = impl_->resources.size();
    result.execution_pending = impl_->pending.has_value();
    result.queued_completion_count =
        impl_->completions.size();
    return result;
}

} // namespace savor::runtime::program
