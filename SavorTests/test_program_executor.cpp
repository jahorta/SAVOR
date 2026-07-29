#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Execution/ProgramExecutor.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramValueArena.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;

ContentHash256 Hash(Byte value)
{
    ContentHash256 result;
    result.bytes.fill(value);
    return result;
}

ProgramBudgets Budgets(
    std::uint64_t maximum_instructions = 4096)
{
    return {
        .maximum_instructions = maximum_instructions,
        .maximum_calls = 64,
        .maximum_call_depth = 16,
        .maximum_action_requests = 64,
        .maximum_emissions = 64,
        .maximum_artifacts = 64,
        .maximum_values = 8192,
        .maximum_value_bytes = 1024 * 1024,
        .maximum_trace_events = 8192,
    };
}

ProgramPolicySet Policies()
{
    return {
        .state_policies = {
            InvocationStatePolicy::ContinueSession,
        },
        .execution_intents = {ExecutionIntent::Live},
    };
}

template <typename T>
ProgramValueGraph ScalarGraph(
    BuiltinType type,
    T value)
{
    return {
        ProgramValueId(1),
        {
            ProgramValue{
                ProgramValueId(1),
                TypeRef::Builtin(type),
                ProgramValuePayload(std::move(value)),
            },
        },
    };
}

ProgramValueGraph UnitGraph()
{
    return ScalarGraph(BuiltinType::Unit, UnitValue{});
}

template <typename T>
const T* ScalarValue(const ProgramValueGraph& graph)
{
    const auto found = std::ranges::find_if(
        graph.values,
        [&graph](const ProgramValue& value) {
            return value.id == graph.root;
        });
    return found == graph.values.end()
        ? nullptr
        : std::get_if<T>(&found->payload);
}

ProgramModule BaseModule(
    std::string canonical_id,
    TypeRef input_type,
    TypeRef output_type,
    std::optional<TypeRef> domain_type,
    std::vector<BasicBlock> blocks,
    ProgramBudgets budgets = Budgets())
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::move(canonical_id),
            .revision = 1,
        },
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type = input_type,
                .output_type = output_type,
                .domain_outcome_type =
                    domain_type.value_or(
                        TypeRef::Builtin(BuiltinType::Bool)),
                .accepted_policies = Policies(),
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId(1),
                .name = "run",
                .arguments = {
                    {ProgramValueId(1), input_type},
                },
                .output_type = output_type,
                .domain_outcome_type = domain_type,
                .entry_block = ProgramBlockId(1),
                .blocks = std::move(blocks),
                .exported = true,
            },
        },
        .accepted_policies = Policies(),
        .budgets = budgets,
        .source_map = {
            .version = 1,
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

std::shared_ptr<const VerifiedProgramModule> Verified(
    ProgramModule module,
    std::optional<std::pair<
        ExactDependencyIdentity,
        ActionDescriptor>> action = std::nullopt)
{
    auto verified = std::make_shared<VerifiedProgramModule>();
    verified->type_closure = module.local_types;
    verified->module =
        std::make_shared<const ProgramModule>(std::move(module));
    verified->dependency_lock.ir_version = kCanonicalIrVersionV1;
    if (action)
    {
        verified->dependency_lock.action_imports.push_back(
            action->first);
        verified->actions.push_back(
            {action->first, std::move(action->second)});
    }
    return verified;
}

ProgramInvocation Invocation(
    const VerifiedProgramModule& verified,
    ProgramValueGraph input,
    ProgramBudgets limits = Budgets(),
    InvocationId invocation_id = InvocationId(41))
{
    return {
        .invocation_id = invocation_id,
        .attempt_id = AttemptId(7),
        .module = verified.module->identity,
        .entrypoint = "run",
        .dependencies = verified.dependency_lock,
        .runtime_profile = {
            .profile_id = "executor.test",
            .game_id = "GEAE8E",
            .disc_identity = "test-disc",
            .executable_identity = "soal-usa.GEAE8E",
            .backend = "fake",
        },
        .state = {
            .policy = InvocationStatePolicy::ContinueSession,
            .session_lineage = "executor-test",
            .expected_session = SessionId(3),
            .expected_epoch = StateEpoch(5),
        },
        .execution = {
            .intent = ExecutionIntent::Live,
            .record_trace = true,
            .record_progress = true,
        },
        .input = std::move(input),
        .limits = limits,
        .provenance = {
            .requesting_component = "ProgramExecutorTest",
        },
    };
}

ProgramActionCompletion Completion(
    const ProgramInvocation& invocation,
    ProgramActionRequestId request_id,
    ProgramHostOperation operation,
    ProgramValueGraph output = UnitGraph(),
    ProgramActionCompletionStatus status =
        ProgramActionCompletionStatus::Completed)
{
    return {
        .request_id = request_id,
        .invocation_id = invocation.invocation_id,
        .attempt_id = invocation.attempt_id,
        .operation = operation,
        .status = status,
        .origin_epoch = invocation.state.expected_epoch,
        .resulting_epoch = invocation.state.expected_epoch,
        .output = std::move(output),
        .cleanup = ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
    };
}

std::optional<ProgramResult> CompleteFinish(
    ProgramExecutor& executor,
    const ProgramInvocation& invocation,
    ProgramExecutorPumpResult pumped,
    std::uint64_t request_value)
{
    if (!pumped.host_request ||
        pumped.host_request->operation !=
            ProgramHostOperation::FinishInvocation)
    {
        return std::nullopt;
    }
    const ProgramActionRequestId request_id(request_value);
    if (!executor.BindPendingAction(request_id) ||
        !executor.DeliverHostCompletion(
            Completion(
                invocation,
                request_id,
                ProgramHostOperation::FinishInvocation)))
    {
        return std::nullopt;
    }
    return executor.Pump().terminal;
}

ProgramModule BranchingModule()
{
    return BaseModule(
        "test.executor.deterministic",
        TypeRef::Builtin(BuiltinType::Bool),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .terminator = Terminator{
                    .kind = TerminatorKind::ConditionalBranch,
                    .source_location = ProgramSourceLocationId(1),
                    .condition_or_selector = ProgramValueId(1),
                    .edges = {
                        {ProgramBlockId(2), {}},
                        {ProgramBlockId(3), {}},
                    },
                },
            },
            BasicBlock{
                .id = ProgramBlockId(2),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(2),
                            TypeRef::Builtin(BuiltinType::U32),
                        },
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::U32),
                            std::uint32_t(17),
                        },
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(3),
                        .result = ValueDefinition{
                            ProgramValueId(3),
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
                    .source_location = ProgramSourceLocationId(4),
                    .return_value = ProgramValueId(2),
                    .domain_outcome = ProgramValueId(3),
                },
            },
            BasicBlock{
                .id = ProgramBlockId(3),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(3),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(5),
                        .result = ValueDefinition{
                            ProgramValueId(4),
                            TypeRef::Builtin(BuiltinType::U32),
                        },
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::U32),
                            std::uint32_t(23),
                        },
                    },
                    Instruction{
                        .id = ProgramInstructionId(4),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(6),
                        .result = ValueDefinition{
                            ProgramValueId(5),
                            TypeRef::Builtin(BuiltinType::Bool),
                        },
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::Bool),
                            false,
                        },
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(7),
                    .return_value = ProgramValueId(4),
                    .domain_outcome = ProgramValueId(5),
                },
            },
        });
}

std::shared_ptr<const VerifiedProgramModule> CleanupProgram(
    std::string canonical_id,
    const CapabilityPackIdentity& pack,
    const ExactDependencyIdentity& cleanup_action)
{
    ActionDescriptor descriptor{
        .identity = cleanup_action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::Unit),
        .output_type = TypeRef::Builtin(BuiltinType::Unit),
        .cancellation = ActionCancellationMode::CleanupRequired,
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
        .cleanup = ActionCleanupGuarantee::VerifiedCompensation,
        .taints_on_unproven_cleanup = true,
    };
    ProgramModule module = BaseModule(
        std::move(canonical_id),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::EnterScope,
                        .source_location = ProgramSourceLocationId(1),
                        .scope = ProgramScopeId(22),
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode =
                            InstructionOpcode::DeferCompensation,
                        .source_location = ProgramSourceLocationId(2),
                        .target = {
                            .kind =
                                InstructionTargetKind::DeferredAction,
                            .dependency = cleanup_action,
                        },
                        .scope = ProgramScopeId(22),
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Branch,
                    .source_location = ProgramSourceLocationId(3),
                    .edges = {{ProgramBlockId(2), {}}},
                },
            },
            BasicBlock{
                .id = ProgramBlockId(2),
                .terminator = Terminator{
                    .kind = TerminatorKind::Branch,
                    .source_location = ProgramSourceLocationId(4),
                    .edges = {{ProgramBlockId(2), {}}},
                },
            },
        });
    module.action_imports = {cleanup_action};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return Verified(
        std::move(module),
        std::pair{cleanup_action, descriptor});
}

std::optional<ProgramResult> RunNumericBinary(
    BuiltinType operand_type,
    LiteralPayload lhs,
    LiteralPayload rhs,
    InstructionOpcode opcode)
{
    const bool comparison =
        opcode == InstructionOpcode::Equal ||
        opcode == InstructionOpcode::NotEqual ||
        opcode == InstructionOpcode::Less ||
        opcode == InstructionOpcode::LessEqual ||
        opcode == InstructionOpcode::Greater ||
        opcode == InstructionOpcode::GreaterEqual;
    const TypeRef source = TypeRef::Builtin(operand_type);
    const TypeRef output = comparison
        ? TypeRef::Builtin(BuiltinType::Bool)
        : source;
    auto verified = Verified(BaseModule(
        "test.executor.numeric-binary",
        TypeRef::Builtin(BuiltinType::Unit),
        output,
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2), source},
                        .literal = LiteralValue{
                            source, std::move(lhs)},
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(3), source},
                        .literal = LiteralValue{
                            source, std::move(rhs)},
                    },
                    Instruction{
                        .id = ProgramInstructionId(3),
                        .opcode = opcode,
                        .source_location = ProgramSourceLocationId(3),
                        .result = ValueDefinition{
                            ProgramValueId(4), output},
                        .operands = {
                            ProgramValueId(2),
                            ProgramValueId(3)},
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(4),
                    .return_value = ProgramValueId(4),
                },
            },
        }));
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    if (!executor.Start(
            verified,
            invocation,
            cancellation.token()))
    {
        return std::nullopt;
    }
    return CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        950);
}

std::optional<ProgramResult> RunNumericConversion(
    BuiltinType source_type,
    LiteralPayload source_value,
    BuiltinType result_type)
{
    const TypeRef source = TypeRef::Builtin(source_type);
    const TypeRef output = TypeRef::Builtin(result_type);
    auto verified = Verified(BaseModule(
        "test.executor.numeric-convert",
        TypeRef::Builtin(BuiltinType::Unit),
        output,
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2), source},
                        .literal = LiteralValue{
                            source, std::move(source_value)},
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::CheckedConvert,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(3), output},
                        .operands = {ProgramValueId(2)},
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(3),
                    .return_value = ProgramValueId(3),
                },
            },
        }));
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    if (!executor.Start(
            verified,
            invocation,
            cancellation.token()))
    {
        return std::nullopt;
    }
    return CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        951);
}

TEST(ProgramExecutor, ExecutesTheSameCfgDeterministically)
{
    const auto verified = Verified(BranchingModule());
    const ProgramInvocation invocation =
        Invocation(
            *verified,
            ScalarGraph(BuiltinType::Bool, true));

    const auto run = [&]() -> std::optional<ProgramResult> {
        CancellationSource cancellation(invocation.invocation_id);
        ProgramExecutor executor;
        std::string diagnostic;
        if (!executor.Start(
                verified,
                invocation,
                cancellation.token(),
                &diagnostic))
        {
            ADD_FAILURE() << diagnostic;
            return std::nullopt;
        }
        return CompleteFinish(
            executor,
            invocation,
            executor.Pump(),
            100);
    };

    const auto first = run();
    const auto second = run();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
    ASSERT_TRUE(first->output);
    ASSERT_TRUE(first->domain_outcome);
    ASSERT_NE(ScalarValue<std::uint32_t>(*first->output), nullptr);
    EXPECT_EQ(*ScalarValue<std::uint32_t>(*first->output), 17u);
    ASSERT_NE(ScalarValue<bool>(*first->domain_outcome), nullptr);
    EXPECT_TRUE(*ScalarValue<bool>(*first->domain_outcome));
}

TEST(ProgramExecutor, YieldsAtTheFixed1024InstructionQuantum)
{
    std::vector<Instruction> instructions;
    instructions.reserve(1025);
    for (std::uint64_t index = 0; index < 1025; ++index)
    {
        instructions.push_back(Instruction{
            .id = ProgramInstructionId(index + 1),
            .opcode = InstructionOpcode::Constant,
            .source_location = ProgramSourceLocationId(index + 1),
            .result = ValueDefinition{
                ProgramValueId(index + 2),
                TypeRef::Builtin(BuiltinType::U32),
            },
            .literal = LiteralValue{
                TypeRef::Builtin(BuiltinType::U32),
                static_cast<std::uint32_t>(index),
            },
        });
    }
    const ProgramValueId final_value(1026);
    auto verified = Verified(BaseModule(
        "test.executor.quantum",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = std::move(instructions),
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(2000),
                    .return_value = final_value,
                },
            },
        },
        Budgets(4096)));
    const ProgramInvocation invocation =
        Invocation(
            *verified,
            ScalarGraph(BuiltinType::U32, std::uint32_t(9)),
            Budgets(4096));
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    const ProgramExecutorPumpResult first = executor.Pump();
    EXPECT_TRUE(first.runnable);
    EXPECT_FALSE(first.host_request);
    EXPECT_FALSE(first.terminal);
    EXPECT_EQ(
        executor.snapshot().instructions_executed,
        kProgramExecutorQuantum);

    ProgramExecutorPumpResult second = executor.Pump();
    ASSERT_TRUE(second.host_request);
    EXPECT_EQ(
        second.host_request->operation,
        ProgramHostOperation::FinishInvocation);
    const auto terminal =
        CompleteFinish(executor, invocation, std::move(second), 101);
    ASSERT_TRUE(terminal);
    ASSERT_TRUE(terminal->output);
    ASSERT_NE(
        ScalarValue<std::uint32_t>(*terminal->output),
        nullptr);
    EXPECT_EQ(
        *ScalarValue<std::uint32_t>(*terminal->output),
        1024u);
}

TEST(ProgramExecutor, SuspendsForActionsAndCorrelatesCompletions)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(1),
    };
    const ExactDependencyIdentity action{
        "test.executor.echo",
        1,
        Hash(2),
    };
    ActionDescriptor descriptor{
        .identity = action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::U32),
        .output_type = TypeRef::Builtin(BuiltinType::U32),
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
    };
    ProgramModule module = BaseModule(
        "test.executor.action",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::AwaitAction,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2),
                            TypeRef::Builtin(BuiltinType::U32),
                        },
                        .operands = {ProgramValueId(1)},
                        .target = {
                            .kind = InstructionTargetKind::Action,
                            .dependency = action,
                        },
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(3),
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
                    .source_location = ProgramSourceLocationId(3),
                    .return_value = ProgramValueId(2),
                    .domain_outcome = ProgramValueId(3),
                },
            },
        });
    module.action_imports = {action};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    auto verified = Verified(
        std::move(module),
        std::pair{action, descriptor});
    const ProgramInvocation invocation =
        Invocation(
            *verified,
            ScalarGraph(
                BuiltinType::U32,
                std::uint32_t(11)));
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult suspended = executor.Pump();
    ASSERT_TRUE(suspended.host_request);
    EXPECT_EQ(
        suspended.host_request->operation,
        ProgramHostOperation::InvokeAction);
    EXPECT_EQ(suspended.host_request->action, action);
    EXPECT_EQ(
        executor.snapshot().activity,
        ProgramExecutorActivity::AwaitingHost);

    const ProgramActionRequestId request_id(500);
    EXPECT_FALSE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            request_id,
            ProgramHostOperation::InvokeAction)));
    ASSERT_TRUE(executor.BindPendingAction(request_id));

    ProgramActionCompletion wrong = Completion(
        invocation,
        ProgramActionRequestId(501),
        ProgramHostOperation::InvokeAction);
    EXPECT_FALSE(executor.DeliverHostCompletion(wrong));
    wrong = Completion(
        invocation,
        request_id,
        ProgramHostOperation::CloseScope);
    EXPECT_FALSE(executor.DeliverHostCompletion(wrong));
    wrong = Completion(
        invocation,
        request_id,
        ProgramHostOperation::InvokeAction);
    wrong.attempt_id = AttemptId(999);
    EXPECT_FALSE(executor.DeliverHostCompletion(wrong));
    wrong = Completion(
        invocation,
        request_id,
        ProgramHostOperation::InvokeAction,
        ScalarGraph(BuiltinType::Bool, true));
    EXPECT_FALSE(executor.DeliverHostCompletion(wrong));
    wrong = Completion(
        invocation,
        request_id,
        ProgramHostOperation::InvokeAction,
        ScalarGraph(
            BuiltinType::U32,
            std::uint32_t(77)));
    wrong.origin_epoch = StateEpoch(4);
    EXPECT_FALSE(executor.DeliverHostCompletion(wrong));
    EXPECT_EQ(
        executor.snapshot().activity,
        ProgramExecutorActivity::AwaitingHost);

    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            request_id,
            ProgramHostOperation::InvokeAction,
            ScalarGraph(
                BuiltinType::U32,
                std::uint32_t(77)))));
    ProgramExecutorPumpResult finish = executor.Pump();
    const auto terminal =
        CompleteFinish(executor, invocation, std::move(finish), 502);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::Completed);
    ASSERT_TRUE(terminal->output);
    ASSERT_NE(
        ScalarValue<std::uint32_t>(*terminal->output),
        nullptr);
    EXPECT_EQ(
        *ScalarValue<std::uint32_t>(*terminal->output),
        77u);
}

TEST(
    ProgramExecutor,
    ValidatesUnitActionOutputWithoutSsaBindingAndDoesNotFabricateCleanup)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(41),
    };
    const ExactDependencyIdentity action{
        "test.executor.unit",
        1,
        Hash(42),
    };
    ActionDescriptor descriptor{
        .identity = action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::Unit),
        .output_type = TypeRef::Builtin(BuiltinType::Unit),
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
        .resource_behavior =
            ActionResourceBehavior::Scoped,
    };
    ProgramModule module = BaseModule(
        "test.executor.unit-action",
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::AwaitAction,
                        .source_location = ProgramSourceLocationId(1),
                        .operands = {ProgramValueId(1)},
                        .target = {
                            .kind = InstructionTargetKind::Action,
                            .dependency = action,
                        },
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(2),
                },
            },
        });
    module.action_imports = {action};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    auto verified = Verified(
        std::move(module),
        std::pair{action, descriptor});
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult suspended = executor.Pump();
    ASSERT_TRUE(suspended.host_request);
    const ProgramActionRequestId request_id(550);
    ASSERT_TRUE(executor.BindPendingAction(request_id));
    EXPECT_FALSE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            request_id,
            ProgramHostOperation::InvokeAction,
            ScalarGraph(BuiltinType::Bool, true))));

    ProgramActionCompletion completed = Completion(
        invocation,
        request_id,
        ProgramHostOperation::InvokeAction);
    completed.resources.push_back(ProgramActionResource{
        .handle = ProgramResourceHandleId(90),
        .receipt = ResourceReceiptId(91),
        .kind = ResourceKind::HostResource,
        .acquisition_epoch = invocation.state.expected_epoch,
        .origin_epoch = invocation.state.expected_epoch,
    });
    ASSERT_TRUE(executor.DeliverHostCompletion(
        std::move(completed)));

    const auto terminal = CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        551);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::Completed);
    EXPECT_TRUE(terminal->cleanup_receipts.empty());
}

TEST(ProgramExecutor, StaleActionCompletionFailsAndStillUnwinds)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(3),
    };
    const ExactDependencyIdentity action{
        "test.executor.stale",
        1,
        Hash(4),
    };
    ActionDescriptor descriptor{
        .identity = action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::U32),
        .output_type = TypeRef::Builtin(BuiltinType::U32),
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
    };
    ProgramModule module = BaseModule(
        "test.executor.stale",
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::AwaitAction,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2),
                            TypeRef::Builtin(BuiltinType::U32),
                        },
                        .operands = {ProgramValueId(1)},
                        .target = {
                            .kind = InstructionTargetKind::Action,
                            .dependency = action,
                        },
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(2),
                    .return_value = ProgramValueId(2),
                },
            },
        });
    module.action_imports = {action};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    auto verified = Verified(
        std::move(module),
        std::pair{action, descriptor});
    const ProgramInvocation invocation =
        Invocation(
            *verified,
            ScalarGraph(
                BuiltinType::U32,
                std::uint32_t(11)));
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult suspended = executor.Pump();
    ASSERT_TRUE(suspended.host_request);
    const ProgramActionRequestId request_id(600);
    ASSERT_TRUE(executor.BindPendingAction(request_id));
    ProgramActionCompletion stale = Completion(
        invocation,
        request_id,
        ProgramHostOperation::InvokeAction,
        UnitGraph(),
        ProgramActionCompletionStatus::StaleEpoch);
    stale.code = "stale_epoch";
    stale.message = "completion belongs to an obsolete state epoch";
    ASSERT_TRUE(executor.DeliverHostCompletion(std::move(stale)));

    ProgramExecutorPumpResult finish = executor.Pump();
    const auto terminal =
        CompleteFinish(executor, invocation, std::move(finish), 601);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);
    ASSERT_FALSE(terminal->diagnostics.empty());
    EXPECT_EQ(terminal->diagnostics.back().code, "stale_epoch");
}

TEST(ProgramExecutor, CancellationRunsDeferredCleanupAndClosesScopes)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(5),
    };
    const ExactDependencyIdentity cleanup_action{
        "test.executor.cleanup",
        1,
        Hash(6),
    };
    ActionDescriptor descriptor{
        .identity = cleanup_action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::Unit),
        .output_type = TypeRef::Builtin(BuiltinType::Unit),
        .cancellation = ActionCancellationMode::CleanupRequired,
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
        .cleanup = ActionCleanupGuarantee::VerifiedCompensation,
        .taints_on_unproven_cleanup = true,
    };
    ProgramModule module = BaseModule(
        "test.executor.cancel-unwind",
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::EnterScope,
                        .source_location = ProgramSourceLocationId(1),
                        .scope = ProgramScopeId(22),
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode =
                            InstructionOpcode::DeferCompensation,
                        .source_location = ProgramSourceLocationId(2),
                        .target = {
                            .kind =
                                InstructionTargetKind::DeferredAction,
                            .dependency = cleanup_action,
                        },
                        .scope = ProgramScopeId(22),
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Branch,
                    .source_location = ProgramSourceLocationId(3),
                    .edges = {{ProgramBlockId(2), {}}},
                },
            },
            BasicBlock{
                .id = ProgramBlockId(2),
                .terminator = Terminator{
                    .kind = TerminatorKind::Branch,
                    .source_location = ProgramSourceLocationId(4),
                    .edges = {{ProgramBlockId(2), {}}},
                },
            },
        });
    module.action_imports = {cleanup_action};
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    auto verified = Verified(
        std::move(module),
        std::pair{cleanup_action, descriptor});
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult open = executor.Pump();
    ASSERT_TRUE(open.host_request);
    ASSERT_EQ(
        open.host_request->operation,
        ProgramHostOperation::OpenScope);
    EXPECT_EQ(open.host_request->scope, ProgramScopeId(22));
    const ProgramActionRequestId open_id(700);
    ASSERT_TRUE(executor.BindPendingAction(open_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            open_id,
            ProgramHostOperation::OpenScope)));

    const ProgramExecutorPumpResult running = executor.Pump(2);
    EXPECT_TRUE(running.runnable);
    EXPECT_FALSE(running.host_request);
    ASSERT_TRUE(executor.RequestCancellation(
        CancellationReason::ExternalRequest));

    ProgramExecutorPumpResult cleanup = executor.Pump();
    ASSERT_TRUE(cleanup.host_request);
    EXPECT_EQ(
        cleanup.host_request->operation,
        ProgramHostOperation::InvokeAction);
    EXPECT_EQ(cleanup.host_request->action, cleanup_action);
    EXPECT_TRUE(cleanup.host_request->cleanup_only);
    const ProgramActionRequestId cleanup_id(701);
    ASSERT_TRUE(executor.BindPendingAction(cleanup_id));
    EXPECT_FALSE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            cleanup_id,
            ProgramHostOperation::InvokeAction,
            ScalarGraph(BuiltinType::Bool, true))));
    ProgramActionCompletion cleaned = Completion(
        invocation,
        cleanup_id,
        ProgramHostOperation::InvokeAction);
    cleaned.cleanup_receipts.push_back(CleanupReceipt{
        ProgramResourceHandleId(70),
        ProgramCleanupStatus::CleanWithDiagnostics,
        "cleanup witness"});
    ASSERT_TRUE(executor.DeliverHostCompletion(
        std::move(cleaned)));

    ProgramExecutorPumpResult close = executor.Pump();
    ASSERT_TRUE(close.host_request);
    EXPECT_EQ(
        close.host_request->operation,
        ProgramHostOperation::CloseScope);
    EXPECT_EQ(close.host_request->scope, ProgramScopeId(22));
    EXPECT_TRUE(close.host_request->cleanup_only);
    const ProgramActionRequestId close_id(702);
    ASSERT_TRUE(executor.BindPendingAction(close_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            close_id,
            ProgramHostOperation::CloseScope)));

    ProgramExecutorPumpResult finish = executor.Pump();
    const auto terminal =
        CompleteFinish(executor, invocation, std::move(finish), 703);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::Cancelled);
    EXPECT_EQ(
        terminal->cleanup,
        ProgramCleanupStatus::CleanWithDiagnostics);
    EXPECT_EQ(
        terminal->session_disposition,
        SessionDisposition::Clean);
    ASSERT_EQ(terminal->cleanup_receipts.size(), 1u);
    EXPECT_EQ(
        terminal->cleanup_receipts.front(),
        (CleanupReceipt{
            ProgramResourceHandleId(70),
            ProgramCleanupStatus::CleanWithDiagnostics,
            "cleanup witness"}));
}

TEST(ProgramExecutor, CleanupTaintIsMonotonicThroughFinishInvocation)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(31),
    };
    const ExactDependencyIdentity cleanup_action{
        "test.executor.cleanup-taint",
        1,
        Hash(32),
    };
    const auto verified = CleanupProgram(
        "test.executor.cleanup-taint",
        pack,
        cleanup_action);
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult open = executor.Pump();
    ASSERT_TRUE(open.host_request);
    ASSERT_EQ(
        open.host_request->operation,
        ProgramHostOperation::OpenScope);
    const ProgramActionRequestId open_id(750);
    ASSERT_TRUE(executor.BindPendingAction(open_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            open_id,
            ProgramHostOperation::OpenScope)));

    ASSERT_TRUE(executor.Pump(2).runnable);
    ASSERT_TRUE(executor.RequestCancellation(
        CancellationReason::ExternalRequest));
    ProgramExecutorPumpResult cleanup = executor.Pump();
    ASSERT_TRUE(cleanup.host_request);
    ASSERT_EQ(
        cleanup.host_request->operation,
        ProgramHostOperation::InvokeAction);
    ASSERT_TRUE(cleanup.host_request->cleanup_only);
    const ProgramActionRequestId cleanup_id(751);
    ASSERT_TRUE(executor.BindPendingAction(cleanup_id));
    ProgramActionCompletion failed_cleanup = Completion(
        invocation,
        cleanup_id,
        ProgramHostOperation::InvokeAction,
        UnitGraph(),
        ProgramActionCompletionStatus::CleanupFailed);
    failed_cleanup.code = "cleanup_fault";
    failed_cleanup.message = "injected compensation failure";
    ASSERT_TRUE(executor.DeliverHostCompletion(
        std::move(failed_cleanup)));

    ProgramExecutorPumpResult close = executor.Pump();
    ASSERT_TRUE(close.host_request);
    ASSERT_EQ(
        close.host_request->operation,
        ProgramHostOperation::CloseScope);
    const ProgramActionRequestId close_id(752);
    ASSERT_TRUE(executor.BindPendingAction(close_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            close_id,
            ProgramHostOperation::CloseScope)));

    const auto terminal = CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        753);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::Cancelled);
    EXPECT_EQ(
        terminal->cleanup,
        ProgramCleanupStatus::Tainted);
    EXPECT_EQ(
        terminal->session_disposition,
        SessionDisposition::Tainted);
    ASSERT_FALSE(terminal->diagnostics.empty());
    EXPECT_EQ(
        terminal->diagnostics.back().code,
        "cleanup_fault");
}

TEST(ProgramExecutor, FailedScopeCloseIsRetiredAndCannotLoopForever)
{
    const CapabilityPackIdentity pack{
        "test.executor.pack",
        1,
        Hash(33),
    };
    const ExactDependencyIdentity cleanup_action{
        "test.executor.close-failure",
        1,
        Hash(34),
    };
    const auto verified = CleanupProgram(
        "test.executor.close-failure",
        pack,
        cleanup_action);
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph());
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult open = executor.Pump();
    ASSERT_TRUE(open.host_request);
    const ProgramActionRequestId open_id(760);
    ASSERT_TRUE(executor.BindPendingAction(open_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            open_id,
            ProgramHostOperation::OpenScope)));

    ASSERT_TRUE(executor.Pump(2).runnable);
    ASSERT_TRUE(executor.RequestCancellation(
        CancellationReason::ExternalRequest));
    ProgramExecutorPumpResult cleanup = executor.Pump();
    ASSERT_TRUE(cleanup.host_request);
    const ProgramActionRequestId cleanup_id(761);
    ASSERT_TRUE(executor.BindPendingAction(cleanup_id));
    ASSERT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            cleanup_id,
            ProgramHostOperation::InvokeAction)));

    ProgramExecutorPumpResult close = executor.Pump();
    ASSERT_TRUE(close.host_request);
    ASSERT_EQ(
        close.host_request->operation,
        ProgramHostOperation::CloseScope);
    const ProgramActionRequestId close_id(762);
    ASSERT_TRUE(executor.BindPendingAction(close_id));
    ProgramActionCompletion failed_close = Completion(
        invocation,
        close_id,
        ProgramHostOperation::CloseScope,
        UnitGraph(),
        ProgramActionCompletionStatus::CleanupFailed);
    failed_close.code = "close_fault";
    failed_close.message = "injected scope close failure";
    ASSERT_TRUE(executor.DeliverHostCompletion(
        std::move(failed_close)));

    // The next cleanup request is the invocation finish, not another close
    // for the already-failed lexical scope.
    ProgramExecutorPumpResult finish = executor.Pump();
    ASSERT_TRUE(finish.host_request);
    EXPECT_EQ(
        finish.host_request->operation,
        ProgramHostOperation::FinishInvocation);
    const auto terminal = CompleteFinish(
        executor,
        invocation,
        std::move(finish),
        763);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->cleanup,
        ProgramCleanupStatus::Tainted);
    EXPECT_EQ(
        terminal->session_disposition,
        SessionDisposition::Tainted);
}

TEST(ProgramExecutor, InstructionBudgetFailureStillFinishesInvocation)
{
    auto limits = Budgets(3);
    auto verified = Verified(BaseModule(
        "test.executor.instruction-budget",
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .terminator = Terminator{
                    .kind = TerminatorKind::Branch,
                    .source_location = ProgramSourceLocationId(1),
                    .edges = {{ProgramBlockId(1), {}}},
                },
            },
        },
        limits));
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph(), limits);
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult finish = executor.Pump();
    const auto terminal =
        CompleteFinish(executor, invocation, std::move(finish), 800);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::BudgetExhausted);
    ASSERT_FALSE(terminal->diagnostics.empty());
    EXPECT_EQ(
        terminal->diagnostics.back().code,
        "instruction_budget");
}

TEST(ProgramExecutor, IntegerComparisonRemainsExactAboveDoublePrecision)
{
    constexpr std::uint64_t lower = (std::uint64_t{1} << 53);
    constexpr std::uint64_t upper = lower + 1;

    const auto less = RunNumericBinary(
        BuiltinType::U64,
        lower,
        upper,
        InstructionOpcode::Less);
    ASSERT_TRUE(less);
    ASSERT_TRUE(less->output);
    ASSERT_NE(ScalarValue<bool>(*less->output), nullptr);
    EXPECT_TRUE(*ScalarValue<bool>(*less->output));

    const auto equal = RunNumericBinary(
        BuiltinType::U64,
        lower,
        upper,
        InstructionOpcode::Equal);
    ASSERT_TRUE(equal);
    ASSERT_TRUE(equal->output);
    ASSERT_NE(ScalarValue<bool>(*equal->output), nullptr);
    EXPECT_FALSE(*ScalarValue<bool>(*equal->output));
}

TEST(ProgramExecutor, CheckedIntegerOperationsHonorFixedWidthBoundaries)
{
    const auto add_overflow = RunNumericBinary(
        BuiltinType::U64,
        std::numeric_limits<std::uint64_t>::max(),
        std::uint64_t{1},
        InstructionOpcode::AddChecked);
    ASSERT_TRUE(add_overflow);
    EXPECT_EQ(
        add_overflow->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);

    const auto multiply_overflow = RunNumericBinary(
        BuiltinType::I64,
        std::numeric_limits<std::int64_t>::max(),
        std::int64_t{2},
        InstructionOpcode::MultiplyChecked);
    ASSERT_TRUE(multiply_overflow);
    EXPECT_EQ(
        multiply_overflow->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);

    const auto divide_overflow = RunNumericBinary(
        BuiltinType::I64,
        std::numeric_limits<std::int64_t>::lowest(),
        std::int64_t{-1},
        InstructionOpcode::DivideChecked);
    ASSERT_TRUE(divide_overflow);
    EXPECT_EQ(
        divide_overflow->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);

    const auto exact_remainder = RunNumericBinary(
        BuiltinType::I64,
        std::numeric_limits<std::int64_t>::lowest(),
        std::int64_t{-1},
        InstructionOpcode::RemainderChecked);
    ASSERT_TRUE(exact_remainder);
    EXPECT_EQ(
        exact_remainder->infrastructure,
        ProgramInfrastructureStatus::Completed);
    ASSERT_TRUE(exact_remainder->output);
    ASSERT_NE(
        ScalarValue<std::int64_t>(*exact_remainder->output),
        nullptr);
    EXPECT_EQ(
        *ScalarValue<std::int64_t>(*exact_remainder->output),
        0);
}

TEST(ProgramExecutor, CheckedIntegerConversionDoesNotRoundThroughDouble)
{
    constexpr std::int64_t exact = (std::int64_t{1} << 53) + 1;
    const auto converted = RunNumericConversion(
        BuiltinType::I64,
        exact,
        BuiltinType::U64);
    ASSERT_TRUE(converted);
    EXPECT_EQ(
        converted->infrastructure,
        ProgramInfrastructureStatus::Completed);
    ASSERT_TRUE(converted->output);
    ASSERT_NE(
        ScalarValue<std::uint64_t>(*converted->output),
        nullptr);
    EXPECT_EQ(
        *ScalarValue<std::uint64_t>(*converted->output),
        static_cast<std::uint64_t>(exact));

    const auto rejected = RunNumericConversion(
        BuiltinType::U64,
        std::numeric_limits<std::uint64_t>::max(),
        BuiltinType::I64);
    ASSERT_TRUE(rejected);
    EXPECT_EQ(
        rejected->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);

    const auto rounded_float = RunNumericConversion(
        BuiltinType::I64,
        exact,
        BuiltinType::F64);
    ASSERT_TRUE(rounded_float);
    EXPECT_EQ(
        rounded_float->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);
}

TEST(ProgramExecutor, InputByteBudgetUsesArenaMeasurementAtBoundary)
{
    const ProgramValueGraph input = UnitGraph();
    const ProgramValueGraphMeasureResult measured =
        MeasureProgramValueGraph(
            input,
            {1, std::numeric_limits<std::uint64_t>::max()});
    ASSERT_TRUE(measured);
    ASSERT_GT(measured.value_bytes, 0u);

    auto limits = Budgets();
    limits.maximum_values = measured.value_count;
    limits.maximum_value_bytes = measured.value_bytes;
    auto verified = Verified(BaseModule(
        "test.executor.input-byte-boundary",
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(1),
                },
            },
        },
        limits));

    ProgramInvocation accepted =
        Invocation(*verified, input, limits);
    CancellationSource accepted_cancellation(
        accepted.invocation_id);
    ProgramExecutor accepted_executor;
    EXPECT_TRUE(accepted_executor.Start(
        verified,
        accepted,
        accepted_cancellation.token()));

    ProgramInvocation rejected = accepted;
    rejected.invocation_id = InvocationId(accepted.invocation_id.value() + 1);
    --rejected.limits.maximum_value_bytes;
    CancellationSource rejected_cancellation(
        rejected.invocation_id);
    ProgramExecutor rejected_executor;
    EXPECT_FALSE(rejected_executor.Start(
        verified,
        rejected,
        rejected_cancellation.token()));
}

TEST(ProgramExecutor, RecordProjectionUsesVerifiedFieldSelector)
{
    const SchemaIdentity record{"test.executor.record", 1, Hash(70)};
    ProgramModule module = BaseModule(
        "test.executor.record-selector",
        TypeRef::Named(record),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2),
                            TypeRef::Builtin(BuiltinType::Bool)},
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::Bool),
                            true},
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::RecordProject,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(3),
                            TypeRef::Builtin(BuiltinType::U32)},
                        .operands = {ProgramValueId(1)},
                        .selector = "second",
                        .ordinal = 0,
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(3),
                    .return_value = ProgramValueId(3),
                    .domain_outcome = ProgramValueId(2),
                },
            },
        });
    module.local_types.push_back(TypeSchemaDefinition{
        .identity = record,
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"first", TypeRef::Builtin(BuiltinType::U32)},
            {"second", TypeRef::Builtin(BuiltinType::U32)},
        },
    });
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    const auto verified = Verified(std::move(module));
    const ProgramValueGraph input{
        .root = ProgramValueId(3),
        .values = {
            {ProgramValueId(1),
             TypeRef::Builtin(BuiltinType::U32),
             std::uint32_t(11)},
            {ProgramValueId(2),
             TypeRef::Builtin(BuiltinType::U32),
             std::uint32_t(22)},
            {ProgramValueId(3),
             TypeRef::Named(record),
             RecordValue{{
                 ProgramValueId(1),
                 ProgramValueId(2)}}},
        },
    };
    const ProgramInvocation invocation =
        Invocation(*verified, input);
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));
    const auto terminal = CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        960);
    ASSERT_TRUE(terminal);
    ASSERT_TRUE(terminal->output);
    ASSERT_NE(
        ScalarValue<std::uint32_t>(*terminal->output),
        nullptr);
    EXPECT_EQ(
        *ScalarValue<std::uint32_t>(*terminal->output),
        22u);
}

TEST(
    ProgramExecutor,
    ActionOutputRemainingBudgetFailureIsAcceptedIntoUnwind)
{
    const CapabilityPackIdentity pack{
        "test.executor.budget.pack", 1, Hash(71)};
    const ExactDependencyIdentity action{
        "test.executor.budget.action", 1, Hash(72)};
    ActionDescriptor descriptor{
        .identity = action,
        .providing_pack = pack,
        .input_type = TypeRef::Builtin(BuiltinType::Unit),
        .output_type = TypeRef::Builtin(BuiltinType::U32),
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
    };
    ProgramBudgets limits = Budgets();
    limits.maximum_values = 1;
    ProgramModule module = BaseModule(
        "test.executor.action-output-budget",
        TypeRef::Builtin(BuiltinType::Unit),
        TypeRef::Builtin(BuiltinType::U32),
        TypeRef::Builtin(BuiltinType::Bool),
        {
            BasicBlock{
                .id = ProgramBlockId(1),
                .instructions = {
                    Instruction{
                        .id = ProgramInstructionId(1),
                        .opcode = InstructionOpcode::AwaitAction,
                        .source_location = ProgramSourceLocationId(1),
                        .result = ValueDefinition{
                            ProgramValueId(2),
                            TypeRef::Builtin(BuiltinType::U32)},
                        .operands = {ProgramValueId(1)},
                        .target = {
                            .kind = InstructionTargetKind::Action,
                            .dependency = action,
                        },
                    },
                    Instruction{
                        .id = ProgramInstructionId(2),
                        .opcode = InstructionOpcode::Constant,
                        .source_location = ProgramSourceLocationId(2),
                        .result = ValueDefinition{
                            ProgramValueId(3),
                            TypeRef::Builtin(BuiltinType::Bool)},
                        .literal = LiteralValue{
                            TypeRef::Builtin(BuiltinType::Bool),
                            true},
                    },
                },
                .terminator = Terminator{
                    .kind = TerminatorKind::Return,
                    .source_location = ProgramSourceLocationId(3),
                    .return_value = ProgramValueId(2),
                    .domain_outcome = ProgramValueId(3),
                },
            },
        },
        limits);
    module.action_imports = {action};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    const auto verified = Verified(
        std::move(module),
        std::pair{action, descriptor});
    const ProgramInvocation invocation =
        Invocation(*verified, UnitGraph(), limits);
    CancellationSource cancellation(invocation.invocation_id);
    ProgramExecutor executor;
    ASSERT_TRUE(executor.Start(
        verified,
        invocation,
        cancellation.token()));

    ProgramExecutorPumpResult suspended = executor.Pump();
    ASSERT_TRUE(suspended.host_request);
    const ProgramActionRequestId request_id(961);
    ASSERT_TRUE(executor.BindPendingAction(request_id));
    EXPECT_TRUE(executor.DeliverHostCompletion(
        Completion(
            invocation,
            request_id,
            ProgramHostOperation::InvokeAction,
            ScalarGraph(
                BuiltinType::U32,
                std::uint32_t(9)))));
    EXPECT_EQ(
        executor.snapshot().activity,
        ProgramExecutorActivity::Unwinding);

    const auto terminal = CompleteFinish(
        executor,
        invocation,
        executor.Pump(),
        962);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(
        terminal->infrastructure,
        ProgramInfrastructureStatus::BudgetExhausted);
    ASSERT_FALSE(terminal->diagnostics.empty());
    EXPECT_EQ(terminal->diagnostics.back().code, "value_budget");
}

} // namespace
