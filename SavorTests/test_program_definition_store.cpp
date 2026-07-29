#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"

#include <cstdint>
#include <vector>

namespace {

using namespace savor::runtime::program;

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

ProgramModule Module(std::string id)
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::move(id),
            .revision = 1,
        },
        .ir_version = kCanonicalIrVersionV1,
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type = TypeRef::Builtin(BuiltinType::U32),
                .output_type = TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
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
        .budgets = Budgets(),
        .source_map = {
            .version = 1,
            .entries = {
                {ProgramSourceLocationId(1),
                 {},
                 {},
                 {},
                 "store.test",
                 "constant"},
                {ProgramSourceLocationId(2),
                 {},
                 {},
                 {},
                 "store.test",
                 "return"},
            },
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

TEST(ProgramDefinitionStore, RegistersAndResolvesExactIdentity)
{
    ProgramDefinitionStore store;
    const ProgramModule module = Module("test.store");
    const ModuleStoreResult registered =
        store.RegisterCompiled(module);
    ASSERT_TRUE(registered.success) << registered.error.message;
    ASSERT_NE(store.Resolve(module.identity), nullptr);

    ModuleIdentity wrong = module.identity;
    wrong.module_hash.bytes[0] ^= 0xff;
    EXPECT_EQ(store.Resolve(wrong), nullptr);
    EXPECT_EQ(store.size(), 1u);

    ProgramModule conflict = module;
    conflict.budgets.maximum_calls++;
    conflict.identity.module_hash =
        ComputeProgramModuleHashV1(conflict);
    const ModuleStoreResult rejected =
        store.RegisterCompiled(std::move(conflict));
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(
        rejected.error.code,
        RegistryErrorCode::IdentityConflict);
    EXPECT_EQ(store.size(), 1u);
}

TEST(ProgramDefinitionStore, RegistersDependencyBatchAtomically)
{
    ProgramDefinitionStore store;
    ProgramModule dependency = Module("test.dependency");
    ProgramModule root = Module("test.root");
    root.module_imports.push_back(
        {dependency.identity, dependency.ir_version});
    root.identity.module_hash =
        ComputeProgramModuleHashV1(root);

    ASSERT_TRUE(
        store.RegisterCompiledBatch({root, dependency}).success);
    RegistryError error;
    const auto closure =
        store.ResolveClosure(root.identity, &error);
    ASSERT_TRUE(closure.has_value()) << error.message;
    ASSERT_EQ(closure->dependency_order.size(), 2u);
    EXPECT_EQ(
        closure->dependency_order[0]->identity,
        dependency.identity);
    EXPECT_EQ(
        closure->dependency_order[1]->identity,
        root.identity);

    ProgramDefinitionStore missing_store;
    const RegistryResult missing =
        missing_store.RegisterCompiledBatch({root});
    EXPECT_FALSE(missing.success);
    EXPECT_EQ(missing.error.code, RegistryErrorCode::DependencyMissing);
    EXPECT_EQ(missing_store.size(), 0u);
}

TEST(ProgramDefinitionStore, AcceptsOnlyCanonicalEncodedModules)
{
    ProgramDefinitionStore store;
    const ProgramModule module = Module("test.encoded");
    const EncodeResult encoded = EncodeProgramModuleV1(module);
    ASSERT_TRUE(encoded) << encoded.status.message;

    const ModuleStoreResult registered =
        store.RegisterEncoded(encoded.bytes, module.identity);
    ASSERT_TRUE(registered.success) << registered.error.message;

    std::vector<Byte> trailing = encoded.bytes;
    trailing.push_back(0);
    ProgramDefinitionStore rejected_store;
    const ModuleStoreResult rejected =
        rejected_store.RegisterEncoded(trailing);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected_store.size(), 0u);
}

} // namespace
