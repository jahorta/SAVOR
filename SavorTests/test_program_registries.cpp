#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"

#include <algorithm>
#include <cstdint>
#include <functional>
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
            CanonicalReducer::BattleMaterializeTurnInput)
            .canonical_id,
        "soa.battle.materialize_turn_input");
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
        value.domain_observation_type.reset();
    });
    changes_hash([](ActionDescriptor& value) {
        value.receipt_type.reset();
    });
    changes_hash([](ActionDescriptor& value) {
        value.diagnostic_type.reset();
    });
    changes_hash([](ActionDescriptor& value) {
        value.required_services ^=
            ServiceMask(SessionServiceCapability::Execution);
    });
    changes_hash([](ActionDescriptor& value) {
        value.effects ^= EffectMask(ActionEffect::ReadGuest);
    });
    changes_hash([](ActionDescriptor& value) {
        value.epoch_policy = ActionEpochPolicy::MayReplaceState;
    });
    changes_hash([](ActionDescriptor& value) {
        value.replay_class = ActionReplayClass::ExternalCommit;
    });
    changes_hash([](ActionDescriptor& value) {
        value.cancellation =
            ActionCancellationMode::BeforeMutationOnly;
    });
    changes_hash([](ActionDescriptor& value) {
        ++value.maximum_non_cancellable_milliseconds;
    });
    changes_hash([](ActionDescriptor& value) {
        value.timing = ActionTimingClass::CancellationDriven;
    });
    changes_hash([](ActionDescriptor& value) {
        ++value.default_host_timeout_milliseconds;
    });
    changes_hash([](ActionDescriptor& value) {
        value.resource_behavior = ActionResourceBehavior::Scoped;
    });
    changes_hash([](ActionDescriptor& value) {
        value.cleanup =
            ActionCleanupGuarantee::VerifiedCompensation;
    });
    changes_hash([](ActionDescriptor& value) {
        value.taints_on_unproven_cleanup =
            !value.taints_on_unproven_cleanup;
    });
    changes_hash([](ActionDescriptor& value) {
        value.idempotency = ActionIdempotency::ReceiptProven;
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

TEST(ActionRegistry, CanonicalRuntimeCatalogCoversEveryGenericAction)
{
    TypeSchemaRegistry schemas;
    ASSERT_TRUE(
        schemas
            .RegisterBatch(BuildCanonicalRuntimeActionSchemas())
            .success);
    ActionRegistry registry(&schemas);
    const std::vector<ActionDescriptor> descriptors =
        BuildCanonicalRuntimeActionDescriptors();
    ASSERT_EQ(descriptors.size(), 30u);
    EXPECT_TRUE(std::ranges::none_of(
        descriptors,
        [](const ActionDescriptor& descriptor)
        {
            return descriptor.identity.canonical_id ==
                "runtime.execution.step_instructions";
        }));
    for (std::size_t index = 0; index < descriptors.size(); ++index)
    {
        EXPECT_EQ(
            descriptors[index].identity,
            CanonicalActionIdentity(
                static_cast<CanonicalAction>(index)));
        EXPECT_EQ(
            descriptors[index].providing_pack,
            CanonicalRuntimePackIdentity());
        if (descriptors[index].timing ==
            ActionTimingClass::BoundedHostOperation)
        {
            EXPECT_NE(
                descriptors[index].default_host_timeout_milliseconds,
                0u);
            EXPECT_NE(
                std::ranges::find(
                    descriptors[index].diagnostic_categories,
                    "timeout"),
                descriptors[index].diagnostic_categories.end());
        }
        else
        {
            EXPECT_EQ(
                descriptors[index].default_host_timeout_milliseconds,
                0u);
            EXPECT_EQ(
                std::ranges::find(
                    descriptors[index].diagnostic_categories,
                    "timeout"),
                descriptors[index].diagnostic_categories.end());
        }
        EXPECT_EQ(
            descriptors[index].input_type,
            CanonicalActionInputType(
                static_cast<CanonicalAction>(index)));
        EXPECT_EQ(
            descriptors[index].output_type,
            CanonicalActionOutputType(
                static_cast<CanonicalAction>(index)));
    }
    ASSERT_TRUE(
        registry.RegisterCatalog(descriptors, {}).success);
    EXPECT_EQ(registry.action_count(), descriptors.size());

    const ActionDescriptor* patch = registry.ResolveAction(
        CanonicalActionIdentity(
            CanonicalAction::GuestPatchExecutable));
    ASSERT_NE(patch, nullptr);
    EXPECT_NE(
        patch->effects &
            EffectMask(ActionEffect::MutateGuest),
        0u);
    EXPECT_EQ(
        patch->cleanup,
        ActionCleanupGuarantee::VerifiedCompensation);
    EXPECT_TRUE(patch->taints_on_unproven_cleanup);

    const ActionDescriptor* capture = registry.ResolveAction(
        CanonicalActionIdentity(CanonicalAction::StateCapture));
    const ActionDescriptor* stop_group = registry.ResolveAction(
        CanonicalActionIdentity(
            CanonicalAction::StopPointsSubscribeGroup));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(stop_group, nullptr);
    EXPECT_NE(capture->input_type, stop_group->input_type);
    EXPECT_NE(capture->output_type, stop_group->output_type);
    ASSERT_TRUE(capture->output_type.named);
    const TypeSchemaDefinition* capture_handle =
        schemas.Resolve(*capture->output_type.named);
    ASSERT_NE(capture_handle, nullptr);
    EXPECT_EQ(
        capture_handle->kind,
        TypeSchemaKind::ResourceHandle);
    ASSERT_TRUE(capture_handle->element_type);
    EXPECT_EQ(
        capture_handle->element_type,
        TypeRef::Named(
            *CanonicalActionResourceContractSchemaIdentity(
                CanonicalAction::StateCapture)));

    const ActionDescriptor* save_artifact =
        registry.ResolveAction(CanonicalActionIdentity(
            CanonicalAction::StateSaveImmutableArtifact));
    ASSERT_NE(save_artifact, nullptr);
    ASSERT_TRUE(save_artifact->output_type.named);
    const TypeSchemaDefinition* pending_publication =
        schemas.Resolve(*save_artifact->output_type.named);
    ASSERT_NE(pending_publication, nullptr);
    EXPECT_EQ(
        pending_publication->kind,
        TypeSchemaKind::BoundedBytes);
    EXPECT_LE(
        pending_publication->maximum_size,
        64u * 1024u);
    EXPECT_NE(
        schemas.Resolve(
            *CanonicalActionArtifactPayloadSchemaIdentity(
                CanonicalAction::StateSaveImmutableArtifact)),
        nullptr);

    const auto finalize_output =
        CanonicalActionOutputSchemaIdentity(
            CanonicalAction::CaptureFinalize);
    ASSERT_TRUE(finalize_output);
    const TypeSchemaDefinition* artifact_list =
        schemas.Resolve(*finalize_output);
    ASSERT_NE(artifact_list, nullptr);
    ASSERT_TRUE(artifact_list->element_type);
    EXPECT_EQ(
        artifact_list->kind,
        TypeSchemaKind::BoundedList);
    EXPECT_EQ(
        artifact_list->element_type,
        TypeRef::Named(
            *CanonicalActionArtifactReferenceSchemaIdentity(
                CanonicalAction::CaptureFinalize)));

    const ActionDescriptor* read_u32 =
        registry.ResolveAction(CanonicalActionIdentity(
            CanonicalAction::GuestReadU32));
    ASSERT_NE(read_u32, nullptr);
    EXPECT_FALSE(read_u32->output_type.is_named());
    EXPECT_EQ(read_u32->output_type.builtin, BuiltinType::U32);
    EXPECT_EQ(
        CanonicalActionInputType(
            CanonicalAction::MovieStopPlayback),
        CanonicalActionOutputType(
            CanonicalAction::MovieStartPlayback));
    EXPECT_EQ(
        CanonicalActionInputType(
            CanonicalAction::CaptureFinalize),
        CanonicalActionOutputType(
            CanonicalAction::CaptureAttach));

    const auto require_record =
        [&](CanonicalAction action,
            std::vector<std::string> fields)
    {
        const auto identity =
            CanonicalActionInputSchemaIdentity(action);
        EXPECT_TRUE(identity);
        const TypeSchemaDefinition* schema =
            identity ? schemas.Resolve(*identity) : nullptr;
        EXPECT_NE(schema, nullptr);
        if (!schema)
            return;
        EXPECT_EQ(schema->kind, TypeSchemaKind::Record);
        std::vector<std::string> actual;
        for (const auto& field : schema->record_fields)
            actual.push_back(field.name);
        EXPECT_EQ(actual, fields);
    };
    require_record(
        CanonicalAction::ExecutionContinueUntil,
        {"wake_group", "input_publication", "static_config"});
    require_record(
        CanonicalAction::InputAwaitGuestPoll,
        {"lease", "input_publication",
         "neutral_witness", "static_config"});
    require_record(
        CanonicalAction::GuestReadU32,
        {"stop_receipt", "address", "static_config"});

    EXPECT_EQ(
        CanonicalActionOutputType(
            CanonicalAction::InputPublishHeld),
        CanonicalActionOutputType(
            CanonicalAction::InputPublishPulse));
    const auto continue_result =
        CanonicalActionOutputSchemaIdentity(
            CanonicalAction::ExecutionContinueUntil);
    ASSERT_TRUE(continue_result);
    const auto* receipt = schemas.Resolve(*continue_result);
    ASSERT_NE(receipt, nullptr);
    EXPECT_EQ(receipt->kind, TypeSchemaKind::Record);
    ASSERT_EQ(receipt->record_fields.size(), 5u);
    EXPECT_EQ(
        receipt->record_fields[0].name,
        "stop_sequence");
    EXPECT_EQ(receipt->record_fields[2].name, "pc");
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
                .kind = SemanticPointPhysicalKind::ProgramCounter,
                .pc = 0x80001000,
                .legacy_key = "legacy.pc",
            },
            {
                .canonical_id = "test.point.memory",
                .kind = SemanticPointPhysicalKind::Memory,
                .memory_address = 0x80300000,
                .memory_size = 4,
                .memory_read = true,
                .memory_write = true,
                .legacy_key = "legacy.memory",
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
            .kind = SemanticPointPhysicalKind::ProgramCounter,
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
            .kind = SemanticPointPhysicalKind::ProgramCounter,
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
