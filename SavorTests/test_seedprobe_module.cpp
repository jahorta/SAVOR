#include <gtest/gtest.h>

#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Phases/RNGSeedDeltaMap.h"
#include "Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "Runner/Runtime/ProgramKind.h"
#include "Runner/IPC/WrmsProtocol.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramValueArena.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "Analysis/IAnalysisDb.h"
#include "Authoring/IAuthoringDb.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindDescriptor.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeJobSpec.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeProgram.h"
#include "Utils/Hash.h"

#include "common/SqliteDbFixture.h"
#include "common/savordb_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;
using namespace savor::runtime::seedprobe;

void PutU32(std::vector<Byte>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
    {
        bytes.push_back(
            static_cast<Byte>((value >> shift) & 0xffu));
    }
}

void PutU64(std::vector<Byte>& bytes, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
    {
        bytes.push_back(
            static_cast<Byte>((value >> shift) & 0xffu));
    }
}

class StaticConfigTestReader final
{
public:
    explicit StaticConfigTestReader(std::span<const Byte> bytes)
        : bytes_(bytes)
    {
    }

    bool Magic(std::string_view expected)
    {
        if (remaining() < expected.size())
            return false;
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            if (bytes_[offset_ + index] !=
                static_cast<Byte>(expected[index]))
            {
                return false;
            }
        }
        offset_ += expected.size();
        return true;
    }

    bool U8(std::uint8_t& value)
    {
        if (remaining() < 1)
            return false;
        value = bytes_[offset_++];
        return true;
    }

    bool U32(std::uint32_t& value)
    {
        if (remaining() < 4)
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_++])
                << shift;
        }
        return true;
    }

    bool String(std::string& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || remaining() < size)
            return false;
        value.assign(
            reinterpret_cast<const char*>(
                bytes_.data() + offset_),
            size);
        offset_ += size;
        return true;
    }

    bool Hash(ContentHash256& value)
    {
        if (remaining() < value.bytes.size())
            return false;
        std::ranges::copy_n(
            bytes_.begin() + offset_,
            value.bytes.size(),
            value.bytes.begin());
        offset_ += value.bytes.size();
        return true;
    }

    [[nodiscard]] bool done() const noexcept
    {
        return offset_ == bytes_.size();
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return bytes_.size() - offset_;
    }

    std::span<const Byte> bytes_;
    std::size_t offset_ = 0;
};

std::vector<Byte> FrameBytes(const savor::GCInputFrame& frame)
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

std::vector<Byte> StopEvidence(std::uint32_t pc)
{
    std::vector<Byte> evidence{'R', 'S', 'E', '1'};
    evidence.push_back(0); // Native-router stop path.
    evidence.push_back(0); // Program-counter point.
    PutU32(evidence, pc);
    PutU32(evidence, pc);
    PutU64(evidence, 0); // Routed value.
    evidence.push_back(0); // Not a post-write watchpoint.
    PutU64(evidence, 1); // Dispatch generation.
    PutU64(evidence, 1); // Physical generation.
    evidence.push_back(0); // No hit-time samples.
    return evidence;
}

ProgramValue ReceiptValue(
    CanonicalAction action,
    CanonicalActionPayload payload,
    ProgramValueId id)
{
    const auto schema = CanonicalActionOutputSchemaIdentity(action);
    if (!schema)
        throw std::logic_error("test receipt action has no schema");
    auto encoded = EncodeCanonicalActionPayload(payload, *schema);
    if (!encoded.ok)
        throw std::logic_error(encoded.diagnostic);
    const auto found = std::ranges::find(
        encoded.graph.values,
        encoded.graph.root,
        &ProgramValue::id);
    if (found == encoded.graph.values.end())
        throw std::logic_error("test receipt has no root");
    ProgramValue value = *found;
    value.id = id;
    return value;
}

ProgramValueGraph ResultGraph(
    const savor::GCInputFrame& frame,
    std::uint32_t pc,
    std::uint64_t epoch = 42,
    std::uint64_t publication = 17,
    std::uint64_t delivery_id = 18,
    std::uint32_t raw_seed = 0x89ABCDEFu)
{
    CanonicalActionPayload delivery_payload;
    if (!delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::DeliveryId,
            delivery_id) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::Binding,
            13) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::Handle,
            7) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::Publication,
            publication) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::ResultSequence,
            19) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::ResultEpoch,
            epoch) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::StateGeneration,
            3) ||
        !delivery_payload.AddUnsigned(
            CanonicalActionPayloadField::CompletedCount,
            1) ||
        !delivery_payload.AddBytes(
            CanonicalActionPayloadField::ResultFrame,
            FrameBytes(frame)))
    {
        throw std::logic_error("test delivery payload failed");
    }

    ProgramValue seed{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::U32),
        raw_seed,
    };
    const auto endpoint = SeedProbeEndpointFromPc(pc);
    if (!endpoint)
        throw std::logic_error("unsupported test SeedProbe endpoint PC");
    ProgramValue endpoint_value{
        ProgramValueId(11),
        TypeRef::Named(SeedProbeEndpointSchemaIdentityV2()),
        EnumValue{
            .schema = SeedProbeEndpointSchemaIdentityV2(),
            .value = static_cast<std::int64_t>(*endpoint),
        },
    };
    ProgramValue sequence{
        ProgramValueId(2),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{23},
    };
    ProgramValue workset_epoch{
        ProgramValueId(3),
        TypeRef::Builtin(BuiltinType::U64),
        epoch,
    };
    ProgramValue stopped_pc{
        ProgramValueId(4),
        TypeRef::Builtin(BuiltinType::U32),
        pc,
    };
    ProgramValue sample{
        ProgramValueId(5),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{29},
    };
    ProgramValue evidence{
        ProgramValueId(6),
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::StopEvidencePayload),
        StopEvidence(pc),
    };
    ProgramValue routed_stop{
        ProgramValueId(7),
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::RoutedStopReceipt),
        RecordValue{{
            sequence.id,
            workset_epoch.id,
            stopped_pc.id,
            sample.id,
            evidence.id,
        }},
    };
    ProgramValue delivery = ReceiptValue(
        CanonicalAction::InputCompleteDelivery,
        std::move(delivery_payload),
        ProgramValueId(9));
    ProgramValue optional_stop{
        ProgramValueId(10),
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt),
        OptionalValue{routed_stop.id},
    };
    const SchemaIdentity reason_schema =
        CanonicalRuntimeSchemaIdentity(
            CanonicalRuntimeSchema::ContinueUntilCompletionReason);
    ProgramValue reason{
        ProgramValueId(13),
        TypeRef::Named(reason_schema),
        EnumValue{
            reason_schema,
            static_cast<std::int64_t>(
                ContinueUntilCompletionReasonV1::Breakpoint),
        },
    };
    ProgramValue movie_input_count{
        ProgramValueId(14),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{31},
    };
    ProgramValue vi_count{
        ProgramValueId(16),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{37},
    };
    ProgramValue completed_count{
        ProgramValueId(17),
        TypeRef::Builtin(BuiltinType::U64),
        std::uint64_t{1},
    };
    ProgramValue stop{
        ProgramValueId(15),
        CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        RecordValue{{
            reason.id,
            optional_stop.id,
            stopped_pc.id,
            movie_input_count.id,
            vi_count.id,
            completed_count.id,
            workset_epoch.id,
        }},
    };
    ProgramValue root{
        ProgramValueId(12),
        TypeRef::Named(SeedProbeResultSchemaIdentityV3()),
        RecordValue{{
            seed.id,
            endpoint_value.id,
            stop.id,
            delivery.id,
        }},
    };
    return {
        .root = root.id,
        .values = {
            std::move(seed),
            std::move(sequence),
            std::move(workset_epoch),
            std::move(stopped_pc),
            std::move(sample),
            std::move(evidence),
            std::move(routed_stop),
            std::move(optional_stop),
            std::move(reason),
            std::move(movie_input_count),
            std::move(vi_count),
            std::move(completed_count),
            std::move(stop),
            std::move(delivery),
            std::move(endpoint_value),
            std::move(root),
        },
    };
}

ProgramValueGraph TrueGraph()
{
    ProgramValue root{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::Bool),
        true,
    };
    return {root.id, {std::move(root)}};
}

std::vector<Byte> ProgramResultBytes(
    const ProgramValueGraph& result_graph,
    std::uint64_t invocation_id = 101,
    std::uint64_t attempt_id = 102)
{
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    std::string execution_diagnostic;
    const auto invocation = phase->BuildResolvedExecution(
        EncodeSeedProbeExecutionInputV2(SeedProbeRequestV2{}),
        InvocationId(invocation_id),
        AttemptId(attempt_id),
        &execution_diagnostic);
    if (!invocation)
        throw std::logic_error(execution_diagnostic);
    ProgramResult result{
        .invocation_id = InvocationId(invocation_id),
        .attempt_id = AttemptId(attempt_id),
        .module = invocation->module,
        .entrypoint = invocation->entrypoint,
        .resolved_dependencies = invocation->dependencies,
        .infrastructure =
            ProgramInfrastructureStatus::Completed,
        .domain_outcome = TrueGraph(),
        .cleanup = ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
        .output = result_graph,
        .provenance = {
            .requesting_component = "test.seedprobe",
        },
    };
    const EncodeResult encoded = EncodeProgramResultV1(result);
    if (!encoded)
        throw std::logic_error(encoded.status.message);
    return encoded.bytes;
}

ProgramModule ProductionSeedProbeModule()
{
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    const auto decoded = DecodeProgramModuleV1(
        phase->module_envelope().payload);
    if (!decoded)
        throw std::logic_error(decoded.status.message);
    return std::move(*decoded.value);
}

std::vector<const Instruction*> Instructions(
    const ProgramModule& module,
    std::string_view selector_prefix)
{
    std::vector<const Instruction*> instructions;
    for (const auto& function : module.functions)
    {
        for (const auto& block : function.blocks)
        {
            for (const auto& instruction : block.instructions)
            {
                if (instruction.selector.starts_with(
                        selector_prefix))
                {
                    instructions.push_back(&instruction);
                }
            }
        }
    }
    return instructions;
}

std::size_t SelectorPosition(
    const std::vector<const Instruction*>& instructions,
    std::string_view suffix)
{
    const auto found = std::ranges::find_if(
        instructions,
        [suffix](const Instruction* instruction)
        {
            return instruction->selector.ends_with(suffix);
        });
    return found == instructions.end()
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(
              found - instructions.begin());
}

using savor::db::ExecutionDbOperationDisposition;
using savor::db::SeedProbeEvidenceState;
using savor::db::SeedProbeRunStatus;
using savor::db::execution::programdb::
    ProgramJobContinuationContext;
using savor::db::execution::programdb::
    ProgramJobContinuationDisposition;
using savor::db::execution::programdb::
    ProgramJobContinuationResult;
using savor::db::execution::programdb::
    ProgramJobMaterializationContext;
using savor::db::execution::programdb::
    ProgramResultProcessingContext;
using savor::db::execution::programdb::
    WorkflowGraphArgument;
using savor::db::execution::programdb::
    WorkflowGraphInputBinding;
using savor::db::execution::programdb::
    WorkflowGraphStepScheduleContext;
using savor::db::execution::programdb::
    WorkflowStepScheduleContext;
using savor::db::execution::programdb::
    WorkflowStepScheduleResult;
using savor::db::execution::programdb::seedprobe::
    BuildSeedProbeProgramDescriptor;
using savor::db::execution::programdb::seedprobe::
    DecodeSeedProbeJobSpec;
using savor::db::execution::programdb::seedprobe::
    EncodeSeedProbeJobSpec;
using savor::db::execution::programdb::seedprobe::
    SeedProbeCancellationGroupKey;
using savor::db::execution::programdb::seedprobe::
    SeedProbeJobSpec;
using savor::db::execution::programdb::seedprobe::
    SeedProbeJobStage;
using savor::db::execution::programdb::seedprobe::
    SeedProbeProgramConfig;

bool Accepted(ExecutionDbOperationDisposition disposition)
{
    return disposition == ExecutionDbOperationDisposition::Applied ||
        disposition ==
            ExecutionDbOperationDisposition::AlreadyApplied;
}

bool EnsureTestSavestate(
    sqlite3* db,
    const std::filesystem::path& temp_root)
{
    const auto object_store_root = temp_root / "object_store";
    std::error_code directory_error;
    if (!std::filesystem::create_directories(
            object_store_root,
            directory_error)
        && directory_error) {
        return false;
    }
    const auto state_path =
        object_store_root / "seedprobe-descriptor-test.sav";
    {
        std::ofstream output(
            state_path,
            std::ios::binary | std::ios::trunc);
        output.put('S');
        if (!output.good())
            return false;
    }
    const auto sha256 =
        hash::sha256_of_file(state_path.string());
    const std::string sql =
        "INSERT INTO state_artifact("
        "artifact_id,sha256,size_bytes,compression_kind,display_filename,"
        "file_ext,artifact_kind,created_at_utc,object_relpath) VALUES("
        "1,'" + sha256 + "',1,0,'seedprobe-descriptor-test.sav',"
        "'.sav','SAV',1000,'seedprobe-descriptor-test.sav');"
        "INSERT INTO state_savestate("
        "savestate_id,artifact_id,savestate_type,note,is_complete,"
        "created_at_utc) VALUES("
        "1,1,'ENTRY','seedprobe descriptor test',1,1000);";
    return ExecSql(db, sql.c_str());
}

std::optional<std::int64_t> SaveTestSeedProbeSpec(
    savor::db::IAuthoringDb* authoring_db,
    int combination_attempts,
    int combination_sampler_tries,
    std::string_view name,
    std::string* error_out)
{
    if (authoring_db == nullptr)
        return std::nullopt;
    std::int64_t spec_id = 0;
    if (!authoring_db->SaveSeedProbeSpec(
            {
                .name = std::string(name),
                .priority = 0,
                .min_value = 0,
                .max_value = 255,
                .cap_trigger_top = false,
                .ignore_trigger_minmax = true,
                .combo_attempts_per_target =
                    combination_attempts,
                .combo_sampler_tries =
                    combination_sampler_tries,
                .auto_schedule_battle_run = false,
                .created_at_utc =
                    savor::db::types::UtcTimePoint(
                        std::chrono::milliseconds(1000)),
                .correlation_id =
                    "seedprobe-descriptor-test",
                .causation_id =
                    "seedprobe-descriptor-test",
            },
            &spec_id,
            error_out))
    {
        return std::nullopt;
    }
    return spec_id;
}

ProgramJobMaterializationContext TestMaterializationContext(
    std::int64_t spec_id,
    std::int64_t workflow_step_id)
{
    return {
        .step =
            WorkflowStepScheduleContext{
                .workflow_instance_id = workflow_step_id - 1,
                .workflow_step_id = workflow_step_id,
                .step_key = "seedprobe.survey",
                .step_kind = "seedprobe.survey",
                .domain_ref_id = 0,
                .step_priority = 0,
            },
        .graph =
            WorkflowGraphStepScheduleContext{
                .workflow_instance_id = workflow_step_id - 1,
                .workflow_step_id = workflow_step_id,
                .workflow_graph_revision_id = 1,
                .step_key = "seedprobe.survey",
                .step_kind = "seedprobe.survey",
                .activation_key = "seedprobe.survey",
                .activation_graph_node_key = "seedprobe.survey",
                .unit_kind = "seed_probe",
                .activation_params_json = "{}",
                .authored_ref_kind =
                    std::string("seed_probe_spec"),
                .authored_ref_id = spec_id,
                .step_priority = 0,
                .inputs =
                    {
                        WorkflowGraphInputBinding{
                            .node_key = "seedprobe.survey",
                            .input_key = "entry_savestate",
                            .data_kind = "state.movie_inactive_savestate_id",
                            .ref_kind = "state.savestate",
                            .ref_id = 1,
                            .source_kind = "test",
                        },
                    },
                .arguments =
                    {
                        WorkflowGraphArgument{
                            .node_key = "seedprobe.survey",
                            .argument_key = "samples_per_axis",
                            .value_type = "INTEGER",
                            .integer_value = 1,
                            .source_kind = "test",
                        },
                    },
            },
    };
}

bool SetJobState(
    sqlite3* db,
    std::int64_t job_id,
    std::string_view state)
{
    const std::string sql =
        "UPDATE exec_job SET state='" + std::string(state)
        + "', ended_at_utc=2000 WHERE job_id="
        + std::to_string(job_id) + ";";
    return ExecSql(db, sql.c_str());
}

std::string ReadSqlText(sqlite3* db, const std::string& sql)
{
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr ||
        sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return {};
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) value = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(statement);
    return value;
}

bool EnsureTestWorkflowStep(
    sqlite3* db,
    std::int64_t workflow_step_id)
{
    const auto workflow_instance_id = workflow_step_id - 1;
    const std::string instance_sql =
        "INSERT OR IGNORE INTO exec_workflow_instance("
        "workflow_instance_id,workflow_kind,state,root_scope_kind,"
        "created_by,created_at_utc) VALUES("
        + std::to_string(workflow_instance_id)
        + ",'seedprobe-test','RUNNING','manual','test',1000);";
    if (!ExecSql(db, instance_sql.c_str()))
        return false;
    const std::string step_sql =
        "INSERT OR IGNORE INTO exec_workflow_step("
        "workflow_step_id,workflow_instance_id,step_key,step_kind,state,"
        "priority,attempts,max_attempts,ready_at_utc,created_at_utc) VALUES("
        + std::to_string(workflow_step_id) + ","
        + std::to_string(workflow_instance_id)
        + ",'seedprobe.run','seedprobe.run','READY',0,0,3,1000,1000);";
    return ExecSql(db, step_sql.c_str());
}

std::optional<std::int64_t> RecordObservation(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t probe_run_id,
    std::int64_t input_frame_id,
    std::int64_t source_job_id,
    std::uint32_t seed,
    std::uint64_t worker_id,
    std::uint64_t process_generation,
    std::uint64_t workset_epoch,
    std::optional<std::int64_t> confirmation_of,
    char hash_digit,
    std::string* error_out)
{
    savor::db::RecordSeedProbeObservationReceipt receipt{};
    if (analysis_db == nullptr ||
        !analysis_db->RecordSeedProbeObservation(
            {
                .probe_run_id = probe_run_id,
                .input_frame_id = input_frame_id,
                .source_job_id = source_job_id,
                .seed_value = seed,
                .origin_worker_id = worker_id,
                .origin_process_generation =
                    process_generation,
                .origin_workset_epoch = workset_epoch,
                .terminal_sha256 =
                    std::string(64, hash_digit),
                .confirmation_of_probe_result_id =
                    confirmation_of,
                .endpoint =
                    savor::db::SeedProbeEndpoint::AfterRandSeedSet,
                .recorded_at_utc =
                    savor::db::types::UtcTimePoint(
                        std::chrono::milliseconds(2000)),
                .correlation_id =
                    "seedprobe-descriptor-test",
                .causation_id =
                    "seedprobe-descriptor-test",
            },
            &receipt,
            error_out) ||
        !receipt.inserted)
    {
        return std::nullopt;
    }
    return receipt.observation.probe_result_id;
}

bool TransitionEvidence(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t result_id,
    SeedProbeEvidenceState expected,
    SeedProbeEvidenceState desired,
    std::string* error_out)
{
    bool changed = false;
    return analysis_db != nullptr &&
        analysis_db->TransitionSeedProbeEvidence(
            {
                .probe_result_id = result_id,
                .expected_state = expected,
                .new_state = desired,
                .changed_at_utc =
                    savor::db::types::UtcTimePoint(
                        std::chrono::milliseconds(3000)),
                .correlation_id =
                    "seedprobe-descriptor-test",
                .causation_id =
                    "seedprobe-descriptor-test",
            },
            &changed,
            error_out) &&
        changed;
}

struct CreatedJobSet
{
    std::int64_t job_set_id = 0;
    std::vector<std::int64_t> job_ids;
};

std::optional<CreatedJobSet> CreateTestJobSet(
    savor::db::IExecutionDb* execution_db,
    std::string materialization_key,
    std::string purpose,
    std::string meta_note,
    std::int64_t probe_run_id,
    const std::vector<SeedProbeJobSpec>& specs,
    std::string* error_out)
{
    if (execution_db == nullptr)
        return std::nullopt;
    savor::db::EnsureMaterializingJobSetReceipt ensured{};
    if (!execution_db->EnsureMaterializingJobSet(
            {
                .materialization_key =
                    std::move(materialization_key),
                .program_kind =
                    static_cast<std::int32_t>(
                        savor::PK_SeedProbe),
                .purpose = std::move(purpose),
                .created_by =
                    std::string(
                        "seedprobe-descriptor-test"),
                .created_at_utc = 1000,
                .priority_boost = 0,
                .expected_total =
                    static_cast<int>(specs.size()),
                .domain_ref_kind =
                    std::string("sp_probe_run"),
                .domain_ref_id = probe_run_id,
                .meta_note = std::move(meta_note),
            },
            &ensured,
            error_out) ||
        !Accepted(ensured.disposition))
    {
        return std::nullopt;
    }

    CreatedJobSet created{
        .job_set_id = ensured.job_set_id,
    };
    created.job_ids.reserve(specs.size());
    for (std::size_t index = 0; index < specs.size(); ++index)
    {
        savor::db::CreatePendingJobReceipt job{};
        if (!execution_db->CreatePendingJob(
                {
                    .job_set_id = ensured.job_set_id,
                    .program_kind =
                        static_cast<std::int32_t>(
                            savor::PK_SeedProbe),
                    .program_version = ProgramVersion,
                    .program_ref_kind =
                        "sp_probe_run",
                    .program_ref_id = probe_run_id,
                    .savestate_id = 1,
                    .fingerprint =
                        "seedprobe-descriptor-test."
                        + std::to_string(
                            ensured.job_set_id)
                        + "." + std::to_string(index),
                    .priority = 0,
                    .max_attempts = 3,
                    .input_ini =
                        EncodeSeedProbeJobSpec(specs[index]),
                    .cancellation_group_key =
                        SeedProbeCancellationGroupKey(
                            probe_run_id,
                            specs[index]),
                },
                &job,
                error_out) ||
            !Accepted(job.disposition))
        {
            return std::nullopt;
        }
        created.job_ids.push_back(job.job_id);
    }
    return created;
}

struct ManualRun
{
    std::int64_t probe_run_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t neutral_input_frame_id = 0;
    std::int64_t neutral_job_id = 0;
    std::int64_t neutral_result_id = 0;
};

std::optional<ManualRun> CreateManualRun(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t spec_id,
    SeedProbeRunStatus run_status,
    SeedProbeEvidenceState neutral_state,
    std::string_view key,
    std::string* error_out)
{
    if (analysis_db == nullptr)
        return std::nullopt;
    std::int64_t probe_set_id = 0;
    if (!analysis_db->CreateSeedProbeSet(
            {
                .name = std::string(key) + ".set",
                .probe_flavor = "BATTLE_PRE",
                .breakpoint_policy_name =
                    "seedprobe.battle",
                .segment_source_kind = "test",
                .created_at_utc =
                    savor::db::types::UtcTimePoint(
                        std::chrono::milliseconds(1000)),
                .correlation_id =
                    "seedprobe-descriptor-test",
                .causation_id =
                    "seedprobe-descriptor-test",
            },
            &probe_set_id,
            error_out))
    {
        return std::nullopt;
    }
    std::int64_t run_id = 0;
    if (!analysis_db->RequestSeedProbeRun(
            {
                .materialization_key =
                    std::string(key) + ".run",
                .probe_set_id = probe_set_id,
                .entry_savestate_id = 1,
                .seed_probe_spec_id = spec_id,
                .launch_samples_per_axis = 1,
                .codec_version = ProgramVersion,
                .status = SeedProbeRunStatus::Survey,
                .requested_at_utc =
                    savor::db::types::UtcTimePoint(
                        std::chrono::milliseconds(1000)),
                .correlation_id =
                    "seedprobe-descriptor-test",
                .causation_id =
                    "seedprobe-descriptor-test",
            },
            &run_id,
            error_out))
    {
        return std::nullopt;
    }
    if (run_status != SeedProbeRunStatus::Survey)
    {
        bool changed = false;
        if (!analysis_db->UpdateSeedProbeRunStatus(
                {
                    .probe_run_id = run_id,
                    .expected_status =
                        SeedProbeRunStatus::Survey,
                    .new_status = run_status,
                    .changed_at_utc =
                        savor::db::types::UtcTimePoint(
                            std::chrono::milliseconds(1500)),
                    .correlation_id =
                        "seedprobe-descriptor-test",
                    .causation_id =
                        "seedprobe-descriptor-test",
                },
                &changed,
                error_out) ||
            !changed)
        {
            return std::nullopt;
        }
    }
    std::int64_t neutral_frame_id = 0;
    // Exact Neutral: buttons are always zero in persisted SeedProbe frames,
    // both sticks are centered at 128/128, and both triggers are zero.
    if (!analysis_db->EnsureSeedProbeInputFrame(
            0x8080,
            0x8080,
            0x0000,
            &neutral_frame_id,
            error_out))
    {
        return std::nullopt;
    }
    const std::vector<SeedProbeJobSpec> neutral_specs{
        {
            .version = 1,
            .stage = SeedProbeJobStage::Survey,
            .input_frame_id = neutral_frame_id,
            .sample_ordinal = 0,
        },
    };
    const auto root = CreateTestJobSet(
        execution_db,
        std::string(key) + ".root",
        "SEEDPROBE_SURVEY",
        "stage=SURVEY",
        run_id,
        neutral_specs,
        error_out);
    if (!root.has_value() || root->job_ids.size() != 1)
        return std::nullopt;

    const auto neutral_result = RecordObservation(
        analysis_db,
        run_id,
        neutral_frame_id,
        root->job_ids.front(),
        100,
        1,
        1,
        42,
        std::nullopt,
        'a',
        error_out);
    if (!neutral_result.has_value() ||
        !TransitionEvidence(
            analysis_db,
            *neutral_result,
            SeedProbeEvidenceState::Observed,
            SeedProbeEvidenceState::Provisional,
            error_out))
    {
        return std::nullopt;
    }
    if (neutral_state == SeedProbeEvidenceState::Confirmed)
    {
        const std::vector<SeedProbeJobSpec> confirmation_specs{
            {
                .version = 1,
                .stage = SeedProbeJobStage::Confirm,
                .input_frame_id = neutral_frame_id,
                .sample_ordinal = 0,
                .confirmation_of_probe_result_id =
                    *neutral_result,
            },
        };
        const auto confirmation_jobs = CreateTestJobSet(
            execution_db,
            std::string(key) + ".neutral-confirmation",
            "SEEDPROBE_TEST_CONFIRMATION",
            "stage=CONFIRM;fixture=neutral",
            run_id,
            confirmation_specs,
            error_out);
        if (!confirmation_jobs.has_value() ||
            confirmation_jobs->job_ids.size() != 1)
        {
            return std::nullopt;
        }
        const auto confirmation_result = RecordObservation(
            analysis_db,
            run_id,
            neutral_frame_id,
            confirmation_jobs->job_ids.front(),
            100,
            2,
            1,
            42,
            *neutral_result,
            'f',
            error_out);
        if (!confirmation_result.has_value() ||
            !TransitionEvidence(
                analysis_db,
                *neutral_result,
                SeedProbeEvidenceState::Provisional,
                SeedProbeEvidenceState::Confirmed,
                error_out))
        {
            return std::nullopt;
        }
    }
    return ManualRun{
        .probe_run_id = run_id,
        .job_set_id = root->job_set_id,
        .neutral_input_frame_id = neutral_frame_id,
        .neutral_job_id = root->job_ids.front(),
        .neutral_result_id = *neutral_result,
    };
}

savor::GCInputFrame InputFrameFor(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t input_frame_id)
{
    const auto row =
        analysis_db->GetAnalysisInputFrame(input_frame_id);
    if (!row.has_value())
        throw std::logic_error("SeedProbe test input frame is missing");
    savor::GCInputFrame frame{};
    frame.buttons = 0;
    frame.main_x = static_cast<std::uint8_t>(row->main_x);
    frame.main_y = static_cast<std::uint8_t>(row->main_y);
    frame.c_x = static_cast<std::uint8_t>(row->cstick_x);
    frame.c_y = static_cast<std::uint8_t>(row->cstick_y);
    frame.trig_l =
        static_cast<std::uint8_t>(row->trigger_x);
    frame.trig_r =
        static_cast<std::uint8_t>(row->trigger_y);
    return frame;
}

ProgramResultProcessingContext SuccessfulResultContext(
    savor::db::IAnalysisDb* analysis_db,
    const savor::db::ExecutionJobRecord& job,
    std::uint32_t raw_seed,
    std::uint64_t worker_id,
    std::uint64_t process_generation,
    std::uint64_t workset_epoch,
    SeedProbeEndpointV2 endpoint =
        SeedProbeEndpointV2::AfterRandSeedSet)
{
    const auto spec = DecodeSeedProbeJobSpec(job.input_ini);
    if (!spec.has_value())
        throw std::logic_error("SeedProbe test job spec is invalid");
    const auto frame =
        InputFrameFor(analysis_db, spec->input_frame_id);
    constexpr std::uint64_t attempt_id = 1;
    const std::uint64_t dispatch_attempt_id =
        static_cast<std::uint64_t>(job.job_id + 1000);
    const auto program_result = ProgramResultBytes(
        ResultGraph(
            frame,
            SeedProbeEndpointPc(endpoint),
            workset_epoch,
            17,
            17,
            raw_seed),
        static_cast<std::uint64_t>(job.job_id),
        attempt_id);

    savor::runtime::DurableWorkerTerminalEnvelope terminal{
        .envelope_version =
            savor::runtime::
                kDurableWorkerTerminalEnvelopeVersion,
        .wrms_protocol_version =
            savor::wrms::ProtocolVersion,
        .worker_id = worker_id,
        .process_generation = process_generation,
        .terminal =
            savor::wrms::WorksetItemTerminalPayload{
                .outbound_sequence = 1,
                .workset_id = dispatch_attempt_id,
                .item_id =
                    static_cast<std::uint64_t>(job.job_id),
                .item_ordinal = 0,
                .invocation_id =
                    static_cast<std::uint64_t>(job.job_id),
                .attempt_id = attempt_id,
                .terminal_id = 1,
                .terminal_order = 1,
                .status =
                    savor::wrms::
                        InvocationTerminalStatus::Succeeded,
                .session_disposition =
                    savor::wrms::
                        SessionDispositionCode::Clean,
                .workset_epoch = workset_epoch,
                .unstarted = false,
                .rejection_code =
                    savor::wrms::RejectionCode::None,
                .result = program_result,
            },
    };
    std::vector<std::uint8_t> envelope;
    std::string error;
    if (!savor::runtime::EncodeDurableWorkerTerminalEnvelope(
            terminal,
            &envelope,
            &error))
    {
        throw std::logic_error(error);
    }
    return {
        .job_id = job.job_id,
        .job_set_id = job.job_set_id,
        .program_kind = job.program_kind,
        .program_version = job.program_version,
        .program_ref_kind = job.program_ref_kind,
        .program_ref_id = job.program_ref_id,
        .fingerprint = job.fingerprint,
        .input_ini = job.input_ini,
        .terminal =
            {
                .job_id = job.job_id,
                .workset_id =
                    job.workset_id.value_or(1),
                .dispatch_attempt_id =
                    static_cast<std::int64_t>(
                        dispatch_attempt_id),
                .reserved_attempt_id = attempt_id,
                .format =
                    "savor.worker-terminal-envelope.v1",
                .sha256 = std::string(64, 'd'),
                .envelope = std::move(envelope),
            },
    };
}

std::optional<SeedProbeRequestV2> ReconstructSeedProbeRequest(
    const savor::db::execution::programdb::
        ProgramKindDescriptor& descriptor,
    const savor::db::ExecutionJobRecord& job,
    std::int64_t workflow_step_id,
    std::int64_t root_job_set_id,
    std::string* error_out)
{
    if (descriptor.workset_reconstruction == nullptr)
        return std::nullopt;
    const auto dispatch_attempt_id = job.job_id + 5000;
    const auto reconstructed =
        descriptor.workset_reconstruction->Reconstruct(
            {
                .workset_id =
                    job.workset_id.value_or(job.job_id + 4000),
                .dispatch_attempt_id =
                    dispatch_attempt_id,
                .workflow_step_id = workflow_step_id,
                .job_set_id = root_job_set_id,
                .dispatch_token =
                    "seedprobe-descriptor-test-claim",
                .contract_key =
                    "seedprobe-descriptor-test",
                .state_compatibility =
                    {
                        .game_id = "GEAE8P",
                        .iso_sha256 =
                            std::string(64, '1'),
                        .emulator_build =
                            "seedprobe-descriptor-test",
                        .runtime_revision = "test",
                    },
                .items =
                    {
                        {
                            .job_id = job.job_id,
                            .workset_item_ordinal = 0,
                            .reserved_attempt_id = 1,
                            .claim_token =
                                "seedprobe-descriptor-test-claim",
                            .program_kind =
                                job.program_kind,
                            .program_version =
                                job.program_version,
                            .program_ref_kind =
                                job.program_ref_kind,
                            .program_ref_id =
                                job.program_ref_id,
                            .savestate_id =
                                job.savestate_id,
                            .fingerprint =
                                job.fingerprint,
                            .input_ini = job.input_ini,
                        },
                    },
            },
            error_out);
    if (!reconstructed.has_value() ||
        reconstructed->workset.items.size() != 1)
    {
        return std::nullopt;
    }
    SeedProbeRequestV2 request{};
    if (!DecodeSeedProbeExecutionInputV2(
            reconstructed->workset.items.front()
                .execution.input_payload,
            request,
            error_out))
    {
        return std::nullopt;
    }
    return request;
}

TEST(SeedProbeModule, RequestCodecUsesFullCanonicalFrameAndBothTargets)
{
    savor::GCInputFrame frame{};
    frame.buttons = savor::GC_A | savor::GC_Z;
    frame.main_x = 4;
    frame.main_y = 250;
    frame.c_x = 33;
    frame.c_y = 199;
    frame.trig_l = 12;
    frame.trig_r = 244;

    const SeedProbeRequestV2 expected{.frame = frame};
    const ProgramValueGraph encoded =
        EncodeSeedProbeRequestV2(expected);
    SeedProbeRequestV2 decoded;
    std::string diagnostic;
    ASSERT_TRUE(DecodeSeedProbeRequestV2(
        encoded,
        decoded,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded, expected);
    const auto compact = EncodeSeedProbeExecutionInputV2(expected);
    EXPECT_EQ(compact.size(), sizeof(savor::GCInputFrame));
    SeedProbeRequestV2 compact_decoded;
    ASSERT_TRUE(DecodeSeedProbeExecutionInputV2(
        compact, compact_decoded, &diagnostic)) << diagnostic;
    EXPECT_EQ(compact_decoded, expected);

    EXPECT_EQ(
        SeedProbeEndpointPc(
            SeedProbeEndpointV2::AfterRandSeedSet),
        0x8000A1DCu);
    EXPECT_EQ(
        SeedProbeEndpointPointId(
            SeedProbeEndpointV2::AfterRandSeedSet),
        "soa.field.point.prebattle.AfterRandSeedSet");
    EXPECT_EQ(
        SeedProbeEndpointPc(
            SeedProbeEndpointV2::RandSeedCommitted),
        0x801012B4u);
    EXPECT_EQ(
        SeedProbeEndpointPointId(
            SeedProbeEndpointV2::RandSeedCommitted),
        "soa.field.point.field_return.RandSeedCommitted");
}

TEST(SeedProbePlanning, NeutralFrameUsesZeroButtonsCenteredSticksAndZeroTriggers)
{
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    const auto survey = phase->PlanSurvey({
        .samples_per_axis = 1,
        .min_value = 0,
        .max_value = 255,
        .ignore_trigger_minmax = true,
        .cap_trigger_top = false,
    });
    ASSERT_FALSE(survey.empty());
    const savor::GCInputFrame neutral = survey.front();
    EXPECT_EQ(neutral.buttons, 0u);
    EXPECT_EQ(neutral.main_x, 128u);
    EXPECT_EQ(neutral.main_y, 128u);
    EXPECT_EQ(neutral.c_x, 128u);
    EXPECT_EQ(neutral.c_y, 128u);
    EXPECT_EQ(neutral.trig_l, 0u);
    EXPECT_EQ(neutral.trig_r, 0u);
}

TEST(SeedProbePlanning, CombinationSamplingIsDeterministicAndWrapsInt32Arithmetic)
{
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    savor::RandSeedProbeResult grid{};
    grid.base_seed = 100;
    grid.entries = {
        {
            .family = savor::SeedFamily::Main,
            .x = 11,
            .y = 12,
            .seed = 101,
            .delta = 1,
            .ok = true,
        },
        {
            .family = savor::SeedFamily::CStick,
            .x = 21,
            .y = 22,
            .seed = 102,
            .delta = 2,
            .ok = true,
        },
        {
            .family = savor::SeedFamily::Triggers,
            .x = 31,
            .y = 32,
            .seed = 104,
            .delta = 4,
            .ok = true,
        },
    };

    const auto first =
        phase->PlanSearch(grid, 2, 100);
    const auto second =
        phase->PlanSearch(grid, 2, 100);
    ASSERT_EQ(first.expected, std::vector<std::int32_t>{7});
    ASSERT_EQ(first.samples.size(), 1u);
    ASSERT_EQ(first.samples.front().target_delta, 7);
    ASSERT_EQ(first.samples.front().frames.size(), 1u);
    EXPECT_EQ(first.samples.front().frames,
              second.samples.front().frames);
    const auto& frame = first.samples.front().frames.front();
    EXPECT_EQ(frame.buttons, 0u);
    EXPECT_EQ(frame.main_x, 11u);
    EXPECT_EQ(frame.main_y, 12u);
    EXPECT_EQ(frame.c_x, 21u);
    EXPECT_EQ(frame.c_y, 22u);
    EXPECT_EQ(frame.trig_l, 31u);
    EXPECT_EQ(frame.trig_r, 32u);

    grid.entries[0].delta =
        (std::numeric_limits<std::int32_t>::max)();
    grid.entries[1].delta = 1;
    grid.entries[2].delta = 1;
    const auto wrapped =
        phase->PlanSearch(grid, 1, 100);
    ASSERT_EQ(
        wrapped.expected,
        std::vector<std::int32_t>{-2147483647});
    ASSERT_EQ(wrapped.samples.size(), 1u);
    EXPECT_EQ(wrapped.samples.front().frames.size(), 1u);
}

TEST(SeedProbePlanning, DeltaUsesWrappedUint32SubtractionInterpretedAsInt32)
{
    EXPECT_EQ(savor::wrapped_seed_delta(0u, 0xFFFFFFFFu), 1);
    EXPECT_EQ(savor::wrapped_seed_delta(0xFFFFFFFFu, 0u), -1);
    EXPECT_EQ(
        savor::wrapped_seed_delta(0x80000000u, 0x7FFFFFFFu),
        1);
    EXPECT_EQ(
        savor::wrapped_seed_delta(0x7FFFFFFFu, 0x80000000u),
        -1);
    EXPECT_EQ(
        savor::wrapped_seed_delta(0x80000000u, 0u),
        (std::numeric_limits<std::int32_t>::min)());
    EXPECT_EQ(
        savor::wrapped_seed_delta(0x7FFFFFFFu, 0u),
        (std::numeric_limits<std::int32_t>::max)());
}

TEST(SeedProbeModule, ProductionEnvelopeIsCanonicalAndVerifierAccepted)
{
    std::string diagnostic;
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    const auto phase_again = SeedProbeFullPhaseDefinitionV2();
    ASSERT_TRUE(phase);
    ASSERT_TRUE(phase_again);
    ASSERT_TRUE(phase->identity());
    EXPECT_EQ(phase.get(), phase_again.get());
    EXPECT_EQ(
        fullphase::ProductionRegistry().Find(phase->identity()),
        phase.get());
    EXPECT_EQ(
        &phase->module_envelope(),
        &phase_again->module_envelope());
    EXPECT_EQ(
        phase->module_envelope().payload.data(),
        phase_again->module_envelope().payload.data());
    EXPECT_EQ(
        phase->runtime_contract().baseline_lineage,
        BaselineLineage);
    EXPECT_EQ(
        fullphase::BuildFullPhaseProgramPackage(*phase)
            .canonical_sha256.size(),
        64u);
    const auto& envelope = phase->module_envelope();
    EXPECT_EQ(
        envelope.identity.canonical_id,
        ModuleCanonicalId);
    EXPECT_EQ(envelope.identity.revision, ModuleRevision);
    EXPECT_EQ(envelope.identity.canonical_hash.size(), 64u);
    EXPECT_EQ(envelope.format_version, kProgramCodecVersionV1);
    EXPECT_FALSE(envelope.development_only);

    const auto decoded =
        DecodeProgramModuleV1(envelope.payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    ASSERT_EQ(decoded.value->entrypoints.size(), 1u);
    EXPECT_EQ(decoded.value->entrypoints.front().name, Entrypoint);
    EXPECT_NE(
        std::ranges::find(
            decoded.value->required_capability_packs,
            CanonicalRuntimePackIdentity()),
        decoded.value->required_capability_packs.end());
    EXPECT_NE(
        std::ranges::find(
            decoded.value->required_capability_packs,
            FieldPackIdentity()),
        decoded.value->required_capability_packs.end());
    EXPECT_EQ(
        decoded.value->identity.module_hash.ToHex(),
        envelope.identity.canonical_hash);

    const SeedProbeRequestV2 request{
        .frame = savor::GCInputFrame{},
    };
    const auto invocation = phase->BuildResolvedExecution(
        EncodeSeedProbeExecutionInputV2(request),
        InvocationId(31),
        AttemptId(32),
        &diagnostic);
    ASSERT_TRUE(invocation) << diagnostic;
    EXPECT_EQ(invocation->module, decoded.value->identity);
    EXPECT_EQ(invocation->entrypoint, Entrypoint);
    EXPECT_EQ(
        invocation->state.policy,
        InvocationStatePolicy::RestoreBaseline);
    EXPECT_EQ(
        invocation->state.session_lineage,
        phase->runtime_contract().baseline_lineage);
    EXPECT_FALSE(invocation->state.expected_session);
    EXPECT_FALSE(invocation->state.expected_epoch);
    EXPECT_TRUE(invocation->execution.allow_input);
    EXPECT_FALSE(invocation->execution.allow_capture);
    const EncodeResult encoded_invocation =
        EncodeProgramInvocationV1(*invocation);
    ASSERT_TRUE(encoded_invocation)
        << encoded_invocation.status.message;
    const auto decoded_invocation =
        DecodeProgramInvocationV1(encoded_invocation.bytes);
    ASSERT_TRUE(decoded_invocation)
        << decoded_invocation.status.message;
    EXPECT_EQ(*decoded_invocation.value, *invocation);

    SeedProbeRequestV2 second_request = request;
    second_request.frame.main_x = 37;
    const auto second_invocation = phase->BuildResolvedExecution(
        EncodeSeedProbeExecutionInputV2(second_request),
        InvocationId(41),
        AttemptId(42),
        &diagnostic);
    ASSERT_TRUE(second_invocation) << diagnostic;
    EXPECT_NE(second_invocation->invocation_id, invocation->invocation_id);
    EXPECT_NE(second_invocation->attempt_id, invocation->attempt_id);
    EXPECT_NE(second_invocation->input, invocation->input);
    EXPECT_EQ(second_invocation->module, invocation->module);
    EXPECT_EQ(second_invocation->entrypoint, invocation->entrypoint);
    EXPECT_EQ(second_invocation->dependencies, invocation->dependencies);
    EXPECT_EQ(second_invocation->runtime_profile, invocation->runtime_profile);
    EXPECT_EQ(second_invocation->state, invocation->state);
    EXPECT_EQ(second_invocation->execution, invocation->execution);
    EXPECT_EQ(second_invocation->limits, invocation->limits);
    EXPECT_EQ(second_invocation->provenance, invocation->provenance);
    EXPECT_EQ(
        phase->runtime_contract().runtime_profile_sha256.size(),
        64u);
    EXPECT_EQ(
        phase->runtime_contract().dependency_lock_sha256.size(),
        64u);
    EXPECT_EQ(
        phase->runtime_contract().verified_dependency_sha256.size(),
        64u);

    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    const RegistryResult registered =
        RegisterSourceCapabilityPacks(
            schemas,
            actions,
            packs);
    ASSERT_TRUE(registered.success)
        << registered.error.message;
    const auto stored =
        modules.RegisterCompiled(*decoded.value);
    ASSERT_TRUE(stored.success)
        << stored.error.message;
    ProgramVerifier verifier(
        modules,
        schemas,
        actions,
        packs);
    const ProgramVerificationResult verified =
        verifier.Verify(
            stored.module->identity,
            SupportedSoaUsaCompatibility());
    ASSERT_TRUE(verified.success)
        << (verified.diagnostics.empty()
                ? ""
                : verified.diagnostics.front().message);
    ASSERT_TRUE(verified.verified);
    EXPECT_EQ(
        phase->runtime_contract().dependency_lock_sha256,
        ComputeProgramDependencyLockHashV1(
            verified.verified->dependency_lock)
            .ToHex());
    EXPECT_NE(
        phase->runtime_contract().dependency_lock_sha256,
        phase->runtime_contract().verified_dependency_sha256);
    const auto input_status = ValidateProgramValueGraph(
        invocation->input,
        decoded.value->entrypoints.front().input_type,
        verified.verified->type_closure,
        {
            .maximum_values =
                decoded.value->budgets.maximum_values,
            .maximum_value_bytes =
                decoded.value->budgets.maximum_value_bytes,
        });
    EXPECT_TRUE(input_status) << input_status.message;
}

TEST(SeedProbeModule, OrdersFactualActionsAndContainsNoLegacyEffects)
{
    const ProgramModule module = ProductionSeedProbeModule();
    const auto all_instructions = Instructions(module, "");
    const std::size_t observe = SelectorPosition(
        all_instructions, "entry/observe-paused-pc");
    ASSERT_NE(observe, std::numeric_limits<std::size_t>::max());
    const std::size_t first_delivery = SelectorPosition(
        all_instructions, "delivery/prebattle/semantic-points");
    ASSERT_NE(first_delivery, std::numeric_limits<std::size_t>::max());
    EXPECT_LT(observe, first_delivery);
    EXPECT_NE(
        std::ranges::find(
            module.action_imports,
            CanonicalActionIdentity(
                CanonicalAction::ExecutionObservePausedPc)),
        module.action_imports.end());

    for (const std::string_view delivery_prefix : {
             std::string_view("delivery/prebattle/"),
             std::string_view("delivery/field-return/"),
         }) {
        const auto instructions = Instructions(module, delivery_prefix);
        const std::size_t semantic_points = SelectorPosition(
            instructions, "semantic-points");
        const std::size_t begin_delivery = SelectorPosition(
            instructions, "binding");
        const std::size_t stop = SelectorPosition(
            instructions, "stop");
        const std::size_t complete_delivery = SelectorPosition(
            instructions, "receipt");
        ASSERT_NE(semantic_points, std::numeric_limits<std::size_t>::max());
        ASSERT_NE(begin_delivery, std::numeric_limits<std::size_t>::max());
        ASSERT_NE(stop, std::numeric_limits<std::size_t>::max());
        ASSERT_NE(complete_delivery, std::numeric_limits<std::size_t>::max());
        EXPECT_LT(semantic_points, begin_delivery);
        EXPECT_LT(begin_delivery, stop);
        EXPECT_LT(stop, complete_delivery);
    }

    for (const std::string_view branch : {
             std::string_view("prebattle/"),
             std::string_view("field-return/"),
         })
    {
        const auto instructions =
            Instructions(module, branch);
        const std::size_t read =
            SelectorPosition(instructions, "/rng/read-paused-u32");
        ASSERT_NE(read, std::numeric_limits<std::size_t>::max());
    }

    for (const CanonicalAction forbidden : {
             CanonicalAction::SavestateSaveImmutableArtifact,
             CanonicalAction::ExecutionStepFrames,
             CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
         })
    {
        EXPECT_EQ(
            std::ranges::find(
                module.action_imports,
                CanonicalActionIdentity(forbidden)),
            module.action_imports.end());
    }
    std::size_t deferred_input_compensations = 0;
    std::size_t scope_exits = 0;
    for (const auto& function : module.functions)
    {
        for (const auto& block : function.blocks)
        {
            for (const auto& instruction : block.instructions)
            {
                if (instruction.opcode ==
                        InstructionOpcode::DeferCompensation)
                {
                    ++deferred_input_compensations;
                }
                if (instruction.opcode ==
                    InstructionOpcode::ExitScope)
                {
                    ++scope_exits;
                }
                EXPECT_NE(
                    instruction.opcode,
                    InstructionOpcode::CallReducer);
                EXPECT_NE(
                    instruction.opcode,
                    InstructionOpcode::PublishArtifact);
            }
        }
    }
    EXPECT_EQ(deferred_input_compensations, 0u);
    EXPECT_EQ(scope_exits, 2u);
    EXPECT_TRUE(std::ranges::any_of(
        module.functions.front().blocks,
        [](const auto& block) {
            return block.terminator.kind == TerminatorKind::StructuredFail &&
                block.terminator.failure.has_value() &&
                block.terminator.failure->code ==
                    "seedprobe_entry_pc_unsupported";
        }));
}

TEST(SeedProbeModule, EachEntryBranchArmsOnlyItsResolvedEndpoint)
{
    const ProgramModule module = ProductionSeedProbeModule();
    const auto field_pack = FieldPackIdentity();
    const auto require_one_point = [&](std::string_view prefix,
                                       SeedProbeEndpointV2 endpoint) {
        const auto configs = Instructions(module, prefix);
        ASSERT_EQ(configs.size(), 1u);
        ASSERT_TRUE(configs.front()->literal);
        const auto* bytes = std::get_if<std::vector<Byte>>(
            &configs.front()->literal->payload);
        ASSERT_NE(bytes, nullptr);
        StaticConfigTestReader reader(*bytes);
        ASSERT_TRUE(reader.Magic("SPS1"));
        std::uint32_t alternative_count = 0;
        ASSERT_TRUE(reader.U32(alternative_count));
        ASSERT_EQ(alternative_count, 1u);
        std::string pack_id;
        std::uint32_t pack_version = 0;
        ContentHash256 pack_hash;
        std::string point_id;
        std::uint8_t kind = 0;
        std::uint32_t pc = 0;
        ASSERT_TRUE(reader.String(pack_id));
        ASSERT_TRUE(reader.U32(pack_version));
        ASSERT_TRUE(reader.Hash(pack_hash));
        ASSERT_TRUE(reader.String(point_id));
        ASSERT_TRUE(reader.U8(kind));
        ASSERT_TRUE(reader.U32(pc));
        EXPECT_EQ(pack_id, field_pack.canonical_id);
        EXPECT_EQ(pack_version, field_pack.version);
        EXPECT_EQ(pack_hash, field_pack.manifest_hash);
        EXPECT_EQ(point_id, SeedProbeEndpointPointId(endpoint));
        EXPECT_EQ(kind, 0u);
        EXPECT_EQ(pc, SeedProbeEndpointPc(endpoint));
        std::uint32_t sample_count = 1;
        ASSERT_TRUE(reader.U32(sample_count));
        EXPECT_EQ(sample_count, 0u);
        EXPECT_TRUE(reader.done());
    };
    require_one_point(
        "delivery/prebattle/semantic-points",
        SeedProbeEndpointV2::AfterRandSeedSet);
    require_one_point(
        "delivery/field-return/semantic-points",
        SeedProbeEndpointV2::RandSeedCommitted);
}

TEST(SeedProbeModule, EntryPcCatalogSelectsOnlySupportedPhaseEndpoints)
{
    EXPECT_EQ(SeedProbeEndpointForEntryPc(PreBattleBeforeRandSeedSetPc),
        SeedProbeEndpointV2::AfterRandSeedSet);
    EXPECT_EQ(SeedProbeEndpointForEntryPc(FieldTransitionFastPreseedPc),
        SeedProbeEndpointV2::RandSeedCommitted);
    EXPECT_EQ(SeedProbeEndpointForEntryPc(FieldTransitionDeferredPreseedPc),
        SeedProbeEndpointV2::RandSeedCommitted);
    EXPECT_FALSE(SeedProbeEndpointForEntryPc(0x80000000u).has_value());
}

TEST(SeedProbeModule, DecodesAndValidatesCorrelatedFactualReceipts)
{
    savor::GCInputFrame frame{};
    frame.main_x = 113;
    frame.main_y = 141;
    const ProgramValueGraph graph = ResultGraph(
        frame,
        PreBattleAfterRandSeedSetPc);

    SeedProbeResultV3 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeSeedProbeResultV3(
        graph,
        result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(result.raw_seed, 0x89ABCDEFu);
    EXPECT_EQ(
        result.endpoint,
        SeedProbeEndpointV2::AfterRandSeedSet);
    EXPECT_EQ(
        result.semantic_stop.pc,
        PreBattleAfterRandSeedSetPc);
    EXPECT_EQ(result.delivery.frame, frame);
    EXPECT_NE(result.delivery.delivery_id, 0u);
    EXPECT_NE(result.delivery.poll_receipt_id, 0u);

    const SeedProbeRequestV2 request{.frame = frame};
    EXPECT_TRUE(ValidateSeedProbeResultV3(
        request,
        result,
        WorksetEpoch(42),
        &diagnostic)) << diagnostic;

    SeedProbeResultV3 decoded_result;
    const auto encoded_result = ProgramResultBytes(graph);
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    ASSERT_TRUE(phase->DecodeProgramResult(
        encoded_result,
        decoded_result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded_result, result);

    const ProgramValueGraph field_return_graph = ResultGraph(
        frame,
        FieldReturnRandSeedCommittedPc,
        43);
    SeedProbeResultV3 field_return_result;
    ASSERT_TRUE(DecodeSeedProbeResultV3(
        field_return_graph,
        field_return_result,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(
        field_return_result.endpoint,
        SeedProbeEndpointV2::RandSeedCommitted);
    EXPECT_TRUE(ValidateSeedProbeResultV3(
        request,
        field_return_result,
        WorksetEpoch(43),
        &diagnostic)) << diagnostic;
}

TEST(SeedProbeModule, RejectsProgramResultsOutsideRetainedPhaseInvariants)
{
    const auto phase = SeedProbeFullPhaseDefinitionV2();
    const auto encoded_result = ProgramResultBytes(
        ResultGraph(savor::GCInputFrame{}, PreBattleAfterRandSeedSetPc));

    const auto expect_rejected = [&](const auto& mutate)
    {
        auto decoded = DecodeProgramResultV1(encoded_result);
        ASSERT_TRUE(decoded) << decoded.status.message;
        mutate(*decoded.value);
        const auto encoded = EncodeProgramResultV1(*decoded.value);
        ASSERT_TRUE(encoded) << encoded.status.message;
        SeedProbeResultV3 observation{};
        std::string diagnostic;
        EXPECT_FALSE(phase->DecodeProgramResult(
            encoded.bytes,
            observation,
            &diagnostic));
        EXPECT_FALSE(diagnostic.empty());
    };

    expect_rejected([](ProgramResult& result)
    {
        ++result.module.revision;
    });
    expect_rejected([](ProgramResult& result)
    {
        result.entrypoint = "not-seedprobe";
    });
    expect_rejected([](ProgramResult& result)
    {
        ++result.resolved_dependencies.ir_version;
    });
}

TEST(SeedProbeModule, RejectsFramePcDeliveryAndEpochMismatches)
{
    savor::GCInputFrame frame{};
    frame.c_x = 101;
    frame.c_y = 155;
    SeedProbeResultV3 result;
    std::string diagnostic;
    ASSERT_TRUE(DecodeSeedProbeResultV3(
        ResultGraph(
            frame,
            PreBattleAfterRandSeedSetPc),
        result,
        &diagnostic)) << diagnostic;

    SeedProbeRequestV2 request{.frame = frame};
    request.frame.trig_l = 1;
    EXPECT_FALSE(ValidateSeedProbeResultV3(
        request,
        result,
        WorksetEpoch(42),
        &diagnostic));

    request.frame = frame;
    EXPECT_FALSE(ValidateSeedProbeResultV3(
        request,
        result,
        WorksetEpoch(43),
        &diagnostic));

    EXPECT_FALSE(DecodeSeedProbeResultV3(
        ResultGraph(
            frame,
            PreBattleAfterRandSeedSetPc,
            42,
            0,
            18),
        result,
        &diagnostic));
}

TEST_F(
    SqliteDbFixture,
    SeedProbeNeutralConfirmationRejectionFailsRun)
{
    auto* execution_db = db_service_->ExecutionDb();
    auto* state_db = db_service_->StateDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(execution_db, nullptr);
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_TRUE(EnsureTestSavestate(db_, temp_root_));
    ASSERT_TRUE(EnsureTestWorkflowStep(db_, 66002));

    std::string error;
    const auto spec_id = SaveTestSeedProbeSpec(
        authoring_db,
        1,
        1,
        "seedprobe-neutral-rejection",
        &error);
    ASSERT_TRUE(spec_id.has_value()) << error;
    const auto run = CreateManualRun(
        execution_db,
        analysis_db,
        *spec_id,
        SeedProbeRunStatus::Confirm,
        SeedProbeEvidenceState::Provisional,
        "seedprobe-neutral-rejection",
        &error);
    ASSERT_TRUE(run.has_value()) << error;
    ASSERT_TRUE(SetJobState(
        db_,
        run->neutral_job_id,
        "SUCCEEDED"));
    ASSERT_TRUE(TransitionEvidence(
        analysis_db,
        run->neutral_result_id,
        SeedProbeEvidenceState::Provisional,
        SeedProbeEvidenceState::Rejected,
        &error)) << error;

    const auto descriptor = BuildSeedProbeProgramDescriptor(
        execution_db,
        state_db,
        analysis_db,
        authoring_db,
        SeedProbeProgramConfig{
            .working_dir_root = temp_root_,
            .maximum_items_per_workset = 16,
        });
    auto materialization =
        TestMaterializationContext(*spec_id, 66002);
    materialization.step.domain_ref_id =
        run->probe_run_id;
    ProgramJobContinuationContext continuation_context{
        .materialization = materialization,
        .job_set_id = run->job_set_id,
        .expected_total = 1,
        .discovered_total = 1,
        .settled_total = 1,
        .succeeded_total = 1,
        .failed_total = 0,
    };
    ProgramJobContinuationResult failed{};
    ASSERT_TRUE(descriptor.job_materializer->Continue(
        continuation_context,
        &failed,
        &error)) << error;
    EXPECT_EQ(
        failed.disposition,
        ProgramJobContinuationDisposition::Failed);
    const auto failed_run =
        analysis_db->GetSeedProbeRun(run->probe_run_id);
    ASSERT_TRUE(failed_run.has_value());
    EXPECT_EQ(
        failed_run->status,
        SeedProbeRunStatus::Failed);
}

TEST_F(
    SqliteDbFixture,
    SeedProbeEndpointMismatchInvalidatesRunCancelsUnfinishedAndKeepsLateMatchingFact)
{
    auto* execution_db = db_service_->ExecutionDb();
    auto* state_db = db_service_->StateDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(execution_db, nullptr);
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_TRUE(EnsureTestSavestate(db_, temp_root_));
    ASSERT_TRUE(EnsureTestWorkflowStep(db_, 68002));

    std::string error;
    const auto spec_id = SaveTestSeedProbeSpec(
        authoring_db,
        0,
        0,
        "seedprobe-endpoint-mismatch",
        &error);
    ASSERT_TRUE(spec_id.has_value()) << error;
    const auto run = CreateManualRun(
        execution_db,
        analysis_db,
        *spec_id,
        SeedProbeRunStatus::Search,
        SeedProbeEvidenceState::Provisional,
        "seedprobe-endpoint-mismatch",
        &error);
    ASSERT_TRUE(run.has_value()) << error;
    ASSERT_TRUE(SetJobState(db_, run->neutral_job_id, "SUCCEEDED"));

    std::vector<SeedProbeJobSpec> specs;
    for (std::int32_t ordinal = 0; ordinal < 4; ++ordinal)
    {
        std::int64_t frame_id = 0;
        ASSERT_TRUE(analysis_db->EnsureSeedProbeInputFrame(
            0x8081 + ordinal,
            0x8080,
            0x0000,
            &frame_id,
            &error)) << error;
        specs.push_back({
            .version = 1,
            .stage = SeedProbeJobStage::Survey,
            .input_frame_id = frame_id,
            .sample_ordinal = ordinal,
        });
    }
    const auto observations = CreateTestJobSet(
        execution_db,
        "seedprobe-endpoint-mismatch.observations",
        "SEEDPROBE_TEST_ENDPOINTS",
        "stage=SURVEY;fixture=endpoints",
        run->probe_run_id,
        specs,
        &error);
    ASSERT_TRUE(observations.has_value()) << error;
    ASSERT_EQ(observations->job_ids.size(), 4u);
    ASSERT_TRUE(SetJobState(
        db_, observations->job_ids[0], "EXECUTION_FINISHED"));
    ASSERT_TRUE(SetJobState(
        db_, observations->job_ids[1], "EXECUTION_FINISHED"));
    ASSERT_TRUE(SetJobState(
        db_, observations->job_ids[2], "QUEUED"));
    ASSERT_TRUE(SetJobState(
        db_, observations->job_ids[3], "EXECUTION_FINISHED"));

    const auto descriptor = BuildSeedProbeProgramDescriptor(
        execution_db,
        state_db,
        analysis_db,
        authoring_db,
        SeedProbeProgramConfig{
            .working_dir_root = temp_root_,
            .maximum_items_per_workset = 16,
        });
    const auto first_job = execution_db->GetExecutionJob(
        observations->job_ids[0]);
    const auto conflicting_job = execution_db->GetExecutionJob(
        observations->job_ids[1]);
    const auto late_job = execution_db->GetExecutionJob(
        observations->job_ids[3]);
    ASSERT_TRUE(first_job.has_value());
    ASSERT_TRUE(conflicting_job.has_value());
    ASSERT_TRUE(late_job.has_value());

    const auto first = descriptor.result_handler->Process(
        SuccessfulResultContext(
            analysis_db,
            *first_job,
            101,
            1,
            1,
            51,
            SeedProbeEndpointV2::AfterRandSeedSet));
    EXPECT_EQ(first.final_job_state, "SUCCEEDED");
    auto established = analysis_db->GetSeedProbeRun(
        run->probe_run_id);
    ASSERT_TRUE(established.has_value());
    EXPECT_EQ(
        established->established_endpoint,
        savor::db::SeedProbeEndpoint::AfterRandSeedSet);
    EXPECT_EQ(
        established->established_endpoint_source_job_id,
        std::optional<std::int64_t>(run->neutral_job_id));

    const auto conflicting = descriptor.result_handler->Process(
        SuccessfulResultContext(
            analysis_db,
            *conflicting_job,
            102,
            2,
            1,
            52,
            SeedProbeEndpointV2::RandSeedCommitted));
    EXPECT_EQ(conflicting.final_job_state, "SUCCEEDED");
    EXPECT_NE(
        std::ranges::find(
            conflicting.cancellations,
            observations->job_ids[2],
            &savor::db::execution::programdb::
                ProgramResultCancellation::job_id),
        conflicting.cancellations.end());
    EXPECT_EQ(
        std::ranges::find(
            conflicting.cancellations,
            observations->job_ids[3],
            &savor::db::execution::programdb::
                ProgramResultCancellation::job_id),
        conflicting.cancellations.end());

    const auto invalidated = analysis_db->GetSeedProbeRun(
        run->probe_run_id);
    ASSERT_TRUE(invalidated.has_value());
    EXPECT_EQ(invalidated->status, SeedProbeRunStatus::Invalidated);
    EXPECT_EQ(
        invalidated->conflicting_endpoint,
        std::optional<savor::db::SeedProbeEndpoint>(
            savor::db::SeedProbeEndpoint::RandSeedCommitted));
    EXPECT_EQ(
        invalidated->conflicting_endpoint_source_job_id,
        std::optional<std::int64_t>(conflicting_job->job_id));
    ASSERT_TRUE(invalidated->invalidation_diagnostic.has_value());

    const auto before_late = analysis_db->ListSeedProbeResults(
        run->probe_run_id);
    EXPECT_EQ(
        std::ranges::count(
            before_late,
            conflicting_job->job_id,
            &savor::db::SeedProbeResultRow::source_job_id),
        1);
    const auto late = descriptor.result_handler->Process(
        SuccessfulResultContext(
            analysis_db,
            *late_job,
            103,
            3,
            1,
            53,
            SeedProbeEndpointV2::AfterRandSeedSet));
    EXPECT_EQ(late.final_job_state, "SUCCEEDED");
    const auto after_late = analysis_db->ListSeedProbeResults(
        run->probe_run_id);
    EXPECT_EQ(
        std::ranges::count(
            after_late,
            late_job->job_id,
            &savor::db::SeedProbeResultRow::source_job_id),
        1);
    EXPECT_EQ(
        analysis_db->GetSeedProbeRun(run->probe_run_id)->status,
        SeedProbeRunStatus::Invalidated);

    auto materialization = TestMaterializationContext(
        *spec_id, 68002);
    materialization.step.domain_ref_id = run->probe_run_id;
    ProgramJobContinuationResult continuation{};
    ASSERT_TRUE(descriptor.job_materializer->Continue(
        {
            .materialization = materialization,
            .job_set_id = run->job_set_id,
        },
        &continuation,
        &error)) << error;
    EXPECT_EQ(
        continuation.disposition,
        ProgramJobContinuationDisposition::Failed);
    EXPECT_EQ(
        continuation.failure_code,
        std::optional<std::string>(
            "SEEDPROBE_RUN_ENDPOINT_INVALIDATED"));
}

} // namespace
