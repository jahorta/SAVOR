#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {

using namespace savor::runtime::program;

ContentHash256 Hash(Byte value)
{
    ContentHash256 result;
    result.bytes.fill(value);
    return result;
}

SchemaIdentity Schema(
    std::string id,
    Byte hash,
    std::uint32_t version = 1)
{
    return {std::move(id), version, Hash(hash)};
}

CapabilityPackIdentity Pack(
    std::string id,
    Byte hash,
    std::uint32_t version = 1)
{
    return {std::move(id), version, Hash(hash)};
}

ExactDependencyIdentity Dependency(
    std::string id,
    Byte hash,
    std::uint32_t version = 1)
{
    return {std::move(id), version, Hash(hash)};
}

TypeSchemaDefinition StringSchema(
    std::string id,
    Byte hash)
{
    return {
        .identity = Schema(std::move(id), hash),
        .kind = TypeSchemaKind::BoundedUtf8String,
        .maximum_size = 64,
    };
}

ActionDescriptor Action(
    ExactDependencyIdentity identity,
    CapabilityPackIdentity pack)
{
    return {
        .identity = std::move(identity),
        .providing_pack = std::move(pack),
        .input_type = TypeRef::Builtin(BuiltinType::U32),
        .output_type = TypeRef::Builtin(BuiltinType::Bool),
        .required_services =
            ServiceMask(SessionServiceCapability::Execution),
        .effects = EffectMask(ActionEffect::AdvanceEmulation),
        .timing = ActionTimingClass::BoundedHostOperation,
        .default_host_timeout_milliseconds = 1000,
        .diagnostic_categories = {"timeout"},
    };
}

RuntimeCompatibility Compatibility()
{
    return {
        .game_id = "GSOE8P",
        .executable_identity = "main.dol-sha256",
        .address_map_revision = "usa-v1",
    };
}

CapabilityPackManifest Manifest(
    CapabilityPackIdentity identity)
{
    return {
        .identity = std::move(identity),
        .compatibility = Compatibility(),
    };
}

TEST(TypeSchemaRegistry, RegistersExactClosureAtomically)
{
    TypeSchemaRegistry registry;
    const SchemaIdentity text = Schema("test.text", 1);
    const SchemaIdentity record = Schema("test.record", 2);

    TypeSchemaDefinition record_definition{
        .identity = record,
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"name", TypeRef::Named(text)},
        },
    };
    ASSERT_TRUE(
        registry
            .RegisterBatch(
                {StringSchema("test.text", 1), record_definition})
            .success);

    RegistryError error;
    const auto closure = registry.ResolveClosure({record}, &error);
    ASSERT_TRUE(closure.has_value()) << error.message;
    ASSERT_EQ(closure->size(), 2u);
    EXPECT_EQ(closure->front().identity, text);
    EXPECT_EQ(closure->back().identity, record);
    EXPECT_EQ(registry.size(), 2u);

    const SchemaIdentity missing = Schema("test.missing", 3);
    TypeSchemaDefinition invalid{
        .identity = Schema("test.invalid", 4),
        .kind = TypeSchemaKind::Record,
        .record_fields = {
            {"missing", TypeRef::Named(missing)},
        },
    };
    const RegistryResult rejected =
        registry.Register(std::move(invalid));
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.error.code, RegistryErrorCode::DependencyMissing);
    EXPECT_EQ(registry.size(), 2u);
}

TEST(TypeSchemaRegistry, RejectsRecursiveSchemasAndIdentityConflicts)
{
    TypeSchemaRegistry registry;
    const SchemaIdentity left = Schema("test.left", 1);
    const SchemaIdentity right = Schema("test.right", 2);
    const RegistryResult cycle = registry.RegisterBatch({
        TypeSchemaDefinition{
            .identity = left,
            .kind = TypeSchemaKind::Record,
            .record_fields = {{"right", TypeRef::Named(right)}},
        },
        TypeSchemaDefinition{
            .identity = right,
            .kind = TypeSchemaKind::Record,
            .record_fields = {{"left", TypeRef::Named(left)}},
        },
    });
    EXPECT_FALSE(cycle.success);
    EXPECT_EQ(cycle.error.code, RegistryErrorCode::DependencyCycle);
    EXPECT_EQ(registry.size(), 0u);

    ASSERT_TRUE(registry.Register(StringSchema("test.text", 4)).success);
    TypeSchemaDefinition conflict = StringSchema("test.text", 5);
    conflict.identity.version = 1;
    EXPECT_EQ(
        registry.Register(std::move(conflict)).error.code,
        RegistryErrorCode::IdentityConflict);
}

TEST(ActionRegistry, ResolvesOnlyExactActionsAndReducers)
{
    ActionRegistry registry;
    const auto pack = Pack("test.pack", 1);
    const auto action_id = Dependency("test.action", 2);
    ASSERT_TRUE(registry.RegisterAction(Action(action_id, pack)).success);

    EXPECT_NE(registry.ResolveAction(action_id), nullptr);
    auto wrong_hash = action_id;
    wrong_hash.signature_hash = Hash(9);
    EXPECT_EQ(registry.ResolveAction(wrong_hash), nullptr);

    const auto reducer_id = Dependency("test.reducer", 3);
    ReducerDescriptor reducer{
        .identity = reducer_id,
        .providing_pack = pack,
        .input_types = {TypeRef::Builtin(BuiltinType::U32)},
        .output_type = TypeRef::Builtin(BuiltinType::Bool),
        .permitted_actions = {action_id},
        .maximum_steps = 32,
        .maximum_value_bytes = 1024,
    };
    ASSERT_TRUE(registry.RegisterReducer(reducer).success);
    EXPECT_NE(registry.ResolveReducer(reducer_id), nullptr);

    auto missing = reducer;
    missing.identity = Dependency("test.reducer.missing", 4);
    missing.permitted_actions = {Dependency("test.unknown", 5)};
    const RegistryResult rejected =
        registry.RegisterReducer(std::move(missing));
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.error.code, RegistryErrorCode::DependencyMissing);
    EXPECT_EQ(registry.reducer_count(), 1u);
}

TEST(ActionRegistry, CanonicalCatalogIdentitiesAreStableAndDistinct)
{
    const auto continue_until = CanonicalActionIdentity(
        CanonicalAction::ExecutionContinueUntil);
    EXPECT_EQ(
        continue_until.canonical_id,
        "runtime.execution.continue_until");
    EXPECT_EQ(continue_until.version, 1u);
    EXPECT_FALSE(continue_until.signature_hash.empty());
    EXPECT_NE(
        continue_until,
        CanonicalActionIdentity(CanonicalAction::ExecutionStepFrames));
    EXPECT_EQ(
        CanonicalReducerIdentity(
            CanonicalReducer::BattlePrepareCommandInteraction)
            .canonical_id,
        "soa.battle.command_interaction.prepare");
}

TEST(ActionRegistry, DescriptorIdentityCoversEveryMaterialPolicyClass)
{
    const ActionDescriptor baseline =
        BuildCanonicalRuntimeActionDescriptors().front();
    EXPECT_EQ(
        baseline.identity.signature_hash,
        ComputeActionDescriptorContractHash(baseline));

    const auto changes_hash =
        [&](const std::function<void(ActionDescriptor&)>& mutate)
    {
        ActionDescriptor changed = baseline;
        mutate(changed);
        EXPECT_NE(
            ComputeActionDescriptorContractHash(changed),
            baseline.identity.signature_hash);
    };
    changes_hash([](ActionDescriptor& value) {
        value.input_type = TypeRef::Builtin(BuiltinType::Bool);
    });
    changes_hash([](ActionDescriptor& value) {
        value.output_type = TypeRef::Builtin(BuiltinType::U64);
    });
    changes_hash([](ActionDescriptor& value) {
        value.domain_observation_type = value.domain_observation_type
            ? std::optional<TypeRef>{}
            : std::optional<TypeRef>{
                  TypeRef::Builtin(BuiltinType::Bool)};
    });
    changes_hash([](ActionDescriptor& value) {
        value.receipt_type = value.receipt_type
            ? std::optional<TypeRef>{}
            : std::optional<TypeRef>{
                  TypeRef::Builtin(BuiltinType::U64)};
    });
    changes_hash([](ActionDescriptor& value) {
        value.diagnostic_type = value.diagnostic_type
            ? std::optional<TypeRef>{}
            : std::optional<TypeRef>{
                  TypeRef::Builtin(BuiltinType::I32)};
    });
    changes_hash([](ActionDescriptor& value) {
        value.required_services ^=
            ServiceMask(SessionServiceCapability::Execution);
    });
    changes_hash([](ActionDescriptor& value) {
        value.effects ^= EffectMask(ActionEffect::ReadGuest);
    });
    changes_hash([](ActionDescriptor& value) {
        value.epoch_policy = value.epoch_policy ==
                ActionEpochPolicy::EpochAgnostic
            ? ActionEpochPolicy::RequiresCurrentEpoch
            : ActionEpochPolicy::EpochAgnostic;
    });
    changes_hash([](ActionDescriptor& value) {
        value.replay_class = value.replay_class ==
                ActionReplayClass::ExternalCommit
            ? ActionReplayClass::Deterministic
            : ActionReplayClass::ExternalCommit;
    });
    changes_hash([](ActionDescriptor& value) {
        value.cancellation = value.cancellation ==
                ActionCancellationMode::BeforeMutationOnly
            ? ActionCancellationMode::Cooperative
            : ActionCancellationMode::BeforeMutationOnly;
    });
    changes_hash([](ActionDescriptor& value) {
        ++value.maximum_non_cancellable_milliseconds;
    });
    changes_hash([](ActionDescriptor& value) {
        value.timing = value.timing ==
                ActionTimingClass::CancellationDriven
            ? ActionTimingClass::BoundedHostOperation
            : ActionTimingClass::CancellationDriven;
    });
    changes_hash([](ActionDescriptor& value) {
        ++value.default_host_timeout_milliseconds;
    });
    changes_hash([](ActionDescriptor& value) {
        value.resource_behavior = value.resource_behavior ==
                ActionResourceBehavior::Scoped
            ? ActionResourceBehavior::None
            : ActionResourceBehavior::Scoped;
    });
    changes_hash([](ActionDescriptor& value) {
        value.cleanup = value.cleanup ==
                ActionCleanupGuarantee::VerifiedCompensation
            ? ActionCleanupGuarantee::None
            : ActionCleanupGuarantee::VerifiedCompensation;
    });
    changes_hash([](ActionDescriptor& value) {
        value.taints_on_unproven_cleanup =
            !value.taints_on_unproven_cleanup;
    });
    changes_hash([](ActionDescriptor& value) {
        value.idempotency = value.idempotency ==
                ActionIdempotency::ReceiptProven
            ? ActionIdempotency::NotRetryable
            : ActionIdempotency::ReceiptProven;
    });
    changes_hash([](ActionDescriptor& value) {
        value.diagnostic_categories.push_back("new_category");
    });

    ActionDescriptor reordered = baseline;
    std::ranges::reverse(reordered.diagnostic_categories);
    EXPECT_EQ(
        ComputeActionDescriptorContractHash(reordered),
        baseline.identity.signature_hash);

    ActionDescriptor provider_rehashed = baseline;
    provider_rehashed.providing_pack.manifest_hash = Hash(0x7f);
    EXPECT_EQ(
        ComputeActionDescriptorContractHash(provider_rehashed),
        baseline.identity.signature_hash);
}

TEST(ActionRegistry, ReducerIdentityCoversOrderedTypesSelectionsAndBudgets)
{
    ReducerDescriptor baseline{
        .identity = Dependency("test.reducer.contract", 1),
        .providing_pack = Pack("test.pack", 2),
        .input_types = {
            TypeRef::Builtin(BuiltinType::U32),
            TypeRef::Builtin(BuiltinType::Bool),
        },
        .output_type = TypeRef::Builtin(BuiltinType::U64),
        .permitted_actions = {
            Dependency("test.action.a", 3),
            Dependency("test.action.b", 4),
        },
        .permitted_subprograms = {
            Dependency("test.program.a", 5),
            Dependency("test.program.b", 6),
        },
        .maximum_steps = 32,
        .maximum_value_bytes = 1024,
    };
    const ContentHash256 identity =
        ComputeReducerDescriptorContractHash(baseline);
    const auto changes_hash =
        [&](const std::function<void(ReducerDescriptor&)>& mutate)
    {
        ReducerDescriptor changed = baseline;
        mutate(changed);
        EXPECT_NE(
            ComputeReducerDescriptorContractHash(changed),
            identity);
    };
    changes_hash([](ReducerDescriptor& value) {
        std::ranges::reverse(value.input_types);
    });
    changes_hash([](ReducerDescriptor& value) {
        value.output_type = TypeRef::Builtin(BuiltinType::Bool);
    });
    changes_hash([](ReducerDescriptor& value) {
        value.permitted_actions.pop_back();
    });
    changes_hash([](ReducerDescriptor& value) {
        value.permitted_subprograms.pop_back();
    });
    changes_hash([](ReducerDescriptor& value) {
        ++value.maximum_steps;
    });
    changes_hash([](ReducerDescriptor& value) {
        ++value.maximum_value_bytes;
    });

    ReducerDescriptor reordered = baseline;
    std::ranges::reverse(reordered.permitted_actions);
    std::ranges::reverse(reordered.permitted_subprograms);
    EXPECT_EQ(
        ComputeReducerDescriptorContractHash(reordered),
        identity);
}

TEST(CapabilityPackRegistry, ManifestHashCoversCompleteNormalizedContract)
{
    CapabilityPackManifest baseline{
        .identity = {
            .canonical_id = "test.contract.pack",
            .version = 1,
        },
        .compatibility = Compatibility(),
        .dependencies = {
            Pack("test.dependency.a", 1),
            Pack("test.dependency.b", 2),
        },
        .schemas = {
            Schema("test.schema.a", 3),
            Schema("test.schema.b", 4),
        },
        .semantic_points = {
            {
                .canonical_id = "test.point.pc",
                .kind = SemanticPointKind::ProgramCounter,
                .pc = 0x80001000,
            },
            {
                .canonical_id = "test.point.memory",
                .kind = SemanticPointKind::Memory,
                .memory_address = 0x80300000,
                .memory_size = 4,
                .memory_read = true,
                .memory_write = true,
            },
        },
        .address_symbols = {{
            .canonical_id = "test.address",
            .address = 0x80300000,
            .value_type = TypeRef::Builtin(BuiltinType::U32),
        }},
        .coherent_queries = {{
            .canonical_id = "test.query",
            .result_schema = Schema("test.query.result", 5),
            .address_dependencies = {"test.z", "test.a"},
            .maximum_guest_reads = 8,
        }},
        .cpu_evaluators = {{
            .canonical_id = "test.evaluator",
            .routed_sample_descriptor_id = 17,
            .address_dependency = "test.address",
            .result_type = TypeRef::Builtin(BuiltinType::U32),
            .operations = {
                CpuEvaluatorOperation::ReadU32,
                CpuEvaluatorOperation::CompareEqual,
            },
            .maximum_reads = 2,
            .maximum_output_bytes = 4,
        }},
        .actions = {
            Dependency("test.action.a", 6),
            Dependency("test.action.b", 7),
        },
        .reducers = {
            Dependency("test.reducer.a", 8),
            Dependency("test.reducer.b", 9),
        },
    };
    const ContentHash256 identity =
        ComputeCapabilityPackManifestContractHash(baseline);
    const auto changes_hash =
        [&](const std::function<void(CapabilityPackManifest&)>& mutate)
    {
        CapabilityPackManifest changed = baseline;
        mutate(changed);
        EXPECT_NE(
            ComputeCapabilityPackManifestContractHash(changed),
            identity);
    };
    changes_hash([](CapabilityPackManifest& value) {
        value.compatibility.address_map_revision += ".changed";
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.dependencies.front().manifest_hash = Hash(0x31);
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.schemas.front().schema_hash = Hash(0x32);
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.semantic_points.front().pc += 4;
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.semantic_points.back().memory_write = false;
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.address_symbols.front().value_type =
            TypeRef::Builtin(BuiltinType::U16);
    });
    changes_hash([](CapabilityPackManifest& value) {
        ++value.coherent_queries.front().maximum_guest_reads;
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.coherent_queries.front().result_schema.schema_hash =
            Hash(0x33);
    });
    changes_hash([](CapabilityPackManifest& value) {
        std::ranges::reverse(
            value.cpu_evaluators.front().operations);
    });
    changes_hash([](CapabilityPackManifest& value) {
        ++value.cpu_evaluators.front().maximum_reads;
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.actions.pop_back();
    });
    changes_hash([](CapabilityPackManifest& value) {
        value.reducers.pop_back();
    });

    CapabilityPackManifest reordered = baseline;
    std::ranges::reverse(reordered.dependencies);
    std::ranges::reverse(reordered.schemas);
    std::ranges::reverse(reordered.semantic_points);
    std::ranges::reverse(reordered.actions);
    std::ranges::reverse(reordered.reducers);
    std::ranges::reverse(
        reordered.coherent_queries.front().address_dependencies);
    EXPECT_EQ(
        ComputeCapabilityPackManifestContractHash(reordered),
        identity);
}

TEST(ActionRegistry, Sap1RepresentationPreservesNominalActionTypes)
{
    const SchemaIdentity write_data =
        *CanonicalActionInputSchemaIdentity(
            CanonicalAction::GuestWriteData);
    const SchemaIdentity patch_executable =
        *CanonicalActionInputSchemaIdentity(
            CanonicalAction::GuestPatchExecutable);
    ASSERT_NE(write_data, patch_executable);

    CanonicalActionPayload payload;
    ASSERT_TRUE(payload.AddUnsigned(
        CanonicalActionPayloadField::Address,
        0x803469a8u));
    const CanonicalActionPayloadResult encoded =
        EncodeCanonicalActionPayload(payload, write_data);
    ASSERT_TRUE(encoded.ok) << encoded.diagnostic;

    CanonicalActionPayload decoded;
    std::string diagnostic;
    EXPECT_TRUE(DecodeCanonicalActionPayload(
        encoded.graph,
        write_data,
        decoded,
        &diagnostic))
        << diagnostic;
    EXPECT_EQ(
        decoded.Unsigned(CanonicalActionPayloadField::Address),
        0x803469a8u);

    CanonicalActionPayload wrong_type;
    EXPECT_FALSE(DecodeCanonicalActionPayload(
        encoded.graph,
        patch_executable,
        wrong_type,
        &diagnostic));
}

TEST(CapabilityPackRegistry, RejectsZeroPcWithoutPublishing)
{
    CapabilityPackRegistry registry;
    auto manifest = Manifest(Pack("test.zero", 1));
    manifest.semantic_points.push_back(
        SemanticPointDescriptor{
            .canonical_id = "test.zero.point",
            .kind = SemanticPointKind::ProgramCounter,
            .pc = 0,
        });
    const RegistryResult result =
        registry.Register(std::move(manifest));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.code, RegistryErrorCode::InvalidArgument);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(CapabilityPackRegistry, ResolvesExactDependencyClosureOnly)
{
    CapabilityPackRegistry registry;
    auto base = Manifest(Pack("test.base", 1));
    base.semantic_points.push_back(
        SemanticPointDescriptor{
            .canonical_id = "test.base.frame",
            .kind = SemanticPointKind::ProgramCounter,
            .pc = 0x801dc288,
        });
    auto dependent = Manifest(Pack("test.dependent", 2));
    dependent.dependencies.push_back(base.identity);
    auto unrelated = Manifest(Pack("test.unrelated", 3));

    ASSERT_TRUE(
        registry
            .RegisterBatch({base, dependent, unrelated})
            .success);
    RegistryError error;
    const auto closure = registry.ResolveClosure(
        {dependent.identity},
        Compatibility(),
        &error);
    ASSERT_TRUE(closure.has_value()) << error.message;
    ASSERT_EQ(closure->dependency_order.size(), 2u);
    EXPECT_EQ(
        closure->dependency_order[0].identity,
        base.identity);
    EXPECT_EQ(
        closure->dependency_order[1].identity,
        dependent.identity);

    RuntimeCompatibility wrong = Compatibility();
    wrong.executable_identity = "other";
    EXPECT_FALSE(
        registry.ResolveClosure(
            {dependent.identity},
            wrong,
            &error)
            .has_value());
    EXPECT_EQ(error.code, RegistryErrorCode::CompatibilityMismatch);
}

} // namespace
