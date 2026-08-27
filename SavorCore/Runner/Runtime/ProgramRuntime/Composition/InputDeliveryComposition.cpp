#include "InputDeliveryComposition.h"

#include "Runner/Runtime/Execution/ExecutionTypes.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace savor::runtime::program::composition {
namespace {

class Writer final
{
public:
    explicit Writer(std::array<char, 4> magic)
    {
        for (char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }
    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<Byte>(value >> shift));
    }
    void Text(std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("input-delivery text is too large");
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& value)
    {
        bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
    }
    std::vector<Byte> Finish() && { return std::move(bytes_); }

private:
    std::vector<Byte> bytes_;
};

InstructionTarget Action(CanonicalAction action)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = CanonicalActionIdentity(action),
    };
}

std::optional<ProgramValueId> ConstantBytes(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::vector<Byte> bytes,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = CanonicalRuntimeType(schema);
    return builder.AddInstruction(
        function, block, InstructionOpcode::Constant, type, {}, {},
        std::move(selector), LiteralValue{.type = type, .payload = std::move(bytes)},
        scope);
}

std::optional<ProgramValueId> Request(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    std::span<const ProgramValueId> fields,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(action), fields, {}, std::move(selector),
        std::nullopt, scope);
}

std::optional<ProgramValueId> Await(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    ProgramValueId request,
    std::string selector,
    ProgramScopeId scope = {})
{
    return builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(action), std::array{request}, Action(action),
        std::move(selector), std::nullopt, scope);
}

std::optional<ProgramValueId> OptionalBinding(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId binding,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function, block, InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(CanonicalRuntimeSchema::OptionalInputExecutionBinding),
        std::array{binding}, {}, std::move(selector), std::nullopt, scope);
}

std::optional<ProgramValueId> EmptyOptional(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function, block, InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(schema), {}, {}, std::move(selector),
        std::nullopt, scope);
}

std::vector<Byte> LeaseConfig()
{
    Writer writer({'I', 'L', 'C', '2'});
    writer.U32(0);
    writer.U32(0);
    writer.Bool(true);
    writer.Bool(true);
    writer.Bool(false);
    return std::move(writer).Finish();
}

std::vector<Byte> ContinueConfig(bool fail_on_movie_end)
{
    Writer writer({'C', 'U', 'C', '2'});
    writer.U8(1);
    writer.Bool(fail_on_movie_end);
    writer.U8(static_cast<std::uint8_t>(
        ExecutionThrottlePolicy::RequireDisabled));
    writer.U8(0);
    return std::move(writer).Finish();
}

} // namespace

std::optional<InputDeliveryLoweringResult> LowerSynchronizedFrameDelivery(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId frame,
    std::span<const SemanticPointReference> endpoints,
    std::string selector,
    bool fail_on_movie_end)
{
    const ProgramScopeId scope = builder.NewScope();
    (void)builder.AddInstruction(
        function, block, InstructionOpcode::EnterScope, std::nullopt, {}, {},
        selector + "/scope", std::nullopt, scope);

    const auto lease_config = ConstantBytes(
        builder, function, block, CanonicalRuntimeSchema::InputLeaseStaticConfig,
        LeaseConfig(), selector + "/lease-config", scope);
    const auto lease_request = lease_config
        ? Request(builder, function, block, CanonicalAction::InputAcquireLease,
              std::array{*lease_config}, selector + "/lease-request", scope)
        : std::nullopt;
    const auto lease = lease_request
        ? Await(builder, function, block, CanonicalAction::InputAcquireLease,
              *lease_request, selector + "/lease", scope)
        : std::nullopt;

    const auto point_set = ConstantBytes(
        builder, function, block, CanonicalRuntimeSchema::SemanticPointSet,
        EncodeSemanticPointSetV1(endpoints), selector + "/semantic-points", scope);
    if (!lease || !point_set)
        return std::nullopt;

    const auto begin_request = Request(
        builder, function, block, CanonicalAction::InputBeginDelivery,
        std::array{*lease, frame}, selector + "/begin-request", scope);
    const auto binding = begin_request
        ? Await(builder, function, block, CanonicalAction::InputBeginDelivery,
              *begin_request, selector + "/binding")
        : std::nullopt;
    const auto optional_binding = binding
        ? OptionalBinding(builder, function, block, *binding,
              selector + "/continue-binding", scope)
        : std::nullopt;
    const auto no_movie = EmptyOptional(
        builder, function, block,
        CanonicalRuntimeSchema::OptionalMoviePlaybackSession,
        selector + "/no-movie", scope);
    const auto no_count = EmptyOptional(
        builder, function, block,
        CanonicalRuntimeSchema::OptionalMovieInputCount,
        selector + "/no-count", scope);
    const auto continue_config = ConstantBytes(
        builder, function, block,
        CanonicalRuntimeSchema::ContinueUntilStaticConfig,
        ContinueConfig(fail_on_movie_end), selector + "/continue-config", scope);
    const TypeRef occurrence_type = TypeRef::Builtin(BuiltinType::U64);
    const auto one_occurrence = builder.AddInstruction(
        function, block, InstructionOpcode::Constant, occurrence_type, {}, {},
        selector + "/one-occurrence",
        LiteralValue{occurrence_type, LiteralPayload{std::uint64_t{1}}}, scope);
    const TypeRef verify_type = TypeRef::Builtin(BuiltinType::Bool);
    const auto no_verify = builder.AddInstruction(
        function, block, InstructionOpcode::Constant, verify_type, {}, {},
        selector + "/no-bound-input-verification",
        LiteralValue{verify_type, LiteralPayload{false}}, scope);
    if (!binding || !optional_binding || !no_movie || !no_count ||
        !one_occurrence || !no_verify || !continue_config)
        return std::nullopt;
    const auto continue_request = Request(
        builder, function, block, CanonicalAction::ExecutionContinueUntil,
        std::array{*point_set, *optional_binding, *no_movie, *no_count,
            *one_occurrence, *no_verify, *continue_config},
        selector + "/continue-request", scope);
    const auto stop = continue_request
        ? Await(builder, function, block, CanonicalAction::ExecutionContinueUntil,
              *continue_request, selector + "/stop")
        : std::nullopt;
    const auto complete_request = Request(
        builder, function, block, CanonicalAction::InputCompleteDelivery,
        std::array{*lease, *binding}, selector + "/complete-request", scope);
    const auto receipt = complete_request
        ? Await(builder, function, block, CanonicalAction::InputCompleteDelivery,
              *complete_request, selector + "/receipt")
        : std::nullopt;
    if (!stop || !receipt)
        return std::nullopt;

    (void)builder.AddInstruction(
        function, block, InstructionOpcode::ExitScope, std::nullopt, {}, {},
        selector + "/release-scope", std::nullopt, scope);
    return InputDeliveryLoweringResult{*stop, *receipt};
}

} // namespace savor::runtime::program::composition
