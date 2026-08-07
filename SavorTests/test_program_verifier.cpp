#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace {

using namespace savor::runtime::program;

ContentHash256 Hash(Byte value)
{
    ContentHash256 result;
    result.bytes.fill(value);
    return result;
}

ProgramBudgets Budgets()
{
    return {
        .maximum_instructions = 100,
        .maximum_calls = 10,
        .maximum_call_depth = 4,
        .maximum_action_requests = 10,
        .maximum_emissions = 10,
        .maximum_artifacts = 10,
        .maximum_values = 100,
        .maximum_value_bytes = 4096,
        .maximum_trace_events = 100,
    };
}

ProgramPolicySet Policies()
{
    return {
        .state_policies = {
            InvocationStatePolicy::RestoreBaseline,
        },
        .execution_intents = {ExecutionIntent::Live},
    };
}

SourceMapEntry Source(
    std::uint64_t id,
    std::string path)
{
    return {
        .id = ProgramSourceLocationId(id),
        .source_name = "verifier.test",
        .semantic_path = std::move(path),
    };
}

ProgramModule ValidModule(std::string id = "test.verified")
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::move(id),
            .revision = 1,
        },
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type = TypeRef::Builtin(BuiltinType::U32),
                .output_type = TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .accepted_policies = Policies(),
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId(1),
                .name = "run",
                .arguments = {
                    {ProgramValueId(1),
                     TypeRef::Builtin(BuiltinType::U32)},
                },
                .output_type = TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .entry_block = ProgramBlockId(1),
                .blocks = {
                    BasicBlock{
                        .id = ProgramBlockId(1),
                        .instructions = {
                            Instruction{
                                .id = ProgramInstructionId(1),
                                .opcode = InstructionOpcode::Constant,
                                .source_location =
                                    ProgramSourceLocationId(1),
                                .result = ValueDefinition{
                                    ProgramValueId(2),
                                    TypeRef::Builtin(BuiltinType::Bool),
                                },
                                .literal = LiteralValue{
                                    TypeRef::Builtin(BuiltinType::Bool),
                                    true,
                                },
                            },
                        },
                        .terminator = Terminator{
                            .kind = TerminatorKind::Return,
                            .source_location =
                                ProgramSourceLocationId(2),
                            .return_value = ProgramValueId(1),
                            .domain_outcome = ProgramValueId(2),
                        },
                    },
                },
                .exported = true,
            },
        },
        .accepted_policies = Policies(),
        .budgets = Budgets(),
        .source_map = {
            .version = 1,
            .entries = {
                Source(1, "constant"),
                Source(2, "return"),
            },
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

RuntimeCompatibility Compatibility()
{
    return {
        .game_id = "GSOE8P",
        .executable_identity = "main.dol",
        .address_map_revision = "usa-v1",
    };
}

bool Has(
    const ProgramVerificationResult& result,
    VerificationErrorCode code)
{
    return std::ranges::any_of(
        result.diagnostics,
        [code](const VerificationDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

struct Harness
{
    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions{&schemas};
    CapabilityPackRegistry packs{&schemas, &actions};

    ProgramVerificationResult Verify(const ModuleIdentity& identity)
    {
        ProgramVerifier verifier(modules, schemas, actions, packs);
        return verifier.Verify(identity, Compatibility());
    }
};

struct TestAction
{
    CapabilityPackIdentity pack;
    ExactDependencyIdentity action;
};

TestAction RegisterTestAction(
    Harness& harness,
    std::string canonical_id,
    TypeRef input,
    TypeRef output,
    ActionCleanupGuarantee cleanup =
        ActionCleanupGuarantee::None,
    ActionIdempotency idempotency =
        ActionIdempotency::NotRetryable,
    ActionResourceBehavior resource =
        ActionResourceBehavior::None,
    ActionEffectMask effects = 0)
{
    TestAction registered{
        .pack = {
            canonical_id + ".pack",
            1,
            Hash(21),
        },
        .action = {
            std::move(canonical_id),
            1,
            Hash(22),
        },
    };
    EXPECT_TRUE(
        harness.actions
            .RegisterAction(ActionDescriptor{
                .identity = registered.action,
                .providing_pack = registered.pack,
                .input_type = std::move(input),
                .output_type = std::move(output),
                .effects = effects,
                .cancellation =
                    cleanup == ActionCleanupGuarantee::None
                    ? ActionCancellationMode::Cooperative
                    : ActionCancellationMode::CleanupRequired,
                .timing = ActionTimingClass::BoundedHostOperation,
                .default_host_timeout_milliseconds = 1000,
                .resource_behavior = resource,
                .cleanup = cleanup,
                .idempotency = idempotency,
            })
            .success);
    EXPECT_TRUE(
        harness.packs
            .Register(CapabilityPackManifest{
                .identity = registered.pack,
                .compatibility = Compatibility(),
                .actions = {registered.action},
            })
            .success);
    return registered;
}

void ImportAction(
    ProgramModule& module,
    const TestAction& action)
{
    module.action_imports.push_back(action.action);
    module.required_capability_packs.push_back(action.pack);
    module.entrypoints.front()
        .required_capability_packs.push_back(action.pack);
}

TEST(ProgramVerifier, ProducesAndCachesExactImmutableClosure)
{
    Harness harness;
    const ProgramModule module = ValidModule();
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult first =
        harness.Verify(module.identity);
    ASSERT_TRUE(first.success)
        << (first.diagnostics.empty()
                ? ""
                : first.diagnostics.front().message);
    ASSERT_NE(first.verified, nullptr);
    EXPECT_EQ(first.verified->module->identity, module.identity);
    EXPECT_EQ(first.verified->dependency_lock.ir_version, 1u);
    EXPECT_EQ(harness.modules.verified_cache_size(), 1u);

    const ProgramVerificationResult second =
        harness.Verify(module.identity);
    ASSERT_TRUE(second.success);
    EXPECT_EQ(first.verified, second.verified);
    EXPECT_EQ(harness.modules.verified_cache_size(), 1u);
}

TEST(
    ProgramVerifier,
    EstablishBaselineRejectsSuccessfulReturnBeforeMoviePlayback)
{
    Harness harness;
    ProgramModule module = ValidModule("test.establish-before-use");
    module.accepted_policies.state_policies = {
        InvocationStatePolicy::EstablishBaseline};
    module.entrypoints.front().accepted_policies.state_policies = {
        InvocationStatePolicy::EstablishBaseline};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(std::ranges::any_of(
        result.diagnostics,
        [](const VerificationDiagnostic& diagnostic)
        {
            return diagnostic.code == VerificationErrorCode::InvalidPolicy &&
                diagnostic.message.find(
                    "successful return path before prepared MovieStartPlayback") !=
                    std::string::npos;
        }));
}

TEST(ProgramVerifier, ChecksDefinitionBeforeUseAndUnreachableBlocks)
{
    Harness harness;
    ProgramModule module = ValidModule("test.invalid.ssa");
    BasicBlock unreachable{
        .id = ProgramBlockId(2),
        .instructions = {
            Instruction{
                .id = ProgramInstructionId(2),
                .opcode = InstructionOpcode::Copy,
                .source_location = ProgramSourceLocationId(3),
                .result = ValueDefinition{
                    ProgramValueId(3),
                    TypeRef::Builtin(BuiltinType::U32),
                },
                .operands = {ProgramValueId(999)},
            },
        },
        .terminator = Terminator{
            .kind = TerminatorKind::Return,
            .source_location = ProgramSourceLocationId(4),
            .return_value = ProgramValueId(3),
            .domain_outcome = ProgramValueId(2),
        },
    };
    module.functions.front().blocks.push_back(
        std::move(unreachable));
    module.source_map.entries.push_back(Source(3, "unreachable.copy"));
    module.source_map.entries.push_back(Source(4, "unreachable.return"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::UndefinedValue));
}

TEST(ProgramVerifier, RejectsActionWhoseEffectIsNotDeclared)
{
    Harness harness;
    const CapabilityPackIdentity pack{
        "test.capture.pack",
        1,
        Hash(1),
    };
    const ExactDependencyIdentity action{
        "test.capture.action",
        1,
        Hash(2),
    };
    ASSERT_TRUE(
        harness.actions
            .RegisterAction(ActionDescriptor{
                .identity = action,
                .providing_pack = pack,
                .input_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .output_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .required_services =
                    ServiceMask(SessionServiceCapability::Capture),
                .effects = EffectMask(ActionEffect::Capture),
                .timing = ActionTimingClass::BoundedHostOperation,
                .default_host_timeout_milliseconds = 1000,
            })
            .success);
    ASSERT_TRUE(
        harness.packs
            .Register(CapabilityPackManifest{
                .identity = pack,
                .compatibility = Compatibility(),
                .actions = {action},
            })
            .success);

    ProgramModule module = ValidModule("test.undeclared.capture");
    module.action_imports = {action};
    module.required_capability_packs = {pack};
    module.entrypoints.front().required_capability_packs = {pack};
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        Instruction{
            .id = ProgramInstructionId(2),
            .opcode = InstructionOpcode::AwaitAction,
            .source_location = ProgramSourceLocationId(3),
            .result = ValueDefinition{
                ProgramValueId(3),
                TypeRef::Builtin(BuiltinType::U32),
            },
            .operands = {ProgramValueId(1)},
            .target = {
                .kind = InstructionTargetKind::Action,
                .dependency = action,
            },
        });
    block.terminator.return_value = ProgramValueId(3);
    module.source_map.entries.push_back(Source(3, "await.capture"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::UndeclaredEffect));
}

TEST(ProgramVerifier, RejectsInvalidBudgetsBeforePublication)
{
    Harness harness;
    ProgramModule module = ValidModule("test.bad.budget");
    module.budgets.maximum_instructions = 0;
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::InvalidBudget));
    EXPECT_EQ(harness.modules.verified_cache_size(), 0u);
}

TEST(ProgramVerifier, RejectsNonFiniteAndOversizedLiterals)
{
    Harness harness;
    ProgramModule module = ValidModule("test.bad.literal");
    const SchemaIdentity text{
        "test.short_text",
        1,
        Hash(7),
    };
    module.local_types.push_back(
        TypeSchemaDefinition{
            .identity = text,
            .kind = TypeSchemaKind::BoundedUtf8String,
            .maximum_size = 4,
        });
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        Instruction{
            .id = ProgramInstructionId(2),
            .opcode = InstructionOpcode::Constant,
            .source_location = ProgramSourceLocationId(3),
            .result = ValueDefinition{
                ProgramValueId(3),
                TypeRef::Named(text),
            },
            .literal = LiteralValue{
                TypeRef::Named(text),
                std::string("too-long"),
            },
        });
    module.source_map.entries.push_back(Source(3, "bad.literal"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::InvalidInstruction));
}

TEST(ProgramVerifier, RequiresLexicalScopesToBalanceAcrossTerminals)
{
    Harness harness;
    ProgramModule module = ValidModule("test.bad.scope");
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        Instruction{
            .id = ProgramInstructionId(2),
            .opcode = InstructionOpcode::EnterScope,
            .source_location = ProgramSourceLocationId(3),
            .scope = ProgramScopeId(1),
        });
    module.source_map.entries.push_back(Source(3, "scope.enter"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::InvalidScope));
}

TEST(ProgramVerifier, RejectsResultFromUnitImportedCall)
{
    Harness harness;
    ProgramModule dependency =
        ValidModule("test.unit.import.dependency");
    dependency.entrypoints.front().output_type =
        TypeRef::Builtin(BuiltinType::Unit);
    dependency.functions.front().output_type =
        TypeRef::Builtin(BuiltinType::Unit);
    dependency.functions.front()
        .blocks.front()
        .terminator.return_value.reset();
    dependency.identity.module_hash =
        ComputeProgramModuleHashV1(dependency);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(dependency).success);

    ProgramModule module =
        ValidModule("test.unit.import.caller");
    module.module_imports.push_back(
        ModuleImportIdentity{
            dependency.identity,
            dependency.ir_version,
        });
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        Instruction{
            .id = ProgramInstructionId(2),
            .opcode = InstructionOpcode::CallImported,
            .source_location = ProgramSourceLocationId(3),
            .result = ValueDefinition{
                ProgramValueId(3),
                TypeRef::Builtin(BuiltinType::U32),
            },
            .operands = {ProgramValueId(1)},
            .target = {
                .kind =
                    InstructionTargetKind::ImportedFunction,
                .dependency = ExactDependencyIdentity{
                    dependency.identity.canonical_id,
                    dependency.identity.revision,
                    dependency.identity.module_hash,
                },
                .member_name = "run",
            },
        });
    module.source_map.entries.push_back(
        Source(3, "call.unit-import"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::TypeMismatch));
}

TEST(ProgramVerifier, RejectsResultFromUnitAction)
{
    Harness harness;
    const TestAction action = RegisterTestAction(
        harness,
        "test.unit.action",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Unit));

    ProgramModule module =
        ValidModule("test.unit.action.caller");
    ImportAction(module, action);
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(
        Instruction{
            .id = ProgramInstructionId(2),
            .opcode = InstructionOpcode::AwaitAction,
            .source_location = ProgramSourceLocationId(3),
            .result = ValueDefinition{
                ProgramValueId(3),
                TypeRef::Builtin(BuiltinType::U32),
            },
            .operands = {ProgramValueId(1)},
            .target = {
                .kind = InstructionTargetKind::Action,
                .dependency = action.action,
            },
        });
    module.source_map.entries.push_back(
        Source(3, "await.unit-action"));
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(Has(result, VerificationErrorCode::TypeMismatch));
}

TEST(ProgramVerifier, EnforcesDeferredCompensationContract)
{
    Harness harness;
    const TestAction safe = RegisterTestAction(
        harness,
        "test.defer.safe",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Unit),
        ActionCleanupGuarantee::VerifiedCompensation,
        ActionIdempotency::NaturallyIdempotent);
    const TestAction unsafe = RegisterTestAction(
        harness,
        "test.defer.unsafe",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Unit));

    const auto make_module =
        [&](std::string id,
            const TestAction& action,
            ProgramValueId operand,
            bool produce_result) {
            ProgramModule module = ValidModule(std::move(id));
            ImportAction(module, action);
            BasicBlock& block =
                module.functions.front().blocks.front();
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(2),
                    .opcode = InstructionOpcode::EnterScope,
                    .source_location =
                        ProgramSourceLocationId(3),
                    .scope = ProgramScopeId(1),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(3),
                    .opcode =
                        InstructionOpcode::DeferCompensation,
                    .source_location =
                        ProgramSourceLocationId(4),
                    .result = produce_result
                        ? std::optional<ValueDefinition>(
                              ValueDefinition{
                                  ProgramValueId(3),
                                  TypeRef::Builtin(
                                      BuiltinType::U32),
                              })
                        : std::nullopt,
                    .operands = {operand},
                    .target = {
                        .kind =
                            InstructionTargetKind::DeferredAction,
                        .dependency = action.action,
                    },
                    .scope = ProgramScopeId(1),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(4),
                    .opcode = InstructionOpcode::ExitScope,
                    .source_location =
                        ProgramSourceLocationId(5),
                    .scope = ProgramScopeId(1),
                });
            module.source_map.entries.push_back(
                Source(3, "scope.enter"));
            module.source_map.entries.push_back(
                Source(4, "scope.defer"));
            module.source_map.entries.push_back(
                Source(5, "scope.exit"));
            module.identity.module_hash =
                ComputeProgramModuleHashV1(module);
            return module;
        };

    ProgramModule wrong_input = make_module(
        "test.defer.wrong-input",
        safe,
        ProgramValueId(2),
        false);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(wrong_input).success);
    const ProgramVerificationResult wrong_input_result =
        harness.Verify(wrong_input.identity);
    EXPECT_FALSE(wrong_input_result.success);
    EXPECT_TRUE(
        Has(
            wrong_input_result,
            VerificationErrorCode::TypeMismatch));

    ProgramModule has_result = make_module(
        "test.defer.has-result",
        safe,
        ProgramValueId(1),
        true);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(has_result).success);
    const ProgramVerificationResult has_result_result =
        harness.Verify(has_result.identity);
    EXPECT_FALSE(has_result_result.success);
    EXPECT_TRUE(
        Has(
            has_result_result,
            VerificationErrorCode::InvalidScope));

    ProgramModule unsafe_action = make_module(
        "test.defer.unsafe-action",
        unsafe,
        ProgramValueId(1),
        false);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(unsafe_action).success);
    const ProgramVerificationResult unsafe_result =
        harness.Verify(unsafe_action.identity);
    EXPECT_FALSE(unsafe_result.success);
    EXPECT_TRUE(
        Has(
            unsafe_result,
            VerificationErrorCode::InvalidScope));
}

TEST(ProgramVerifier, EnforcesResourcePromotionContract)
{
    Harness harness;
    const SchemaIdentity resource{
        "test.promote.resource",
        1,
        Hash(31),
    };
    const SchemaIdentity handle{
        "test.promote.handle",
        1,
        Hash(32),
    };
    ASSERT_TRUE(
        harness.schemas
            .RegisterBatch(
                {
                    TypeSchemaDefinition{
                        .identity = resource,
                        .kind = TypeSchemaKind::Record,
                    },
                    TypeSchemaDefinition{
                        .identity = handle,
                        .kind =
                            TypeSchemaKind::ResourceHandle,
                        .element_type =
                            TypeRef::Named(resource),
                    },
                })
            .success);
    const TestAction acquire = RegisterTestAction(
        harness,
        "test.promote.acquire",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Named(handle),
        ActionCleanupGuarantee::Automatic,
        ActionIdempotency::NotRetryable,
        ActionResourceBehavior::Promotable);

    const auto make_module =
        [&](std::string id,
            ProgramValueId promoted,
            bool produce_result) {
            ProgramModule module = ValidModule(std::move(id));
            module.type_imports = {resource, handle};
            ImportAction(module, acquire);
            module.accepted_policies
                .permits_resource_promotion = true;
            module.entrypoints.front()
                .accepted_policies
                .permits_resource_promotion = true;
            BasicBlock& block =
                module.functions.front().blocks.front();
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(2),
                    .opcode = InstructionOpcode::EnterScope,
                    .source_location =
                        ProgramSourceLocationId(3),
                    .scope = ProgramScopeId(1),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(3),
                    .opcode = InstructionOpcode::EnterScope,
                    .source_location =
                        ProgramSourceLocationId(4),
                    .scope = ProgramScopeId(2),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(4),
                    .opcode = InstructionOpcode::AwaitAction,
                    .source_location =
                        ProgramSourceLocationId(5),
                    .result = ValueDefinition{
                        ProgramValueId(3),
                        TypeRef::Named(handle),
                    },
                    .operands = {ProgramValueId(1)},
                    .target = {
                        .kind =
                            InstructionTargetKind::Action,
                        .dependency = acquire.action,
                    },
                    .scope = ProgramScopeId(2),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(5),
                    .opcode =
                        InstructionOpcode::PromoteResource,
                    .source_location =
                        ProgramSourceLocationId(6),
                    .result = produce_result
                        ? std::optional<ValueDefinition>(
                              ValueDefinition{
                                  ProgramValueId(4),
                                  TypeRef::Builtin(
                                      BuiltinType::U32),
                              })
                        : std::nullopt,
                    .operands = {promoted},
                    .scope = ProgramScopeId(1),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(6),
                    .opcode = InstructionOpcode::ExitScope,
                    .source_location =
                        ProgramSourceLocationId(7),
                    .scope = ProgramScopeId(2),
                });
            block.instructions.push_back(
                Instruction{
                    .id = ProgramInstructionId(7),
                    .opcode = InstructionOpcode::ExitScope,
                    .source_location =
                        ProgramSourceLocationId(8),
                    .scope = ProgramScopeId(1),
                });
            module.source_map.entries.push_back(
                Source(3, "scope.outer.enter"));
            module.source_map.entries.push_back(
                Source(4, "scope.inner.enter"));
            module.source_map.entries.push_back(
                Source(5, "resource.acquire"));
            module.source_map.entries.push_back(
                Source(6, "resource.promote"));
            module.source_map.entries.push_back(
                Source(7, "scope.inner.exit"));
            module.source_map.entries.push_back(
                Source(8, "scope.outer.exit"));
            module.identity.module_hash =
                ComputeProgramModuleHashV1(module);
            return module;
        };

    ProgramModule wrong_type = make_module(
        "test.promote.wrong-type",
        ProgramValueId(1),
        false);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(wrong_type).success);
    const ProgramVerificationResult wrong_type_result =
        harness.Verify(wrong_type.identity);
    EXPECT_FALSE(wrong_type_result.success);
    EXPECT_TRUE(
        Has(
            wrong_type_result,
            VerificationErrorCode::TypeMismatch));

    ProgramModule has_result = make_module(
        "test.promote.has-result",
        ProgramValueId(3),
        true);
    ASSERT_TRUE(
        harness.modules.RegisterCompiled(has_result).success);
    const ProgramVerificationResult has_result_result =
        harness.Verify(has_result.identity);
    EXPECT_FALSE(has_result_result.success);
    EXPECT_TRUE(
        Has(
            has_result_result,
            VerificationErrorCode::InvalidScope));
}

TEST(ProgramVerifier, RecordFieldSelectorIsAuthoritative)
{
    Harness harness;
    const SchemaIdentity record{"test.record", 1, Hash(40)};
    ProgramModule module = ValidModule("test.record.selector");
    module.local_types.push_back(TypeSchemaDefinition{
        .identity = record,
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"first", TypeRef::Builtin(BuiltinType::U32)},
            {"second", TypeRef::Builtin(BuiltinType::U32)},
        },
    });
    module.entrypoints.front().input_type = TypeRef::Named(record);
    module.functions.front().arguments.front().type =
        TypeRef::Named(record);
    BasicBlock& block = module.functions.front().blocks.front();
    block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(2),
        .opcode = InstructionOpcode::RecordProject,
        .source_location = ProgramSourceLocationId(3),
        .result = ValueDefinition{
            ProgramValueId(3),
            TypeRef::Builtin(BuiltinType::U32)},
        .operands = {ProgramValueId(1)},
        .selector = "second",
        .ordinal = 0,
    });
    block.terminator.return_value = ProgramValueId(3);
    module.source_map.entries.push_back(Source(3, "record.second"));
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_TRUE(result.success)
        << (result.diagnostics.empty()
                ? ""
                : result.diagnostics.front().message);
}

TEST(ProgramVerifier, RejectsSignedListIndexAndNonnumericOrdering)
{
    Harness harness;
    const SchemaIdentity list{"test.u32.list", 1, Hash(41)};
    ProgramModule indexed = ValidModule("test.list.signed-index");
    indexed.local_types.push_back(TypeSchemaDefinition{
        .identity = list,
        .kind = TypeSchemaKind::BoundedList,
        .maximum_size = 4,
        .element_type = TypeRef::Builtin(BuiltinType::U32),
    });
    indexed.entrypoints.front().input_type = TypeRef::Named(list);
    indexed.functions.front().arguments.front().type =
        TypeRef::Named(list);
    BasicBlock& indexed_block =
        indexed.functions.front().blocks.front();
    indexed_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(2),
        .opcode = InstructionOpcode::Constant,
        .source_location = ProgramSourceLocationId(3),
        .result = ValueDefinition{
            ProgramValueId(3),
            TypeRef::Builtin(BuiltinType::I32)},
        .literal = LiteralValue{
            TypeRef::Builtin(BuiltinType::I32),
            std::int32_t(0)},
    });
    indexed_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(3),
        .opcode = InstructionOpcode::ListIndex,
        .source_location = ProgramSourceLocationId(4),
        .result = ValueDefinition{
            ProgramValueId(4),
            TypeRef::Builtin(BuiltinType::U32)},
        .operands = {ProgramValueId(1), ProgramValueId(3)},
    });
    indexed_block.terminator.return_value = ProgramValueId(4);
    indexed.source_map.entries.push_back(Source(3, "signed.index"));
    indexed.source_map.entries.push_back(Source(4, "list.index"));
    indexed.identity.module_hash =
        ComputeProgramModuleHashV1(indexed);
    ASSERT_TRUE(harness.modules.RegisterCompiled(indexed).success);
    const ProgramVerificationResult indexed_result =
        harness.Verify(indexed.identity);
    EXPECT_FALSE(indexed_result.success);
    EXPECT_TRUE(
        Has(indexed_result, VerificationErrorCode::TypeMismatch));

    ProgramModule ordered = ValidModule("test.order.bool");
    ordered.entrypoints.front().output_type =
        TypeRef::Builtin(BuiltinType::Bool);
    ordered.functions.front().output_type =
        TypeRef::Builtin(BuiltinType::Bool);
    BasicBlock& ordered_block =
        ordered.functions.front().blocks.front();
    ordered_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(2),
        .opcode = InstructionOpcode::Less,
        .source_location = ProgramSourceLocationId(3),
        .result = ValueDefinition{
            ProgramValueId(3),
            TypeRef::Builtin(BuiltinType::Bool)},
        .operands = {ProgramValueId(2), ProgramValueId(2)},
    });
    ordered_block.terminator.return_value = ProgramValueId(3);
    ordered.source_map.entries.push_back(Source(3, "bool.less"));
    ordered.identity.module_hash =
        ComputeProgramModuleHashV1(ordered);
    ASSERT_TRUE(harness.modules.RegisterCompiled(ordered).success);
    const ProgramVerificationResult ordered_result =
        harness.Verify(ordered.identity);
    EXPECT_FALSE(ordered_result.success);
    EXPECT_TRUE(
        Has(ordered_result, VerificationErrorCode::TypeMismatch));
}

TEST(ProgramVerifier, RejectsNoExitLoopButAllowsDataDependentExit)
{
    Harness harness;
    ProgramModule trapped = ValidModule("test.loop.trapped");
    trapped.functions.front().blocks.front().terminator = Terminator{
        .kind = TerminatorKind::Branch,
        .source_location = ProgramSourceLocationId(2),
        .edges = {{ProgramBlockId(1), {}}},
    };
    trapped.identity.module_hash =
        ComputeProgramModuleHashV1(trapped);
    ASSERT_TRUE(harness.modules.RegisterCompiled(trapped).success);
    const ProgramVerificationResult trapped_result =
        harness.Verify(trapped.identity);
    EXPECT_FALSE(trapped_result.success);
    EXPECT_TRUE(
        Has(
            trapped_result,
            VerificationErrorCode::InvalidControlFlow));

    ProgramModule budgeted = ValidModule("test.loop.data-dependent");
    budgeted.entrypoints.front().input_type =
        TypeRef::Builtin(BuiltinType::Bool);
    budgeted.entrypoints.front().output_type =
        TypeRef::Builtin(BuiltinType::Bool);
    ProgramFunction& function = budgeted.functions.front();
    function.arguments.front().type =
        TypeRef::Builtin(BuiltinType::Bool);
    function.output_type = TypeRef::Builtin(BuiltinType::Bool);
    function.blocks.front().instructions.clear();
    function.blocks.front().terminator = Terminator{
        .kind = TerminatorKind::ConditionalBranch,
        .source_location = ProgramSourceLocationId(2),
        .condition_or_selector = ProgramValueId(1),
        .edges = {
            {ProgramBlockId(1), {}},
            {ProgramBlockId(2), {}},
        },
    };
    function.blocks.push_back(BasicBlock{
        .id = ProgramBlockId(2),
        .terminator = Terminator{
            .kind = TerminatorKind::Return,
            .source_location = ProgramSourceLocationId(3),
            .return_value = ProgramValueId(1),
            .domain_outcome = ProgramValueId(1),
        },
    });
    budgeted.source_map.entries.push_back(Source(3, "loop.exit"));
    budgeted.identity.module_hash =
        ComputeProgramModuleHashV1(budgeted);
    ASSERT_TRUE(harness.modules.RegisterCompiled(budgeted).success);
    const ProgramVerificationResult budgeted_result =
        harness.Verify(budgeted.identity);
    EXPECT_TRUE(budgeted_result.success)
        << (budgeted_result.diagnostics.empty()
                ? ""
                : budgeted_result.diagnostics.front().message);
}

TEST(ProgramVerifier, EntrypointDeclarationsDoNotLeakAcrossReachability)
{
    Harness harness;
    const SchemaIdentity emission{"test.entry.emission", 1, Hash(50)};
    const SchemaIdentity payload{"test.entry.payload", 1, Hash(51)};
    const SchemaIdentity artifact{"test.entry.artifact", 1, Hash(52)};
    const TestAction action = RegisterTestAction(
        harness,
        "test.entry.capture",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::U32),
        ActionCleanupGuarantee::None,
        ActionIdempotency::NotRetryable,
        ActionResourceBehavior::None,
        EffectMask(ActionEffect::Capture));

    ProgramModule module = ValidModule("test.entry.isolation");
    module.local_types = {
        TypeSchemaDefinition{
            .identity = emission,
            .kind = TypeSchemaKind::Record,
        },
        TypeSchemaDefinition{
            .identity = payload,
            .kind = TypeSchemaKind::Record,
        },
        TypeSchemaDefinition{
            .identity = artifact,
            .kind = TypeSchemaKind::ArtifactReference,
            .element_type = TypeRef::Named(payload),
        },
    };
    module.action_imports = {action.action};
    module.required_capability_packs = {action.pack};
    module.accepted_policies.permits_capture = true;
    module.entrypoints.front().input_type = TypeRef::Named(artifact);
    module.entrypoints.front().output_type = TypeRef::Named(artifact);
    module.entrypoints.front().required_capability_packs = {action.pack};
    ProgramFunction& first = module.functions.front();
    first.arguments.front().type = TypeRef::Named(artifact);
    first.output_type = TypeRef::Named(artifact);
    BasicBlock& first_block = first.blocks.front();
    first_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(2),
        .opcode = InstructionOpcode::RecordConstruct,
        .source_location = ProgramSourceLocationId(3),
        .result = ValueDefinition{
            ProgramValueId(3),
            TypeRef::Named(emission)},
    });
    first_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(3),
        .opcode = InstructionOpcode::EmitRecord,
        .source_location = ProgramSourceLocationId(4),
        .operands = {ProgramValueId(3)},
    });
    first_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(4),
        .opcode = InstructionOpcode::PublishArtifact,
        .source_location = ProgramSourceLocationId(5),
        .operands = {ProgramValueId(1)},
    });
    first_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(5),
        .opcode = InstructionOpcode::Constant,
        .source_location = ProgramSourceLocationId(6),
        .result = ValueDefinition{
            ProgramValueId(4),
            TypeRef::Builtin(BuiltinType::U32)},
        .literal = LiteralValue{
            TypeRef::Builtin(BuiltinType::U32),
            std::uint32_t(7)},
    });
    first_block.instructions.push_back(Instruction{
        .id = ProgramInstructionId(6),
        .opcode = InstructionOpcode::AwaitAction,
        .source_location = ProgramSourceLocationId(7),
        .result = ValueDefinition{
            ProgramValueId(5),
            TypeRef::Builtin(BuiltinType::U32)},
        .operands = {ProgramValueId(4)},
        .target = {
            .kind = InstructionTargetKind::Action,
            .dependency = action.action,
        },
    });
    first_block.terminator.return_value = ProgramValueId(1);

    module.entrypoints.push_back(ProgramEntrypoint{
        .name = "declared",
        .function = ProgramFunctionId(2),
        .input_type = TypeRef::Named(artifact),
        .output_type = TypeRef::Named(artifact),
        .domain_outcome_type =
            TypeRef::Builtin(BuiltinType::Bool),
        .emission_schemas = {emission},
        .artifact_schemas = {payload},
        .required_capability_packs = {action.pack},
        .accepted_policies = [] {
            ProgramPolicySet policies = Policies();
            policies.permits_capture = true;
            return policies;
        }(),
    });
    module.functions.push_back(ProgramFunction{
        .id = ProgramFunctionId(2),
        .name = "declared",
        .arguments = {
            {ProgramValueId(10), TypeRef::Named(artifact)},
        },
        .output_type = TypeRef::Named(artifact),
        .domain_outcome_type =
            TypeRef::Builtin(BuiltinType::Bool),
        .entry_block = ProgramBlockId(2),
        .blocks = {
            BasicBlock{
                .id = ProgramBlockId(2),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(7),
                        .opcode = InstructionOpcode::Constant,
                        .source_location =
                            ProgramSourceLocationId(8),
                        .result = ValueDefinition{
                            ProgramValueId(11),
                            TypeRef::Builtin(BuiltinType::Bool)},
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::Bool),
                            true},
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(9),
                    .return_value = ProgramValueId(10),
                    .domain_outcome = ProgramValueId(11),
                },
            },
        },
        .exported = true,
    });
    for (std::uint64_t id = 3; id <= 9; ++id)
        module.source_map.entries.push_back(
            Source(id, "entry.isolation"));
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    ASSERT_TRUE(harness.modules.RegisterCompiled(module).success);

    const ProgramVerificationResult result =
        harness.Verify(module.identity);
    EXPECT_FALSE(result.success);
    const auto has_message = [&](std::string_view text) {
        return std::ranges::any_of(
            result.diagnostics,
            [&](const VerificationDiagnostic& diagnostic) {
                return diagnostic.message.find(text) !=
                    std::string::npos;
            });
    };
    EXPECT_TRUE(has_message("reachable action"));
    EXPECT_TRUE(has_message("reachable emission"));
    EXPECT_TRUE(has_message("reachable artifact"));
}

} // namespace
