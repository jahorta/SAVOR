#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;

void AppendU64LittleEndian(
    std::vector<Byte>& bytes,
    std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        bytes.push_back(
            static_cast<Byte>(value >> shift));
    }
}

std::vector<Byte> EncodedBudgets(
    const ProgramBudgets& budgets)
{
    std::vector<Byte> bytes;
    bytes.reserve(9 * sizeof(std::uint64_t));
    AppendU64LittleEndian(bytes, budgets.maximum_instructions);
    AppendU64LittleEndian(bytes, budgets.maximum_calls);
    AppendU64LittleEndian(bytes, budgets.maximum_call_depth);
    AppendU64LittleEndian(bytes, budgets.maximum_action_requests);
    AppendU64LittleEndian(bytes, budgets.maximum_emissions);
    AppendU64LittleEndian(bytes, budgets.maximum_artifacts);
    AppendU64LittleEndian(bytes, budgets.maximum_values);
    AppendU64LittleEndian(bytes, budgets.maximum_value_bytes);
    AppendU64LittleEndian(bytes, budgets.maximum_trace_events);
    return bytes;
}

std::optional<std::size_t> FindUniqueBytes(
    const std::vector<Byte>& bytes,
    const std::vector<Byte>& pattern)
{
    const auto first = std::search(
        bytes.begin(),
        bytes.end(),
        pattern.begin(),
        pattern.end());
    if (first == bytes.end())
        return std::nullopt;
    const auto second = std::search(
        std::next(first),
        bytes.end(),
        pattern.begin(),
        pattern.end());
    if (second != bytes.end())
        return std::nullopt;
    return static_cast<std::size_t>(
        std::distance(bytes.begin(), first));
}

void RewriteEnvelopePayloadLength(std::vector<Byte>& bytes)
{
    const std::uint32_t payload_size =
        static_cast<std::uint32_t>(
            bytes.size() - kProgramCodecHeaderSizeV1);
    bytes[6] = static_cast<Byte>(payload_size);
    bytes[7] = static_cast<Byte>(payload_size >> 8);
    bytes[8] = static_cast<Byte>(payload_size >> 16);
    bytes[9] = static_cast<Byte>(payload_size >> 24);
}

void InsertLegacyActiveDeadline(
    std::vector<Byte>& bytes,
    std::size_t budget_offset,
    std::uint64_t active_deadline_milliseconds)
{
    std::vector<Byte> encoded_deadline;
    encoded_deadline.reserve(sizeof(std::uint64_t));
    AppendU64LittleEndian(
        encoded_deadline,
        active_deadline_milliseconds);
    bytes.insert(
        bytes.begin() + budget_offset +
            9 * sizeof(std::uint64_t),
        encoded_deadline.begin(),
        encoded_deadline.end());
    RewriteEnvelopePayloadLength(bytes);
}

ContentHash256 FilledHash(Byte value)
{
    ContentHash256 hash;
    hash.bytes.fill(value);
    return hash;
}

ProgramValueGraph ScalarGraph(
    ProgramValueId id,
    TypeRef type,
    ProgramValuePayload payload)
{
    return ProgramValueGraph{
        .root = id,
        .values = {
            ProgramValue{
                .id = id,
                .type = std::move(type),
                .payload = std::move(payload),
            },
        },
    };
}

ProgramModule MakeModule()
{
    ProgramModule module{
        .identity = ModuleIdentity{
            .canonical_id = "test.codec",
            .revision = 1,
        },
        .ir_version = kCanonicalIrVersionV1,
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type = TypeRef::Builtin(BuiltinType::U32),
                .output_type = TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
                .accepted_policies = ProgramPolicySet{
                    .state_policies = {
                        InvocationStatePolicy::ContinueSession,
                        InvocationStatePolicy::Boot,
                    },
                    .execution_intents = {ExecutionIntent::Live},
                },
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId(1),
                .name = "run",
                .arguments = {
                    ValueDefinition{
                        .id = ProgramValueId(1),
                        .type = TypeRef::Builtin(BuiltinType::U32),
                    },
                },
                .output_type = TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type = TypeRef::Builtin(BuiltinType::Bool),
                .entry_block = ProgramBlockId(1),
                .blocks = {
                    BasicBlock{
                        .id = ProgramBlockId(1),
                        .instructions = {
                            Instruction{
                                .id = ProgramInstructionId(1),
                                .opcode = InstructionOpcode::Constant,
                                .source_location = ProgramSourceLocationId(1),
                                .result = ValueDefinition{
                                    .id = ProgramValueId(2),
                                    .type = TypeRef::Builtin(BuiltinType::Bool),
                                },
                                .literal = LiteralValue{
                                    .type = TypeRef::Builtin(BuiltinType::Bool),
                                    .payload = true,
                                },
                            },
                        },
                        .terminator = Terminator{
                            .kind = TerminatorKind::Return,
                            .source_location = ProgramSourceLocationId(2),
                            .return_value = ProgramValueId(1),
                            .domain_outcome = ProgramValueId(2),
                        },
                    },
                },
                .exported = true,
            },
        },
        .action_imports = {
            ExactDependencyIdentity{
                .canonical_id = "runtime.z",
                .version = 1,
                .signature_hash = FilledHash(0x22),
            },
            ExactDependencyIdentity{
                .canonical_id = "runtime.a",
                .version = 1,
                .signature_hash = FilledHash(0x11),
            },
        },
        .accepted_policies = ProgramPolicySet{
            .state_policies = {
                InvocationStatePolicy::ContinueSession,
                InvocationStatePolicy::Boot,
            },
            .execution_intents = {ExecutionIntent::Live},
        },
        .budgets = ProgramBudgets{
            .maximum_instructions = 100,
            .maximum_calls = 10,
            .maximum_call_depth = 4,
            .maximum_action_requests = 5,
            .maximum_emissions = 3,
            .maximum_artifacts = 2,
            .maximum_values = 100,
            .maximum_value_bytes = 4096,
            .maximum_trace_events = 100,
        },
        .source_map = ProgramSourceMap{
            .version = 1,
            .entries = {
                SourceMapEntry{
                    .id = ProgramSourceLocationId(2),
                    .source_name = "codec.test",
                    .semantic_path = "return",
                    .line = 2,
                },
                SourceMapEntry{
                    .id = ProgramSourceLocationId(1),
                    .source_name = "codec.test",
                    .semantic_path = "constant",
                    .line = 1,
                },
            },
        },
    };
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module;
}

ProgramInvocation MakeInvocation(const ProgramModule& module)
{
    return ProgramInvocation{
        .invocation_id = InvocationId(10),
        .attempt_id = AttemptId(20),
        .module = module.identity,
        .entrypoint = "run",
        .dependencies = ProgramDependencyLock{
            .ir_version = 1,
            .action_imports = module.action_imports,
        },
        .runtime_profile = RuntimeProfile{
            .profile_id = "test",
            .game_id = "GSOE8P",
            .disc_identity = "disc",
            .executable_identity = "dol",
            .backend = "jit64",
        },
        .state = InvocationStateRequest{
            .policy = InvocationStatePolicy::ContinueSession,
            .session_lineage = "lineage",
            .expected_session = SessionId(30),
            .expected_epoch = StateEpoch(40),
        },
        .execution = InvocationExecutionPolicy{
            .intent = ExecutionIntent::Live,
        },
        .input = ScalarGraph(
            ProgramValueId(1),
            TypeRef::Builtin(BuiltinType::U32),
            std::uint32_t(42)),
        .limits = module.budgets,
        .provenance = ProgramProvenance{
            .requesting_component = "test",
            .attributes = {
                {"z", "last"},
                {"a", "first"},
            },
        },
    };
}

ArtifactReferenceValue SourceArtifact(
    std::string artifact_id,
    Byte hash_byte)
{
    return {
        .artifact_id = std::move(artifact_id),
        .schema = {
            .canonical_id = "test.source-artifact",
            .version = 1,
            .schema_hash = FilledHash(0x44),
        },
        .content_hash = FilledHash(hash_byte),
        .storage_reference = "memory://source",
        .complete = true,
    };
}

ProgramResult MakeResult(
    const ProgramModule& module,
    const ProgramInvocation& invocation)
{
    return {
        .invocation_id = invocation.invocation_id,
        .attempt_id = invocation.attempt_id,
        .module = module.identity,
        .entrypoint = invocation.entrypoint,
        .resolved_dependencies = invocation.dependencies,
        .infrastructure = ProgramInfrastructureStatus::Completed,
        .domain_outcome = ScalarGraph(
            ProgramValueId(1),
            TypeRef::Builtin(BuiltinType::Bool),
            false),
        .cleanup = ProgramCleanupStatus::Tainted,
        .session_disposition = SessionDisposition::Tainted,
        .output = ScalarGraph(
            ProgramValueId(1),
            TypeRef::Builtin(BuiltinType::U32),
            std::uint32_t(42)),
        .diagnostics = {
            ProgramDiagnostic{
                .severity = DiagnosticSeverity::Warning,
                .code = "cleanup.failed",
                .message = "session cannot be reused",
            },
        },
        .provenance = invocation.provenance,
    };
}

TEST(ProgramCanonicalCodecV1, EncodesSprmHeaderAndLittleEndianLength)
{
    const auto module = MakeModule();
    const auto encoded = EncodeProgramModuleV1(module);
    ASSERT_TRUE(encoded) << encoded.status.message;
    ASSERT_GE(encoded.bytes.size(), kProgramCodecHeaderSizeV1);

    EXPECT_EQ(
        std::vector<Byte>(encoded.bytes.begin(), encoded.bytes.begin() + 4),
        (std::vector<Byte>{'S', 'P', 'R', 'M'}));
    EXPECT_EQ(encoded.bytes[4], 1);
    EXPECT_EQ(encoded.bytes[5], 0);

    const std::uint32_t declared =
        encoded.bytes[6] |
        (static_cast<std::uint32_t>(encoded.bytes[7]) << 8) |
        (static_cast<std::uint32_t>(encoded.bytes[8]) << 16) |
        (static_cast<std::uint32_t>(encoded.bytes[9]) << 24);
    EXPECT_EQ(declared, encoded.bytes.size() - kProgramCodecHeaderSizeV1);
}

TEST(ProgramCanonicalCodecV1, RoundTripsCanonicalModuleAndValidatesHash)
{
    const auto module = MakeModule();
    ASSERT_TRUE(ValidateProgramModuleIdentityV1(module));

    const auto encoded = EncodeProgramModuleV1(module);
    ASSERT_TRUE(encoded) << encoded.status.message;
    const auto decoded = DecodeProgramModuleV1(encoded.bytes);
    ASSERT_TRUE(decoded) << decoded.status.message;
    EXPECT_EQ(decoded.value->identity, module.identity);
    ASSERT_EQ(decoded.value->action_imports.size(), 2u);
    EXPECT_EQ(decoded.value->action_imports[0].canonical_id, "runtime.a");
    EXPECT_EQ(decoded.value->source_map.entries[0].id,
              ProgramSourceLocationId(1));

    const auto reencoded = EncodeProgramModuleV1(*decoded.value);
    ASSERT_TRUE(reencoded);
    EXPECT_EQ(reencoded.bytes, encoded.bytes);
}

TEST(ProgramCanonicalCodecV1, CanonicalizesUnorderedDeclarations)
{
    auto first = MakeModule();
    auto second = first;
    std::ranges::reverse(second.action_imports);
    std::ranges::reverse(second.source_map.entries);
    second.accepted_policies.state_policies = {
        InvocationStatePolicy::Boot,
        InvocationStatePolicy::ContinueSession,
    };
    second.identity.module_hash = ComputeProgramModuleHashV1(second);

    EXPECT_EQ(first.identity.module_hash, second.identity.module_hash);
    EXPECT_EQ(
        EncodeProgramModuleV1(first).bytes,
        EncodeProgramModuleV1(second).bytes);
}

TEST(ProgramCanonicalCodecV1, HashIncludesBudgetsSourceMapAndIr)
{
    const auto base = MakeModule();

    auto budget = base;
    ++budget.budgets.maximum_instructions;
    EXPECT_NE(ComputeProgramModuleHashV1(base),
              ComputeProgramModuleHashV1(budget));

    auto source = base;
    source.source_map.entries[0].semantic_path += ".changed";
    EXPECT_NE(ComputeProgramModuleHashV1(base),
              ComputeProgramModuleHashV1(source));

    auto instruction = base;
    instruction.functions[0].blocks[0].instructions[0].literal =
        LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::Bool),
            .payload = false,
        };
    EXPECT_NE(ComputeProgramModuleHashV1(base),
              ComputeProgramModuleHashV1(instruction));
}

TEST(ProgramCanonicalCodecV1, RejectsMalformedEnvelopeBeforeProducingValue)
{
    const auto encoded = EncodeProgramModuleV1(MakeModule());
    ASSERT_TRUE(encoded);

    auto wrong_magic = encoded.bytes;
    wrong_magic[0] = 'X';
    EXPECT_EQ(
        DecodeProgramModuleV1(wrong_magic).status.error,
        CodecError::InvalidMagic);

    auto wrong_version = encoded.bytes;
    wrong_version[4] = 2;
    EXPECT_EQ(
        DecodeProgramModuleV1(wrong_version).status.error,
        CodecError::UnsupportedVersion);

    auto truncated = encoded.bytes;
    truncated.pop_back();
    EXPECT_EQ(
        DecodeProgramModuleV1(truncated).status.error,
        CodecError::Truncated);

    auto trailing = encoded.bytes;
    trailing.push_back(0);
    EXPECT_EQ(
        DecodeProgramModuleV1(trailing).status.error,
        CodecError::TrailingBytes);

    CodecLimits tiny;
    tiny.maximum_payload_bytes = 8;
    EXPECT_EQ(
        DecodeProgramModuleV1(encoded.bytes, tiny).status.error,
        CodecError::Oversized);
}

TEST(ProgramCanonicalCodecV1, RejectsHashMismatchAndDuplicateImports)
{
    auto module = MakeModule();
    module.identity.module_hash.bytes[0] ^= 0xff;
    const auto encoded = EncodeProgramModuleV1(module);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(
        DecodeProgramModuleV1(encoded.bytes).status.error,
        CodecError::HashMismatch);

    module = MakeModule();
    module.action_imports.push_back(module.action_imports.front());
    EXPECT_EQ(
        EncodeProgramModuleV1(module).status.error,
        CodecError::InvalidValue);
}

TEST(
    ProgramCanonicalCodecV1,
    RejectsLegacyBudgetBearingModuleV1Layout)
{
    ProgramModule module = MakeModule();
    module.budgets = ProgramBudgets{
        .maximum_instructions = 0x101,
        .maximum_calls = 0x202,
        .maximum_call_depth = 0x303,
        .maximum_action_requests = 0x404,
        .maximum_emissions = 0x505,
        .maximum_artifacts = 0x606,
        .maximum_values = 0x707,
        .maximum_value_bytes = 0x808,
        .maximum_trace_events = 0x909,
    };
    const std::vector<Byte> budget_bytes =
        EncodedBudgets(module.budgets);

    // Reconstruct the former canonical hash material exactly: v1 encoded
    // ProgramBudgets used to append active_deadline_milliseconds.
    EncodeResult legacy_hash_material =
        EncodeProgramModuleV1(
            module,
            CanonicalHashMode::OmitDeclaredHash);
    ASSERT_TRUE(legacy_hash_material)
        << legacy_hash_material.status.message;
    const auto hash_budget_offset = FindUniqueBytes(
        legacy_hash_material.bytes,
        budget_bytes);
    ASSERT_TRUE(hash_budget_offset);
    InsertLegacyActiveDeadline(
        legacy_hash_material.bytes,
        *hash_budget_offset,
        5000);

    const std::string legacy_hash_hex = hash::sha256(
        legacy_hash_material.bytes.data(),
        legacy_hash_material.bytes.size());
    const auto legacy_hash =
        ContentHash256::FromHex(legacy_hash_hex);
    ASSERT_TRUE(legacy_hash);
    module.identity.module_hash = *legacy_hash;

    EncodeResult legacy_module =
        EncodeProgramModuleV1(module);
    ASSERT_TRUE(legacy_module)
        << legacy_module.status.message;
    const auto module_budget_offset = FindUniqueBytes(
        legacy_module.bytes,
        budget_bytes);
    ASSERT_TRUE(module_budget_offset);
    InsertLegacyActiveDeadline(
        legacy_module.bytes,
        *module_budget_offset,
        5000);

    const DecodeResult<ProgramModule> rejected =
        DecodeProgramModuleV1(legacy_module.bytes);
    EXPECT_FALSE(rejected);
    EXPECT_FALSE(rejected.value);
}

TEST(
    ProgramCanonicalCodecV1,
    RejectsLegacyBudgetBearingInvocationV1Layout)
{
    const ProgramModule module = MakeModule();
    ProgramInvocation invocation = MakeInvocation(module);
    invocation.limits = ProgramBudgets{
        .maximum_instructions = 0x111,
        .maximum_calls = 0x222,
        .maximum_call_depth = 0x333,
        .maximum_action_requests = 0x444,
        .maximum_emissions = 0x555,
        .maximum_artifacts = 0x666,
        .maximum_values = 0x777,
        .maximum_value_bytes = 0x888,
        .maximum_trace_events = 0x999,
    };
    const std::vector<Byte> budget_bytes =
        EncodedBudgets(invocation.limits);

    EncodeResult legacy_invocation =
        EncodeProgramInvocationV1(invocation);
    ASSERT_TRUE(legacy_invocation)
        << legacy_invocation.status.message;
    const auto budget_offset = FindUniqueBytes(
        legacy_invocation.bytes,
        budget_bytes);
    ASSERT_TRUE(budget_offset);
    InsertLegacyActiveDeadline(
        legacy_invocation.bytes,
        *budget_offset,
        5000);

    const DecodeResult<ProgramInvocation> rejected =
        DecodeProgramInvocationV1(legacy_invocation.bytes);
    EXPECT_FALSE(rejected);
    EXPECT_FALSE(rejected.value);
}

TEST(ProgramCanonicalCodecV1, RejectsNonfiniteValuesAndOpenGraphs)
{
    auto invocation = MakeInvocation(MakeModule());
    invocation.input = ScalarGraph(
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::F64),
        std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(
        EncodeProgramInvocationV1(invocation).status.error,
        CodecError::InvalidValue);

    invocation = MakeInvocation(MakeModule());
    invocation.input.root = ProgramValueId(99);
    EXPECT_EQ(
        EncodeProgramInvocationV1(invocation).status.error,
        CodecError::InvalidValue);

    invocation = MakeInvocation(MakeModule());
    invocation.input.values.front().payload =
        OptionalValue{ProgramValueId(1)};
    EXPECT_EQ(
        EncodeProgramInvocationV1(invocation).status.error,
        CodecError::InvalidValue);

    invocation = MakeInvocation(MakeModule());
    invocation.input.values.push_back(ProgramValue{
        .id = ProgramValueId(2),
        .type = TypeRef::Builtin(BuiltinType::Unit),
        .payload = UnitValue{},
    });
    EXPECT_EQ(
        EncodeProgramInvocationV1(invocation).status.error,
        CodecError::InvalidValue);
}

TEST(ProgramCanonicalCodecV1, RoundTripsSpriAndSprrWithIndependentStatuses)
{
    const auto module = MakeModule();
    const auto invocation = MakeInvocation(module);
    const auto encoded_invocation = EncodeProgramInvocationV1(invocation);
    ASSERT_TRUE(encoded_invocation) << encoded_invocation.status.message;
    EXPECT_EQ(
        std::vector<Byte>(
            encoded_invocation.bytes.begin(),
            encoded_invocation.bytes.begin() + 4),
        (std::vector<Byte>{'S', 'P', 'R', 'I'}));
    const auto decoded_invocation =
        DecodeProgramInvocationV1(encoded_invocation.bytes);
    ASSERT_TRUE(decoded_invocation) << decoded_invocation.status.message;
    EXPECT_EQ(decoded_invocation.value->invocation_id, InvocationId(10));
    EXPECT_EQ(decoded_invocation.value->input.root, ProgramValueId(1));
    EXPECT_EQ(decoded_invocation.value->provenance.attributes[0].key, "a");

    ProgramResult result = MakeResult(module, invocation);
    const auto encoded_result = EncodeProgramResultV1(result);
    ASSERT_TRUE(encoded_result) << encoded_result.status.message;
    EXPECT_EQ(
        std::vector<Byte>(
            encoded_result.bytes.begin(),
            encoded_result.bytes.begin() + 4),
        (std::vector<Byte>{'S', 'P', 'R', 'R'}));
    const auto decoded_result = DecodeProgramResultV1(encoded_result.bytes);
    ASSERT_TRUE(decoded_result) << decoded_result.status.message;
    EXPECT_EQ(
        decoded_result.value->infrastructure,
        ProgramInfrastructureStatus::Completed);
    EXPECT_EQ(decoded_result.value->cleanup, ProgramCleanupStatus::Tainted);
    EXPECT_EQ(
        decoded_result.value->session_disposition,
        SessionDisposition::Tainted);

    ProgramResult internal_only = result;
    ArtifactReferenceValue incomplete =
        SourceArtifact("pending-state", 0x55);
    incomplete.complete = false;
    internal_only.artifacts.push_back({
        ProgramArtifactSequence(1),
        std::move(incomplete)});
    EXPECT_EQ(
        EncodeProgramResultV1(internal_only).status.error,
        CodecError::InvalidValue);
}

TEST(
    ProgramCanonicalCodecV1,
    CanonicalizesUniqueProvenanceArtifactsAndRejectsDuplicateIds)
{
    const ProgramModule module = MakeModule();
    ProgramInvocation invocation = MakeInvocation(module);
    invocation.provenance.source_artifacts = {
        SourceArtifact("artifact-b", 0x22),
        SourceArtifact("artifact-a", 0x11),
    };
    ProgramInvocation reordered = invocation;
    std::ranges::reverse(
        reordered.provenance.source_artifacts);

    const EncodeResult encoded =
        EncodeProgramInvocationV1(invocation);
    const EncodeResult reordered_encoded =
        EncodeProgramInvocationV1(reordered);
    ASSERT_TRUE(encoded) << encoded.status.message;
    ASSERT_TRUE(reordered_encoded)
        << reordered_encoded.status.message;
    EXPECT_EQ(encoded.bytes, reordered_encoded.bytes);
    const DecodeResult<ProgramInvocation> decoded =
        DecodeProgramInvocationV1(encoded.bytes);
    ASSERT_TRUE(decoded) << decoded.status.message;
    ASSERT_EQ(
        decoded.value->provenance.source_artifacts.size(),
        2u);
    EXPECT_EQ(
        decoded.value->provenance.source_artifacts[0]
            .artifact_id,
        "artifact-a");

    ProgramInvocation duplicate_invocation = invocation;
    duplicate_invocation.provenance.source_artifacts[1]
        .artifact_id = "artifact-b";
    EXPECT_EQ(
        EncodeProgramInvocationV1(duplicate_invocation)
            .status.error,
        CodecError::InvalidValue);

    std::vector<Byte> malformed_invocation = encoded.bytes;
    const std::string from = "artifact-b";
    const std::string to = "artifact-a";
    const auto invocation_id = std::search(
        malformed_invocation.begin(),
        malformed_invocation.end(),
        from.begin(),
        from.end());
    ASSERT_NE(invocation_id, malformed_invocation.end());
    std::copy(to.begin(), to.end(), invocation_id);
    EXPECT_EQ(
        DecodeProgramInvocationV1(malformed_invocation)
            .status.error,
        CodecError::InvalidValue);

    ProgramResult result = MakeResult(module, invocation);
    ProgramResult reordered_result =
        MakeResult(module, reordered);
    const EncodeResult encoded_result =
        EncodeProgramResultV1(result);
    const EncodeResult reordered_result_encoded =
        EncodeProgramResultV1(reordered_result);
    ASSERT_TRUE(encoded_result)
        << encoded_result.status.message;
    ASSERT_TRUE(reordered_result_encoded)
        << reordered_result_encoded.status.message;
    EXPECT_EQ(
        encoded_result.bytes,
        reordered_result_encoded.bytes);

    result.provenance.source_artifacts[1].artifact_id =
        "artifact-b";
    EXPECT_EQ(
        EncodeProgramResultV1(result).status.error,
        CodecError::InvalidValue);

    std::vector<Byte> malformed_result =
        encoded_result.bytes;
    const auto result_id = std::search(
        malformed_result.begin(),
        malformed_result.end(),
        from.begin(),
        from.end());
    ASSERT_NE(result_id, malformed_result.end());
    std::copy(to.begin(), to.end(), result_id);
    EXPECT_EQ(
        DecodeProgramResultV1(malformed_result).status.error,
        CodecError::InvalidValue);
}

} // namespace
