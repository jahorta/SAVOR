#include "SessionProgramActionHost.h"

#include "CanonicalActionPayload.h"
#include "../Capabilities/SourceCapabilityPacks.h"
#include "../Capabilities/SourceReducers.h"
#include "../Composition/SemanticObservationComposition.h"
#include "../Model/ProgramValueArena.h"
#include "../Registry/CanonicalActionCatalog.h"
#include "../../EmulationSession.h"
#include "Core/Memory/MemView.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Core/Memory/Soa/SoaStructReaders.h"
#include "Utils/Hash.h"
#include "Utils/Log.h"

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

[[nodiscard]] bool IsDiagnosticAction(CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::InputAcquireLease:
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
    case CanonicalAction::InputCompleteDelivery:
    case CanonicalAction::ExecutionContinueUntil:
    case CanonicalAction::ExecutionStepFrames:
    case CanonicalAction::ExecutionContinueUntilInputObserved:
        return true;
    default:
        return false;
    }
}

constexpr ResourceServiceId kSavestateService{1};
constexpr ResourceServiceId kExecutionService{2};
constexpr ResourceServiceId kStopPointService{3};
constexpr ResourceServiceId kInputService{4};
constexpr ResourceServiceId kMovieService{5};
constexpr ResourceServiceId kMutationService{6};

constexpr std::uint32_t kMaximumStopAlternatives = 128;

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
    WorksetEpoch epoch;
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
        !record || record->fields.size() != 2)
    {
        return false;
    }

    std::uint64_t epoch = 0;
    return ScalarValue(graph, record->fields[0], epoch) &&
        ScalarValue(
            graph,
            record->fields[1],
            output.expected_pc) &&
        epoch != 0 &&
        (output.epoch = WorksetEpoch(epoch), true);
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
    WorksetEpoch epoch,
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
    WorksetEpoch epoch,
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
    WorksetEpoch epoch,
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
    const auto* turn_type_definition =
        soa::battle::find_turn_type_definition(
            static_cast<std::int64_t>(turn_type));
    if (turn_type_definition == nullptr)
    {
        diagnostic = "battle turn type is invalid";
        return false;
    }
    candidate.turn_type = turn_type_definition->type;
    candidate.battle_phase =
        static_cast<std::uint32_t>(battle_phase);
    candidate.turn_count =
        static_cast<std::uint32_t>(current_turn);
    output = std::move(candidate);
    return true;
}

bool ReadNavigationContext(
    GuestMemory& memory,
    WorksetEpoch epoch,
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
    for (const CanonicalActionDefinition& definition :
         CanonicalActionDefinitions())
    {
        if (CanonicalActionIdentity(definition.action) == identity)
            return definition.action;
    }
    return std::nullopt;
}

bool ReadBattleCompletionSnapshot(
    GuestMemory& memory,
    WorksetEpoch epoch,
    battlecompletion::BattleCompletionSnapshotV1& output,
    std::string& diagnostic)
{
    using battlecompletion::BattleCompletionSnapshotV1;
    constexpr std::uint32_t kCharacterData = 0x8030b7f4u;
    constexpr std::uint32_t kNormalExperience = 0x803082f8u;
    constexpr std::uint32_t kMagicExperience = 0x803082fcu;
    constexpr std::uint32_t kGold = 0x80308300u;
    constexpr std::uint32_t kRewardItems = 0x80308304u;
    constexpr std::uint32_t kRng = 0x803469a8u;
    constexpr std::uint32_t kBattleInputState = 0x80347338u;
    constexpr std::uint32_t kRewardPhase = 0x8034737cu;

    BattleCompletionSnapshotV1 snapshot{};
    for (std::size_t index = 0;
         index != battlecompletion::CharacterCount; ++index)
    {
        const auto address = kCharacterData + static_cast<std::uint32_t>(
            index * battlecompletion::CharacterRecordSize);
        const auto bytes = memory.ReadBytes(
            address,
            battlecompletion::CharacterRecordSize,
            epoch);
        if (!bytes.result.ok ||
            bytes.bytes.size() != battlecompletion::CharacterRecordSize)
        {
            diagnostic = "Battle completion character record " +
                std::to_string(index) + " is unavailable";
            return false;
        }
        std::ranges::copy(bytes.bytes, snapshot.character_records[index].begin());
    }
    const auto read_u32 = [&](std::uint32_t address, std::uint32_t& value,
                              std::string_view name) {
        std::uint64_t scalar = 0;
        if (!ReadGuestScalar(
                memory, address, GuestScalarWidth::U32, epoch, scalar))
        {
            diagnostic = std::string(name) + " is unavailable";
            return false;
        }
        value = static_cast<std::uint32_t>(scalar);
        return true;
    };
    if (!read_u32(kNormalExperience, snapshot.normal_experience_reward,
                  "Battle normal-experience reward") ||
        !read_u32(kMagicExperience, snapshot.magic_experience_reward,
                  "Battle magic-experience reward") ||
        !read_u32(kGold, snapshot.gold_reward, "Battle gold reward") ||
        !read_u32(kRng, snapshot.rng_seed, "Battle RNG seed") ||
        !read_u32(kBattleInputState, snapshot.battle_input_state,
                  "Battle input state") ||
        !read_u32(kRewardPhase, snapshot.reward_phase,
                  "Battle reward phase"))
        return false;

    for (std::size_t index = 0;
         index != battlecompletion::RewardItemCount; ++index)
    {
        const auto address = kRewardItems + static_cast<std::uint32_t>(index * 4);
        std::uint64_t item_id = 0;
        const auto tail = memory.ReadBytes(address + 2, 2, epoch);
        if (!ReadGuestScalar(
                memory, address, GuestScalarWidth::U16, epoch, item_id) ||
            !tail.result.ok || tail.bytes.size() != 2)
        {
            diagnostic = "Battle reward item " + std::to_string(index) +
                " is unavailable";
            return false;
        }
        snapshot.reward_items[index] = {
            .item_id = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(item_id)),
            .quantity = tail.bytes[0],
            .opaque = tail.bytes[1],
        };
    }
    output = std::move(snapshot);
    diagnostic.clear();
    return true;
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

bool AddInputExecutionBindingResult(
    CanonicalActionPayload& payload,
    const InputExecutionBindingReceipt& binding)
{
    return binding.lease && binding.binding && binding.epoch &&
        payload.AddUnsigned(
            Field::Handle,
            binding.lease.value()) &&
        payload.AddUnsigned(
            Field::Binding,
            binding.binding.value()) &&
        payload.AddUnsigned(
            Field::Publication,
            binding.publication.value()) &&
        payload.AddUnsigned(
            Field::ResultEpoch,
            binding.epoch.value()) &&
        payload.AddUnsigned(
            Field::StateGeneration,
            binding.state_generation) &&
        payload.AddBytes(
            Field::ResultFrame,
            EncodeInputFrame(binding.frame));
}

std::optional<InputExecutionBindingEvidence>
InputExecutionBindingEvidenceFromPayload(
    const CanonicalActionPayload& payload)
{
    const auto lease = payload.Unsigned(Field::ParentHandle);
    const auto binding = payload.Unsigned(Field::Binding);
    const auto publication =
        payload.Unsigned(Field::Publication);
    const auto epoch = payload.Unsigned(Field::ResultEpoch);
    const auto generation = payload.Unsigned(Field::StateGeneration);
    const auto encoded = payload.Bytes(Field::ResultFrame);
    savor::GCInputFrame frame{};
    if (!lease || !binding || !publication || !epoch ||
        !generation || !encoded ||
        !DecodeInputFrame(*encoded, frame))
    {
        return std::nullopt;
    }
    return InputExecutionBindingEvidence{
        InputLeaseId(*lease),
        InputExecutionBindingId(*binding),
        InputPublicationToken(*publication),
        WorksetEpoch(*epoch),
        *generation,
        frame};
}

bool UsesTypedRequestRecord(CanonicalAction action) noexcept
{
    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
    case CanonicalAction::ExecutionStepFrames:
    case CanonicalAction::ExecutionContinueUntilInputObserved:
    case CanonicalAction::InputAcquireLease:
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
    case CanonicalAction::InputCompleteDelivery:
    case CanonicalAction::MovieAdoptRestoredReadOnlyPlayback:
    case CanonicalAction::MovieObserveState:
    case CanonicalAction::MovieStartRecording:
    case CanonicalAction::GuestReadU8:
    case CanonicalAction::GuestReadU16:
    case CanonicalAction::GuestReadU32:
    case CanonicalAction::GuestReadU64:
    case CanonicalAction::GuestRunCoherentQuery:
    case CanonicalAction::ExecutionRequirePausedPc:
    case CanonicalAction::ExecutionObservePausedPc:
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
    CanonicalActionPayload& payload,
    CanonicalActionPayloadField field = Field::Handle)
{
    if (value.type != CanonicalActionOutputType(producer))
        return false;
    const auto* handle =
        std::get_if<ResourceHandleValue>(&value.payload);
    return handle && handle->handle_id &&
        payload.AddUnsigned(
            field,
            handle->handle_id.value());
}

bool DecodeStopReceipt(
    const ProgramValueGraph& graph,
    const ProgramValue& value,
    WorksetEpoch current_epoch,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    if (value.type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil))
    {
        diagnostic = "Observation stop receipt has the wrong result type";
        return false;
    }
    const auto* record =
        std::get_if<RecordValue>(&value.payload);
    if (!record || record->fields.size() != 6)
    {
        diagnostic = "Observation stop receipt has the wrong result shape";
        return false;
    }
    const ProgramValue* reason_value = FindValue(
        graph,
        record->fields[0]);
    const auto* reason = reason_value
        ? std::get_if<EnumValue>(&reason_value->payload)
        : nullptr;
    if (!reason_value)
    {
        diagnostic =
            "Observation stop receipt lacks a completion reason";
        return false;
    }
    if (reason_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason))
    {
        diagnostic =
            "Observation stop receipt has the wrong completion-reason type";
        return false;
    }
    if (!reason)
    {
        diagnostic =
            "Observation stop receipt has a malformed completion reason";
        return false;
    }
    if (reason->value != static_cast<std::int64_t>(
            ContinueUntilCompletionReasonV1::Breakpoint))
    {
        diagnostic =
            "Observation stop receipt did not complete at a breakpoint: reason=" +
            std::to_string(reason->value);
        return false;
    }
    const ProgramValue* routed_value = nullptr;
    if (!OptionalElement(
            graph,
            record->fields[1],
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt,
            routed_value) ||
        !routed_value ||
        routed_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt))
    {
        diagnostic =
            "Observation stop receipt lacks its routed breakpoint evidence";
        return false;
    }
    const auto* routed = std::get_if<RecordValue>(
        &routed_value->payload);
    if (!routed || routed->fields.size() != 5)
    {
        diagnostic =
            "Observation routed stop receipt has the wrong shape";
        return false;
    }
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint32_t routed_pc = 0;
    std::uint32_t result_pc = 0;
    std::uint64_t sample = 0;
    const ProgramValue* evidence = RequireTypedValue(
        graph,
        routed->fields[4],
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload));
    const auto* evidence_bytes = evidence
        ? std::get_if<std::vector<Byte>>(
              &evidence->payload)
        : nullptr;
    if (!ScalarValue(graph, routed->fields[0], sequence) ||
        !ScalarValue(graph, routed->fields[1], epoch) ||
        !ScalarValue(graph, routed->fields[2], routed_pc) ||
        !ScalarValue(graph, routed->fields[3], sample) ||
        !ScalarValue(graph, record->fields[2], result_pc))
    {
        diagnostic =
            "Observation routed stop receipt has malformed scalar evidence";
        return false;
    }
    if (!evidence_bytes || evidence_bytes->size() < 4 ||
        (*evidence_bytes)[0] != static_cast<Byte>('R') ||
        (*evidence_bytes)[1] != static_cast<Byte>('S') ||
        (*evidence_bytes)[2] != static_cast<Byte>('E') ||
        (*evidence_bytes)[3] != static_cast<Byte>('1'))
    {
        diagnostic =
            "Observation routed stop receipt has malformed evidence bytes";
        return false;
    }
    if (sequence == 0 || sample == 0 || routed_pc == 0)
    {
        diagnostic =
            "Observation routed stop receipt has a zero identity: sequence=" +
            std::to_string(sequence) + ", sample=" +
            std::to_string(sample) + ", pc=" +
            std::to_string(routed_pc);
        return false;
    }
    if (routed_pc != result_pc)
    {
        diagnostic =
            "Observation routed stop PC disagrees with the paused result PC: routed=" +
            std::to_string(routed_pc) + ", result=" +
            std::to_string(result_pc);
        return false;
    }
    if (epoch != current_epoch.value())
    {
        diagnostic =
            "Observation routed stop belongs to another WorksetEpoch: routed=" +
            std::to_string(epoch) + ", active=" +
            std::to_string(current_epoch.value());
        return false;
    }
    if (!payload.AddUnsigned(
            Field::ResultStopSequence,
            sequence) ||
        !payload.AddUnsigned(Field::ResultEpoch, epoch) ||
        !payload.AddUnsigned(Field::ResultPc, routed_pc))
    {
        diagnostic =
            "Observation routed stop receipt could not be encoded";
        return false;
    }
    return true;
}

bool DecodeStopGroupConfig(
    std::span<const Byte> bytes,
    CanonicalActionPayload& payload,
    std::string& diagnostic)
{
    auto decoded = composition::DecodeSemanticPointSetV1(bytes);
    if (!decoded)
    {
        diagnostic = std::move(decoded.diagnostic);
        return false;
    }
    std::vector<Byte> pcs;
    pcs.reserve(decoded.value->program_counters.size() * 4);
    for (const std::uint32_t pc : decoded.value->program_counters)
        for (unsigned shift = 0; shift != 32; shift += 8)
            pcs.push_back(static_cast<Byte>((pc >> shift) & 0xffu));
    std::vector<Byte> sample_descriptor_ids;
    sample_descriptor_ids.reserve(
        decoded.value->hit_time_sample_descriptor_ids.size() * 4);
    for (const std::uint32_t descriptor_id :
         decoded.value->hit_time_sample_descriptor_ids)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            sample_descriptor_ids.push_back(
                static_cast<Byte>((descriptor_id >> shift) & 0xffu));
        }
    }
    return payload.AddBytes(Field::PcAlternatives, std::move(pcs)) &&
        payload.AddBytes(
            Field::HitTimeSampleDescriptorIds,
            std::move(sample_descriptor_ids));
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
    StaticConfigReader reader(bytes, {'I', 'L', 'C', '2'});
    std::uint32_t port = 0;
    std::uint32_t priority = 0;
    bool suspendable = false;
    bool borrowable = false;
    bool movie_exclusive = false;
    if (!reader.U32(port) || !reader.U32(priority) ||
        !reader.Bool(suspendable) ||
        !reader.Bool(borrowable) ||
        !reader.Bool(movie_exclusive) || !reader.done() ||
        port > std::numeric_limits<std::uint8_t>::max() ||
        priority >
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max()))
    {
        diagnostic = "ILC2 contains an invalid input lease policy";
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
            Field::MovieExclusive,
            movie_exclusive);
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
    WorksetEpoch current_epoch,
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
    const auto optional_binding =
        [&](std::size_t index, bool& present) {
            const ProgramValue* element = nullptr;
            if (index >= record->fields.size() ||
                !OptionalElement(
                    graph,
                    record->fields[index],
                    CanonicalRuntimeSchema::OptionalInputExecutionBinding,
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
                    CanonicalAction::InputApplyState,
                    receipt))
            {
                return false;
            }
            const auto lease = receipt.Unsigned(Field::Handle);
            const auto binding = receipt.Unsigned(Field::Binding);
            const auto publication = receipt.Unsigned(Field::Publication);
            const auto epoch = receipt.Unsigned(Field::ResultEpoch);
            const auto generation = receipt.Unsigned(Field::StateGeneration);
            const auto frame = receipt.Bytes(Field::ResultFrame);
            if (!lease || !binding || !publication || !epoch ||
                !generation || !frame)
                return false;
            return payload.AddUnsigned(Field::ParentHandle, *lease) &&
                payload.AddUnsigned(Field::Binding, *binding) &&
                payload.AddUnsigned(Field::Publication, *publication) &&
                payload.AddUnsigned(Field::ResultEpoch, *epoch) &&
                payload.AddUnsigned(Field::StateGeneration, *generation) &&
                payload.AddBytes(
                    Field::ResultFrame,
                    std::vector<Byte>(frame->begin(), frame->end()));
        };
    const auto optional_handle =
        [&](std::size_t index,
            CanonicalRuntimeSchema optional_schema,
            CanonicalAction producer,
            CanonicalActionPayloadField field,
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
            return !element || AddResourceHandle(
                *element,
                producer,
                payload,
                field);
        };
    const auto optional_u64 =
        [&](std::size_t index,
            CanonicalRuntimeSchema optional_schema,
            CanonicalActionPayloadField field,
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
            std::uint64_t value = 0;
            return ScalarValue(graph, element->id, value) &&
                payload.AddUnsigned(field, value);
        };

    switch (action)
    {
    case CanonicalAction::ExecutionContinueUntil:
    {
        bool binding = false;
        bool playback = false;
        bool expected_count = false;
        const auto* points = bytes(
            0,
            CanonicalRuntimeSchema::SemanticPointSet);
        const auto* config = bytes(
            4,
            CanonicalRuntimeSchema::
                ContinueUntilStaticConfig);
        if (record->fields.size() != 5 ||
            !points ||
            !DecodeStopGroupConfig(
                *points,
                payload,
                diagnostic) ||
            !optional_binding(1, binding) ||
            !optional_handle(
                2,
                CanonicalRuntimeSchema::
                    OptionalMoviePlaybackSession,
                CanonicalAction::MovieStartPlayback,
                Field::PlaybackHandle,
                playback) ||
            !optional_u64(
                3,
                CanonicalRuntimeSchema::
                    OptionalMovieInputCount,
                Field::ExpectedMovieInputCount,
                expected_count) ||
            !config ||
            !DecodeContinueConfig(
                *config,
                payload,
                diagnostic) ||
            (expected_count && !playback) ||
            (playback &&
             UnsignedOr(
                 payload,
                 Field::MovieEndedPolicy,
                 static_cast<std::uint64_t>(MovieEndedPolicy::Ignore)) !=
                 static_cast<std::uint64_t>(MovieEndedPolicy::Ignore)))
        {
            if (diagnostic.empty())
            {
                diagnostic = expected_count && !playback
                    ? "ContinueUntil expected input count requires an exact playback session"
                    : playback
                    ? "ContinueUntil playback ownership requires static movie policy Ignore"
                    : "ContinueUntilRequest is malformed";
            }
            return false;
        }
        (void)binding;
        return true;
    }
    case CanonicalAction::ExecutionStepFrames:
    {
        std::uint64_t count = 0;
        bool binding = false;
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::
                ExecutionAdvanceStaticConfig);
        if (record->fields.size() != 3 ||
            !u64(0, count) || count == 0 ||
            !payload.AddUnsigned(Field::Count, count) ||
            !optional_binding(1, binding) ||
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
        (void)binding;
        return true;
    }
    case CanonicalAction::ExecutionContinueUntilInputObserved:
    {
        const ProgramValue* binding = record->fields.size() == 3
            ? FindValue(graph, record->fields[0])
            : nullptr;
        CanonicalActionPayload receipt;
        std::uint64_t expected_count = 0;
        const auto* config = bytes(
            2,
            CanonicalRuntimeSchema::ExecutionAdvanceStaticConfig);
        if (!binding ||
            !DecodeReceiptPayload(
                *binding,
                CanonicalAction::InputBeginDelivery,
                receipt) ||
            !u64(1, expected_count) ||
            !config ||
            !DecodeAdvanceConfig(*config, payload, diagnostic))
        {
            if (diagnostic.empty())
                diagnostic =
                    "ContinueUntilInputObservedRequest is malformed";
            return false;
        }
        const auto id = receipt.Unsigned(Field::Binding);
        const auto lease = receipt.Unsigned(Field::Handle);
        const auto publication = receipt.Unsigned(Field::Publication);
        const auto epoch = receipt.Unsigned(Field::ResultEpoch);
        const auto generation = receipt.Unsigned(Field::StateGeneration);
        const auto frame = receipt.Bytes(Field::ResultFrame);
        return id && lease && publication && epoch && generation && frame &&
            payload.AddUnsigned(Field::Binding, *id) &&
            payload.AddUnsigned(Field::ParentHandle, *lease) &&
            payload.AddUnsigned(Field::Publication, *publication) &&
            payload.AddUnsigned(Field::ResultEpoch, *epoch) &&
            payload.AddUnsigned(Field::StateGeneration, *generation) &&
            payload.AddBytes(Field::ResultFrame,
                std::vector<Byte>(frame->begin(), frame->end())) &&
            payload.AddUnsigned(
                Field::ExpectedMovieInputCount, expected_count);
    }
    case CanonicalAction::ExecutionRequirePausedPc:
    {
        std::uint64_t expected_pc = 0;
        if (record->fields.size() != 1 ||
            !u64(0, expected_pc) || expected_pc == 0 ||
            expected_pc > std::numeric_limits<std::uint32_t>::max() ||
            !payload.AddUnsigned(Field::ExpectedPc, expected_pc))
        {
            diagnostic = "RequirePausedPcRequest has an invalid expected PC";
            return false;
        }
        return true;
    }
    case CanonicalAction::ExecutionObservePausedPc:
        if (!record->fields.empty())
        {
            diagnostic = "ObservePausedPcRequest must be empty";
            return false;
        }
        return true;
    case CanonicalAction::InputAcquireLease:
    {
        const auto* config = bytes(
            0,
            CanonicalRuntimeSchema::InputLeaseStaticConfig);
        return record->fields.size() == 1 && config &&
            DecodeLeaseConfig(*config, payload, diagnostic);
    }
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
    {
        const auto* input = bytes(
            1,
            CanonicalRuntimeSchema::InputFramePayload);
        return record->fields.size() == 2 &&
            handle(0, CanonicalAction::InputAcquireLease) &&
            input && payload.AddBytes(Field::InputFrame, *input);
    }
    case CanonicalAction::InputCompleteDelivery:
    {
        const ProgramValue* binding = record->fields.size() == 2
            ? FindValue(graph, record->fields[1])
            : nullptr;
        CanonicalActionPayload receipt;
        return record->fields.size() == 2 &&
            handle(0, CanonicalAction::InputAcquireLease) &&
            binding &&
            DecodeReceiptPayload(
                *binding,
                CanonicalAction::InputBeginDelivery,
                receipt) &&
            [&] {
                const auto id = receipt.Unsigned(Field::Binding);
                const auto lease = receipt.Unsigned(Field::Handle);
                const auto publication = receipt.Unsigned(Field::Publication);
                const auto epoch = receipt.Unsigned(Field::ResultEpoch);
                const auto generation = receipt.Unsigned(Field::StateGeneration);
                const auto frame = receipt.Bytes(Field::ResultFrame);
                return id && lease && publication && epoch && generation && frame &&
                    payload.AddUnsigned(Field::Binding, *id) &&
                    payload.AddUnsigned(Field::ParentHandle, *lease) &&
                    payload.AddUnsigned(Field::Publication, *publication) &&
                    payload.AddUnsigned(Field::ResultEpoch, *epoch) &&
                    payload.AddUnsigned(Field::StateGeneration, *generation) &&
                    payload.AddBytes(Field::ResultFrame,
                        std::vector<Byte>(frame->begin(), frame->end()));
            }();
    }
    case CanonicalAction::MovieAdoptRestoredReadOnlyPlayback:
        if (!record->fields.empty())
        {
            diagnostic =
                "Restored movie playback adoption request must have no fields";
            return false;
        }
        return true;
    case CanonicalAction::MovieObserveState:
        if (!record->fields.empty())
        {
            diagnostic = "Movie state observation request must have no fields";
            return false;
        }
        return true;
    case CanonicalAction::MovieStartRecording:
    {
        const auto* config = bytes(
            1,
            CanonicalRuntimeSchema::MovieRecordingStaticConfig);
        if (record->fields.size() != 2 ||
            !handle(0, CanonicalAction::MovieStartPlayback) ||
            !config)
        {
            diagnostic =
                "Movie recording requires its exact playback handle and MRC1 config";
            return false;
        }
        StaticConfigReader reader(*config, {'M','R','C','1'});
        std::string path;
        std::string label;
        if (!reader.String(path, 8192) ||
            !reader.String(label, 4096) ||
            !reader.done() ||
            !payload.AddUtf8(Field::Path, std::move(path)) ||
            !payload.AddUtf8(Field::Label, std::move(label)))
        {
            diagnostic = "Movie recording MRC1 config is malformed";
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
                 payload,
                 diagnostic)))
        {
            if (diagnostic.empty())
            {
                diagnostic =
                    "Observation request has a malformed stop receipt";
            }
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

bool DecodeSampleDescriptorIds(
    std::span<const Byte> bytes,
    std::vector<std::uint32_t>& ids)
{
    if (bytes.size() % 4 != 0 ||
        bytes.size() / 4 > kMaxRoutedHitSamples)
    {
        return false;
    }
    ids.clear();
    ids.reserve(bytes.size() / 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
    {
        const std::uint32_t id =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
        if (id == 0 || std::ranges::find(ids, id) != ids.end())
            return false;
        ids.push_back(id);
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
    std::string_view diagnostic_selector)
{
    std::vector<std::uint32_t> pcs;
    std::vector<std::uint32_t> sample_descriptor_ids;
    if (const auto encoded = payload.Bytes(Field::PcAlternatives))
        (void)DecodePcAlternatives(*encoded, pcs);
    else if (const auto address = payload.Unsigned(Field::Address);
             address && *address <=
                 std::numeric_limits<std::uint32_t>::max())
        pcs.push_back(static_cast<std::uint32_t>(*address));
    if (const auto encoded = payload.Bytes(
            Field::HitTimeSampleDescriptorIds))
    {
        (void)DecodeSampleDescriptorIds(
            *encoded,
            sample_descriptor_ids);
    }

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
        diagnostic_selector.empty()
            ? "canonical program action"
            : std::string(diagnostic_selector)};
    const std::uint64_t subscription_seed =
        UnsignedOr(payload, Field::SubscriptionId, seed | 0x80u);
    for (std::size_t index = 0; index < pcs.size(); ++index)
    {
        definition.subscriptions.push_back({
            .id = StopSubscriptionId(
                subscription_seed + index),
            .point = PcStopPointSpec{pcs[index]},
            .route = ForegroundStopWait{
                .suppress_immediate_reentry = true,
            },
            .lifetime = StopSubscriptionLifetime::Scoped,
            .sample_descriptor_ids = sample_descriptor_ids,
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
        terminal.status == ExecutionTerminalStatus::InputObserved ||
        terminal.status == ExecutionTerminalStatus::Paused ||
        terminal.status == ExecutionTerminalStatus::CursorOverrun ||
        (terminal.status == ExecutionTerminalStatus::MovieEnded &&
         !terminal.error);
}

ProgramActionResolutionStatus ExecutionCompletionStatus(
    const ExecutionTerminalResult& terminal) noexcept
{
    switch (terminal.status)
    {
    case ExecutionTerminalStatus::Cancelled:
        return ProgramActionResolutionStatus::Cancelled;
    case ExecutionTerminalStatus::TimedOut:
        return ProgramActionResolutionStatus::TimedOut;
    case ExecutionTerminalStatus::WorksetEpochMismatch:
        return ProgramActionResolutionStatus::StaleEpoch;
    case ExecutionTerminalStatus::Unsupported:
        return ProgramActionResolutionStatus::Unsupported;
    default:
        return ExecutionSucceeded(terminal)
            ? ProgramActionResolutionStatus::Completed
            : ProgramActionResolutionStatus::Failed;
    }
}

ProgramValueGraph ResourceHandleGraph(
    CanonicalAction action,
    ProgramResourceHandleId handle,
    WorksetEpoch epoch)
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
    ContinueUntilCompletionReasonV1 reason;
    switch (terminal.status)
    {
    case ExecutionTerminalStatus::RequestedCompletion:
        reason = ContinueUntilCompletionReasonV1::Breakpoint;
        break;
    case ExecutionTerminalStatus::CursorOverrun:
        reason = ContinueUntilCompletionReasonV1::CursorOverrun;
        break;
    case ExecutionTerminalStatus::MovieEnded:
        reason = ContinueUntilCompletionReasonV1::MovieEnded;
        break;
    default:
        return {};
    }

    std::vector<ProgramValue> values;
    std::optional<ProgramValueId> routed_stop;
    std::uint64_t next_id = 1;
    const auto add = [&](TypeRef type, ProgramValuePayload payload) {
        ProgramValue value{
            ProgramValueId(next_id++),
            std::move(type),
            std::move(payload)};
        const ProgramValueId id = value.id;
        values.push_back(std::move(value));
        return id;
    };

    if (reason == ContinueUntilCompletionReasonV1::Breakpoint)
    {
        if (!terminal.stop || !terminal.stop->event)
            return {};
        const StopRouteReceipt& stop = *terminal.stop;
        const RoutedStopEvent& event = *stop.event;
        if (!stop.identity.sequence ||
            !stop.identity.sample_snapshot ||
            !stop.identity.workset_epoch)
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
        if (const auto* pc = std::get_if<PcStopPointSpec>(
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

        const ProgramValueId sequence = add(
            TypeRef::Builtin(BuiltinType::U64),
            stop.identity.sequence.value());
        const ProgramValueId epoch = add(
            TypeRef::Builtin(BuiltinType::U64),
            stop.identity.workset_epoch.value());
        const ProgramValueId pc = add(
            TypeRef::Builtin(BuiltinType::U32),
            event.evidence.hit_pc);
        const ProgramValueId sample = add(
            TypeRef::Builtin(BuiltinType::U64),
            stop.identity.sample_snapshot.value());
        const ProgramValueId evidence_id = add(
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::StopEvidencePayload),
            std::move(evidence));
        routed_stop = add(
            CanonicalRuntimeType(
                CanonicalRuntimeSchema::RoutedStopReceipt),
            RecordValue{{sequence, epoch, pc, sample, evidence_id}});
    }

    const ProgramValueId optional_stop = add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt),
        OptionalValue{routed_stop});
    const ProgramValueId reason_id = add(
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason),
        EnumValue{
            CanonicalRuntimeSchemaIdentity(
                CanonicalRuntimeSchema::ContinueUntilCompletionReason),
            static_cast<std::int64_t>(reason)});
    const ProgramValueId pc = add(
        TypeRef::Builtin(BuiltinType::U32),
        terminal.evidence.pc);
    const ProgramValueId input_count = add(
        TypeRef::Builtin(BuiltinType::U64),
        terminal.evidence.movie_input_count);
    const ProgramValueId vi_count = add(
        TypeRef::Builtin(BuiltinType::U64),
        terminal.evidence.vi_count);
    const ProgramValueId epoch = add(
        TypeRef::Builtin(BuiltinType::U64),
        terminal.workset_epoch.value());
    const ProgramValueId root = add(
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        RecordValue{{reason_id, optional_stop, pc, input_count, vi_count, epoch}});
    return {root, std::move(values)};
}

ProgramValueGraph PausedPcReceiptGraph(
    std::uint32_t pc,
    std::uint64_t vi_count,
    WorksetEpoch epoch)
{
    std::vector<ProgramValue> values;
    values.push_back({
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U32),
        pc});
    values.push_back({
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U64),
        vi_count});
    values.push_back({
        ProgramValueId(3),
        TypeRef::Builtin(BuiltinType::U64),
        epoch.value()});
    values.push_back({
        ProgramValueId(4),
        CanonicalActionOutputType(
            CanonicalAction::ExecutionRequirePausedPc),
        RecordValue{{
            ProgramValueId(1),
            ProgramValueId(2),
            ProgramValueId(3)}}});
    return {ProgramValueId(4), std::move(values)};
}

ProgramValueGraph InputObservedExecutionResultGraph(
    const ExecutionTerminalResult& terminal)
{
    if (terminal.status != ExecutionTerminalStatus::InputObserved)
        return {};
    std::vector<ProgramValue> values;
    values.push_back({
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U32),
        terminal.evidence.pc});
    values.push_back({
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U64),
        terminal.evidence.movie_input_count});
    values.push_back({
        ProgramValueId(3),
        TypeRef::Builtin(BuiltinType::U64),
        terminal.evidence.vi_count});
    values.push_back({
        ProgramValueId(4),
        TypeRef::Builtin(BuiltinType::U64),
        terminal.workset_epoch.value()});
    values.push_back({
        ProgramValueId(5),
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntilInputObserved),
        RecordValue{{
            ProgramValueId(1),
            ProgramValueId(2),
            ProgramValueId(3),
            ProgramValueId(4)}}});
    return {ProgramValueId(5), std::move(values)};
}

ProgramValueGraph MovieStateObservationGraph(
    const MovieStateSnapshot& snapshot)
{
    std::vector<ProgramValue> values;
    values.push_back({
        ProgramValueId(1),
        CanonicalRuntimeType(CanonicalRuntimeSchema::MovieState),
        EnumValue{
            CanonicalRuntimeSchemaIdentity(CanonicalRuntimeSchema::MovieState),
            static_cast<std::int64_t>(snapshot.state)}});
    values.push_back({
        ProgramValueId(2), TypeRef::Builtin(BuiltinType::U64),
        snapshot.workset_epoch.value()});
    values.push_back({
        ProgramValueId(3), TypeRef::Builtin(BuiltinType::Bool),
        snapshot.read_only});
    values.push_back({
        ProgramValueId(4), TypeRef::Builtin(BuiltinType::U64),
        snapshot.current_frame});
    values.push_back({
        ProgramValueId(5), TypeRef::Builtin(BuiltinType::U64),
        snapshot.current_input_count});
    values.push_back({
        ProgramValueId(6),
        CanonicalActionOutputType(CanonicalAction::MovieObserveState),
        RecordValue{{ProgramValueId(1), ProgramValueId(2), ProgramValueId(3),
                     ProgramValueId(4), ProgramValueId(5)}}});
    return {ProgramValueId(6), std::move(values)};
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

} // namespace

struct SessionProgramActionHost::Impl
{
    enum class BaselineStage : std::uint8_t
    {
        Established,
        AwaitingMoviePreparation,
        MoviePrepared,
    };

    struct ActiveInvocation
    {
        InvocationId invocation;
        AttemptId attempt;
        ResourceOwnerId owner;
        ResourceScopeId root_scope;
        CancellationSource cancellation;
        BaselineStage baseline_stage = BaselineStage::Established;

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
        WorksetEpoch epoch;
        std::uint64_t concrete_id = 0;
        std::shared_ptr<StopSubscriptionGroupHandle> stop_group;
        std::optional<StopSubscriptionGroupDefinition>
            stop_group_definition;
        std::filesystem::path artifact_path;
        std::shared_ptr<bool> finalized;
    };

    struct SavedArtifact
    {
        SavestateArtifactId artifact;
        std::filesystem::path path;
        std::string sha256;
    };

    enum class PendingKind : std::uint8_t
    {
        Action,
        Cleanup,
    };

    struct PendingExecution
    {
        PendingKind kind = PendingKind::Action;
        ProgramActionRequest request;
        ExecutionOperationId operation;
        std::optional<InputExecutionRelationshipId> input_binding;
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

    [[nodiscard]] ProgramActionResolution Completion(
        const ProgramActionRequest& request,
        ProgramActionResolutionStatus status,
        std::string code = {},
        std::string message = {}) const
    {
        const SessionSnapshot current = session.snapshot();
        ProgramActionResolution completion;
        completion.request_id = request.request_id;
        completion.invocation_id = request.invocation_id;
        completion.attempt_id = request.attempt_id;
        completion.operation = request.operation;
        completion.status = status;
        completion.workset_epoch = current.workset_epoch;
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
        ProgramActionResolution resolution)
    {
        return {
            true,
            ActorActionResult{std::move(resolution), {}},
            {}};
    }

    [[nodiscard]] ProgramActionDispatchResult Reject(
        const ProgramActionRequest& request,
        ProgramActionResolutionStatus status,
        std::string code,
        std::string message)
    {
        ProgramActionResolution completion = Completion(
            request,
            status,
            std::move(code),
            std::move(message));
        const std::string diagnostic = completion.message;
        return {
            false,
            ActorActionResult{std::move(completion), {}},
            diagnostic};
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
        WorksetEpoch epoch,
        SessionResourceReleaseCallback release,
        std::uint64_t concrete_id,
        std::shared_ptr<StopSubscriptionGroupHandle> stop_group,
        std::string label,
        std::string& diagnostic)
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
                    session.snapshot().workset_epoch,
                    true});
            if (compensated.status !=
                    ResourceReleaseStatus::Released)
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
        mapping.concrete_id = concrete_id;
        mapping.stop_group = std::move(stop_group);
        const ProgramActionResource resource{
            mapping.handle,
            mapping.receipt,
            mapping.kind,
            mapping.epoch};
        resources.emplace(mapping.handle.value(), std::move(mapping));
        return resource;
    }

    [[nodiscard]] bool HasCompletionCapacity() const noexcept
    {
        return completions.size() <
            config.maximum_retained_completions;
    }

    [[nodiscard]] bool Queue(ProgramActionResolution resolution)
    {
        return Queue(ActorActionResult{
            std::move(resolution), {}});
    }

    [[nodiscard]] bool Queue(ActorActionResult result)
    {
        if (!HasCompletionCapacity())
        {
            session.MarkTainted(
                "Program action completion queue exceeded its bound");
            // This is an invariant failure: accepted asynchronous work is
            // admitted only while one completion slot is available. Surface
            // the terminal anyway so its waiter cannot hang.
            result.resolution.status =
                ProgramActionResolutionStatus::CleanupFailed;
            result.resolution.cleanup = ProgramCleanupStatus::Tainted;
            result.resolution.session_disposition =
                SessionDisposition::Tainted;
            result.resolution.code = "completion_capacity_invariant";
            result.resolution.message =
                "Accepted program action completed without its reserved completion slot";
            completions.push_back(std::move(result));
            return false;
        }
        completions.push_back(std::move(result));
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
                    ExecutionThrottlePolicy::RequireDisabled)));
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
        policy.diagnostic_invocation = request.invocation_id.value();
        policy.diagnostic_attempt = request.attempt_id.value();
        policy.diagnostic_request = request.request_id.value();
        policy.diagnostic_selector = request.diagnostic_selector;
        return policy;
    }

    [[nodiscard]] ProgramActionDispatchResult SubmitExecutionAction(
        ProgramActionRequest request,
        ExecutionRequest execution,
        PendingKind kind = PendingKind::Action,
        std::optional<InputExecutionRelationshipId> binding = {})
    {
        if (!HasCompletionCapacity())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "completion_capacity_exhausted",
                "Program action completion capacity must be drained before accepting asynchronous work");
        }
        if (pending)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
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
                    ? input->RemoveExecutionRelationship(*binding)
                    : InputArbiterOperationReceipt{
                          false,
                          InputArbiterErrorCode::Stopped,
                          "InputArbiter is unavailable"};
                if (!removed.ok)
                {
                    session.MarkTainted(
                        "Rejected program execution left its input execution relationship live");
                    return Reject(
                        request,
                        ProgramActionResolutionStatus::CleanupFailed,
                        "input_binding_compensation_failed",
                        std::string(removed.message));
                }
            }
            const auto status =
                submitted.error.code ==
                        ExecutionErrorCode::WorksetEpochMismatch
                ? ProgramActionResolutionStatus::StaleEpoch
                : submitted.error.code ==
                            ExecutionErrorCode::Unsupported
                ? ProgramActionResolutionStatus::Unsupported
                : ProgramActionResolutionStatus::Failed;
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
                ProgramActionResolutionStatus::Rejected,
                "invalid_state_request",
                "Invocation state preparation is malformed or overlaps another invocation");
        }
        const SessionSnapshot current = session.snapshot();
        if (!current.open ||
            current.workset_epoch != request.expected_epoch)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::StaleEpoch,
                "stale_epoch",
                "Invocation state preparation expected another session epoch");
        }
        const InvocationStateRequest& state = *request.state_request;
        if ((state.expected_session &&
             state.expected_session != current.session_id) ||
            (state.expected_epoch &&
             state.expected_epoch != current.workset_epoch))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "state_identity_mismatch",
                "Invocation state policy identifies another session or epoch");
        }
        if (state.session_lineage.empty())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "lineage_required",
                "Invocation state policy requires an explicit session lineage");
        }
        if (request.state_already_prepared &&
            (!CompleteSha256(request.prepared_baseline_sha256) ||
             !state.expected_session || !state.expected_epoch ||
             state.expected_session != current.session_id ||
             state.expected_epoch != current.workset_epoch))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "prepared_baseline_mismatch",
                "Prepared baseline must identify the exact current session and epoch");
        }

        switch (state.policy)
        {
        case InvocationStatePolicy::RestoreBaseline:
            if (!request.state_already_prepared)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "prepared_baseline_required",
                    "RestoreBaseline requires a workset-prepared artifact state");
            }
            break;
        case InvocationStatePolicy::EstablishBaseline:
            if (request.state_already_prepared)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "unestablished_baseline_required",
                    "EstablishBaseline must begin from a staged but unestablished movie artifact");
            }
            break;
        default:
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "invalid_state_policy",
                "Invocation state preparation contains an unknown state policy");
        }

        SessionResourceLedger* ledger = session.resources();
        if (!ledger)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
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
                ProgramActionResolutionStatus::Failed,
                "scope_open_failed",
                invocation_scope.error.message);
        }
        scopes.emplace(
            request.scope.value(),
            invocation_scope.scope.id);
        active->root_scope = invocation_scope.scope.id;

        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
        if (request.state_already_prepared)
        {
            return Immediate(std::move(completion));
        }
        // EstablishBaseline deliberately opens only the invocation scope.
        // Preparation stops the guest core; only successful consumption by
        // MovieStartPlayback establishes guest state. WorksetEpoch is stable.
        active->baseline_stage = BaselineStage::AwaitingMoviePreparation;
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
            ProgramActionResolutionStatus::Failed,
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
    [[nodiscard]] ProgramActionDispatchResult InvokeDerivedStateQuery(
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
    [[nodiscard]] ProgramActionResolution ExecutionCompletion(
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
    void AbandonQueuedStagedOutputs() noexcept;
    void ReleaseSavedArtifactRecords() noexcept;

    EmulationSession& session;
    SessionProgramActionHostConfig config;
    ProgramStopConsumer stop_consumer;
    std::thread::id owner_thread;
    std::optional<ActiveInvocation> active;
    std::unordered_map<std::uint64_t, ResourceScopeId> scopes;
    std::unordered_map<std::uint64_t, ResourceMapping> resources;
    std::unordered_map<std::string, SavedArtifact> saved_artifacts;
    std::optional<PendingExecution> pending;
    std::vector<ActorActionResult> completions;
    std::vector<ForegroundSemanticStopObservationV1>
        foreground_semantic_stops;
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
            ProgramActionResolutionStatus::Failed,
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
            ProgramActionResolutionStatus::Failed,
            "result_encoding_failed",
            "Canonical action output is not an SAP1 receipt");
    }
    CanonicalActionPayloadResult encoded =
        EncodeCanonicalActionPayload(payload, *schema);
    if (!encoded.ok)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Failed,
            "result_encoding_failed",
            encoded.diagnostic);
    }
    ProgramActionResolution completion = Completion(
        request,
        ProgramActionResolutionStatus::Completed);
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
        ProgramActionResolutionStatus::Failed,
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
            ProgramActionResolutionStatus::Rejected,
            "wrong_thread",
            "Program action host dispatch ran outside its actor thread");
    }
    if (stopped)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "runtime_stopping",
            "Program action host is shut down");
    }
    if (!request.request_id || !request.invocation_id ||
        !request.attempt_id || !request.expected_epoch)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
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
            ProgramActionResolutionStatus::Rejected,
            "invocation_mismatch",
            "Program action does not identify the active invocation");
    }
    if (request.operation !=
            ProgramHostOperation::PrepareInvocationState &&
        request.expected_epoch != session.snapshot().workset_epoch)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::StaleEpoch,
            "stale_epoch",
            "Program action expected a stale WorksetEpoch");
    }
    if (pending)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "action_pending",
            "A program action is already awaiting service completion");
    }
    if (!HasCompletionCapacity())
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "completion_capacity_exhausted",
            "Program action completions must be drained before accepting more work");
    }
    if (const auto deadline = Deadline(request);
        deadline && *deadline <= std::chrono::steady_clock::now())
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::TimedOut,
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
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
                effects
                    ? "action_effect_not_authorized"
                    : "unsupported_action",
                effects
                    ? "Invocation policy does not authorize every declared action effect"
                    : "The exact action identity has no registered effect contract");
        }
    }
    if (request.operation == ProgramHostOperation::FinishInvocation &&
        active && active->baseline_stage != BaselineStage::Established)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "baseline_not_established",
            "An EstablishBaseline invocation cannot return successfully before movie playback establishes guest state");
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
        ProgramActionResolutionStatus::Rejected,
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
            ProgramActionResolutionStatus::Rejected,
            "invalid_scope",
            "Program lexical scope mapping is malformed");
    }
    SessionResourceLedger* ledger = session.resources();
    if (!ledger || !active)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Failed,
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
            ProgramActionResolutionStatus::Failed,
            "scope_open_failed",
            opened.error.message);
    }
    scopes.emplace(request.scope.value(), opened.scope.id);
    return Immediate(Completion(
        request,
        ProgramActionResolutionStatus::Completed));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::BeginUnwind(
    ProgramActionRequest request,
    ResourceUnwindResult unwind,
    bool finish)
{
    if (unwind.completed())
    {
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
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
            ProgramActionResolutionStatus::CleanupFailed,
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
            ? ProgramActionResolutionStatus::CleanupFailed
            : ProgramActionResolutionStatus::Failed,
        taint ? "cleanup_failed" : "scope_close_failed",
        unwind.error.message.empty()
            ? "Program resource unwind failed"
            : unwind.error.message);
    if (failed.immediate_result)
    {
        failed.immediate_result->resolution.cleanup_receipts =
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
            ProgramActionResolutionStatus::Rejected,
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
            ProgramActionResolutionStatus::Rejected,
            "resource_unavailable",
            "Program resource or promotion scope is unavailable");
    }
    const ResourcePromotionResult promoted =
        ledger->Promote(mapping->receipt, destination);
    if (!promoted.success)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "promotion_rejected",
            promoted.error.message);
    }
    return Immediate(Completion(
        request,
        ProgramActionResolutionStatus::Completed));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::Invoke(
    ProgramActionRequest request)
{
    if (!request.action)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "missing_action",
            "Program action request has no exact action identity");
    }
    if (*request.action ==
            capabilities::BattleCaptureContextActionIdentity() ||
        *request.action ==
            capabilities::BattleCompletionCaptureSnapshotActionIdentity() ||
        *request.action ==
            capabilities::NavigationCaptureContextActionIdentity())
    {
        if (active && active->baseline_stage != BaselineStage::Established)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "baseline_not_established",
                "Guest context reads are unavailable before baseline establishment");
        }
        return InvokeSourceQuery(std::move(request));
    }
    if (*request.action ==
            capabilities::BattleDerivedTurnEntryActionIdentity() ||
        *request.action ==
            capabilities::BattleDerivedTurnOrderActionIdentity() ||
        *request.action ==
            capabilities::BattleDerivedRewardsActionIdentity())
    {
        if (active && active->baseline_stage != BaselineStage::Established)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "baseline_not_established",
                "Derived state is unavailable before baseline establishment");
        }
        return InvokeDerivedStateQuery(std::move(request));
    }
    const auto canonical = ResolveCanonicalAction(*request.action);
    if (!canonical)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Unsupported,
            "unsupported_action",
            "The exact action identity has no session host implementation");
    }
    if (*canonical == CanonicalAction::MoviePrepareReadOnlyPlayback &&
        (!active || active->baseline_stage !=
            BaselineStage::AwaitingMoviePreparation))
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "invalid_baseline_stage",
            "Movie preparation is available only as the first EstablishBaseline lifecycle action");
    }
    if (*canonical == CanonicalAction::MovieStartPlayback &&
        (!active || active->baseline_stage != BaselineStage::MoviePrepared))
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "invalid_baseline_stage",
            "MovieStartPlayback requires the current invocation's prepared movie boundary");
    }
    CanonicalActionPayload payload;
    const TypeRef input_type =
        CanonicalActionInputType(*canonical);
    if (input_type.named &&
        input_type.named->canonical_id.ends_with(".Result"))
    {
        if (active &&
            active->baseline_stage != BaselineStage::Established &&
            !(*canonical == CanonicalAction::MovieStartPlayback &&
              active->baseline_stage == BaselineStage::MoviePrepared))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "baseline_not_established",
                "Guest-dependent resource actions are unavailable before baseline establishment");
        }
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
                ProgramActionResolutionStatus::Rejected,
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
            ProgramActionResolutionStatus::Rejected,
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
            ProgramActionResolutionStatus::Rejected,
            "invalid_action_payload",
            diagnostic.empty()
                ? "Canonical action payload is malformed"
                : std::move(diagnostic));
    }
    if (active && active->baseline_stage != BaselineStage::Established)
    {
        const bool prepare =
            *canonical == CanonicalAction::MoviePrepareReadOnlyPlayback &&
            active->baseline_stage ==
                BaselineStage::AwaitingMoviePreparation;
        if (!prepare)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "baseline_not_established",
                "Movie preparation must precede MovieStartPlayback");
        }
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
    const bool completion_snapshot = request.action ==
        capabilities::BattleCompletionCaptureSnapshotActionIdentity();
    const std::string_view input_schema = (battle || completion_snapshot)
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
            ProgramActionResolutionStatus::Rejected,
            "invalid_context_request",
            "Coherent context request has malformed or stale paused-state evidence");
    }
    const std::optional<ExecutionSnapshot> execution =
        session.execution_snapshot();
    if (!execution ||
        execution->activity != ExecutionActivity::IdlePaused ||
        !execution->evidence.pause_confirmed ||
        execution->evidence.pc != evidence.expected_pc)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Failed,
            "observation_point_unavailable",
            "Coherent context acquisition is not paused at its bound receipt PC");
    }
    GuestMemory* memory = session.guest_memory();
    if (!memory)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Unsupported,
            "guest_memory_unavailable",
            "GuestMemory is unavailable");
    }

    ProgramActionResolution completion = Completion(
        request,
        ProgramActionResolutionStatus::Completed);
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
                ProgramActionResolutionStatus::Failed,
                "battle_context_unavailable",
                std::move(diagnostic));
        }
        completion.output =
            capabilities::EncodeBattleContextValue(context);
    }
    else if (completion_snapshot)
    {
        battlecompletion::BattleCompletionSnapshotV1 snapshot{};
        if (!ReadBattleCompletionSnapshot(
                *memory,
                request.expected_epoch,
                snapshot,
                diagnostic))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "battle_completion_snapshot_unavailable",
                std::move(diagnostic));
        }
        completion.output =
            capabilities::EncodeBattleCompletionSnapshotValue(snapshot);
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
                ProgramActionResolutionStatus::Failed,
                "navigation_context_unavailable",
                std::move(diagnostic));
        }
        completion.output =
            capabilities::EncodeNavigationContextValue(context);
    }
    return Immediate(std::move(completion));
}

ProgramActionDispatchResult
SessionProgramActionHost::Impl::InvokeDerivedStateQuery(
    ProgramActionRequest request)
{
    derived::DerivedStateQueryV1 query;
    if (!capabilities::DecodeBattleDerivedQueryValue(
            request.input, query) ||
        (query.freshness == derived::DerivedStateFreshness::SameRoutedEvent &&
         query.workset_epoch != request.expected_epoch))
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Rejected,
            "invalid_derived_state_request",
            "Battle derived-state request is malformed or stale");
    }
    auto* service = session.derived_state();
    if (!service)
    {
        return Reject(
            request,
            ProgramActionResolutionStatus::Unsupported,
            "derived_state_unavailable",
            "DerivedStateService is unavailable");
    }
    query.workset_epoch = request.expected_epoch;
    query.item_id = service->item_id();

    ProgramActionResolution completion = Completion(
        request,
        ProgramActionResolutionStatus::Completed);
    if (request.action ==
        capabilities::BattleDerivedTurnEntryActionIdentity())
    {
        const auto result = service->QueryBattleTurnEntry(query);
        if (!result.receipt.ok || !result.snapshot)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "derived_state_evidence_unavailable",
                result.receipt.message);
        }
        completion.output = capabilities::EncodeBattleDerivedSnapshotValue(
            *result.snapshot);
    }
    else if (request.action ==
             capabilities::BattleDerivedTurnOrderActionIdentity())
    {
        const auto result = service->QueryBattleTurnOrder(query);
        if (!result.receipt.ok || !result.snapshot)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "derived_state_evidence_unavailable",
                result.receipt.message);
        }
        completion.output = capabilities::EncodeBattleDerivedSnapshotValue(
            *result.snapshot);
    }
    else
    {
        const auto result = service->QueryBattleRewards(query);
        if (!result.receipt.ok || !result.snapshot)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "derived_state_evidence_unavailable",
                result.receipt.message);
        }
        completion.output = capabilities::EncodeBattleDerivedSnapshotValue(
            *result.snapshot);
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
            ProgramActionResolutionStatus::Failed,
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
                if (completed.immediate_result)
                {
                    completed.immediate_result->resolution.
                        cleanup_receipts =
                            std::move(cleanup_receipts);
                }
                return completed;
            }
        }
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
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
            ProgramActionResolutionStatus::CleanupFailed,
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
            ? ProgramActionResolutionStatus::CleanupFailed
            : ProgramActionResolutionStatus::Failed,
        "resource_release_failed",
        unwind.error.message.empty()
            ? "Program resource release failed"
            : unwind.error.message);
    if (failed.immediate_result)
    {
        failed.immediate_result->resolution.cleanup_receipts =
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
            ProgramActionResolutionStatus::Rejected,
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
        ProgramActionResolution completion = Completion(
            completed_request,
            ProgramActionResolutionStatus::Completed);
        completion.output = ResourceHandleGraph(
            action,
            resource.handle,
            resource.acquisition_epoch);
        if (completion.output.values.empty())
        {
            return Reject(
                completed_request,
                ProgramActionResolutionStatus::Failed,
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
            ProgramActionResolutionStatus::Failed,
            std::move(code),
            std::move(message));
    };

    switch (action)
    {
    case CanonicalAction::SavestateSaveImmutableArtifact:
    {
        const bool has_active_workset =
            static_cast<bool>(session.snapshot().workset_epoch);
        const auto path = payload.Utf8(Field::Path);
        if (!has_active_workset || !path || path->empty())
        {
            return Reject(
                request,
                has_active_workset
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
                "invalid_artifact_request",
                "State artifact capture requires SavestateService and a caller-declared path");
        }
        SavestateCaptureRequest capture;
        capture.path = std::filesystem::path(*path);
        const std::uint64_t movie_artifact_mode = UnsignedOr(
            payload,
            Field::MovieArtifactMode,
            static_cast<std::uint64_t>(
                SavestateMovieArtifactMode::ExactCheckpointSidecar));
        if (movie_artifact_mode > static_cast<std::uint64_t>(
                SavestateMovieArtifactMode::DeferredFinalRecordingPair))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "invalid_artifact_request",
                "State artifact capture has an unknown movie pairing mode");
        }
        capture.movie_artifact_mode =
            static_cast<SavestateMovieArtifactMode>(movie_artifact_mode);
        capture.lineage.edge = std::string(
            payload.Utf8(Field::Label).value_or(
                "program state artifact"));
        capture.lineage.producer = "ProgramRuntime";
        ImmutableSavestateArtifactCaptureReceipt artifact =
            session.CaptureImmutableSavestateArtifact(capture);
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
            (void)session.AbandonImmutableSavestateArtifact(
                artifact.artifact);
            return service_failure(
                "result_encoding_failed",
                "Pending state-artifact receipt could not be encoded");
        }
        ProgramActionDispatchResult completed =
            CompleteWithPayload(
                std::move(request),
                std::move(result));
        if (!completed.immediate_result)
        {
            (void)session.AbandonImmutableSavestateArtifact(
                artifact.artifact);
            return service_failure(
                "result_encoding_failed",
                "Pending state-artifact receipt was not completed");
        }
        completed.immediate_result->staged_outputs.emplace_back(
            StagedSavestateOutput{
                std::move(artifact_id),
                std::move(artifact)});
        return completed;
    }
    case CanonicalAction::ExecutionContinueUntil:
    {
        StopSubscriptionGroupDefinition wake = BuildPcGroup(
            payload,
            request.invocation_id,
            request.request_id,
            request.diagnostic_selector);
        const std::uint64_t seed =
            ProgramStopIdentitySeed(
                request.invocation_id,
                request.request_id);
        const bool suppress =
            UnsignedOr(payload, Field::Flags, 0) != 0;
        for (std::size_t index = 0;
             index < wake.subscriptions.size();
             ++index)
        {
            StopSubscriptionDefinition& subscription =
                wake.subscriptions[index];
            subscription.id =
                StopSubscriptionId(
                    (seed | 0x80u) + index);
            auto* foreground =
                std::get_if<ForegroundStopWait>(
                    &subscription.route);
            if (foreground == nullptr)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "invalid_stop_route",
                    "ContinueUntil requires foreground-wait alternatives");
            }
            foreground->suppress_immediate_reentry = suppress;
            subscription.consumer = &stop_consumer;
        }
        if (wake.subscriptions.empty())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "invalid_stop_alternatives",
                "ContinueUntil semantic point set has no bounded alternatives");
        }
        std::optional<InputExecutionRelationshipId>
            input_relationship;
        if (payload.Contains(Field::Binding))
        {
            InputArbiter* input = session.input_arbiter();
            const auto binding =
                InputExecutionBindingEvidenceFromPayload(payload);
            if (!input || !binding)
            {
                return Reject(
                    request,
                    input
                        ? ProgramActionResolutionStatus::Rejected
                        : ProgramActionResolutionStatus::Unsupported,
                    "input_relationship_invalid",
                    "ContinueUntil requires a complete typed input execution binding");
            }
            const InputExecutionRelationshipReceipt relationship =
                input->CreateExecutionRelationship(*binding);
            if (!relationship.ok)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "input_binding_invalid",
                    relationship.message);
            }
            input_relationship = relationship.relationship;
        }
        std::optional<std::uint64_t> expected_movie_input_count;
        if (const auto expected = payload.Unsigned(
                Field::ExpectedMovieInputCount))
        {
            expected_movie_input_count = *expected;
        }
        if (const auto playback_handle = payload.Unsigned(
                Field::PlaybackHandle))
        {
            PruneReleasedResources();
            ResourceMapping* playback = Resource(
                ProgramResourceHandleId(*playback_handle));
            MovieService* movies = session.movie_service();
            if (!playback ||
                playback->kind != ResourceKind::MovieSession ||
                playback->concrete_id != static_cast<std::uint64_t>(
                    MovieState::ReadOnlyPlayback) ||
                !movies ||
                movies->state() != MovieState::ReadOnlyPlayback)
            {
                return Reject(
                    request,
                    movies
                        ? ProgramActionResolutionStatus::Rejected
                        : ProgramActionResolutionStatus::Unsupported,
                    "movie_session_unavailable",
                    "ContinueUntil requires its exact live read-only playback handle");
            }
        }
        else if (expected_movie_input_count)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "movie_session_required",
                "ContinueUntil input-count observation requires playback ownership");
        }
        ExecutionRequestPolicy policy =
            ExecutionPolicy(request, payload);
        if (payload.Contains(Field::PlaybackHandle))
            policy.movie_ended = MovieEndedPolicy::Complete;
        policy.input_relationship = input_relationship;
        ContinueUntilRequest execution{
            std::move(policy),
            std::move(wake),
            expected_movie_input_count};
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
                ProgramActionResolutionStatus::Rejected,
                "invalid_step_count",
                "Frame step count is outside its bounded range");
        }
        std::optional<InputExecutionRelationshipId> input_relationship;
        if (payload.Contains(Field::Binding))
        {
            InputArbiter* input = session.input_arbiter();
            const auto binding =
                InputExecutionBindingEvidenceFromPayload(payload);
            if (!input || !binding)
            {
                return Reject(
                    request,
                    input ? ProgramActionResolutionStatus::Rejected
                          : ProgramActionResolutionStatus::Unsupported,
                    "input_binding_invalid",
                    "StepFrames requires a complete typed input execution binding");
            }
            const InputExecutionRelationshipReceipt relationship =
                input->CreateExecutionRelationship(*binding);
            if (!relationship.ok)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "input_binding_invalid",
                    relationship.message);
            }
            input_relationship = relationship.relationship;
        }
        ExecutionRequestPolicy policy = ExecutionPolicy(request, payload);
        policy.input_relationship = input_relationship;
        return SubmitExecutionAction(
            std::move(request),
            StepFramesRequest{
                std::move(policy),
                static_cast<std::uint32_t>(count)},
            PendingKind::Action,
            input_relationship);
    }
    case CanonicalAction::ExecutionContinueUntilInputObserved:
    {
        InputArbiter* input = session.input_arbiter();
        const auto binding =
            InputExecutionBindingEvidenceFromPayload(payload);
        const auto expected_count =
            payload.Unsigned(Field::ExpectedMovieInputCount);
        if (!input || !binding || !expected_count)
        {
            return Reject(
                request,
                input ? ProgramActionResolutionStatus::Rejected
                      : ProgramActionResolutionStatus::Unsupported,
                "input_binding_invalid",
                "Input-observation execution requires an exact input binding and recording cursor");
        }
        const InputExecutionRelationshipReceipt relationship =
            input->CreateExecutionRelationship(*binding);
        if (!relationship.ok)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "input_binding_invalid",
                relationship.message);
        }
        ExecutionRequestPolicy policy =
            ExecutionPolicy(request, payload);
        policy.input_relationship = relationship.relationship;
        return SubmitExecutionAction(
            std::move(request),
            ContinueUntilInputObservedRequest{
                std::move(policy),
                *expected_count},
            PendingKind::Action,
            relationship.relationship);
    }
    case CanonicalAction::ExecutionRequirePausedPc:
    {
        const auto expected = payload.Unsigned(Field::ExpectedPc);
        if (!expected ||
            *expected > std::numeric_limits<std::uint32_t>::max())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "invalid_expected_pc",
                "Exact paused-PC qualification requires one nonzero PC");
        }
        const std::optional<ExecutionSnapshot> execution =
            session.execution_snapshot();
        if (!execution)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "execution_unavailable",
                "Exact paused-PC qualification requires committed workset execution evidence");
        }
        if (execution->workset_epoch != request.expected_epoch)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::StaleEpoch,
                "stale_epoch",
                "Exact paused-PC qualification observed another workset epoch");
        }
        if (execution->activity != ExecutionActivity::IdlePaused ||
            !execution->evidence.pause_confirmed)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "execution_not_paused",
                "Exact paused-PC qualification requires a confirmed paused session");
        }
        if (execution->evidence.pc !=
            static_cast<std::uint32_t>(*expected))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "paused_pc_mismatch",
                "Expected paused PC " +
                    std::to_string(*expected) +
                    " but observed " +
                    std::to_string(execution->evidence.pc));
        }
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
        completion.output = PausedPcReceiptGraph(
            execution->evidence.pc,
            execution->evidence.vi_count,
            execution->workset_epoch);
        return Immediate(std::move(completion));
    }
    case CanonicalAction::ExecutionObservePausedPc:
    {
        const std::optional<ExecutionSnapshot> execution =
            session.execution_snapshot();
        if (!execution)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "execution_unavailable",
                "Paused-PC observation requires committed workset execution evidence");
        }
        if (execution->workset_epoch != request.expected_epoch)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::StaleEpoch,
                "stale_epoch",
                "Paused-PC observation observed another workset epoch");
        }
        if (execution->activity != ExecutionActivity::IdlePaused ||
            !execution->evidence.pause_confirmed)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "execution_not_paused",
                "Paused-PC observation requires a confirmed paused session");
        }
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
        completion.output = PausedPcReceiptGraph(
            execution->evidence.pc,
            execution->evidence.vi_count,
            execution->workset_epoch);
        return Immediate(std::move(completion));
    }
    case CanonicalAction::InputAcquireLease:
    {
        InputArbiter* input = session.input_arbiter();
        if (!input)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Unsupported,
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
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::InputLease,
            kInputService,
            leased.epoch,
            [input, lease = leased.lease](
                const ResourceReleaseRequest& release) {
                const InputLeaseCloseReceipt closed =
                    input->CloseLease(lease, release.current_epoch);
                return ResourceReleaseResult{
                    closed.ok &&
                            closed.status == InputLeaseStatus::Released
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    closed.message};
            },
            leased.lease.value(),
            {},
            "program input lease",
            diagnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        return complete_resource(std::move(request), *resource);
    }
    case CanonicalAction::InputApplyState:
    case CanonicalAction::InputBeginDelivery:
    case CanonicalAction::InputCompleteDelivery:
    {
        InputArbiter* input = session.input_arbiter();
        ResourceMapping* mapping = require_handle();
        if (!input || !mapping ||
            mapping->kind != ResourceKind::InputLease)
        {
            return Reject(
                request,
                input
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
                "input_lease_unavailable",
                "Input action requires a current typed input lease");
        }
        const InputLeaseId lease(mapping->concrete_id);
        if (action == CanonicalAction::InputCompleteDelivery)
        {
            const auto binding = payload.Unsigned(Field::Binding);
            const auto bound_lease = payload.Unsigned(Field::ParentHandle);
            if (!binding || !bound_lease || *bound_lease != lease.value())
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "delivery_binding_mismatch",
                    "Input delivery completion requires the exact lease binding");
            }
            const InputDeliveryReceipt completed =
                input->CompleteDelivery(
                    lease,
                    InputExecutionBindingId(*binding),
                    request.expected_epoch);
            if (!completed.ok)
            {
                return service_failure(
                    "input_delivery_incomplete",
                    completed.message);
            }
            CanonicalActionPayload result;
            if (!result.AddUnsigned(Field::DeliveryId, completed.delivery.value()) ||
                !result.AddUnsigned(Field::Binding, completed.binding.value()) ||
                !result.AddUnsigned(Field::Handle, completed.lease.value()) ||
                !result.AddUnsigned(Field::Publication, completed.publication.value()) ||
                !result.AddUnsigned(Field::ResultSequence, completed.poll.value()) ||
                !result.AddUnsigned(Field::ResultEpoch, completed.epoch.value()) ||
                !result.AddUnsigned(Field::StateGeneration, completed.state_generation) ||
                !result.AddUnsigned(Field::CompletedCount, completed.callback_count) ||
                !result.AddBytes(Field::ResultFrame, EncodeInputFrame(completed.frame)))
            {
                return service_failure(
                    "result_encoding_failed",
                    "Input delivery receipt could not be encoded");
            }
            return CompleteWithPayload(
                std::move(request),
                std::move(result));
        }

        savor::GCInputFrame frame{};
        const auto encoded = payload.Bytes(Field::InputFrame);
        if (!encoded || !DecodeInputFrame(*encoded, frame))
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "invalid_input_frame",
                "Input state action requires one canonical GC frame");
        }
        const InputExecutionBindingReceipt applied =
            action == CanonicalAction::InputApplyState
            ? input->ApplyState(lease, frame, request.expected_epoch)
            : input->BeginDelivery(lease, frame, request.expected_epoch);
        if (!applied.ok)
        {
            return service_failure(
                action == CanonicalAction::InputApplyState
                    ? "input_state_failed"
                    : "input_delivery_failed",
                applied.message);
        }
        CanonicalActionPayload result;
        if (!AddInputExecutionBindingResult(result, applied))
        {
            return service_failure(
                "result_encoding_failed",
                "Input execution binding could not be encoded");
        }
        return CompleteWithPayload(
            std::move(request),
            std::move(result));
    }
    case CanonicalAction::MovieObserveState:
    {
        MovieService* movies = session.movie_service();
        if (!movies)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Unsupported,
                "movie_service_unavailable",
                "MovieService is unavailable");
        }
        const MovieStateSnapshot observed =
            movies->ReconcilePausedState(request.expected_epoch);
        if (!observed.result.ok)
        {
            if (observed.result.integrity == GuestIntegrity::Unknown)
            {
                session.MarkTainted(
                    observed.result.message.empty()
                        ? "Movie state observation has unknown integrity"
                        : observed.result.message);
            }
            return service_failure(
                "movie_state_observation_failed",
                observed.result.message.empty()
                    ? "Movie state could not be observed"
                    : observed.result.message);
        }
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
        completion.output = MovieStateObservationGraph(observed);
        return Immediate(std::move(completion));
    }
    case CanonicalAction::MovieAdoptRestoredReadOnlyPlayback:
    {
        MovieService* movies = session.movie_service();
        if (!movies)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Unsupported,
                "movie_service_unavailable",
                "MovieService is unavailable");
        }

        PruneReleasedResources();
        const bool already_adopted = std::ranges::any_of(
            resources,
            [&request](const auto& item) {
                const ResourceMapping& mapping = item.second;
                return mapping.kind == ResourceKind::MovieSession &&
                    mapping.epoch == request.expected_epoch &&
                    mapping.concrete_id == static_cast<std::uint64_t>(
                        MovieState::ReadOnlyPlayback) &&
                    (!mapping.finalized || !*mapping.finalized);
            });
        if (already_adopted)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "movie_playback_already_adopted",
                "The restored read-only playback already has an invocation cleanup handle");
        }

        const MovieStateSnapshot observed =
            movies->ReconcilePausedState(request.expected_epoch);
        if (!observed.result.ok)
        {
            if (observed.result.integrity == GuestIntegrity::Unknown)
            {
                session.MarkTainted(
                    observed.result.message.empty()
                        ? "Restored movie playback state could not be observed"
                        : observed.result.message);
            }
            return service_failure(
                "restored_movie_playback_observation_failed",
                observed.result.message.empty()
                    ? "Restored movie playback state is unavailable"
                    : observed.result.message);
        }
        if (observed.state != MovieState::ReadOnlyPlayback)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "restored_movie_playback_unavailable",
                "Movie adoption requires read-only playback restored by the current savestate baseline");
        }

        const MovieReservationId reservation = movies->reservation();
        const MovieCheckpointReceipt captured = movies->CaptureCheckpoint();
        if (!captured.result.ok)
        {
            if (captured.result.integrity == GuestIntegrity::Unknown)
            {
                session.MarkTainted(
                    captured.result.message.empty()
                        ? "Restored movie playback could not prove its active checkpoint"
                        : captured.result.message);
            }
            return service_failure(
                "restored_movie_playback_invalid",
                captured.result.message.empty()
                    ? "Restored movie playback checkpoint is unavailable"
                    : captured.result.message);
        }

        const MovieCheckpointMetadata* movie = captured.checkpoint
            ? &*captured.checkpoint
            : nullptr;
        const bool valid =
            captured.workset_epoch == request.expected_epoch &&
            reservation &&
            movie &&
            movie->mode == MovieCheckpointMode::ReadOnlyPlayback &&
            movie->cursor_known &&
            !movie->dtm_bytes.empty() &&
            !movie->dtm_sha256.empty() &&
            !movie->dtm_path.empty() &&
            observed.workset_epoch == request.expected_epoch &&
            observed.state == MovieState::ReadOnlyPlayback &&
            observed.read_only &&
            observed.current_frame == movie->current_frame &&
            observed.current_input_count == movie->current_input_count;
        if (!valid)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Failed,
                "restored_movie_playback_invalid",
                "Restored read-only playback lacks its active epoch, reservation, movie, or exact cursor");
        }

        auto finalized = std::make_shared<bool>(false);
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::MovieSession,
            kMovieService,
            captured.workset_epoch,
            [movies,
             acquisition_epoch = captured.workset_epoch,
             finalized](const ResourceReleaseRequest& release) {
                if (*finalized)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                const MovieState current = movies->state();
                if (current == MovieState::Inactive)
                {
                    *finalized = true;
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        release.current_epoch != acquisition_epoch
                            ? "movie session was reconciled inactive by state replacement"
                            : std::string{}};
                }
                if (current != MovieState::ReadOnlyPlayback &&
                    current != MovieState::PlaybackEnded)
                {
                    if (release.current_epoch != acquisition_epoch)
                    {
                        *finalized = true;
                        return ResourceReleaseResult{
                            ResourceReleaseStatus::Released,
                            "movie session was superseded by state replacement"};
                    }
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Failed,
                        "movie service activity no longer matches its adopted playback handle"};
                }
                const MovieOperationReceipt stopped =
                    movies->StopPlayback();
                if (stopped.result.ok)
                    *finalized = true;
                return ResourceReleaseResult{
                    stopped.result.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    stopped.result.message};
            },
            static_cast<std::uint64_t>(
                MovieState::ReadOnlyPlayback),
            {},
            "adopted restored read-only movie playback",
            diagnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);

        if (ResourceMapping* mapping = Resource(resource->handle))
        {
            mapping->artifact_path = movie->dtm_path;
            mapping->finalized = std::move(finalized);
        }
        ProgramActionDispatchResult completed =
            complete_resource(std::move(request), *resource);
        if (completed.immediate_result)
        {
            completed.immediate_result->resolution.workset_epoch =
                captured.workset_epoch;
        }
        return completed;
    }
    case CanonicalAction::MoviePrepareReadOnlyPlayback:
    {
        MovieService* movies = session.movie_service();
        const auto path = payload.Utf8(Field::Path);
        if (!movies || !path || path->empty())
        {
            return Reject(
                request,
                movies
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
                movies ? "movie_path_required" : "movie_service_unavailable",
                movies
                    ? "Movie preparation requires a caller-declared DTM path"
                    : "MovieService is unavailable");
        }
        const std::optional<MovieOperationReceipt> adopted =
            movies->PreparedReadOnlyPlaybackReceipt();
        if (!adopted || !adopted->result.ok ||
            !adopted->preparation)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "movie_not_initialized",
                "Movie preparation requires the exact paused preparation committed by workset initialization");
        }
        const MovieOperationReceipt& prepared = *adopted;
        if (prepared.artifact_path.lexically_normal() !=
            std::filesystem::path(*path).lexically_normal())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "movie_artifact_mismatch",
                "Movie preparation request does not identify the workset baseline DTM");
        }

        auto consumed = std::make_shared<bool>(false);
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::PreparedMoviePlayback,
            kMovieService,
            prepared.workset_epoch,
            [movies,
             preparation = prepared.preparation,
             consumed](const ResourceReleaseRequest&) {
                if (*consumed)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                const MovieOperationReceipt abandoned =
                    movies->AbandonPreparedReadOnlyPlayback(preparation);
                return ResourceReleaseResult{
                    abandoned.result.ok
                        ? ResourceReleaseStatus::Released
                        : ResourceReleaseStatus::Failed,
                    abandoned.result.message};
            },
            prepared.preparation.value(),
            {},
            "prepared read-only movie playback",
            diagnostic);
        if (!resource)
            return ResourceFailure(std::move(request), diagnostic);
        if (ResourceMapping* mapping = Resource(resource->handle))
        {
            mapping->artifact_path = prepared.artifact_path;
            mapping->finalized = consumed;
        }
        ProgramActionDispatchResult completed =
            complete_resource(std::move(request), *resource);
        if (completed.immediate_result)
            completed.immediate_result->resolution.workset_epoch =
                prepared.workset_epoch;
        if (active && completed.accepted &&
            completed.immediate_result &&
            completed.immediate_result->resolution.status ==
                ProgramActionResolutionStatus::Completed)
        {
            active->baseline_stage = BaselineStage::MoviePrepared;
        }
        return completed;
    }
    case CanonicalAction::MovieStartPlayback:
    case CanonicalAction::MovieStartRecording:
    {
        MovieService* movies = session.movie_service();
        if (!movies)
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Unsupported,
                "movie_service_unavailable",
                "MovieService is unavailable");
        }
        MovieOperationReceipt started;
        std::filesystem::path declared_artifact;
        if (action ==
            CanonicalAction::MovieStartPlayback)
        {
            ResourceMapping* preparation = require_handle();
            if (!preparation ||
                preparation->kind != ResourceKind::PreparedMoviePlayback ||
                !preparation->finalized)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "movie_preparation_unavailable",
                    "Movie playback requires its exact prepared-movie handle");
            }
            started = session.StartPreparedReadOnlyPlayback(
                MoviePreparationId(preparation->concrete_id));
            if (started.result.ok)
            {
                *preparation->finalized = true;
                SessionResourceLedger* ledger = session.resources();
                SessionResourceBindingTable* bindings =
                    session.resource_bindings();
                const ProgramResourceHandleId consumed_handle =
                    preparation->handle;
                if (!ledger || !bindings ||
                    !ledger->Release(
                        preparation->receipt,
                        *bindings).completed())
                {
                    session.MarkTainted(
                        "Prepared movie handle could not be consumed after playback started");
                    return service_failure(
                        "movie_preparation_cleanup_failed",
                        "Prepared movie handle could not be consumed after playback started");
                }
                resources.erase(consumed_handle.value());
            }
        }
        else
        {
            ResourceMapping* playback = require_handle();
            if (!playback ||
                playback->kind != ResourceKind::MovieSession ||
                playback->concrete_id != static_cast<std::uint64_t>(
                    MovieState::ReadOnlyPlayback) ||
                !playback->finalized)
            {
                return Reject(
                    request,
                    ProgramActionResolutionStatus::Rejected,
                    "movie_playback_unavailable",
                    "Movie recording requires its exact active read-only playback handle");
            }
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
                    ProgramActionResolutionStatus::Rejected,
                    "movie_artifact_path_required",
                    "Movie recording start must declare its immutable DTM output path");
            }
            started = movies->StartRecording(recording);
            if (started.result.ok)
            {
                *playback->finalized = true;
                SessionResourceLedger* ledger = session.resources();
                SessionResourceBindingTable* bindings =
                    session.resource_bindings();
                const ProgramResourceHandleId consumed_handle =
                    playback->handle;
                if (!ledger || !bindings ||
                    !ledger->Release(
                        playback->receipt,
                        *bindings).completed())
                {
                    (void)movies->CancelRecording();
                    session.MarkTainted(
                        "Playback handle could not be consumed after recording branched");
                    return service_failure(
                        "movie_playback_cleanup_failed",
                        "Playback handle could not be consumed after recording branched");
                }
                resources.erase(consumed_handle.value());
            }
        }
        if (!started.result.ok)
        {
            if (started.result.integrity == GuestIntegrity::Unknown)
            {
                session.MarkTainted(
                    started.result.message.empty()
                        ? "Movie start left guest integrity unknown"
                        : started.result.message);
            }
            return service_failure(
                "movie_start_failed",
                started.result.message);
        }
        PruneReleasedResources();
        const MovieState state = started.state;
        auto finalized = std::make_shared<bool>(false);
        std::string diagnostic;
        auto resource = RegisterResource(
            request.scope,
            ResourceKind::MovieSession,
            kMovieService,
            started.workset_epoch,
            [movies,
             state,
             acquisition_epoch = started.workset_epoch,
             finalized](
                const ResourceReleaseRequest& release) {
                if (*finalized)
                {
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        {}};
                }
                const MovieState current =
                    movies->state();
                if (current == MovieState::Inactive)
                {
                    *finalized = true;
                    return ResourceReleaseResult{
                        ResourceReleaseStatus::Released,
                        release.current_epoch != acquisition_epoch
                            ? "movie session was reconciled inactive by state replacement"
                            : std::string{}};
                }
                const bool ended_owned_playback =
                    state == MovieState::ReadOnlyPlayback &&
                    current == MovieState::PlaybackEnded;
                if (current != state && !ended_owned_playback)
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
                    state == MovieState::Recording
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
            static_cast<std::uint64_t>(state),
            {},
            "program movie session",
            diagnostic);
        if (!resource)
        {
            // RegisterResource may have failed before installing its release
            // callback, or it may already have invoked that callback while
            // compensating a ledger failure. Reconcile the actual service
            // state so a successfully started movie is never left without a
            // resource-ledger cleanup authority and an already-compensated
            // movie is not stopped twice.
            MovieOperationReceipt compensated;
            bool compensation_required = true;
            switch (movies->state())
            {
            case MovieState::ReadOnlyPlayback:
            case MovieState::PlaybackEnded:
                compensated = movies->StopPlayback();
                break;
            case MovieState::Recording:
                compensated = movies->CancelRecording();
                break;
            case MovieState::Inactive:
                compensation_required = false;
                break;
            case MovieState::PreparedReadOnlyPlayback:
                compensated = movies->AbandonPreparedReadOnlyPlayback(
                    movies->preparation());
                break;
            case MovieState::Unknown:
                compensated.result = MovieServiceResult::Failure(
                    MovieServiceErrorCode::IntegrityFailure,
                    "Movie state is unknown during resource compensation",
                    GuestIntegrity::Unknown);
                break;
            }
            if (compensation_required && !compensated.result.ok)
                session.MarkTainted(
                    "Movie activity could not be compensated after resource registration failed");
            return ResourceFailure(std::move(request), diagnostic);
        }
        ResourceMapping* mapping = Resource(resource->handle);
        if (mapping)
        {
            mapping->artifact_path = std::move(declared_artifact);
            mapping->finalized = std::move(finalized);
        }
        ProgramActionDispatchResult completed =
            complete_resource(std::move(request), *resource);
        if (completed.immediate_result)
        {
            completed.immediate_result->resolution.workset_epoch =
                started.workset_epoch;
        }
        if (action == CanonicalAction::MovieStartPlayback &&
            active && completed.accepted &&
            completed.immediate_result &&
            completed.immediate_result->resolution.status ==
                ProgramActionResolutionStatus::Completed)
        {
            active->baseline_stage = BaselineStage::Established;
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
                ProgramActionResolutionStatus::Rejected,
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
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
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
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
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
                ProgramActionResolutionStatus::CleanupFailed,
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
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
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
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
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
            ProgramActionResolutionStatus::Unsupported,
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
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
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
    case CanonicalAction::CaptureMark:
    {
        CaptureService* capture = session.capture_service();
        const auto marker = payload.Utf8(Field::MarkerId);
        if (!capture || !marker)
        {
            return Reject(
                request,
                capture
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
                "capture_attachment_unavailable",
                "Capture marker requires the workset capture binding and marker ID");
        }
        const CaptureServiceReceipt marked = capture->Mark(
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
    case CanonicalAction::ScreenshotCapture:
    {
        const auto label = payload.Utf8(Field::Label);
        if (!label || label->empty())
        {
            return Reject(
                request,
                ProgramActionResolutionStatus::Rejected,
                "screenshot_label_required",
                "Screenshot capture requires a logical artifact label");
        }
        RuntimeArtifactSink* sink = session.artifact_sink();
        if (sink == nullptr)
        {
            return service_failure(
                "artifact_sink_unavailable",
                "Screenshot capture requires a host runtime artifact sink");
        }
        std::string reservation_error;
        const auto reservation = sink->Reserve(
            *label,
            ".png",
            &reservation_error);
        if (!reservation.has_value())
        {
            return service_failure(
                "artifact_reservation_failed",
                reservation_error);
        }
        const auto timeout = Timeout(request, payload);
        const SessionOperationReceipt captured =
            session.CaptureScreenshot(
                reservation->output_path,
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
            digest = hash::sha256_of_file(
                reservation->output_path.string());
        }
        catch (const std::exception& ex)
        {
            return service_failure(
                "screenshot_hash_failed",
                ex.what());
        }
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::Completed);
        completion.output = ArtifactResultGraph(
            action,
            "screenshot:" + reservation->logical_label + ":" +
                std::to_string(request.request_id.value()),
            reservation->output_path.string(),
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
                    ? ProgramActionResolutionStatus::Rejected
                    : ProgramActionResolutionStatus::Unsupported,
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
        ProgramActionResolutionStatus::Unsupported,
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
    policy.expected_epoch = current.workset_epoch;
    policy.movie_ended = MovieEndedPolicy::Ignore;
    policy.throttle = ExecutionThrottlePolicy::RequireDisabled;
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

ProgramActionResolution
SessionProgramActionHost::Impl::ExecutionCompletion(
    const PendingExecution& operation,
    const ExecutionTerminalResult& terminal)
{
    ProgramActionResolution completion = Completion(
        operation.request,
        ExecutionCompletionStatus(terminal));
    completion.workset_epoch = terminal.workset_epoch
        ? terminal.workset_epoch
        : session.snapshot().workset_epoch;
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
            ProgramActionResolutionStatus::Failed;
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
                ProgramActionResolutionStatus::Failed;
            completion.code = "result_encoding_failed";
            completion.message =
                "ContinueUntil completed without a typed terminal observation";
            return completion;
        }
        completion.code.clear();
        completion.message.clear();
        return completion;
    }
    if (*action == CanonicalAction::ExecutionContinueUntilInputObserved)
    {
        completion.output = InputObservedExecutionResultGraph(terminal);
        if (completion.output.values.empty())
        {
            completion.status = ProgramActionResolutionStatus::Failed;
            completion.code = "result_encoding_failed";
            completion.message =
                "Input-observation execution completed without its typed terminal evidence";
            return completion;
        }
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
        completion.workset_epoch.value());
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
            ProgramActionResolutionStatus::Failed;
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
    if (ExecutionSucceeded(terminal) && completed.request.action &&
        ResolveCanonicalAction(*completed.request.action) ==
            CanonicalAction::ExecutionContinueUntil &&
        terminal.stop && terminal.stop->event &&
        terminal.stop->terminal == StopRouteTerminal::ForegroundMatched)
    {
        foreground_semantic_stops.push_back({
            completed.request.invocation_id,
            completed.request.attempt_id,
            *terminal.stop->event,
            terminal.evidence,
        });
    }
    ProgramActionResolution completion =
        ExecutionCompletion(completed, terminal);

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
    if (completed.request.action)
    {
        const auto action = ResolveCanonicalAction(*completed.request.action);
        if (action && IsDiagnosticAction(*action))
        {
            SCLOGDX(
                SC_TAGS("program.action", "program.action.resolved"),
                "invocation=%llu attempt=%llu request=%llu epoch=%llu selector=%s action=%.*s operation=%llu status=%u code=%s",
                completed.request.invocation_id.value(),
                completed.request.attempt_id.value(),
                completed.request.request_id.value(),
                completion.workset_epoch.value(),
                completed.request.diagnostic_selector.empty()
                    ? "<unnamed>"
                    : completed.request.diagnostic_selector.c_str(),
                static_cast<int>(CanonicalActionName(*action).size()),
                CanonicalActionName(*action).data(), terminal.operation_id.value(),
                static_cast<unsigned>(completion.status),
                completion.code.empty() ? "<none>" : completion.code.c_str());
        }
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
        ProgramActionResolution completion = Completion(
            continuation.request,
            ProgramActionResolutionStatus::CleanupFailed,
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
        ProgramActionResolution completion = Completion(
            request,
            ProgramActionResolutionStatus::CleanupFailed,
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

    ProgramActionResolution completion = Completion(
        continuation.request,
        ProgramActionResolutionStatus::Completed);
    completion.cleanup_receipts =
        std::move(cleanup_receipts);
    completion.cleanup = CleanupStatusOf(unwind.disposition);
    if (unwind.disposition ==
        ResourceCleanupDisposition::TaintRequired)
    {
        session.MarkTainted(
            "Program resource cleanup requires session taint");
        completion.status =
            ProgramActionResolutionStatus::CleanupFailed;
        completion.session_disposition =
            SessionDisposition::Tainted;
    }
    (void)Queue(std::move(completion));
}

void SessionProgramActionHost::Impl::
AbandonQueuedStagedOutputs() noexcept
{
    for (const ActorActionResult& result : completions)
    {
        for (const StagedProgramOutput& output : result.staged_outputs)
        {
            const auto* savestate =
                std::get_if<StagedSavestateOutput>(&output);
            if (!savestate)
                continue;
            const SavestateServiceResult abandoned =
                session.AbandonImmutableSavestateArtifact(
                    savestate->capture.artifact);
            if (!abandoned.ok &&
                abandoned.code != SavestateServiceErrorCode::NotFound)
            {
                session.MarkTainted(
                    abandoned.message.empty()
                        ? "Queued staged program output could not be abandoned"
                        : abandoned.message);
            }
        }
    }
    completions.clear();
}

void SessionProgramActionHost::Impl::
ReleaseSavedArtifactRecords() noexcept
{
    for (const auto& [identity, artifact] : saved_artifacts)
    {
        (void)identity;
        const SavestateServiceResult released =
            session.ReleaseSavestateArtifact(artifact.artifact);
        if (!released.ok &&
            released.code != SavestateServiceErrorCode::NotFound)
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
    const auto action = request.action
        ? ResolveCanonicalAction(*request.action)
        : std::nullopt;
    const bool diagnostic = action && IsDiagnosticAction(*action);
    const auto invocation = request.invocation_id.value();
    const auto attempt = request.attempt_id.value();
    const auto request_id = request.request_id.value();
    const auto epoch = request.expected_epoch.value();
    const std::string selector = request.diagnostic_selector;
    if (diagnostic)
    {
        SCLOGDX(
            SC_TAGS("program.action", "program.action.dispatch"),
            "invocation=%llu attempt=%llu request=%llu epoch=%llu selector=%s action=%.*s",
            invocation, attempt, request_id, epoch,
            selector.empty() ? "<unnamed>" : selector.c_str(),
            static_cast<int>(CanonicalActionName(*action).size()),
            CanonicalActionName(*action).data());
    }
    ProgramActionDispatchResult result = impl_->Dispatch(std::move(request));
    if (diagnostic)
    {
        const auto status = result.immediate_result
            ? static_cast<unsigned>(result.immediate_result->resolution.status)
            : 0u;
        SCLOGDX(
            SC_TAGS("program.action", "program.action.accepted"),
            "invocation=%llu attempt=%llu request=%llu epoch=%llu selector=%s action=%.*s accepted=%u immediate=%u status=%u diagnostic=%s",
            invocation, attempt, request_id, epoch,
            selector.empty() ? "<unnamed>" : selector.c_str(),
            static_cast<int>(CanonicalActionName(*action).size()),
            CanonicalActionName(*action).data(), result.accepted ? 1u : 0u,
            result.immediate_result ? 1u : 0u, status,
            result.diagnostic.empty() ? "<none>" : result.diagnostic.c_str());
    }
    return result;
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

std::vector<ActorActionResult>
SessionProgramActionHost::DrainResults()
{
    if (!impl_ || !impl_->BindOrCheckOwner())
        return {};
    std::vector<ActorActionResult> result;
    result.swap(impl_->completions);
    return result;
}

std::vector<ForegroundSemanticStopObservationV1>
SessionProgramActionHost::DrainForegroundSemanticStops()
{
    if (!impl_ || !impl_->BindOrCheckOwner())
        return {};
    std::vector<ForegroundSemanticStopObservationV1> result;
    result.swap(impl_->foreground_semantic_stops);
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
    impl_->AbandonQueuedStagedOutputs();
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
    result.epoch = impl_->session.snapshot().workset_epoch;
    result.mapped_scope_count = impl_->scopes.size();
    result.mapped_resource_count = impl_->resources.size();
    result.execution_pending = impl_->pending.has_value();
    result.queued_completion_count =
        impl_->completions.size();
    return result;
}

} // namespace savor::runtime::program
