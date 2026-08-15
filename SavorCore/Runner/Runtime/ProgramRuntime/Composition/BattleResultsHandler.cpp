#include "BattleResultsHandler.h"

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <array>
#include <stdexcept>

namespace savor::runtime::program::composition {
namespace {

using Builder = program::composition::detail::ModuleFragmentBuilder;

SemanticPointReference Point(std::string id, std::uint32_t pc)
{
    return {
        .capability_pack = capabilities::BattleResultsPackIdentity(),
        .canonical_id = std::move(id),
        .kind = SemanticPointKind::ProgramCounter,
        .physical_pc = pc,
    };
}

InteractionSegmentDefinition Neutral(
    std::string id, std::vector<SemanticPointReference> gates)
{
    return {
        .canonical_id = std::move(id),
        .gate_alternatives = std::move(gates),
        .requested_input_parameter = 1,
        .input_kind = InteractionInputKind::Neutral,
        .completion_mapper = capabilities::
            BattleResultsCompleteSegmentReducerIdentity(),
    };
}

InteractionSegmentDefinition Accept(
    std::string id,
    std::string point_id,
    std::uint32_t accepted_pc)
{
    auto segment = InteractionSegmentDefinition{
        .canonical_id = std::move(id),
        .gate_alternatives = {Point(std::move(point_id), accepted_pc)},
        .requested_input_parameter = 1,
        .input_kind = InteractionInputKind::Held,
        .post_release_gate = Point(
            "soa.battle.results.point.ControllerNeutralCopied", 0x801c7948u),
        .completion_mapper = capabilities::
            BattleResultsCompleteSegmentReducerIdentity(),
    };
    return segment;
}

void AddAction(Builder& builder, CanonicalAction action)
{
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(CanonicalActionIdentity(action));
    for (const auto& schema : CanonicalActionTypeSchemaClosure(action))
        builder.AddTypeImport(schema);
}

InstructionTarget Action(CanonicalAction action)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = CanonicalActionIdentity(action),
    };
}

ProgramValueId Need(std::optional<ProgramValueId> value, std::string_view what)
{
    if (!value)
        throw std::logic_error("Battle Results handler lowering failed: " +
            std::string(what));
    return *value;
}

ProgramValueId Constant(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    TypeRef type, LiteralPayload payload, std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::Constant, type, {}, {},
        std::move(selector), LiteralValue{type, std::move(payload)}),
        "constant");
}

ProgramValueId Project(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId value, TypeRef type, std::string field)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordProject, type,
        std::array{value}, {}, std::move(field)), "record projection");
}

std::vector<Byte> ObservationConfig(std::string_view id)
{
    std::vector<Byte> bytes{'O','S','C','1'};
    const auto size = static_cast<std::uint32_t>(id.size());
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<Byte>(size >> shift));
    bytes.insert(bytes.end(), id.begin(), id.end());
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

ProgramValueId RequirePc(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    std::uint32_t pc, std::string selector)
{
    const auto expected = Constant(
        builder, function, block, TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(pc), selector + "/expected");
    const auto request = Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(CanonicalAction::ExecutionRequirePausedPc),
        std::array{expected}, {}, selector + "/request"), "PC request");
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        CanonicalActionOutputType(CanonicalAction::ExecutionRequirePausedPc),
        std::array{request}, Action(CanonicalAction::ExecutionRequirePausedPc),
        selector + "/require"), "PC qualification");
}

ProgramValueId ReadU32(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    std::uint32_t address, std::string selector)
{
    const auto no_stop = Need(builder.AddInstruction(
        function, block, InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(CanonicalRuntimeSchema::OptionalContinueUntilResult),
        {}, {}, selector + "/no-stop"), "optional stop");
    const auto location = Constant(
        builder, function, block, TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(address), selector + "/address");
    const auto config = Constant(
        builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ObservationStaticConfig),
        ObservationConfig(selector), selector + "/config");
    const auto request = Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(CanonicalAction::GuestReadU32),
        std::array{no_stop, location, config}, {}, selector + "/request"),
        "read request");
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        TypeRef::Builtin(BuiltinType::U32), std::array{request},
        Action(CanonicalAction::GuestReadU32), selector + "/read"),
        "guest read");
}

ProgramValueId ReadU8AsU32(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    std::uint32_t address, std::string selector)
{
    const auto no_stop = Need(builder.AddInstruction(
        function, block, InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(CanonicalRuntimeSchema::OptionalContinueUntilResult),
        {}, {}, selector + "/no-stop"), "optional stop");
    const auto location = Constant(
        builder, function, block, TypeRef::Builtin(BuiltinType::U64),
        static_cast<std::uint64_t>(address), selector + "/address");
    const auto config = Constant(
        builder, function, block,
        CanonicalRuntimeType(CanonicalRuntimeSchema::ObservationStaticConfig),
        ObservationConfig(selector), selector + "/config");
    const auto request = Need(builder.AddInstruction(
        function, block, InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(CanonicalAction::GuestReadU8),
        std::array{no_stop, location, config}, {}, selector + "/request"),
        "read request");
    const auto byte = Need(builder.AddInstruction(
        function, block, InstructionOpcode::AwaitAction,
        TypeRef::Builtin(BuiltinType::U8), std::array{request},
        Action(CanonicalAction::GuestReadU8), selector + "/read"),
        "guest read");
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::CheckedConvert,
        TypeRef::Builtin(BuiltinType::U32), std::array{byte}, {},
        selector + "/widen"), "u8 widening");
}

ProgramValueId Equal(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId lhs, ProgramValueId rhs, std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::Equal,
        TypeRef::Builtin(BuiltinType::Bool), std::array{lhs, rhs}, {},
        std::move(selector)), "invariant comparison");
}

ProgramValueId And(
    Builder& builder, ProgramFunction& function, BasicBlock& block,
    ProgramValueId lhs, ProgramValueId rhs, std::string selector)
{
    return Need(builder.AddInstruction(
        function, block, InstructionOpcode::BooleanAnd,
        TypeRef::Builtin(BuiltinType::Bool), std::array{lhs, rhs}, {},
        std::move(selector)), "invariant conjunction");
}

} // namespace

bool DecodeBattleResultsHandlerReceiptV1(
    std::span<const std::uint8_t> bytes,
    BattleResultsHandlerReceiptV1& output,
    std::string* diagnostic)
{
    constexpr std::size_t kReceiptBytes = 125;
    if (bytes.size() != kReceiptBytes || bytes[0] != 'B' ||
        bytes[1] != 'R' || bytes[2] != 'R' || bytes[3] != '1' ||
        bytes[4] != 1 || bytes[5] != 0 || bytes[6] != 0 ||
        bytes[7] != 0 || bytes[80] != 13)
    {
        if (diagnostic) *diagnostic =
            "Battle Results handler receipt has an invalid BRR1 envelope";
        return false;
    }
    std::size_t offset = 8;
    const auto take32 = [&]()
    {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
        return value;
    };
    const auto take64 = [&]()
    {
        const auto low = take32();
        const auto high = take32();
        return static_cast<std::uint64_t>(low) |
            (static_cast<std::uint64_t>(high) << 32u);
    };
    BattleResultsHandlerReceiptV1 decoded{};
    decoded.expected.gold_pages = take32();
    decoded.expected.normal_exp_pages = take32();
    decoded.expected.level_panels = take32();
    decoded.expected.stat_waves = take32();
    decoded.expected.magic_exp_pages = take32();
    decoded.expected.magic_rank_events = take32();
    decoded.expected.learned_magic_waves = take32();
    decoded.expected.item_popups = take32();
    for (auto& count : decoded.observed_segment_counts) count = take32();
    ++offset; // terminal interaction segment
    decoded.terminal.pc = take32();
    decoded.terminal.vi_count = take64();
    decoded.terminal.workset_epoch = take64();
    decoded.entry_rng = take32();
    decoded.exit_rng = take32();
    const auto lifecycle = take32();
    const auto completed = take32();
    const auto result_pointer = take32();
    const auto game_mode = take32();
    const auto& expected = decoded.expected;
    const auto& observed = decoded.observed_segment_counts;
    const auto expected_magic_entries =
        expected.magic_rank_events == 0 ? 0u : 1u;
    const auto expected_stat_waves =
        expected.level_panels == 0 ? 0u : 3u;
    if (offset != bytes.size() || decoded.terminal.pc != 0x800e3694u ||
        decoded.terminal.workset_epoch == 0 ||
        decoded.entry_rng != decoded.exit_rng || lifecycle != 0xffu ||
        completed != 1u || result_pointer != 0u || game_mode != 6u ||
        expected.stat_waves != expected_stat_waves ||
        observed[1] != expected.gold_pages ||
        observed[2] != expected.normal_exp_pages ||
        observed[3] != expected.stat_waves ||
        observed[4] != expected_magic_entries ||
        observed[5] != expected.magic_exp_pages ||
        observed[6] != expected.learned_magic_waves ||
        observed[7] != expected.item_popups)
    {
        if (diagnostic) *diagnostic =
            "Battle Results handler receipt has invalid presentation or terminal provenance";
        return false;
    }
    decoded.observed.gold_pages = observed[1];
    decoded.observed.normal_exp_pages = observed[2];
    // The Results screen exposes three stat waves when any number of level
    // panels is present and one magic-entry gate when any number of magic-rank
    // events is present. Once those gates match the manifest, retain the exact
    // manifest counts rather than fabricating them from the normalized screen
    // occurrences.
    decoded.observed.level_panels = expected.level_panels;
    decoded.observed.stat_waves = observed[3];
    decoded.observed.magic_rank_events = expected.magic_rank_events;
    decoded.observed.magic_exp_pages = observed[5];
    decoded.observed.learned_magic_waves = observed[6];
    decoded.observed.item_popups = observed[7];
    decoded.lifecycle_complete = lifecycle == 0xffu;
    decoded.completion_flag_set = completed == 1u;
    decoded.result_pointer_cleared = result_pointer == 0u;
    decoded.field_mode_restored = game_mode == 6u;
    decoded.rng_unchanged = decoded.entry_rng == decoded.exit_rng;
    output = std::move(decoded);
    if (diagnostic) diagnostic->clear();
    return true;
}

InteractionDefinition BattleResultsInteractionV1()
{
    const auto input_frame = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
    const auto continue_result = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    InteractionDefinition definition{
        .canonical_id = "soa.battle.results.handler",
        .revision = 1,
        .source_name = "BattleResultsHandler",
        .parameters = {
            {"completion_manifest", TypeRef::Named(
                 capabilities::BattleCompletionManifestSchemaIdentity())},
            {"press_a", input_frame},
        },
        .state_type = TypeRef::Named(
            capabilities::BattleResultsStateSchemaIdentity()),
        .output_type = TypeRef::Named(
            capabilities::BattleResultsReceiptSchemaIdentity()),
        .lease_type = CanonicalActionOutputType(
            CanonicalAction::InputAcquireLease),
        .point_receipt_type = continue_result,
        .input_execution_binding_type = CanonicalActionOutputType(
            CanonicalAction::InputApplyState),
        .segment_result_type = continue_result,
        .adaptive_transition_type = TypeRef::Named(
            capabilities::BattleResultsTransitionSchemaIdentity()),
        .adaptive_segment_id_type = TypeRef::Named(
            capabilities::BattleResultsSegmentSchemaIdentity()),
        .adaptive_selection = InteractionAdaptiveSelectionMap{
            .segments = {
                {0, "descriptor"}, {1, "dispatch"}, {2, "intro"},
                {3, "gold"}, {4, "normal-exp"}, {5, "stat-wave"},
                {6, "magic-entry"}, {7, "magic-exp"},
                {8, "learned-magic"}, {9, "item-popup"},
                {10, "confirm"}, {11, "fade"}, {12, "cleanup"},
            },
            .complete = 13,
        },
        .initialize_reducer = capabilities::BattleResultsInitializeReducerIdentity(),
        .advance_reducer = capabilities::BattleResultsAdvanceReducerIdentity(),
        .finalize_reducer = capabilities::BattleResultsFinalizeReducerIdentity(),
        .actions = {
            .acquire_input_lease = CanonicalActionIdentity(
                CanonicalAction::InputAcquireLease),
            .apply_input_state = CanonicalActionIdentity(
                CanonicalAction::InputApplyState),
            .continue_until = CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil),
            .step_frames = CanonicalActionIdentity(
                CanonicalAction::ExecutionStepFrames),
        },
        .segments = {
            Neutral("descriptor", {
                Point("soa.battle.results.point.DescriptorReady", 0x800e35f0u)}),
            Neutral("dispatch", {
                Point("soa.battle.results.point.Dispatch", 0x800e4660u),
                Point("soa.battle.results.point.IntroReady", 0x800e46bcu),
                Point("soa.battle.results.point.GoldReady", 0x800e488cu),
                Point("soa.battle.results.point.NormalExpReady", 0x800e4d40u),
                Point("soa.battle.results.point.StatWaveReady", 0x800e4f2cu),
                Point("soa.battle.results.point.MagicEntryReady", 0x800e52d8u),
                Point("soa.battle.results.point.MagicExpReady", 0x800e5460u),
                Point("soa.battle.results.point.LearnedMagicReady", 0x800e5c3cu),
                Point("soa.battle.results.point.ItemPopupReady", 0x800e5f80u),
                Point("soa.battle.results.point.ConfirmReady", 0x800e6128u),
                Point("soa.battle.results.point.FadeReady", 0x800e6470u),
                Point("soa.battle.results.point.LifecycleExit", 0x800e64a0u),
            }),
            Accept("intro", "soa.battle.results.point.IntroAccepted", 0x800e46ccu),
            Accept("gold", "soa.battle.results.point.GoldAccepted", 0x800e48a8u),
            Accept("normal-exp", "soa.battle.results.point.NormalExpAccepted", 0x800e4d50u),
            Accept("stat-wave", "soa.battle.results.point.StatWaveAccepted", 0x800e4f3cu),
            Accept("magic-entry", "soa.battle.results.point.MagicEntryAccepted", 0x800e52e8u),
            Accept("magic-exp", "soa.battle.results.point.MagicExpAccepted", 0x800e5470u),
            Accept("learned-magic", "soa.battle.results.point.LearnedMagicAccepted", 0x800e5c4cu),
            Accept("item-popup", "soa.battle.results.point.ItemPopupAccepted", 0x800e5f90u),
            Accept("confirm", "soa.battle.results.point.ConfirmAccepted", 0x800e6138u),
            Accept("fade", "soa.battle.results.point.FadeAccepted", 0x800e6480u),
            Neutral("cleanup", {
                Point("soa.battle.results.point.CleanupComplete", 0x800e3694u)}),
        },
        .first_segment = "descriptor",
        .budgets = {
            .maximum_instructions = 65'536,
            .maximum_calls = 1'024,
            .maximum_call_depth = 8,
            .maximum_action_requests = 8'192,
            .maximum_values = 131'072,
            .maximum_value_bytes = 32 * 1024 * 1024,
            .maximum_trace_events = 65'536,
        },
    };
    return definition;
}

CompositionResult LowerBattleResultsHandlerV1(ProgramModule& module)
{
    const auto interaction = LowerInteraction(BattleResultsInteractionV1(), module);
    if (!interaction || !interaction.function) return interaction;

    ProgramModule candidate = module;
    Builder builder(candidate, "BattleResultsHandler", "battle-results/handler/v1");
    for (const auto action : {
             CanonicalAction::ExecutionRequirePausedPc,
             CanonicalAction::GuestReadU8,
             CanonicalAction::GuestReadU32})
        AddAction(builder, action);
    builder.AddCapabilityImport(capabilities::BattleResultsPackIdentity());
    builder.AddReducerImport(capabilities::BattleResultsAttachInvariantsReducerIdentity());
    builder.AddTypeImport(capabilities::BattleResultsHandlerInputSchemaIdentity());
    builder.AddTypeImport(capabilities::BattleCompletionManifestSchemaIdentity());
    builder.AddTypeImport(capabilities::BattleResultsReceiptSchemaIdentity());
    builder.AddTypeImport(CanonicalRuntimeSchemaIdentity(
        CanonicalRuntimeSchema::InputFramePayload));

    const auto handler_input = builder.NewArgument(TypeRef::Named(
        capabilities::BattleResultsHandlerInputSchemaIdentity()));
    const auto press_a = builder.NewArgument(CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload));
    auto& function = builder.AddFunction(
        "interact.soa.battle.results.builtin",
        std::array{handler_input, press_a},
        TypeRef::Named(capabilities::BattleResultsReceiptSchemaIdentity()));
    function.blocks.reserve(5);
    auto& entry = builder.AddBlock(function);
    const auto manifest = Project(
        builder, function, entry, handler_input.id,
        TypeRef::Named(capabilities::BattleCompletionManifestSchemaIdentity()),
        "completion");
    const auto bound_postseed = Project(
        builder, function, entry, handler_input.id,
        CanonicalActionOutputType(CanonicalAction::ExecutionRequirePausedPc),
        "postseed_entry");
    const auto actual_postseed = RequirePc(
        builder, function, entry, 0x801012b4u, "entry/postseed");
    const auto bound_pc = Project(
        builder, function, entry, bound_postseed,
        TypeRef::Builtin(BuiltinType::U32), "pc");
    const auto bound_vi = Project(
        builder, function, entry, bound_postseed,
        TypeRef::Builtin(BuiltinType::U64), "vi_count");
    const auto bound_epoch = Project(
        builder, function, entry, bound_postseed,
        TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
    const auto actual_pc = Project(
        builder, function, entry, actual_postseed,
        TypeRef::Builtin(BuiltinType::U32), "pc");
    const auto actual_vi = Project(
        builder, function, entry, actual_postseed,
        TypeRef::Builtin(BuiltinType::U64), "vi_count");
    const auto actual_epoch = Project(
        builder, function, entry, actual_postseed,
        TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
    const auto expected_postseed_pc = Constant(
        builder, function, entry, TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t{0x801012b4u}, "entry/expected-postseed-pc");
    auto postseed_matches = Equal(
        builder, function, entry, bound_pc, expected_postseed_pc,
        "entry/bound-pc-is-postseed");
    postseed_matches = And(
        builder, function, entry, postseed_matches,
        Equal(builder, function, entry, bound_pc, actual_pc,
              "entry/correlate-pc"),
        "entry/and-pc");
    postseed_matches = And(
        builder, function, entry, postseed_matches,
        Equal(builder, function, entry, bound_vi, actual_vi,
              "entry/correlate-vi"),
        "entry/and-vi");
    postseed_matches = And(
        builder, function, entry, postseed_matches,
        Equal(builder, function, entry, bound_epoch, actual_epoch,
              "entry/correlate-epoch"),
        "entry/and-epoch");

    auto& correlated = builder.AddBlock(function);
    auto& invalid_entry = builder.AddBlock(function);
    builder.SetTerminator(function, entry, {
        .kind = TerminatorKind::ConditionalBranch,
        .condition_or_selector = postseed_matches,
        .edges = {{.target = correlated.id}, {.target = invalid_entry.id}},
    }, "entry/validate-postseed-provenance");
    builder.SetTerminator(function, invalid_entry, {
        .kind = TerminatorKind::StructuredFail,
        .failure = StructuredFailure{
            "battle_results_postseed_provenance_mismatch",
            "Battle Results postseed provenance did not match the current paused execution point"},
    }, "entry/fail-provenance");

    const auto entry_rng = ReadU32(
        builder, function, correlated, 0x803469a8u, "entry/rng");
    const auto presentation_receipt = Need(builder.AddInstruction(
        function, correlated, InstructionOpcode::CallLocal,
        TypeRef::Named(capabilities::BattleResultsReceiptSchemaIdentity()),
        std::array{manifest, press_a.id},
        {
            .kind = InstructionTargetKind::LocalFunction,
            .local_function = *interaction.function,
        },
        "results/adaptive-presentation"), "Results interaction");
    (void)RequirePc(builder, function, correlated, 0x800e3694u, "terminal/cleanup");
    const auto exit_rng = ReadU32(
        builder, function, correlated, 0x803469a8u, "terminal/rng");
    const auto lifecycle = ReadU8AsU32(
        builder, function, correlated, 0x80346dd0u, "terminal/lifecycle");
    const auto completed = ReadU32(
        builder, function, correlated, 0x80346dd4u, "terminal/completion-flag");
    const auto result_pointer = ReadU32(
        builder, function, correlated, 0x80346dccu, "terminal/result-pointer");
    const auto game_mode = ReadU32(
        builder, function, correlated, 0x803475ccu, "terminal/game-mode");
    const auto expected_ff = Constant(
        builder, function, correlated, TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t{0xff}, "terminal/expected-lifecycle");
    const auto expected_one = Constant(
        builder, function, correlated, TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t{1}, "terminal/expected-completion");
    const auto expected_zero = Constant(
        builder, function, correlated, TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t{0}, "terminal/expected-null");
    const auto expected_mode = Constant(
        builder, function, correlated, TypeRef::Builtin(BuiltinType::U32),
        std::uint32_t{6}, "terminal/expected-mode");
    auto invariants = Equal(
        builder, function, correlated, entry_rng, exit_rng, "terminal/rng-unchanged");
    invariants = And(builder, function, correlated, invariants,
        Equal(builder, function, correlated, lifecycle, expected_ff,
              "terminal/lifecycle-complete"), "terminal/and-lifecycle");
    invariants = And(builder, function, correlated, invariants,
        Equal(builder, function, correlated, completed, expected_one,
              "terminal/completion-set"), "terminal/and-completion");
    invariants = And(builder, function, correlated, invariants,
        Equal(builder, function, correlated, result_pointer, expected_zero,
              "terminal/pointer-cleared"), "terminal/and-pointer");
    invariants = And(builder, function, correlated, invariants,
        Equal(builder, function, correlated, game_mode, expected_mode,
              "terminal/mode-field"), "terminal/and-mode");

    auto& valid = builder.AddBlock(function);
    auto& invalid = builder.AddBlock(function);
    builder.SetTerminator(function, correlated, {
        .kind = TerminatorKind::ConditionalBranch,
        .condition_or_selector = invariants,
        .edges = {{.target = valid.id}, {.target = invalid.id}},
    }, "terminal/validate-invariants");
    const auto receipt = Need(builder.AddInstruction(
        function, valid, InstructionOpcode::CallReducer,
        TypeRef::Named(capabilities::BattleResultsReceiptSchemaIdentity()),
        std::array{presentation_receipt, entry_rng, exit_rng, lifecycle,
                   completed, result_pointer, game_mode},
        {
            .kind = InstructionTargetKind::Reducer,
            .dependency = capabilities::BattleResultsAttachInvariantsReducerIdentity(),
        },
        "terminal/typed-receipt"), "typed Results receipt");
    builder.SetTerminator(function, valid, {
        .kind = TerminatorKind::Return,
        .return_value = receipt,
    }, "terminal/return");
    builder.SetTerminator(function, invalid, {
        .kind = TerminatorKind::StructuredFail,
        .failure = StructuredFailure{
            "battle_results_terminal_invariant_failed",
            "Battle Results cleanup state or RNG invariants failed"},
    }, "terminal/fail");
    candidate.identity.module_hash = {};
    module = std::move(candidate);
    return {.ok = true, .function = function.id};
}

} // namespace savor::runtime::program::composition
