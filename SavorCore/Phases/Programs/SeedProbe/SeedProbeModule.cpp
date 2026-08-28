#include "SeedProbeModule.h"

#include "../../../Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "../../../Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../../../Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "../../../Runner/Runtime/ProgramRuntime/Composition/InputDeliveryComposition.h"
#include "../../../Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../../../Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "../../../Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "../../../Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "../../../Runner/Runtime/ProgramKind.h"
#include "../../../Utils/Hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace savor::runtime::seedprobe {
namespace {

using namespace program;
using composition::detail::ModuleFragmentBuilder;

constexpr std::string_view kPreBattlePointId =
    "soa.field.point.prebattle.AfterRandSeedSet";
constexpr std::string_view kFieldReturnPointId =
    "soa.field.point.field_return.RandSeedCommitted";

struct SeedProbeEntryRouteDefinition
{
    std::uint32_t entry_pc = 0;
    SeedProbeEndpointV2 endpoint =
        SeedProbeEndpointV2::AfterRandSeedSet;
};

constexpr std::array<SeedProbeEntryRouteDefinition, 3> kEntryRoutes{{
    {PreBattleBeforeRandSeedSetPc,
     SeedProbeEndpointV2::AfterRandSeedSet},
    {FieldTransitionFastPreseedPc,
     SeedProbeEndpointV2::RandSeedCommitted},
    {FieldTransitionDeferredPreseedPc,
     SeedProbeEndpointV2::RandSeedCommitted},
}};

consteval bool EntryRoutesAreValid()
{
    for (std::size_t left = 0; left < kEntryRoutes.size(); ++left)
    {
        if (kEntryRoutes[left].entry_pc == 0 ||
            (kEntryRoutes[left].endpoint !=
                 SeedProbeEndpointV2::AfterRandSeedSet &&
             kEntryRoutes[left].endpoint !=
                 SeedProbeEndpointV2::RandSeedCommitted))
            return false;
        for (std::size_t right = left + 1;
             right < kEntryRoutes.size(); ++right)
        {
            if (kEntryRoutes[left].entry_pc ==
                kEntryRoutes[right].entry_pc)
                return false;
        }
    }
    return true;
}

static_assert(EntryRoutesAreValid());

constexpr std::string_view kEndpointSchemaContract =
    "enum SeedProbeEndpoint/2{"
    "AfterRandSeedSet=0,"
    "RandSeedCommitted=1}";
constexpr std::string_view kRequestSchemaContract =
    "record SeedProbeRequest/2{"
    "frame:runtime.input.InputFramePayload/1}";
constexpr std::string_view kResultSchemaContract =
    "record SeedProbeResult/3{"
    "raw_seed:u32,"
    "endpoint:soa.seed_probe.Endpoint/2,"
    "semantic_stop:runtime.execution.ContinueUntilResult/1,"
    "delivery:runtime.input.InputDeliveryReceipt/1}";

void SetDiagnostic(std::string* diagnostic, std::string message)
{
    if (diagnostic != nullptr)
        *diagnostic = std::move(message);
}

bool IsValidEndpoint(SeedProbeEndpointV2 endpoint) noexcept
{
    return endpoint == SeedProbeEndpointV2::AfterRandSeedSet ||
        endpoint == SeedProbeEndpointV2::RandSeedCommitted;
}

TypeRef EndpointType()
{
    return TypeRef::Named(SeedProbeEndpointSchemaIdentityV2());
}

TypeRef RequestType()
{
    return TypeRef::Named(SeedProbeRequestSchemaIdentityV2());
}

TypeRef ResultType()
{
    return TypeRef::Named(SeedProbeResultSchemaIdentityV3());
}

TypeRef FramePayloadType()
{
    return CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
}

std::vector<Byte> EncodeFrame(const savor::GCInputFrame& frame)
{
    return {
        static_cast<Byte>(frame.buttons),
        static_cast<Byte>(frame.buttons >> 8u),
        frame.main_x,
        frame.main_y,
        frame.c_x,
        frame.c_y,
        frame.trig_l,
        frame.trig_r,
    };
}

bool DecodeFrame(
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

class GraphReader final
{
public:
    explicit GraphReader(const ProgramValueGraph& graph)
    {
        valid_ = static_cast<bool>(graph.root);
        for (const auto& value : graph.values)
        {
            if (!value.id || !values_.emplace(value.id, &value).second)
            {
                valid_ = false;
                return;
            }
        }
        root_ = Find(graph.root);
        valid_ = valid_ && root_ != nullptr;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const ProgramValue* root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] const ProgramValue* Find(ProgramValueId id) const
    {
        const auto found = values_.find(id);
        return found == values_.end() ? nullptr : found->second;
    }

    template <typename Payload>
    [[nodiscard]] const Payload* PayloadOf(ProgramValueId id) const
    {
        const ProgramValue* value = Find(id);
        return value == nullptr
            ? nullptr
            : std::get_if<Payload>(&value->payload);
    }

private:
    bool valid_ = false;
    const ProgramValue* root_ = nullptr;
    std::map<ProgramValueId, const ProgramValue*> values_;
};

template <typename Scalar>
bool ReadScalar(
    const GraphReader& graph,
    ProgramValueId id,
    BuiltinType type,
    Scalar& output)
{
    const ProgramValue* value = graph.Find(id);
    const auto* scalar = value == nullptr
        ? nullptr
        : std::get_if<Scalar>(&value->payload);
    if (value == nullptr ||
        value->type != TypeRef::Builtin(type) ||
        scalar == nullptr)
    {
        return false;
    }
    output = *scalar;
    return true;
}

bool ReadNamedBytes(
    const GraphReader& graph,
    ProgramValueId id,
    const SchemaIdentity& schema,
    std::vector<Byte>& output)
{
    const ProgramValue* value = graph.Find(id);
    const auto* bytes = value == nullptr
        ? nullptr
        : std::get_if<std::vector<Byte>>(&value->payload);
    if (value == nullptr ||
        value->type != TypeRef::Named(schema) ||
        bytes == nullptr)
    {
        return false;
    }
    output = *bytes;
    return true;
}

bool ReadReceiptPayload(
    const GraphReader& graph,
    ProgramValueId id,
    CanonicalAction action,
    CanonicalActionPayload& output)
{
    const ProgramValue* value = graph.Find(id);
    const auto schema = CanonicalActionOutputSchemaIdentity(action);
    if (value == nullptr || !schema ||
        value->type != TypeRef::Named(*schema) ||
        !std::holds_alternative<std::vector<Byte>>(value->payload))
    {
        return false;
    }
    ProgramValueGraph receipt{
        .root = value->id,
        .values = {*value},
    };
    return DecodeCanonicalActionPayload(
        receipt,
        *schema,
        output);
}

bool DecodeDelivery(
    const GraphReader& graph,
    ProgramValueId id,
    InputDeliveryReceiptV3& output)
{
    CanonicalActionPayload payload;
    if (!ReadReceiptPayload(
            graph,
            id,
            CanonicalAction::InputCompleteDelivery,
            payload))
    {
        return false;
    }
    const auto delivery = payload.Unsigned(
        CanonicalActionPayloadField::DeliveryId);
    const auto binding = payload.Unsigned(
        CanonicalActionPayloadField::Binding);
    const auto lease = payload.Unsigned(
        CanonicalActionPayloadField::Handle);
    const auto publication = payload.Unsigned(
        CanonicalActionPayloadField::Publication);
    const auto epoch = payload.Unsigned(
        CanonicalActionPayloadField::ResultEpoch);
    const auto poll = payload.Unsigned(
        CanonicalActionPayloadField::ResultSequence);
    const auto generation = payload.Unsigned(
        CanonicalActionPayloadField::StateGeneration);
    const auto callback_count = payload.Unsigned(
        CanonicalActionPayloadField::CompletedCount);
    const auto frame = payload.Bytes(
        CanonicalActionPayloadField::ResultFrame);
    savor::GCInputFrame decoded{};
    if (!delivery || !binding || !lease || !publication || !poll ||
        !epoch || !generation || !callback_count || !frame ||
        *delivery == 0 || *binding == 0 || *lease == 0 ||
        *publication == 0 || *poll == 0 || *epoch == 0 ||
        *generation == 0 || *callback_count == 0 ||
        !DecodeFrame(*frame, decoded))
    {
        return false;
    }
    output = {
        .delivery_id = *delivery,
        .binding_id = *binding,
        .lease_id = *lease,
        .publication_id = *publication,
        .poll_receipt_id = *poll,
        .workset_epoch = WorksetEpoch(*epoch),
        .state_generation = *generation,
        .callback_count = static_cast<std::uint32_t>(*callback_count),
        .frame = decoded,
    };
    return true;
}

bool DecodeStop(
    const GraphReader& graph,
    ProgramValueId id,
    SemanticStopReceiptV2& output)
{
    const ProgramValue* value = graph.Find(id);
    const auto* record = value == nullptr
        ? nullptr
        : std::get_if<RecordValue>(&value->payload);
    if (value == nullptr ||
        value->type != CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil) ||
        record == nullptr || record->fields.size() != 7)
    {
        return false;
    }

    const ProgramValue* reason_value = graph.Find(record->fields[0]);
    const auto* reason = reason_value == nullptr
        ? nullptr
        : std::get_if<EnumValue>(&reason_value->payload);
    const auto* optional_stop =
        graph.PayloadOf<OptionalValue>(record->fields[1]);
    const ProgramValue* routed_value =
        optional_stop && optional_stop->value
        ? graph.Find(*optional_stop->value)
        : nullptr;
    const auto* routed = routed_value == nullptr
        ? nullptr
        : std::get_if<RecordValue>(&routed_value->payload);
    if (reason_value == nullptr ||
        reason_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason) ||
        reason == nullptr ||
        reason->value != static_cast<std::int64_t>(
            ContinueUntilCompletionReasonV1::Breakpoint) ||
        routed_value == nullptr ||
        routed_value->type != CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt) ||
        routed == nullptr || routed->fields.size() != 5)
    {
        return false;
    }

    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint32_t pc = 0;
    std::uint32_t result_pc = 0;
    std::uint64_t sample = 0;
    std::vector<Byte> evidence;
    if (!ReadScalar(
            graph,
            routed->fields[0],
            BuiltinType::U64,
            sequence) ||
        !ReadScalar(
            graph,
            routed->fields[1],
            BuiltinType::U64,
            epoch) ||
        !ReadScalar(
            graph,
            routed->fields[2],
            BuiltinType::U32,
            pc) ||
        !ReadScalar(
            graph,
            routed->fields[3],
            BuiltinType::U64,
            sample) ||
        !ReadNamedBytes(
            graph,
            routed->fields[4],
            CanonicalRuntimeSchemaIdentity(
                CanonicalRuntimeSchema::StopEvidencePayload),
            evidence) ||
        !ReadScalar(
            graph,
            record->fields[2],
            BuiltinType::U32,
            result_pc) ||
        sequence == 0 || epoch == 0 || pc == 0 ||
        pc != result_pc || sample == 0 || evidence.empty())
    {
        return false;
    }

    output = {
        .stop_sequence = sequence,
        .workset_epoch = WorksetEpoch(epoch),
        .pc = pc,
        .sample_snapshot_id = sample,
        .evidence = std::move(evidence),
    };
    return true;
}

bool StopEvidenceMatches(
    const SemanticStopReceiptV2& receipt) noexcept
{
    // ContinueUntilResultGraph emits RSE1 followed by path, point kind,
    // registered PC point, and hit PC. Validate the redundant physical
    // evidence rather than trusting only the projected pc field.
    const auto& bytes = receipt.evidence;
    if (bytes.size() < 14 ||
        bytes[0] != static_cast<Byte>('R') ||
        bytes[1] != static_cast<Byte>('S') ||
        bytes[2] != static_cast<Byte>('E') ||
        bytes[3] != static_cast<Byte>('1') ||
        bytes[5] != 0)
    {
        return false;
    }
    const auto u32 = [&bytes](std::size_t offset)
    {
        return static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
    };
    return u32(6) == receipt.pc && u32(10) == receipt.pc;
}

class StaticConfigWriter final
{
public:
    explicit StaticConfigWriter(std::array<char, 4> magic)
    {
        for (const char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            bytes_.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
        }
    }
    void String(std::string_view value)
    {
        if (value.size() >
            std::numeric_limits<std::uint32_t>::max())
        {
            throw std::length_error("SeedProbe static config text is too large");
        }
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& value)
    {
        bytes_.insert(
            bytes_.end(),
            value.bytes.begin(),
            value.bytes.end());
    }
    [[nodiscard]] std::vector<Byte> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<Byte> bytes_;
};

struct SemanticPoint final
{
    std::string_view id;
    std::uint32_t pc = 0;
};

RuntimeProfile InvocationRuntimeProfile(
    const ProgramDependencyLock& dependencies)
{
    return {
        .profile_id = "soa-usa-jit64-v1",
        .game_id = std::string(
            program::capabilities::kSupportedGameId),
        .disc_identity = std::string(
            program::capabilities::kSupportedGameId),
        .executable_identity = std::string(
            program::capabilities::
                kSupportedExecutableIdentity),
        .backend = "jit64",
        .capability_packs = dependencies.capability_packs,
    };
}

std::string RuntimeProfileHash(const RuntimeProfile& profile)
{
    std::string canonical = profile.profile_id;
    const auto append = [&canonical](std::string_view value)
    {
        canonical.push_back('\0');
        canonical.append(value);
    };
    append(profile.game_id);
    append(profile.disc_identity);
    append(profile.executable_identity);
    append(profile.backend);
    return hash::sha256(canonical.data(), canonical.size());
}

std::vector<Byte> ObservationConfig()
{
    StaticConfigWriter writer({'O', 'S', 'C', '1'});
    writer.String("soa.seed_probe.rng_seed");
    writer.U8(0); // Required.
    writer.Bool(false); // Scalar read, not coherent query.
    writer.U8(0); // No pointer dereference.
    return std::move(writer).Finish();
}

InstructionTarget ActionTarget(
    const ExactDependencyIdentity& dependency)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = dependency,
    };
}

ProgramValueId Required(
    std::optional<ProgramValueId> value,
    std::string_view operation)
{
    if (!value)
    {
        throw std::logic_error(
            "SeedProbe module lowering failed at " +
            std::string(operation));
    }
    return *value;
}

ProgramValueId ConstantBytes(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::vector<Byte> bytes,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = CanonicalRuntimeType(schema);
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::Constant,
            type,
            {},
            {},
            std::move(selector),
            LiteralValue{
                .type = type,
                .payload = std::move(bytes),
            },
            scope),
        "constant bytes");
}

ProgramValueId ConstantU64(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint64_t value,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = TypeRef::Builtin(BuiltinType::U64);
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::Constant,
            type,
            {},
            {},
            std::move(selector),
            LiteralValue{
                .type = type,
                .payload = value,
            },
            scope),
        "constant u64");
}

ProgramValueId ConstantU32(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    std::uint32_t value,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = TypeRef::Builtin(BuiltinType::U32);
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::Constant,
            type,
            {},
            {},
            std::move(selector),
            LiteralValue{
                .type = type,
                .payload = value,
            },
            scope),
        "constant u32");
}

ProgramValueId ConstantEndpoint(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    SeedProbeEndpointV2 endpoint,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = EndpointType();
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::Constant,
            type,
            {},
            {},
            std::move(selector),
            LiteralValue{
                .type = type,
                .payload = EnumValue{
                    .schema = SeedProbeEndpointSchemaIdentityV2(),
                    .value = static_cast<std::int64_t>(endpoint),
                },
            },
            scope),
        "constant SeedProbe endpoint");
}

ProgramValueId ConstructRequest(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    std::span<const ProgramValueId> fields,
    std::string selector,
    ProgramScopeId scope)
{
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordConstruct,
            CanonicalActionInputType(action),
            fields,
            {},
            std::move(selector),
            std::nullopt,
            scope),
        "canonical request");
}

ProgramValueId OptionalValue(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::optional<ProgramValueId> value,
    std::string selector,
    ProgramScopeId scope)
{
    std::vector<ProgramValueId> operands;
    if (value)
        operands.push_back(*value);
    return Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::OptionalConstruct,
            CanonicalRuntimeType(schema),
            operands,
            {},
            std::move(selector),
            std::nullopt,
            scope),
        "optional value");
}

void AddCanonicalAction(
    ModuleFragmentBuilder& builder,
    CanonicalAction action)
{
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const auto& schema :
         CanonicalActionTypeSchemaClosure(action))
    {
        builder.AddTypeImport(schema);
    }
}

void LowerObservation(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramBlockId complete_block,
    ProgramValueId delivery,
    ProgramValueId stop,
    ProgramScopeId scope,
    SemanticPoint point,
    SeedProbeEndpointV2 endpoint)
{
    const std::string prefix =
        point.pc == PreBattleAfterRandSeedSetPc
        ? "prebattle"
        : "field-return";

    const ProgramValueId optional_stop = OptionalValue(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalContinueUntilResult,
        stop,
        prefix + "/rng/stop-receipt",
        scope);
    const ProgramValueId rng_address = ConstantU64(
        builder,
        function,
        block,
        RngSeedAddress,
        prefix + "/rng/address",
        scope);
    const ProgramValueId observation_config = ConstantBytes(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::ObservationStaticConfig,
        ObservationConfig(),
        prefix + "/rng/static-config",
        scope);
    const ProgramValueId read_request = ConstructRequest(
        builder,
        function,
        block,
        CanonicalAction::GuestReadU32,
        std::array{
            optional_stop,
            rng_address,
            observation_config,
        },
        prefix + "/rng/request",
        scope);
    const ProgramValueId raw_seed = Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            TypeRef::Builtin(BuiltinType::U32),
            std::array{read_request},
            ActionTarget(CanonicalActionIdentity(
                CanonicalAction::GuestReadU32)),
            prefix + "/rng/read-paused-u32",
            std::nullopt,
            {}),
        "paused RNG read");

    const ProgramValueId endpoint_value = ConstantEndpoint(
        builder,
        function,
        block,
        endpoint,
        prefix + "/endpoint",
        scope);

    const ProgramValueId result = Required(
        builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordConstruct,
            ResultType(),
            std::array{
                raw_seed,
                endpoint_value,
                stop,
                delivery,
            },
            {},
            prefix + "/result",
            std::nullopt,
            scope),
        "SeedProbe factual result");

    builder.SetTerminator(
        function,
        block,
        Terminator{
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = complete_block,
                .arguments = {result},
            }},
        },
        prefix + "/complete");
}

} // namespace

SchemaIdentity SeedProbeEndpointSchemaIdentityV2()
{
    return composition::ExactSchema(
        "soa.seed_probe.Endpoint",
        2,
        kEndpointSchemaContract);
}

SchemaIdentity SeedProbeRequestSchemaIdentityV2()
{
    return composition::ExactSchema(
        "soa.seed_probe.Request",
        2,
        kRequestSchemaContract);
}

SchemaIdentity SeedProbeResultSchemaIdentityV3()
{
    return composition::ExactSchema(
        "soa.seed_probe.Result",
        3,
        kResultSchemaContract);
}

std::uint32_t SeedProbeEndpointPc(SeedProbeEndpointV2 endpoint) noexcept
{
    switch (endpoint)
    {
    case SeedProbeEndpointV2::AfterRandSeedSet:
        return PreBattleAfterRandSeedSetPc;
    case SeedProbeEndpointV2::RandSeedCommitted:
        return FieldReturnRandSeedCommittedPc;
    }
    return 0;
}

std::string_view SeedProbeEndpointPointId(
    SeedProbeEndpointV2 endpoint) noexcept
{
    switch (endpoint)
    {
    case SeedProbeEndpointV2::AfterRandSeedSet:
        return kPreBattlePointId;
    case SeedProbeEndpointV2::RandSeedCommitted:
        return kFieldReturnPointId;
    }
    return {};
}

std::optional<SeedProbeEndpointV2>
SeedProbeEndpointFromPc(std::uint32_t pc) noexcept
{
    if (pc == PreBattleAfterRandSeedSetPc)
        return SeedProbeEndpointV2::AfterRandSeedSet;
    if (pc == FieldReturnRandSeedCommittedPc)
        return SeedProbeEndpointV2::RandSeedCommitted;
    return std::nullopt;
}

std::optional<SeedProbeEndpointV2>
SeedProbeEndpointForEntryPc(std::uint32_t pc) noexcept
{
    const auto found = std::ranges::find(
        kEntryRoutes,
        pc,
        &SeedProbeEntryRouteDefinition::entry_pc);
    return found == kEntryRoutes.end()
        ? std::nullopt
        : std::optional<SeedProbeEndpointV2>(found->endpoint);
}

ProgramValueGraph EncodeSeedProbeRequestV2(
    const SeedProbeRequestV2& request)
{
    ProgramValue frame{
        .id = ProgramValueId(1),
        .type = FramePayloadType(),
        .payload = EncodeFrame(request.frame),
    };
    ProgramValue root{
        .id = ProgramValueId(2),
        .type = RequestType(),
        .payload = RecordValue{{frame.id}},
    };
    return {
        .root = root.id,
        .values = {
            std::move(frame),
            std::move(root),
        },
    };
}

bool DecodeSeedProbeRequestV2(
    const ProgramValueGraph& graph,
    SeedProbeRequestV2& request,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    GraphReader reader(graph);
    const ProgramValue* root = reader.root();
    const auto* record = root == nullptr
        ? nullptr
        : std::get_if<RecordValue>(&root->payload);
    if (!reader.valid() || root == nullptr ||
        root->type != RequestType() ||
        record == nullptr || record->fields.size() != 1)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe request root is not the exact Request/2 record");
        return false;
    }

    const ProgramValue* frame_value =
        reader.Find(record->fields[0]);
    const auto* frame_bytes = frame_value == nullptr
        ? nullptr
        : std::get_if<std::vector<Byte>>(
              &frame_value->payload);
    savor::GCInputFrame frame{};
    if (frame_value == nullptr ||
        frame_value->type != FramePayloadType() ||
        frame_bytes == nullptr ||
        !DecodeFrame(*frame_bytes, frame))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe request frame is malformed");
        return false;
    }
    request = {.frame = frame};
    return true;
}

std::vector<std::uint8_t> EncodeSeedProbeExecutionInputV2(
    const SeedProbeRequestV2& request)
{
    return EncodeFrame(request.frame);
}

bool DecodeSeedProbeExecutionInputV2(
    std::span<const std::uint8_t> payload,
    SeedProbeRequestV2& request,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!DecodeFrame(payload, request.frame))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe scalar execution input is not one canonical GCInputFrame");
        return false;
    }
    return true;
}

bool DecodeSeedProbeResultV3(
    const ProgramValueGraph& graph,
    SeedProbeResultV3& result,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    GraphReader reader(graph);
    const ProgramValue* root = reader.root();
    const auto* record = root == nullptr
        ? nullptr
        : std::get_if<RecordValue>(&root->payload);
    if (!reader.valid() || root == nullptr ||
        root->type != ResultType() ||
        record == nullptr || record->fields.size() != 4)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe output root is not the exact Result/3 record");
        return false;
    }

    SeedProbeResultV3 decoded{};
    const ProgramValue* endpoint_value =
        reader.Find(record->fields[1]);
    const auto* endpoint_enum = endpoint_value == nullptr
        ? nullptr
        : std::get_if<EnumValue>(&endpoint_value->payload);
    if (!ReadScalar(
            reader,
            record->fields[0],
            BuiltinType::U32,
            decoded.raw_seed) ||
        endpoint_value == nullptr ||
        endpoint_value->type != EndpointType() ||
        endpoint_enum == nullptr ||
        endpoint_enum->schema != SeedProbeEndpointSchemaIdentityV2() ||
        !DecodeStop(
            reader,
            record->fields[2],
            decoded.semantic_stop) ||
        !DecodeDelivery(
            reader,
            record->fields[3],
            decoded.delivery))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe output contains a malformed seed or receipt");
        return false;
    }
    decoded.endpoint = static_cast<SeedProbeEndpointV2>(
        endpoint_enum->value);
    if (!IsValidEndpoint(decoded.endpoint) ||
        SeedProbeEndpointPc(decoded.endpoint) !=
            decoded.semantic_stop.pc)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe endpoint does not match the factual semantic stop");
        return false;
    }
    if (!StopEvidenceMatches(decoded.semantic_stop))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe semantic stop projection disagrees with physical stop evidence");
        return false;
    }
    if (decoded.semantic_stop.workset_epoch !=
        decoded.delivery.workset_epoch)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe stop and delivery receipt are not correlated");
        return false;
    }

    result = std::move(decoded);
    return true;
}

namespace {

bool DecodeSeedProbeProgramResultAgainstDefinition(
    std::span<const Byte> encoded_result,
    const ModuleIdentity& expected_module,
    const ProgramDependencyLock& expected_dependencies,
    SeedProbeResultV3& seed_probe_result,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    const auto decoded = DecodeProgramResultV1(encoded_result);
    if (!decoded)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe ProgramResultV2 is not canonical: " +
                decoded.status.message);
        return false;
    }
    const ProgramResult& result = *decoded.value;
    if (result.module != expected_module ||
        result.entrypoint != Entrypoint)
    {
        SetDiagnostic(
            diagnostic,
            "Program result does not belong to the exact SeedProbe module and entrypoint");
        return false;
    }
    if (result.resolved_dependencies != expected_dependencies)
    {
        SetDiagnostic(
            diagnostic,
            "Program result does not carry the verified SeedProbe dependency lock");
        return false;
    }
    if (result.infrastructure !=
            ProgramInfrastructureStatus::Completed ||
        result.cleanup != ProgramCleanupStatus::Clean ||
        result.session_disposition !=
            SessionDisposition::Clean ||
        !result.output || !result.domain_outcome)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe ProgramResult did not complete with clean output");
        return false;
    }

    GraphReader domain(*result.domain_outcome);
    const ProgramValue* domain_root = domain.root();
    const auto* succeeded = domain_root == nullptr
        ? nullptr
        : std::get_if<bool>(&domain_root->payload);
    if (!domain.valid() || domain_root == nullptr ||
        domain_root->type !=
            TypeRef::Builtin(BuiltinType::Bool) ||
        succeeded == nullptr || !*succeeded)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe ProgramResult domain outcome is not true");
        return false;
    }
    return DecodeSeedProbeResultV3(
        *result.output,
        seed_probe_result,
        diagnostic);
}

} // namespace

bool ValidateSeedProbeResultV3(
    const SeedProbeRequestV2& request,
    const SeedProbeResultV3& result,
    WorksetEpoch terminal_workset_epoch,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!IsValidEndpoint(result.endpoint) ||
        result.semantic_stop.pc !=
            SeedProbeEndpointPc(result.endpoint))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe observation endpoint does not match its semantic stop");
        return false;
    }
    if (result.delivery.frame != request.frame)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe delivery receipt contains a different input frame");
        return false;
    }
    if (!terminal_workset_epoch ||
        result.semantic_stop.workset_epoch !=
            terminal_workset_epoch ||
        result.delivery.workset_epoch !=
            terminal_workset_epoch)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe receipt epochs do not match the terminal workset epoch");
        return false;
    }
    if (result.delivery.delivery_id == 0 ||
        result.delivery.binding_id == 0 ||
        result.delivery.publication_id == 0 ||
        result.delivery.poll_receipt_id == 0 ||
        result.delivery.callback_count == 0 ||
        !StopEvidenceMatches(result.semantic_stop))
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe delivery was not factually observed at the semantic stop");
        return false;
    }
    return true;
}

namespace {

ProgramModule ConstructSeedProbeModuleV2()
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::string(ModuleCanonicalId),
            .revision = ModuleRevision,
        },
    };
    ModuleFragmentBuilder builder(
        module,
        "SavorCore/SeedProbeModule",
        "soa.seed_probe/probe");

    const TypeRef frame_type = FramePayloadType();
    const TypeRef endpoint_type = EndpointType();
    const TypeRef request_type = RequestType();
    const TypeRef result_type = ResultType();
    const TypeRef stop_type = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    const TypeRef delivery_type = CanonicalActionOutputType(
        CanonicalAction::InputCompleteDelivery);

    builder.AddLocalType({
        .identity = SeedProbeEndpointSchemaIdentityV2(),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {
                "AfterRandSeedSet",
                static_cast<std::int64_t>(
                    SeedProbeEndpointV2::AfterRandSeedSet),
            },
            {
                "RandSeedCommitted",
                static_cast<std::int64_t>(
                    SeedProbeEndpointV2::RandSeedCommitted),
            },
        },
    });
    builder.AddLocalType({
        .identity = SeedProbeRequestSchemaIdentityV2(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"frame", frame_type},
        },
    });
    builder.AddLocalType({
        .identity = SeedProbeResultSchemaIdentityV3(),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"raw_seed", TypeRef::Builtin(BuiltinType::U32)},
            {"endpoint", endpoint_type},
            {"semantic_stop", stop_type},
            {"delivery", delivery_type},
        },
    });

    builder.AddTypeImport(frame_type);
    for (const CanonicalAction action : {
             CanonicalAction::ExecutionObservePausedPc,
             CanonicalAction::InputAcquireLease,
             CanonicalAction::InputBeginDelivery,
             CanonicalAction::ExecutionContinueUntil,
             CanonicalAction::InputCompleteDelivery,
             CanonicalAction::GuestReadU32,
         })
    {
        AddCanonicalAction(builder, action);
    }
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddCapabilityImport(
        program::capabilities::FieldPackIdentity());

    const ValueDefinition request_argument =
        builder.NewArgument(request_type);
    auto& function = builder.AddFunction(
        "probe",
        std::array{request_argument},
        result_type,
        TypeRef::Builtin(BuiltinType::Bool),
        true);
    function.blocks.reserve(16);
    auto& entry = builder.AddBlock(function);

    const ProgramValueId frame = Required(
        builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::RecordProject,
            frame_type,
            std::array{request_argument.id},
            {},
            "frame",
            std::nullopt,
            {}),
        "request frame projection");
    const ProgramScopeId scope{};
    const ProgramValueId observe_request = ConstructRequest(
        builder,
        function,
        entry,
        CanonicalAction::ExecutionObservePausedPc,
        std::span<const ProgramValueId>{},
        "entry/observe-paused-pc/request",
        scope);
    const ProgramValueId observed_entry = Required(
        builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::AwaitAction,
            CanonicalActionOutputType(
                CanonicalAction::ExecutionObservePausedPc),
            std::array{observe_request},
            ActionTarget(CanonicalActionIdentity(
                CanonicalAction::ExecutionObservePausedPc)),
            "entry/observe-paused-pc",
            std::nullopt,
            scope),
        "paused entry-PC observation");
    const ProgramValueId entry_pc = Required(
        builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::RecordProject,
            TypeRef::Builtin(BuiltinType::U32),
            std::array{observed_entry},
            {},
            "pc",
            std::nullopt,
            scope),
        "entry PC projection");
    const ProgramValueId prebattle_entry_pc = ConstantU32(
        builder,
        function,
        entry,
        kEntryRoutes[0].entry_pc,
        "entry/prebattle-pc",
        scope);
    const ProgramValueId is_prebattle = Required(
        builder.AddInstruction(
            function,
            entry,
            InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{entry_pc, prebattle_entry_pc},
            {},
            "entry/is-prebattle",
            std::nullopt,
            scope),
        "prebattle entry comparison");

    auto& prebattle = builder.AddBlock(function);
    auto& check_fast_field = builder.AddBlock(function);
    auto& field_return = builder.AddBlock(function);
    auto& check_deferred_field = builder.AddBlock(function);
    auto& invalid_entry = builder.AddBlock(function);
    const ValueDefinition completed_result =
        builder.NewArgument(result_type);
    auto& complete = builder.AddBlock(
        function,
        std::array{completed_result});

    builder.SetTerminator(
        function,
        entry,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = is_prebattle,
            .edges = {
                {.target = prebattle.id},
                {.target = check_fast_field.id},
            },
        },
        "entry/dispatch-prebattle");

    const ProgramValueId fast_field_pc = ConstantU32(
        builder,
        function,
        check_fast_field,
        kEntryRoutes[1].entry_pc,
        "entry/fast-field-pc",
        scope);
    const ProgramValueId is_fast_field = Required(
        builder.AddInstruction(
            function,
            check_fast_field,
            InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{entry_pc, fast_field_pc},
            {},
            "entry/is-fast-field",
            std::nullopt,
            scope),
        "fast field entry comparison");
    builder.SetTerminator(
        function,
        check_fast_field,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = is_fast_field,
            .edges = {
                {.target = field_return.id},
                {.target = check_deferred_field.id},
            },
        },
        "entry/dispatch-fast-field");

    const ProgramValueId deferred_field_pc = ConstantU32(
        builder,
        function,
        check_deferred_field,
        kEntryRoutes[2].entry_pc,
        "entry/deferred-field-pc",
        scope);
    const ProgramValueId is_deferred_field = Required(
        builder.AddInstruction(
            function,
            check_deferred_field,
            InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{entry_pc, deferred_field_pc},
            {},
            "entry/is-deferred-field",
            std::nullopt,
            scope),
        "deferred field entry comparison");
    builder.SetTerminator(
        function,
        check_deferred_field,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = is_deferred_field,
            .edges = {
                {.target = field_return.id},
                {.target = invalid_entry.id},
            },
        },
        "entry/dispatch-deferred-field");
    builder.SetTerminator(
        function,
        invalid_entry,
        Terminator{
            .kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{
                "seedprobe_entry_pc_unsupported",
                "SeedProbe restored at an unsupported paused entry PC"},
        },
        "entry/unsupported");

    const CapabilityPackIdentity field =
        program::capabilities::FieldPackIdentity();
    const std::array<composition::SemanticPointReference, 1>
        prebattle_points{{{
            .capability_pack = field,
            .canonical_id = std::string(kPreBattlePointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = PreBattleAfterRandSeedSetPc,
        }}};
    const auto prebattle_delivery =
        composition::LowerSynchronizedFrameDelivery(
            builder,
            function,
            prebattle,
            frame,
            prebattle_points,
            "delivery/prebattle");
    if (!prebattle_delivery)
        throw std::logic_error(
            "SeedProbe prebattle delivery could not be lowered");

    LowerObservation(
        builder,
        function,
        prebattle,
        complete.id,
        prebattle_delivery->receipt,
        prebattle_delivery->stop,
        scope,
        {
            .id = kPreBattlePointId,
            .pc = PreBattleAfterRandSeedSetPc,
        },
        SeedProbeEndpointV2::AfterRandSeedSet);

    const std::array<composition::SemanticPointReference, 1>
        field_return_points{{{
            .capability_pack = field,
            .canonical_id = std::string(kFieldReturnPointId),
            .kind = program::SemanticPointKind::ProgramCounter,
            .physical_pc = FieldReturnRandSeedCommittedPc,
        }}};
    const auto field_delivery =
        composition::LowerSynchronizedFrameDelivery(
            builder,
            function,
            field_return,
            frame,
            field_return_points,
            "delivery/field-return");
    if (!field_delivery)
        throw std::logic_error(
            "SeedProbe field-return delivery could not be lowered");
    LowerObservation(
        builder,
        function,
        field_return,
        complete.id,
        field_delivery->receipt,
        field_delivery->stop,
        scope,
        {
            .id = kFieldReturnPointId,
            .pc = FieldReturnRandSeedCommittedPc,
        },
        SeedProbeEndpointV2::RandSeedCommitted);

    const ProgramValueId succeeded = Required(
        builder.AddInstruction(
            function,
            complete,
            InstructionOpcode::Constant,
            TypeRef::Builtin(BuiltinType::Bool),
            {},
            {},
            "domain/succeeded",
            LiteralValue{
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            scope),
        "domain outcome");
    builder.SetTerminator(
        function,
        complete,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = completed_result.id,
            .domain_outcome = succeeded,
        },
        "return/factual-observation");

    module.accepted_policies = {
        .state_policies = {
            InvocationStatePolicy::RestoreBaseline,
        },
        .execution_intents = {
            ExecutionIntent::Live,
        },
    };
    module.budgets = {
        .maximum_instructions = 256,
        .maximum_calls = 8,
        .maximum_call_depth = 4,
        .maximum_action_requests = 16,
        .maximum_emissions = 1,
        .maximum_artifacts = 1,
        .maximum_values = 512,
        .maximum_value_bytes = 512u * 1024u,
        .maximum_trace_events = 512,
    };
    module.entrypoints = {{
        .name = std::string(Entrypoint),
        .function = function.id,
        .input_type = request_type,
        .output_type = result_type,
        .domain_outcome_type =
            TypeRef::Builtin(BuiltinType::Bool),
        .required_capability_packs =
            module.required_capability_packs,
        .accepted_policies = module.accepted_policies,
    }};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

std::optional<ProgramDependencyLock> VerifySeedProbeModuleV2(
    const ProgramModule& module,
    std::string* diagnostic)
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const RegistryResult source_registered =
        program::capabilities::RegisterSourceCapabilityPacks(
            schemas,
            actions,
            packs);
    if (!source_registered.success)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe source capability registration failed: " +
                source_registered.error.message);
        return std::nullopt;
    }

    const auto stored = modules.RegisterCompiled(module);
    if (!stored.success)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe module registration failed: " +
                stored.error.message);
        return std::nullopt;
    }
    ProgramVerifier verifier(modules, schemas, actions, packs);
    const ProgramVerificationResult verified = verifier.Verify(
        stored.module->identity,
        program::capabilities::SupportedSoaUsaCompatibility());
    if (!verified.success || !verified.verified)
    {
        SetDiagnostic(
            diagnostic,
            verified.diagnostics.empty()
                ? "SeedProbe module verification failed"
                : "SeedProbe module verification failed: " +
                    verified.diagnostics.front().message);
        return std::nullopt;
    }
    return verified.verified->dependency_lock;
}

std::optional<ProgramInvocation> ResolveSeedProbeExecutionV2(
    const SeedProbeRequestV2& request,
    InvocationId invocation_id,
    AttemptId attempt_id,
    const ModuleIdentity& module_identity,
    const ProgramDependencyLock& dependencies,
    const RuntimeProfile& runtime_profile,
    const InvocationExecutionPolicy& execution,
    const ProgramBudgets& limits,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    if (!invocation_id || !attempt_id)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe execution and attempt identities must be nonzero");
        return std::nullopt;
    }
    const ProgramValueGraph input =
        EncodeSeedProbeRequestV2(request);
    if (!input.root || input.values.empty())
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe request could not be encoded");
        return std::nullopt;
    }
    return ProgramInvocation{
        .invocation_id = invocation_id,
        .attempt_id = attempt_id,
        .module = module_identity,
        .entrypoint = std::string(Entrypoint),
        .dependencies = dependencies,
        .runtime_profile = runtime_profile,
        .state = {
            .policy = InvocationStatePolicy::RestoreBaseline,
            .session_lineage = std::string(BaselineLineage),
        },
        .execution = execution,
        .input = input,
        .limits = limits,
        .provenance = {
            .requesting_component = "SavorDb.PK_SeedProbe",
            .attributes = {{
                "contract",
                "soa.seed_probe/probe@2",
            }},
        },
    };
}

std::optional<std::string> ComputeSeedProbeInvocationCompatibilityV2(
    const ModuleIdentity& module_identity,
    const ProgramDependencyLock& dependencies,
    const RuntimeProfile& runtime_profile,
    const InvocationExecutionPolicy& execution,
    const ProgramBudgets& limits,
    std::string* diagnostic)
{
    const auto invocation = ResolveSeedProbeExecutionV2(
        SeedProbeRequestV2{},
        InvocationId(1),
        AttemptId(1),
        module_identity,
        dependencies,
        runtime_profile,
        execution,
        limits,
        diagnostic);
    if (!invocation)
        return std::nullopt;
    const std::string hash =
        ComputeProgramInvocationCompatibilityHashV1(
            *invocation);
    if (hash.size() != 64)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe invocation compatibility hash is incomplete");
        return std::nullopt;
    }
    return hash;
}

std::optional<EncodedModuleEnvelope> EncodeSeedProbeModuleEnvelopeV2(
    const ProgramModule& module,
    std::string* diagnostic)
{
    SetDiagnostic(diagnostic, {});
    const EncodeResult encoded = EncodeProgramModuleV1(module);
    if (!encoded)
    {
        SetDiagnostic(
            diagnostic,
            "SeedProbe module encoding failed: " +
                encoded.status.message);
        return std::nullopt;
    }
    return EncodedModuleEnvelope{
        .identity = {
            .canonical_id = module.identity.canonical_id,
            .revision = module.identity.revision,
            .canonical_hash =
                module.identity.module_hash.ToHex(),
        },
        .format_version = kProgramCodecVersionV1,
        .development_only = false,
        .payload = encoded.bytes,
    };
}

std::string FrameKey(const GCInputFrame& frame)
{
    std::string key(sizeof(frame), '\0');
    std::memcpy(key.data(), &frame, sizeof(frame));
    return key;
}

class SeedProbeFullPhaseDefinition final
    : public ISeedProbeFullPhaseDefinitionV2
{
public:
    SeedProbeFullPhaseDefinition()
    {
        std::string diagnostic;
        ProgramModule module = ConstructSeedProbeModuleV2();
        const auto dependencies = VerifySeedProbeModuleV2(
            module,
            &diagnostic);
        if (!dependencies)
        {
            throw std::logic_error(
                "SeedProbe Full Phase definition is invalid: " +
                diagnostic);
        }
        auto module_envelope = EncodeSeedProbeModuleEnvelopeV2(
            module,
            &diagnostic);
        if (!module_envelope)
        {
            throw std::logic_error(
                "SeedProbe Full Phase definition is invalid: " +
                diagnostic);
        }
        const RuntimeProfile runtime_profile =
            InvocationRuntimeProfile(*dependencies);
        const ContentHash256 dependency_lock_hash =
            ComputeProgramDependencyLockHashV1(*dependencies);
        if (dependency_lock_hash.empty())
        {
            throw std::logic_error(
                "SeedProbe Full Phase dependency-lock hash could not be computed");
        }
        const InvocationExecutionPolicy execution{
            .intent = ExecutionIntent::Live,
            .allow_movie_playback = false,
            .allow_movie_recording = false,
            .allow_input = true,
            .allow_capture = false,
            .record_trace = false,
        };
        const auto compatibility =
            ComputeSeedProbeInvocationCompatibilityV2(
                module.identity,
                *dependencies,
                runtime_profile,
                execution,
                module.budgets,
                &diagnostic);
        if (!compatibility)
        {
            throw std::logic_error(
                "SeedProbe Full Phase definition is invalid: " +
                diagnostic);
        }

        module_identity_ = module.identity;
        dependency_lock_ = *dependencies;
        runtime_profile_ = runtime_profile;
        module_envelope_ = std::move(*module_envelope);

        runtime_ = {
            .module = module_envelope_.identity,
            .entrypoint = std::string(Entrypoint),
            .dependency_lock_sha256 = dependency_lock_hash.ToHex(),
            .verified_dependency_sha256 = *compatibility,
            .runtime_profile_sha256 =
                RuntimeProfileHash(runtime_profile_),
            .state_policy = InvocationStatePolicy::RestoreBaseline,
            .execution = execution,
            .limits = module.budgets,
            .baseline_lineage = std::string(BaselineLineage),
            .movie_policy_sha256 = []
            {
                constexpr std::string_view value =
                    "soa.seed_probe/no-movie/v2";
                return hash::sha256(value.data(), value.size());
            }(),
            .service_policy_sha256 = []
            {
                constexpr std::string_view value =
                    "soa.seed_probe/input-stop-memory/v2";
                return hash::sha256(value.data(), value.size());
            }(),
        };

        std::string canonical =
            "savor.full_phase/definition/v1";
        canonical.push_back('\0');
        canonical.append("soa.seed_probe");
        canonical.push_back('\0');
        canonical.append(std::to_string(savor::PK_SeedProbe));
        canonical.push_back('\0');
        canonical.append(std::to_string(ProgramVersion));
        canonical.push_back('\0');
        canonical.append(runtime_.module.canonical_hash);
        canonical.push_back('\0');
        canonical.append(runtime_.entrypoint);
        canonical.push_back('\0');
        canonical.append(runtime_.dependency_lock_sha256);
        canonical.push_back('\0');
        canonical.append(runtime_.verified_dependency_sha256);
        canonical.push_back('\0');
        canonical.append(runtime_.runtime_profile_sha256);
        canonical.push_back('\0');
        canonical.append(runtime_.baseline_lineage);
        canonical.push_back('\0');
        canonical.append(runtime_.movie_policy_sha256);
        canonical.push_back('\0');
        canonical.append(runtime_.service_policy_sha256);
        canonical.push_back('\0');
        canonical.append("soa.seed_probe/survey-jct-planning/v2");
        identity_ = {
            .program_kind = static_cast<std::int32_t>(
                savor::PK_SeedProbe),
            .program_version = ProgramVersion,
            .canonical_id = "savor.full_phase.seed_probe",
            .contract_revision = 2,
            .canonical_sha256 = hash::sha256(
                canonical.data(), canonical.size()),
        };
    }

    const fullphase::FullPhaseProgramIdentity& identity()
        const noexcept override
    {
        return identity_;
    }

    const fullphase::FullPhaseRuntimeContract& runtime_contract()
        const noexcept override
    {
        return runtime_;
    }

    const EncodedModuleEnvelope& module_envelope()
        const noexcept override
    {
        return module_envelope_;
    }

    std::optional<ProgramInvocation> BuildResolvedExecution(
        std::span<const std::uint8_t> input_payload,
        ProgramExecutionId execution_id,
        AttemptId attempt_id,
        std::string* diagnostic) const override
    {
        SeedProbeRequestV2 request{};
        if (!DecodeSeedProbeExecutionInputV2(
                input_payload, request, diagnostic))
        {
            return std::nullopt;
        }
        return ResolveSeedProbeExecutionV2(
            request,
            execution_id,
            attempt_id,
            module_identity_,
            dependency_lock_,
            runtime_profile_,
            runtime_.execution,
            runtime_.limits,
            diagnostic);
    }

    bool DecodeProgramResult(
        std::span<const Byte> encoded_result,
        SeedProbeResultV3& result,
        std::string* diagnostic) const override
    {
        return DecodeSeedProbeProgramResultAgainstDefinition(
            encoded_result,
            module_identity_,
            dependency_lock_,
            result,
            diagnostic);
    }

    std::vector<GCInputFrame> PlanSurvey(
        const SeedProbeSurveyPlanSettingsV2& settings) const override
    {
        if (settings.samples_per_axis <= 0 ||
            settings.min_value < 0 || settings.min_value > 255 ||
            settings.max_value < 0 || settings.max_value > 255 ||
            settings.min_value > settings.max_value)
        {
            return {};
        }

        std::vector<GCInputFrame> frames;
        std::unordered_set<std::string> seen;
        const auto append = [&](GCInputFrame frame)
        {
            frame.buttons = 0;
            if (seen.insert(FrameKey(frame)).second)
            {
                frames.push_back(frame);
            }
        };

        append(GCInputFrame{});
        for (auto frame : build_grid_main(
                 settings.samples_per_axis,
                 settings.min_value,
                 settings.max_value))
        {
            frame.c_x = 128;
            frame.c_y = 128;
            frame.trig_l = 0;
            frame.trig_r = 0;
            append(frame);
        }
        for (auto frame : build_grid_cstick(
                 settings.samples_per_axis,
                 settings.min_value,
                 settings.max_value))
        {
            frame.main_x = 128;
            frame.main_y = 128;
            frame.trig_l = 0;
            frame.trig_r = 0;
            append(frame);
        }
        const int trigger_min = settings.ignore_trigger_minmax
            ? 0
            : settings.min_value;
        const int trigger_max = settings.ignore_trigger_minmax
            ? 255
            : settings.max_value;
        for (auto frame : build_grid_trig(
                 settings.samples_per_axis,
                 trigger_min,
                 trigger_max,
                 settings.cap_trigger_top))
        {
            frame.main_x = 128;
            frame.main_y = 128;
            frame.c_x = 128;
            frame.c_y = 128;
            append(frame);
        }
        return frames;
    }

    JCTComboSamples PlanSearch(
        const RandSeedProbeResult& survey,
        std::uint32_t attempts_per_target,
        std::uint32_t sampler_tries) const override
    {
        return PlanJCTComboSamples(
            survey, attempts_per_target, sampler_tries);
    }

private:
    fullphase::FullPhaseProgramIdentity identity_;
    fullphase::FullPhaseRuntimeContract runtime_;
    EncodedModuleEnvelope module_envelope_;
    ModuleIdentity module_identity_;
    ProgramDependencyLock dependency_lock_;
    RuntimeProfile runtime_profile_;
};

} // namespace

std::shared_ptr<const ISeedProbeFullPhaseDefinitionV2>
SeedProbeFullPhaseDefinitionV2()
{
    static const auto definition =
        std::make_shared<const SeedProbeFullPhaseDefinition>();
    return definition;
}

} // namespace savor::runtime::seedprobe
