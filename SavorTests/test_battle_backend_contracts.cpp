#include "gtest/gtest.h"

#include "../SavorCore/Core/Input/SoaBattle/BattlePlanValidation.h"
#include "../SavorCore/Core/Input/SoaBattle/BattleCommandCodec.h"
#include "../SavorCore/Phases/Programs/BattleSingleTurn/BattleSingleTurnModule.h"
#include "../SavorCore/Runner/Runtime/Predicates/PredicateExecution.h"
#include "../SavorCore/Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "../SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnProgram.h"
#include "../SavorDb/Execution/ProgramDB/WorksetDerivedStateBinding.h"
#include "../SavorDb/Authoring/IAuthoringDb.h"

namespace {

using namespace soa::battle::actions;
using namespace savor::runtime;
using namespace savor::runtime::predicates;

soa::battle::ctx::BattleContext Context()
{
    soa::battle::ctx::BattleContext context{};
    context.slots_[0].present = 1;
    context.slots_[0].is_player = 1;
    context.slots_[0].is_alive = 1;
    context.slots_[4].present = 1;
    context.slots_[4].is_player = 0;
    context.slots_[4].is_alive = 1;
    return context;
}

BattleTurnExecutionSpec AttackPlan(std::uint8_t target = 4)
{
    return {.commands = {{.actor_slot = 0,
        .macro = BattleAction::Attack,
        .params = {.target_slot = target}}}};
}

savor::db::BattlePlanActionSnapshot AuthoredAttack(
    int actor,
    int ordinal,
    savor::db::BattlePlanTargetKind target_kind,
    std::optional<int> single = std::nullopt,
    std::optional<int> mask = std::nullopt,
    std::optional<int> same_as = std::nullopt)
{
    return {
        .actor_slot = actor,
        .action_preset = {
            .macro = BattleAction::Attack,
            .target_kind = target_kind,
            .target_mask_bits = mask,
            .target_single_slot = single,
            .target_same_as_actor_slot = same_as,
        },
        .ordinal = ordinal,
    };
}

TEST(BattlePlanValidation, ValidatesLiveActorsAndTargets)
{
    auto context = Context();
    EXPECT_TRUE(ValidateBattleTurnPlan(context, AttackPlan()));
    context.slots_[4].is_alive = 0;
    EXPECT_EQ(ValidateBattleTurnPlan(context, AttackPlan()).error,
              BattlePlanValidationError::TargetNotAlive);
    auto invalid_actor = AttackPlan();
    invalid_actor.commands.front().actor_slot = 3;
    EXPECT_EQ(ValidateBattleTurnPlan(context, invalid_actor).error,
              BattlePlanValidationError::InvalidActor);
}

TEST(BattlePlanValidation, UseItemDistinguishesInventoryFromUnsupportedInteraction)
{
    auto context = Context();
    BattleTurnExecutionSpec plan{.commands = {{.actor_slot = 0,
        .macro = BattleAction::UseItem,
        .params = {.target_slot = 0, .item_id = 42}}}};
    EXPECT_EQ(ValidateBattleTurnPlan(context, plan).error,
              BattlePlanValidationError::ItemUnavailable);
    context.state.useable_items[0] = {.item_id = 42, .count = 1};
    EXPECT_EQ(ValidateBattleTurnPlan(context, plan).error,
              BattlePlanValidationError::UnsupportedCommand);
}

TEST(BattleEnumDefinitions, DomainMapsDriveRegisteredIrSchemas)
{
    EXPECT_EQ(
        soa::battle::find_turn_type_definition(0)->type,
        soa::battle::BackAttack);
    EXPECT_EQ(
        find_battle_action_definition(4)->action,
        BattleAction::UseItem);
    EXPECT_EQ(soa::battle::find_turn_type_definition(99), nullptr);
    EXPECT_EQ(find_battle_action_definition(99), nullptr);

    const auto catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    const auto turn_type = std::ranges::find(
        catalog.schemas,
        std::string_view("soa.battle.TurnType"),
        [](const program::TypeSchemaDefinition& schema)
        {
            return std::string_view(schema.identity.canonical_id);
        });
    const auto battle_action = std::ranges::find(
        catalog.schemas,
        std::string_view("soa.battle.BattleAction"),
        [](const program::TypeSchemaDefinition& schema)
        {
            return std::string_view(schema.identity.canonical_id);
        });
    ASSERT_NE(turn_type, catalog.schemas.end());
    ASSERT_NE(battle_action, catalog.schemas.end());
    ASSERT_EQ(
        turn_type->enum_members.size(),
        soa::battle::TurnTypeDefinitions.size());
    ASSERT_EQ(
        battle_action->enum_members.size(),
        BattleActionDefinitions.size());
    for (const auto& definition : soa::battle::TurnTypeDefinitions)
    {
        EXPECT_NE(
            std::ranges::find(
                turn_type->enum_members,
                program::EnumMemberDefinition{
                    std::string(definition.name),
                    static_cast<std::int64_t>(definition.type)}),
            turn_type->enum_members.end());
    }
    for (const auto& definition : BattleActionDefinitions)
    {
        EXPECT_NE(
            std::ranges::find(
                battle_action->enum_members,
                program::EnumMemberDefinition{
                    std::string(definition.name),
                    static_cast<std::int64_t>(definition.action)}),
            battle_action->enum_members.end());
    }
}

TEST(BattleEnumDefinitions, SingleTurnOutcomeMapDrivesModuleSchema)
{
    EXPECT_EQ(
        battlesingleturn::FindBattleSingleTurnOutcomeDefinitionV1(3)->outcome,
        battlesingleturn::BattleSingleTurnOutcomeV1::PredicateRejected);
    EXPECT_EQ(
        battlesingleturn::FindBattleSingleTurnOutcomeDefinitionV1(99),
        nullptr);

    std::string diagnostic;
    const auto phase =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            false,
            EmptyPredicateExecutionPackageV1(),
            &diagnostic);
    ASSERT_TRUE(phase) << diagnostic;
    const auto decoded = program::DecodeProgramModuleV1(
        phase->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    const auto outcome = std::ranges::find(
        decoded.value->local_types,
        std::string_view("soa.battle.single_turn.Outcome"),
        [](const program::TypeSchemaDefinition& schema)
        {
            return std::string_view(schema.identity.canonical_id);
        });
    ASSERT_NE(outcome, decoded.value->local_types.end());
    ASSERT_EQ(
        outcome->enum_members.size(),
        battlesingleturn::BattleSingleTurnOutcomeDefinitionsV1.size());
    for (const auto& definition :
         battlesingleturn::BattleSingleTurnOutcomeDefinitionsV1)
    {
        EXPECT_NE(
            std::ranges::find(
                outcome->enum_members,
                program::EnumMemberDefinition{
                    std::string(definition.name),
                    static_cast<std::int64_t>(definition.outcome)}),
            outcome->enum_members.end());
    }
}

TEST(PredicateExecution, CanonicalEmptyPackageRoundTrips)
{
    const auto package = EmptyPredicateExecutionPackageV1();
    ASSERT_TRUE(ValidatePredicateExecutionPackageV1(package));
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodePredicateExecutionPackageV1(package, encoded));
    PredicateExecutionPackageV1 decoded;
    ASSERT_TRUE(DecodePredicateExecutionPackageV1(encoded, decoded));
    EXPECT_EQ(decoded.content_sha256, package.content_sha256);
    EXPECT_EQ(decoded.group.content_sha256, package.group.content_sha256);
    EXPECT_EQ(decoded.hook_contract.content_sha256,
              package.hook_contract.content_sha256);
    EXPECT_TRUE(decoded.execution_bindings.empty());
}

TEST(PredicateExecution, BattleHookContractExcludesStartActionAndRetainsStartTurn)
{
    const auto contract = BattlePredicateHookContractV1();
    EXPECT_EQ(std::ranges::count_if(contract.points, [](const auto& point) {
        return point.canonical_id.find("StartAction") != std::string::npos;
    }), 0);
    EXPECT_EQ(std::ranges::count_if(contract.points, [](const auto& point) {
        return point.canonical_id.find("StartTurn") != std::string::npos
            && point.pc == 0x800715DCu;
    }), 1);
}

TEST(PredicateExecution, RejectsHashDriftBeforeExecution)
{
    auto package = EmptyPredicateExecutionPackageV1();
    package.content_sha256.assign(64, '0');
    const auto validation = ValidatePredicateExecutionPackageV1(package);
    EXPECT_FALSE(validation);
    EXPECT_EQ(validation.code, "predicate.package_hash_mismatch");
}

PredicateExecutionPackageV1 ActiveLiteralPackage(
    program::composition::PredicateReaction reaction)
{
    auto hook_contract = BattlePredicateHookContractV1();
    const auto hook = std::ranges::find_if(hook_contract.points, [](const auto& point) {
        return point.canonical_id.ends_with(".StartTurn");
    });
    EXPECT_NE(hook, hook_contract.points.end());
    program::composition::PredicateDefinition definition{
        .canonical_id = "test.battle.literal_true",
        .revision = 1,
        .source_name = "test.battle",
        .expression = {{
            .kind = program::composition::PredicateExpressionKind::Literal,
            .literal = program::LiteralValue{
                .type = program::TypeRef::Builtin(program::BuiltinType::Bool),
                .payload = true},
            .result_type = program::TypeRef::Builtin(program::BuiltinType::Bool),
            .source_label = "true",
        }},
        .root_expression = 0,
    };
    ResolvedPredicateDefinitionV1 resolved_definition{
        .revision_id = 84,
        .definition = definition,
    };
    resolved_definition.content_sha256 =
        ComputePredicateDefinitionHashV1(definition);
    PredicateExecutionBindingV1 binding{
        .execution_binding_revision_id = 85,
        .canonical_id = "test.battle.binding.literal_true",
        .revision = 1,
        .definition = resolved_definition,
    };
    binding.content_sha256 = ComputePredicateExecutionBindingHashV1(binding);
    ResolvedPredicateGroupV1 group{
        .predicate_group_revision_id = 42,
        .canonical_id = "test.battle.group",
        .revision = 1,
        .members = {{
            .ordinal = 0,
            .execution_binding_revision_id = binding.execution_binding_revision_id,
            .semantic_hook_ids = {hook->canonical_id},
            .occurrence = PredicateOccurrencePolicyV1::First,
            .reaction = reaction,
        }},
    };
    group.content_sha256 = ComputeResolvedPredicateGroupHashV1(group);
    PredicateExecutionPackageV1 package{
        .hook_contract = std::move(hook_contract),
        .group = std::move(group),
        .execution_bindings = {std::move(binding)},
    };
    package.content_sha256 = ComputePredicateExecutionPackageHashV1(package);
    return package;
}

PredicateExecutionPackageV1 ActiveDerivedEnemyCountPackage()
{
    auto hook_contract = BattlePredicateHookContractV1();
    const auto hook = std::ranges::find_if(
        hook_contract.points,
        [](const auto& point) {
            return point.canonical_id.ends_with(".TurnIsReady");
        });
    EXPECT_NE(hook, hook_contract.points.end());
    const auto query =
        program::capabilities::BattleDerivedTurnOrderActionIdentity();
    const auto reducer = program::capabilities::BattleDerivedReducerIdentity(
        "soa.battle.derived.enemy_count");
    const auto catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    const auto action = std::ranges::find(
        catalog.actions, query,
        &program::ActionDescriptor::identity);
    EXPECT_NE(action, catalog.actions.end());
    const auto snapshot_type = action->output_type;

    program::composition::PredicateDefinition definition{
        .canonical_id = "test.battle.derived.enemy_count",
        .revision = 1,
        .source_name = "test.battle",
        .witnesses = {{"turn_order", snapshot_type}},
        .expression = {
            {
                .kind = program::composition::PredicateExpressionKind::Witness,
                .witness_index = 0,
                .result_type = snapshot_type,
                .source_label = "turn order snapshot",
            },
            {
                .kind = program::composition::PredicateExpressionKind::ImportedReducer,
                .operands = {0},
                .reducer = reducer,
                .result_type = program::TypeRef::Builtin(program::BuiltinType::U32),
                .source_label = "enemy count",
            },
            {
                .kind = program::composition::PredicateExpressionKind::Literal,
                .literal = program::LiteralValue{
                    .type = program::TypeRef::Builtin(program::BuiltinType::U32),
                    .payload = std::uint32_t{0}},
                .result_type = program::TypeRef::Builtin(program::BuiltinType::U32),
                .source_label = "zero",
            },
            {
                .kind = program::composition::PredicateExpressionKind::Greater,
                .operands = {1, 2},
                .result_type = program::TypeRef::Builtin(program::BuiltinType::Bool),
                .source_label = "has enemy",
            },
        },
        .root_expression = 3,
    };
    ResolvedPredicateDefinitionV1 resolved_definition{
        .revision_id = 85,
        .definition = definition,
    };
    resolved_definition.content_sha256 =
        ComputePredicateDefinitionHashV1(definition);
    PredicateExecutionBindingV1 binding{
        .execution_binding_revision_id = 86,
        .canonical_id = "test.battle.binding.derived.enemy_count",
        .revision = 1,
        .definition = resolved_definition,
        .witnesses = {{
            .witness_ordinal = 0,
            .source_kind = PredicateWitnessSourceKindV1::DerivedStateQuery,
            .value_type = snapshot_type,
            .source = query,
        }},
    };
    binding.content_sha256 = ComputePredicateExecutionBindingHashV1(binding);
    ResolvedPredicateGroupV1 group{
        .predicate_group_revision_id = 43,
        .canonical_id = "test.battle.group.derived",
        .revision = 1,
        .members = {{
            .ordinal = 0,
            .execution_binding_revision_id = binding.execution_binding_revision_id,
            .semantic_hook_ids = {hook->canonical_id},
            .occurrence = PredicateOccurrencePolicyV1::First,
        }},
    };
    group.content_sha256 = ComputeResolvedPredicateGroupHashV1(group);
    PredicateExecutionPackageV1 package{
        .hook_contract = std::move(hook_contract),
        .group = std::move(group),
        .execution_bindings = {std::move(binding)},
    };
    package.content_sha256 = ComputePredicateExecutionPackageHashV1(package);
    return package;
}

TEST(BattleSingleTurnModule, PreparesAndCachesActivePredicateVariant)
{
    auto package = ActiveLiteralPackage(
        program::composition::PredicateReaction::AbortOnFail);
    ASSERT_TRUE(ValidatePredicateExecutionPackageV1(package));
    std::string diagnostic;
    const auto first = battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
        false, package, &diagnostic);
    ASSERT_TRUE(first) << diagnostic;
    const auto second = battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
        false, package, &diagnostic);
    ASSERT_TRUE(second) << diagnostic;
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(first->predicate_package().group.content_sha256,
              package.group.content_sha256);
    EXPECT_EQ(first->predicate_package().content_sha256,
              package.content_sha256);

    const auto first_turn =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            true, package, &diagnostic);
    ASSERT_TRUE(first_turn) << diagnostic;
    EXPECT_NE(first_turn->identity().canonical_sha256,
              first->identity().canonical_sha256);
}

TEST(BattleSingleTurnModule, DeclaresSharedPredicateEvaluationSchemaOnce)
{
    auto package = ActiveLiteralPackage(
        program::composition::PredicateReaction::RecordAndContinue);
    const auto end_turn = std::ranges::find_if(
        package.hook_contract.points,
        [](const auto& point) {
            return point.canonical_id.ends_with(".EndTurn");
        });
    const auto victory = std::ranges::find_if(
        package.hook_contract.points,
        [](const auto& point) {
            return point.canonical_id.ends_with(".EndBattleVictory");
        });
    ASSERT_NE(end_turn, package.hook_contract.points.end());
    ASSERT_NE(victory, package.hook_contract.points.end());

    auto& member = package.group.members.front();
    member.semantic_hook_ids = {end_turn->canonical_id, victory->canonical_id};
    std::ranges::sort(member.semantic_hook_ids);
    member.emit_evidence = true;
    package.group.content_sha256 =
        ComputeResolvedPredicateGroupHashV1(package.group);
    package.content_sha256 =
        ComputePredicateExecutionPackageHashV1(package);

    ASSERT_TRUE(ValidatePredicateExecutionPackageV1(package));
    std::string diagnostic;
    const auto prepared =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            false, package, &diagnostic);
    ASSERT_TRUE(prepared) << diagnostic;
    const auto decoded = program::DecodeProgramModuleV1(
        prepared->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    ASSERT_EQ(decoded.value->entrypoints.size(), 1u);
    ASSERT_EQ(decoded.value->entrypoints.front().emission_schemas.size(), 1u);
    EXPECT_EQ(
        decoded.value->entrypoints.front().emission_schemas.front().canonical_id,
        package.execution_bindings.front().definition.definition.canonical_id +
            ".Evaluation");
}

TEST(BattleSingleTurnModule, DerivedPredicateImportsSelectItsExactStaticBlock)
{
    auto predicate = ActiveDerivedEnemyCountPackage();
    ASSERT_TRUE(ValidatePredicateExecutionPackageV1(predicate));
    std::string diagnostic;
    const auto prepared = battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
        false, predicate, &diagnostic);
    ASSERT_TRUE(prepared) << diagnostic;
    const auto package = fullphase::BuildFullPhaseProgramPackage(*prepared);

    savor::db::execution::programdb::ResolvedWorksetDerivedStateBindingV1 resolved;
    const std::vector<std::string> no_defaults;
    ASSERT_TRUE(savor::db::execution::programdb::ResolveWorksetDerivedStateBindingV1(
        no_defaults, package, &resolved, &diagnostic)) << diagnostic;
    ASSERT_EQ(resolved.binding.blocks.size(), 1u);
    EXPECT_EQ(resolved.binding.blocks.front().identity.canonical_id,
        derived::kBattleCoreBlockId);

    const std::array duplicate_defaults{
        std::string(derived::kBattleCoreBlockId),
        std::string(derived::kBattleCoreBlockId),
    };
    EXPECT_FALSE(savor::db::execution::programdb::ResolveWorksetDerivedStateBindingV1(
        duplicate_defaults, package, &resolved, &diagnostic));
    EXPECT_EQ(
        diagnostic,
        "Program-kind derived-state defaults contain a duplicate block ID");
}

TEST(BattleSingleTurnModule, DerivedSnapshotCaptureIsIndependentFromEvaluationHook)
{
    auto package = ActiveDerivedEnemyCountPackage();
    const auto end_turn = std::ranges::find_if(
        package.hook_contract.points,
        [](const auto& point) {
            return point.canonical_id.ends_with(".EndTurn");
        });
    ASSERT_NE(end_turn, package.hook_contract.points.end());
    package.group.members.front().semantic_hook_ids = {
        end_turn->canonical_id};
    package.group.content_sha256 =
        ComputeResolvedPredicateGroupHashV1(package.group);
    package.content_sha256 =
        ComputePredicateExecutionPackageHashV1(package);
    EXPECT_TRUE(ValidatePredicateExecutionPackageV1(package));

    const auto turn_inputs = std::ranges::find_if(
        package.hook_contract.points,
        [](const auto& point) {
            return point.canonical_id.ends_with(".TurnInputs");
        });
    ASSERT_NE(turn_inputs, package.hook_contract.points.end());
    package.group.members.front().semantic_hook_ids = {
        turn_inputs->canonical_id};
    package.group.content_sha256 =
        ComputeResolvedPredicateGroupHashV1(package.group);
    package.content_sha256 =
        ComputePredicateExecutionPackageHashV1(package);
    const auto invalid = ValidatePredicateExecutionPackageV1(package);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.code, "predicate.observation_unschedulable");
}

TEST(BattleSingleTurnModule, AdmissionRequiresExactStaticReducerIdentity)
{
    std::string diagnostic;
    const auto phase =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            false,
            EmptyPredicateExecutionPackageV1(),
            &diagnostic);
    ASSERT_TRUE(phase) << diagnostic;
    const auto decoded = program::DecodeProgramModuleV1(
        phase->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;

    const auto verify = [](program::ProgramModule module)
    {
        program::ProgramDefinitionStore modules;
        program::TypeSchemaRegistry schemas;
        program::ActionRegistry actions(&schemas);
        program::CapabilityPackRegistry packs(&schemas, &actions);
        const auto registered =
            program::capabilities::RegisterSourceCapabilityPacks(
                schemas, actions, packs);
        EXPECT_TRUE(registered.success)
            << registered.error.message;
        const auto stored = modules.RegisterCompiled(std::move(module));
        EXPECT_TRUE(stored.success) << stored.error.message;
        if (!registered.success || !stored.success)
            return program::ProgramVerificationResult{};
        program::ProgramVerifier verifier(
            modules, schemas, actions, packs);
        return verifier.Verify(
            stored.module->identity,
            program::capabilities::SupportedSoaUsaCompatibility());
    };

    const auto accepted = verify(*decoded.value);
    ASSERT_TRUE(accepted.success);

    program::ProgramModule mutated = *decoded.value;
    ASSERT_FALSE(mutated.reducer_imports.empty());
    mutated.reducer_imports.front().signature_hash.bytes.front() ^= 0xff;
    mutated.identity.module_hash =
        program::ComputeProgramModuleHashV1(mutated);
    const auto rejected = verify(std::move(mutated));
    EXPECT_FALSE(rejected.success);
    EXPECT_TRUE(std::ranges::any_of(
        rejected.diagnostics,
        [](const program::VerificationDiagnostic& item)
        {
            return item.code ==
                program::VerificationErrorCode::DependencyMissing;
        }));
}

TEST(BattleSingleTurnModule, FakeAttackUsesLegacySevenFrameTargetDwell)
{
    EXPECT_EQ(battlesingleturn::FakeAttackTargetNeutralFrames, 7u);
    EXPECT_EQ(battlesingleturn::MaximumFakeAttackMemoryPolls, 120u);

    std::string diagnostic;
    const auto phase =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            false,
            EmptyPredicateExecutionPackageV1(),
            &diagnostic);
    ASSERT_TRUE(phase) << diagnostic;
    const auto decoded = program::DecodeProgramModuleV1(
        phase->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    std::vector<const program::Instruction*> instructions;
    for (const auto& function : decoded.value->functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                instructions.push_back(&instruction);
    const auto position = [&](std::string_view selector)
    {
        const auto found = std::ranges::find_if(
            instructions,
            [&](const program::Instruction* instruction)
            {
                return instruction->selector.contains(selector);
            });
        return found == instructions.end()
            ? instructions.size()
            : static_cast<std::size_t>(found - instructions.begin());
    };
    const std::size_t target_ready = position(
        "fake-accept/held-through-semantic-successor");
    const std::size_t rng_change = position(
        "fake-accept/bounded-memory-change-while-state-held");
    const std::size_t release = position(
        "fake-accept/release-to-neutral");
    const std::size_t seven_frames = position(
        "fake-accept/post-gate-neutral/exact-frames");
    const std::size_t back = position(
        "fake-back/apply-state-before-departure");
    ASSERT_LT(target_ready, instructions.size());
    ASSERT_LT(rng_change, instructions.size());
    ASSERT_LT(release, instructions.size());
    ASSERT_LT(seven_frames, instructions.size());
    ASSERT_LT(back, instructions.size());
    EXPECT_LT(target_ready, rng_change);
    EXPECT_LT(rng_change, release);
    EXPECT_LT(release, seven_frames);
    EXPECT_LT(seven_frames, back);

    const auto count = std::ranges::find_if(
        instructions,
        [](const program::Instruction* instruction)
        {
            return instruction->selector.contains(
                "fake-accept/post-gate-neutral/count");
        });
    ASSERT_NE(count, instructions.end());
    ASSERT_TRUE((*count)->literal);
    const auto* value = std::get_if<std::uint64_t>(
        &(*count)->literal->payload);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7u);
}

TEST(BattleSingleTurnModule, SharedAttackAcceptEnumRoutesToHeldAInputSegment)
{
    std::string diagnostic;
    const auto phase =
        battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            false,
            EmptyPredicateExecutionPackageV1(),
            &diagnostic);
    ASSERT_TRUE(phase) << diagnostic;
    const auto decoded = program::DecodeProgramModuleV1(
        phase->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;

    const auto interaction = std::ranges::find_if(
        decoded.value->functions,
        [](const program::ProgramFunction& function) {
            return function.name == "interact.soa.battle.command_entry";
        });
    ASSERT_NE(interaction, decoded.value->functions.end());
    const auto attack_accept = std::ranges::find_if(
        interaction->blocks,
        [](const program::BasicBlock& block) {
            return std::ranges::any_of(
                block.instructions,
                [](const program::Instruction& instruction) {
                    return instruction.selector.contains(
                        "segment/attack-accept/scope");
                });
        });
    ASSERT_NE(attack_accept, interaction->blocks.end());
    ASSERT_TRUE(std::ranges::any_of(
        attack_accept->instructions,
        [](const program::Instruction& instruction) {
            return instruction.selector.contains(
                "segment/attack-accept/apply-state-before-departure");
        }));

    const auto adaptive_block = std::ranges::find_if(
        interaction->blocks,
        [](const program::BasicBlock& block) {
            return block.terminator.kind == program::TerminatorKind::EnumSwitch;
        });
    ASSERT_NE(adaptive_block, interaction->blocks.end());
    const auto attack_case = std::ranges::find(
        adaptive_block->terminator.enum_cases,
        program::capabilities::BattleCommandSegmentValue(
            program::capabilities::BattleCommandSegment::AttackAccept),
        &program::EnumSwitchCase::enum_value);
    ASSERT_NE(attack_case, adaptive_block->terminator.enum_cases.end());
    EXPECT_EQ(attack_case->edge.target, attack_accept->id);
}

TEST(BattleWaveRanking, UsesFakeFramesPassedThenStableJobIdentity)
{
    using savor::db::execution::programdb::battle::BattleWaveCandidateRank;
    using savor::db::execution::programdb::battle::
        BattleWaveCandidateRanksBefore;
    const BattleWaveCandidateRank base{
        .cumulative_fake_attacks = 2,
        .delta_vi = 40,
        .pred_passed = 5,
        .stable_job_id = 10,
    };
    auto candidate = base;
    candidate.cumulative_fake_attacks = 1;
    EXPECT_TRUE(BattleWaveCandidateRanksBefore(candidate, base));
    candidate = base;
    candidate.delta_vi = 39;
    EXPECT_TRUE(BattleWaveCandidateRanksBefore(candidate, base));
    candidate = base;
    candidate.pred_passed = 6;
    EXPECT_TRUE(BattleWaveCandidateRanksBefore(candidate, base));
    candidate = base;
    candidate.stable_job_id = 9;
    EXPECT_TRUE(BattleWaveCandidateRanksBefore(candidate, base));
    EXPECT_FALSE(BattleWaveCandidateRanksBefore(base, base));
}

TEST(BattlePlanStructure, RequiresPositiveOneBasedContiguousTurnRows)
{
    using savor::db::BattlePlanSnapshot;
    using savor::db::BattlePlanTurnSnapshot;
    using savor::db::execution::programdb::battle::
        ValidateBattlePlanTurnSequence;

    std::string error;
    BattlePlanSnapshot plan{};
    EXPECT_FALSE(ValidateBattlePlanTurnSequence(plan, &error));

    plan.turns = {
        BattlePlanTurnSnapshot{.turn_index = 2},
        BattlePlanTurnSnapshot{.turn_index = 1},
    };
    EXPECT_TRUE(ValidateBattlePlanTurnSequence(plan, &error)) << error;

    plan.turns = {
        BattlePlanTurnSnapshot{.turn_index = 1},
        BattlePlanTurnSnapshot{.turn_index = 3},
    };
    EXPECT_FALSE(ValidateBattlePlanTurnSequence(plan, &error));

    plan.turns = {
        BattlePlanTurnSnapshot{.turn_index = 1},
        BattlePlanTurnSnapshot{.turn_index = 1},
    };
    EXPECT_FALSE(ValidateBattlePlanTurnSequence(plan, &error));

    plan.turns = {BattlePlanTurnSnapshot{.turn_index = 0}};
    EXPECT_FALSE(ValidateBattlePlanTurnSequence(plan, &error));
    plan.turns = {BattlePlanTurnSnapshot{.turn_index = -1}};
    EXPECT_FALSE(ValidateBattlePlanTurnSequence(plan, &error));
}

TEST(BattleTargetAvailability, UsesOptionalLiveEnemyEvidence)
{
    using savor::db::execution::programdb::battle::BattleTargetAvailability;
    using savor::db::execution::programdb::battle::ClassifyBattleTurnTargets;

    const auto commands = AttackPlan().commands;
    EXPECT_EQ(ClassifyBattleTurnTargets(nullptr, commands),
              BattleTargetAvailability::Unknown);

    auto context = Context();
    EXPECT_EQ(ClassifyBattleTurnTargets(&context, commands),
              BattleTargetAvailability::Available);
    context.slots_[4].is_alive = 0;
    EXPECT_EQ(ClassifyBattleTurnTargets(&context, commands),
              BattleTargetAvailability::Unavailable);
    context = Context();
    context.slots_[4].present = 0;
    EXPECT_EQ(ClassifyBattleTurnTargets(&context, commands),
              BattleTargetAvailability::Unavailable);
    context = Context();
    context.slots_[4].is_player = 1;
    EXPECT_EQ(ClassifyBattleTurnTargets(&context, commands),
              BattleTargetAvailability::Unavailable);
}

TEST(BattleTargetMaterialization, ExpandsAnyEnemyAndTransitiveSameAsDeterministically)
{
    using savor::db::BattlePlanTargetKind;
    using savor::db::BattlePlanTurnSnapshot;
    using savor::db::execution::programdb::battle::CompileBattleTurnVariants;

    BattlePlanTurnSnapshot turn{
        .turn_index = 1,
        .actions = {
            AuthoredAttack(0, 0, BattlePlanTargetKind::SameAsOtherPC,
                           std::nullopt, std::nullopt, 2),
            AuthoredAttack(1, 1, BattlePlanTargetKind::SameAsOtherPC,
                           std::nullopt, std::nullopt, 0),
            AuthoredAttack(2, 2, BattlePlanTargetKind::AnyEnemy),
        },
    };
    std::string error;
    const auto structural = CompileBattleTurnVariants(turn, nullptr, &error);
    ASSERT_TRUE(structural) << error;
    ASSERT_EQ(structural->structural_variants.size(), 8u);
    ASSERT_EQ(structural->context_viable_variants.size(), 8u);
    for (std::size_t index = 0;
         index < structural->structural_variants.size(); ++index) {
        const auto& variant = structural->structural_variants[index];
        ASSERT_EQ(variant.commands.size(), 3u);
        EXPECT_EQ(variant.commands[0].params.target_slot, index + 4);
        EXPECT_EQ(variant.commands[1].params.target_slot, index + 4);
        EXPECT_EQ(variant.commands[2].params.target_slot, index + 4);
        EXPECT_EQ(variant.encoded_commands,
            encode_battle_turn_commands_hex(variant.commands));
        std::vector<std::uint8_t> command_bytes;
        encode_battle_turn_commands_to_buffer(
            variant.commands, command_bytes);
        EXPECT_EQ(variant.variant_key,
            hash::sha256(command_bytes.data(), command_bytes.size()));
    }

    auto context = Context();
    context.slots_[5].present = 1;
    context.slots_[5].is_alive = 1;
    const auto narrowed = CompileBattleTurnVariants(turn, &context, &error);
    ASSERT_TRUE(narrowed) << error;
    ASSERT_EQ(narrowed->structural_variants.size(), 8u);
    ASSERT_EQ(narrowed->context_viable_variants.size(), 2u);
    EXPECT_EQ(narrowed->context_viable_variants[0].commands[0].params.target_slot, 4);
    EXPECT_EQ(narrowed->context_viable_variants[1].commands[0].params.target_slot, 5);
}

TEST(BattleTargetMaterialization, MaskedSelectorsFormCartesianAlternatives)
{
    using savor::db::BattlePlanTargetKind;
    savor::db::BattlePlanTurnSnapshot turn{
        .turn_index = 1,
        .actions = {
            AuthoredAttack(0, 0, BattlePlanTargetKind::MultipleEnemies,
                           std::nullopt, (1 << 4) | (1 << 5)),
            AuthoredAttack(1, 1, BattlePlanTargetKind::MultipleEnemies,
                           std::nullopt, (1 << 6) | (1 << 7)),
        },
    };
    std::string error;
    const auto variants = savor::db::execution::programdb::battle::
        CompileBattleTurnVariants(turn, nullptr, &error);
    ASSERT_TRUE(variants) << error;
    ASSERT_EQ(variants->structural_variants.size(), 4u);
    EXPECT_EQ(variants->structural_variants[0].commands[0].params.target_slot, 4);
    EXPECT_EQ(variants->structural_variants[0].commands[1].params.target_slot, 6);
    EXPECT_EQ(variants->structural_variants[3].commands[0].params.target_slot, 5);
    EXPECT_EQ(variants->structural_variants[3].commands[1].params.target_slot, 7);
}

TEST(BattleTargetMaterialization, RejectsMalformedSelectorGraphs)
{
    using savor::db::BattlePlanTargetKind;
    using savor::db::execution::programdb::battle::CompileBattleTurnVariants;
    std::string error;

    savor::db::BattlePlanTurnSnapshot turn{
        .turn_index = 1,
        .actions = {
            AuthoredAttack(0, 0, BattlePlanTargetKind::SameAsOtherPC,
                           std::nullopt, std::nullopt, 1),
            AuthoredAttack(1, 1, BattlePlanTargetKind::SameAsOtherPC,
                           std::nullopt, std::nullopt, 0),
        },
    };
    EXPECT_FALSE(CompileBattleTurnVariants(turn, nullptr, &error));
    EXPECT_NE(error.find("acyclic"), std::string::npos) << error;

    turn.actions = {
        AuthoredAttack(0, 1, BattlePlanTargetKind::AnyEnemy),
    };
    EXPECT_FALSE(CompileBattleTurnVariants(turn, nullptr, &error));
    EXPECT_NE(error.find("ordinals"), std::string::npos) << error;

    turn.actions = {
        AuthoredAttack(0, 0, BattlePlanTargetKind::MultipleEnemies,
                       std::nullopt, 1 << 3),
    };
    EXPECT_FALSE(CompileBattleTurnVariants(turn, nullptr, &error));
    EXPECT_NE(error.find("mask"), std::string::npos) << error;

    turn.actions = {
        AuthoredAttack(0, 0, BattlePlanTargetKind::SameAsOtherPC,
                       std::nullopt, std::nullopt, 1),
    };
    EXPECT_FALSE(CompileBattleTurnVariants(turn, nullptr, &error));
    EXPECT_NE(error.find("not present"), std::string::npos) << error;

    turn.actions = {
        AuthoredAttack(0, 0, BattlePlanTargetKind::AnyEnemy),
        AuthoredAttack(0, 1, BattlePlanTargetKind::SingleEnemy, 4),
    };
    EXPECT_FALSE(CompileBattleTurnVariants(turn, nullptr, &error));
    EXPECT_NE(error.find("unique slots"), std::string::npos) << error;
}

TEST(BattleTargetMaterialization, ExactTargetAndAllInvalidContextRemainExplicit)
{
    using savor::db::BattlePlanTargetKind;
    savor::db::BattlePlanTurnSnapshot turn{
        .turn_index = 1,
        .actions = {
            AuthoredAttack(0, 0, BattlePlanTargetKind::SingleEnemy, 7),
        },
    };
    auto context = Context();
    std::string error;
    const auto variants = savor::db::execution::programdb::battle::
        CompileBattleTurnVariants(turn, &context, &error);
    ASSERT_TRUE(variants) << error;
    ASSERT_EQ(variants->structural_variants.size(), 1u);
    EXPECT_TRUE(variants->context_viable_variants.empty());
    EXPECT_EQ(variants->structural_variants.front()
                  .commands.front().params.target_slot,
              7);
}

} // namespace
