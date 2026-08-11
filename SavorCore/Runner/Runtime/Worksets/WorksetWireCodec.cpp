#include "WorksetWireCodec.h"

#include <chrono>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

class Writer
{
public:
    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            U8(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            U8(static_cast<std::uint8_t>(value >> shift));
    }
    void String(std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail("Workset string exceeds the wire bound");
            return;
        }
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Blob(std::span<const std::uint8_t> value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail("Workset blob exceeds the wire bound");
            return;
        }
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Count(std::size_t value)
    {
        if (value > std::numeric_limits<std::uint32_t>::max())
        {
            Fail("Workset collection exceeds the wire bound");
            return;
        }
        U32(static_cast<std::uint32_t>(value));
    }
    void Fail(std::string message)
    {
        if (error_.empty())
            error_ = std::move(message);
    }
    WorksetWireCodecResult Finish(
        std::vector<std::uint8_t>& output,
        std::size_t maximum)
    {
        if (error_.empty() && bytes_.size() > maximum)
            error_ = "Encoded workset payload exceeds its bound";
        if (!error_.empty())
            return {false, std::move(error_)};
        output = std::move(bytes_);
        return {true, {}};
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::string error_;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes)
    {
    }

    bool U8(std::uint8_t& value)
    {
        if (!Need(1))
            return false;
        value = bytes_[offset_++];
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        if (!Need(4))
            return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_++])
                << shift;
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        if (!Need(8))
            return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(
                         bytes_[offset_++])
                << shift;
        return true;
    }
    bool String(std::string& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || !Need(size))
            return false;
        value.assign(
            reinterpret_cast<const char*>(
                bytes_.data() + offset_),
            size);
        offset_ += size;
        return true;
    }
    bool Blob(std::vector<std::uint8_t>& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || !Need(size))
            return false;
        value.assign(
            bytes_.begin() + offset_,
            bytes_.begin() + offset_ + size);
        offset_ += size;
        return true;
    }
    bool Count(std::uint32_t& value, std::uint32_t maximum)
    {
        if (!U32(value))
            return false;
        if (value > maximum)
        {
            Fail("Workset collection count exceeds its bound");
            return false;
        }
        return true;
    }
    void Fail(std::string message)
    {
        if (error_.empty())
            error_ = std::move(message);
    }
    WorksetWireCodecResult Finish()
    {
        if (error_.empty() && offset_ != bytes_.size())
            error_ = "Workset payload has trailing bytes";
        return error_.empty()
            ? WorksetWireCodecResult{true, {}}
            : WorksetWireCodecResult{false, std::move(error_)};
    }

private:
    bool Need(std::size_t count)
    {
        if (!error_.empty())
            return false;
        if (count > bytes_.size() - offset_)
        {
            Fail("Workset payload is truncated");
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    std::string error_;
};

void WriteModule(Writer& writer, const ProgramModuleIdentity& module)
{
    writer.String(module.canonical_id);
    writer.U32(module.revision);
    writer.String(module.canonical_hash);
}

bool ReadModule(Reader& reader, ProgramModuleIdentity& module)
{
    return reader.String(module.canonical_id) &&
        reader.U32(module.revision) &&
        reader.String(module.canonical_hash);
}

void WriteFullPhaseProgram(
    Writer& writer,
    const fullphase::FullPhaseProgramIdentity& program)
{
    writer.U32(static_cast<std::uint32_t>(program.program_kind));
    writer.U32(static_cast<std::uint32_t>(program.program_version));
    writer.String(program.canonical_id);
    writer.U32(program.contract_revision);
    writer.String(program.canonical_sha256);
}

bool ReadFullPhaseProgram(
    Reader& reader,
    fullphase::FullPhaseProgramIdentity& program)
{
    std::uint32_t kind = 0;
    std::uint32_t version = 0;
    if (!reader.U32(kind) || !reader.U32(version) ||
        !reader.String(program.canonical_id) ||
        !reader.U32(program.contract_revision) ||
        !reader.String(program.canonical_sha256))
    {
        return false;
    }
    program.program_kind = static_cast<std::int32_t>(kind);
    program.program_version = static_cast<std::int32_t>(version);
    return true;
}

void WriteBudgets(
    Writer& writer,
    const program::ProgramBudgets& budgets)
{
    writer.U64(budgets.maximum_instructions);
    writer.U64(budgets.maximum_calls);
    writer.U64(budgets.maximum_call_depth);
    writer.U64(budgets.maximum_action_requests);
    writer.U64(budgets.maximum_emissions);
    writer.U64(budgets.maximum_artifacts);
    writer.U64(budgets.maximum_values);
    writer.U64(budgets.maximum_value_bytes);
    writer.U64(budgets.maximum_trace_events);
}

bool ReadBudgets(
    Reader& reader,
    program::ProgramBudgets& budgets)
{
    return reader.U64(budgets.maximum_instructions) &&
        reader.U64(budgets.maximum_calls) &&
        reader.U64(budgets.maximum_call_depth) &&
        reader.U64(budgets.maximum_action_requests) &&
        reader.U64(budgets.maximum_emissions) &&
        reader.U64(budgets.maximum_artifacts) &&
        reader.U64(budgets.maximum_values) &&
        reader.U64(budgets.maximum_value_bytes) &&
        reader.U64(budgets.maximum_trace_events);
}

void WriteRuntimeContract(
    Writer& writer,
    const fullphase::FullPhaseRuntimeContract& contract)
{
    WriteModule(writer, contract.module);
    writer.String(contract.entrypoint);
    writer.String(contract.dependency_lock_sha256);
    writer.String(contract.verified_dependency_sha256);
    writer.String(contract.runtime_profile_sha256);
    writer.U8(static_cast<std::uint8_t>(contract.state_policy));
    writer.U8(static_cast<std::uint8_t>(contract.execution.intent));
    writer.U8(contract.execution.allow_movie_playback ? 1 : 0);
    writer.U8(contract.execution.allow_movie_recording ? 1 : 0);
    writer.U8(contract.execution.allow_input ? 1 : 0);
    writer.U8(contract.execution.allow_capture ? 1 : 0);
    writer.U8(contract.execution.record_trace ? 1 : 0);
    WriteBudgets(writer, contract.limits);
    writer.String(contract.baseline_lineage);
    writer.String(contract.movie_policy_sha256);
    writer.String(contract.service_policy_sha256);
}

bool ReadBool(Reader& reader, bool& output)
{
    std::uint8_t value = 0;
    if (!reader.U8(value) || value > 1)
        return false;
    output = value != 0;
    return true;
}

bool ReadRuntimeContract(
    Reader& reader,
    fullphase::FullPhaseRuntimeContract& contract)
{
    std::uint8_t state_policy = 0;
    std::uint8_t intent = 0;
    if (!ReadModule(reader, contract.module) ||
        !reader.String(contract.entrypoint) ||
        !reader.String(contract.dependency_lock_sha256) ||
        !reader.String(contract.verified_dependency_sha256) ||
        !reader.String(contract.runtime_profile_sha256) ||
        !reader.U8(state_policy) ||
        state_policy > static_cast<std::uint8_t>(
            program::InvocationStatePolicy::EstablishBaseline) ||
        !reader.U8(intent) ||
        intent > static_cast<std::uint8_t>(
            program::ExecutionIntent::Replay) ||
        !ReadBool(reader, contract.execution.allow_movie_playback) ||
        !ReadBool(reader, contract.execution.allow_movie_recording) ||
        !ReadBool(reader, contract.execution.allow_input) ||
        !ReadBool(reader, contract.execution.allow_capture) ||
        !ReadBool(reader, contract.execution.record_trace) ||
        !ReadBudgets(reader, contract.limits) ||
        !reader.String(contract.baseline_lineage) ||
        !reader.String(contract.movie_policy_sha256) ||
        !reader.String(contract.service_policy_sha256))
    {
        return false;
    }
    contract.state_policy =
        static_cast<program::InvocationStatePolicy>(state_policy);
    contract.execution.intent =
        static_cast<program::ExecutionIntent>(intent);
    return true;
}

void WriteProgramPackage(
    Writer& writer,
    const fullphase::FullPhaseProgramPackage& package)
{
    WriteFullPhaseProgram(writer, package.identity);
    WriteRuntimeContract(writer, package.runtime_contract);
    writer.Count(package.module_closure.size());
    for (const EncodedModuleEnvelope& module : package.module_closure)
    {
        WriteModule(writer, module.identity);
        writer.U32(module.format_version);
        writer.U8(module.development_only ? 1 : 0);
        writer.Blob(module.payload);
    }
    writer.String(package.canonical_sha256);
}

bool ReadProgramPackage(
    Reader& reader,
    fullphase::FullPhaseProgramPackage& package)
{
    std::uint32_t module_count = 0;
    if (!ReadFullPhaseProgram(reader, package.identity) ||
        !ReadRuntimeContract(reader, package.runtime_contract) ||
        !reader.Count(module_count, 64))
    {
        return false;
    }
    package.module_closure.reserve(module_count);
    for (std::uint32_t index = 0; index < module_count; ++index)
    {
        EncodedModuleEnvelope module;
        if (!ReadModule(reader, module.identity) ||
            !reader.U32(module.format_version) ||
            !ReadBool(reader, module.development_only) ||
            !reader.Blob(module.payload))
        {
            return false;
        }
        package.module_closure.push_back(std::move(module));
    }
    return reader.String(package.canonical_sha256);
}

void WriteCommonInput(
    Writer& writer,
    const fullphase::FullPhaseCommonInput& input)
{
    writer.String(input.schema_id);
    writer.U32(input.schema_version);
    writer.Blob(input.payload);
    writer.String(input.content_sha256);
}

bool ReadCommonInput(
    Reader& reader,
    fullphase::FullPhaseCommonInput& input)
{
    return reader.String(input.schema_id) &&
        reader.U32(input.schema_version) &&
        reader.Blob(input.payload) &&
        reader.String(input.content_sha256);
}

void WriteCompatibility(
    Writer& writer,
    const ArtifactCompatibilityToken& value)
{
    writer.String(value.game_id);
    writer.String(value.iso_sha256);
    writer.String(value.emulator_build);
    writer.String(value.runtime_revision);
}

bool ReadCompatibility(
    Reader& reader,
    ArtifactCompatibilityToken& value)
{
    return reader.String(value.game_id) &&
        reader.String(value.iso_sha256) &&
        reader.String(value.emulator_build) &&
        reader.String(value.runtime_revision);
}

void WriteBaseline(
    Writer& writer,
    const ProgramBaselineDefinition& baseline)
{
    const ProgramBaselineArtifact& artifact = baseline.artifact;
    writer.U8(static_cast<std::uint8_t>(artifact.kind));
    writer.String(artifact.state_path.generic_string());
    writer.String(artifact.state_sha256);
    writer.U8(artifact.movie_path ? 1 : 0);
    if (artifact.movie_path)
        writer.String(artifact.movie_path->generic_string());
    writer.String(artifact.movie_sha256);
    WriteCompatibility(writer, artifact.compatibility);
    writer.String(artifact.lineage.edge);
    writer.String(artifact.lineage.producer);
    writer.String(baseline.lineage);
    writer.Count(baseline.components.size());
    for (const ProgramBaselineComponent& component :
         baseline.components)
    {
        writer.String(component.canonical_id);
        writer.U32(component.revision);
        writer.String(component.schema_id);
        writer.String(component.content_sha256);
        writer.U8(static_cast<std::uint8_t>(component.policy));
        writer.Blob(component.immutable_bytes);
    }
}

bool ReadBaseline(
    Reader& reader,
    ProgramBaselineDefinition& baseline)
{
    std::uint8_t artifact_kind = 0;
    if (!reader.U8(artifact_kind) ||
        artifact_kind > static_cast<std::uint8_t>(
            ProgramBaselineArtifactKind::ReadOnlyMovie))
    {
        reader.Fail("Program baseline enum is invalid");
        return false;
    }
    ProgramBaselineArtifact artifact;
    artifact.kind =
        static_cast<ProgramBaselineArtifactKind>(artifact_kind);
    std::string state_path;
    std::uint8_t has_movie = 0;
    std::string movie_path;
    if (!reader.String(state_path) ||
        !reader.String(artifact.state_sha256) ||
        !reader.U8(has_movie) || has_movie > 1 ||
        (has_movie && !reader.String(movie_path)) ||
        !reader.String(artifact.movie_sha256) ||
        !ReadCompatibility(reader, artifact.compatibility) ||
        !reader.String(artifact.lineage.edge) ||
        !reader.String(artifact.lineage.producer))
    {
        reader.Fail("Program baseline artifact is invalid");
        return false;
    }
    artifact.state_path = std::move(state_path);
    if (has_movie)
        artifact.movie_path = std::move(movie_path);
    baseline.artifact = std::move(artifact);
    std::uint32_t component_count = 0;
    if (!reader.String(baseline.lineage) ||
        !reader.Count(component_count, 256))
    {
        return false;
    }
    baseline.components.reserve(component_count);
    for (std::uint32_t index = 0; index < component_count; ++index)
    {
        ProgramBaselineComponent component;
        std::uint8_t policy = 0;
        if (!reader.String(component.canonical_id) ||
            !reader.U32(component.revision) ||
            !reader.String(component.schema_id) ||
            !reader.String(component.content_sha256) ||
            !reader.U8(policy) ||
            policy >
                static_cast<std::uint8_t>(
                    ProgramBaselineComponentPolicy::
                        ResetForEveryItem) ||
            !reader.Blob(component.immutable_bytes))
        {
            reader.Fail("Program baseline component is invalid");
            return false;
        }
        component.policy =
            static_cast<ProgramBaselineComponentPolicy>(policy);
        baseline.components.push_back(std::move(component));
    }
    return true;
}

void WriteProgressSchema(
    Writer& writer,
    const progress::ProgressSchemaIdentity& identity)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.revision);
    writer.String(identity.sha256);
}

bool ReadProgressSchema(
    Reader& reader,
    progress::ProgressSchemaIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.revision) &&
        reader.String(identity.sha256);
}

void WriteProgressFormatter(
    Writer& writer,
    const progress::ProgressFormatterIdentity& identity)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.revision);
    writer.String(identity.sha256);
}

bool ReadProgressFormatter(
    Reader& reader,
    progress::ProgressFormatterIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.revision) &&
        reader.String(identity.sha256);
}

void WriteProgressPlan(
    Writer& writer,
    const progress::ProgressPlanV1& plan)
{
    writer.U32(plan.version);
    writer.Count(plan.points.size());
    for (const progress::ProgressPointBindingV1& point : plan.points)
    {
        writer.String(point.library_id);
        writer.U32(point.library_revision);
        writer.String(point.library_sha256);
        writer.String(point.point_id);
        writer.U8(static_cast<std::uint8_t>(point.provider));
        writer.U8(point.breakpoint_pc.has_value() ? 1 : 0);
        if (point.breakpoint_pc)
            writer.U32(*point.breakpoint_pc);
        WriteProgressSchema(writer, point.schema);
        WriteProgressFormatter(writer, point.formatter);
        writer.Count(point.runtime_sample_trigger_pcs.size());
        for (const std::uint32_t pc :
             point.runtime_sample_trigger_pcs)
        {
            writer.U32(pc);
        }
        writer.Blob(point.configuration);
    }
    writer.String(plan.content_sha256);
}

bool ReadProgressPlan(
    Reader& reader,
    progress::ProgressPlanV1& plan)
{
    std::uint32_t point_count = 0;
    if (!reader.U32(plan.version) ||
        !reader.Count(point_count, 256))
    {
        return false;
    }
    plan.points.clear();
    plan.points.reserve(point_count);
    for (std::uint32_t index = 0; index < point_count; ++index)
    {
        progress::ProgressPointBindingV1 point;
        std::uint8_t provider = 0;
        std::uint8_t has_pc = 0;
        std::uint32_t pc = 0;
        std::uint32_t trigger_count = 0;
        if (!reader.String(point.library_id) ||
            !reader.U32(point.library_revision) ||
            !reader.String(point.library_sha256) ||
            !reader.String(point.point_id) ||
            !reader.U8(provider) ||
            provider < static_cast<std::uint8_t>(
                progress::ProgressProviderKind::BreakpointCapture) ||
            provider > static_cast<std::uint8_t>(
                progress::ProgressProviderKind::PhaseLibrary) ||
            !reader.U8(has_pc) || has_pc > 1 ||
            (has_pc && !reader.U32(pc)) ||
            !ReadProgressSchema(reader, point.schema) ||
            !ReadProgressFormatter(reader, point.formatter) ||
            !reader.Count(trigger_count, 128))
        {
            return false;
        }
        point.runtime_sample_trigger_pcs.reserve(trigger_count);
        for (std::uint32_t trigger = 0;
             trigger < trigger_count;
             ++trigger)
        {
            std::uint32_t trigger_pc = 0;
            if (!reader.U32(trigger_pc))
                return false;
            point.runtime_sample_trigger_pcs.push_back(trigger_pc);
        }
        if (
            !reader.Blob(point.configuration))
        {
            return false;
        }
        point.provider =
            static_cast<progress::ProgressProviderKind>(provider);
        if (has_pc)
            point.breakpoint_pc = pc;
        plan.points.push_back(std::move(point));
    }
    return reader.String(plan.content_sha256);
}

void WriteCaptureBinding(
    Writer& writer,
    const std::optional<WorksetCaptureBindingV1>& capture)
{
    writer.U8(capture.has_value() ? 1 : 0);
    if (!capture)
        return;
    writer.U32(capture->version);
    writer.U8(static_cast<std::uint8_t>(capture->storage));
    writer.String(capture->profile_json);
    writer.String(capture->profile_sidecar_path.generic_string());
    writer.String(capture->profile_sha256);
    writer.String(capture->expected_module_sha256);
    writer.String(capture->resolved_observation_sha256);
    writer.String(capture->output_directory.generic_string());
    writer.String(capture->content_sha256);
}

bool ReadCaptureBinding(
    Reader& reader,
    std::optional<WorksetCaptureBindingV1>& capture)
{
    std::uint8_t present = 0;
    if (!reader.U8(present) || present > 1)
        return false;
    if (!present)
    {
        capture.reset();
        return true;
    }
    WorksetCaptureBindingV1 value;
    std::uint8_t storage = 0;
    std::string sidecar;
    std::string output_directory;
    if (!reader.U32(value.version) ||
        !reader.U8(storage) ||
        storage < static_cast<std::uint8_t>(
            CaptureProfileStorageV1::Inline) ||
        storage > static_cast<std::uint8_t>(
            CaptureProfileStorageV1::ContentAddressedSidecar) ||
        !reader.String(value.profile_json) ||
        !reader.String(sidecar) ||
        !reader.String(value.profile_sha256) ||
        !reader.String(value.expected_module_sha256) ||
        !reader.String(value.resolved_observation_sha256) ||
        !reader.String(output_directory) ||
        !reader.String(value.content_sha256))
    {
        return false;
    }
    value.storage = static_cast<CaptureProfileStorageV1>(storage);
    value.profile_sidecar_path = std::move(sidecar);
    value.output_directory = std::move(output_directory);
    capture = std::move(value);
    return true;
}

void WriteLimits(Writer& writer, const WorkerWorksetLimits& limits)
{
    writer.U32(limits.maximum_items_per_workset);
    writer.U64(limits.maximum_encoded_workset_bytes);
    writer.U64(limits.maximum_capture_profile_bytes);
    writer.U32(limits.maximum_item_credits);
    writer.U32(limits.maximum_active_and_staged_items);
    writer.U32(limits.finalizer_threads);
    writer.U32(limits.maximum_pending_finalizers);
    writer.U64(limits.maximum_pending_finalizer_bytes);
    writer.U32(limits.maximum_retained_terminals);
    writer.U64(limits.maximum_retained_terminal_bytes);
    writer.U32(limits.progressive_start_concurrency);
}

bool ReadLimits(Reader& reader, WorkerWorksetLimits& limits)
{
    std::uint64_t encoded_bytes = 0;
    std::uint64_t capture_profile_bytes = 0;
    std::uint64_t finalizer_bytes = 0;
    std::uint64_t terminal_bytes = 0;
    if (!reader.U32(limits.maximum_items_per_workset) ||
        !reader.U64(encoded_bytes) ||
        !reader.U64(capture_profile_bytes) ||
        !reader.U32(limits.maximum_item_credits) ||
        !reader.U32(limits.maximum_active_and_staged_items) ||
        !reader.U32(limits.finalizer_threads) ||
        !reader.U32(limits.maximum_pending_finalizers) ||
        !reader.U64(finalizer_bytes) ||
        !reader.U32(limits.maximum_retained_terminals) ||
        !reader.U64(terminal_bytes) ||
        !reader.U32(limits.progressive_start_concurrency))
    {
        return false;
    }
    limits.maximum_encoded_workset_bytes =
        static_cast<std::size_t>(encoded_bytes);
    limits.maximum_capture_profile_bytes =
        static_cast<std::size_t>(capture_profile_bytes);
    limits.maximum_pending_finalizer_bytes =
        static_cast<std::size_t>(finalizer_bytes);
    limits.maximum_retained_terminal_bytes =
        static_cast<std::size_t>(terminal_bytes);
    return true;
}

} // namespace

WorksetWireCodecResult EncodeWorkerWorksetV4(
    const WorkerWorksetDefinition& definition,
    std::vector<std::uint8_t>& output)
{
    Writer writer;
    writer.U32(kWorksetWireVersionV4);
    writer.U64(definition.workset_id.value());
    writer.U64(definition.phase_invocation.invocation_id.workflow_step_id);
    writer.U64(definition.phase_invocation.invocation_id.root_job_set_id);
    WriteProgramPackage(
        writer, definition.phase_invocation.program_package);
    WriteCommonInput(
        writer, definition.phase_invocation.common_input);
    const WorkerWorksetExecutionKey& key = definition.execution_key;
    WriteModule(writer, key.module);
    writer.String(key.entrypoint);
    writer.String(key.verified_dependency_sha256);
    writer.String(key.runtime_profile_sha256);
    writer.String(key.baseline.sha256);
    writer.String(key.movie_policy_sha256);
    writer.String(key.service_policy_sha256);
    writer.String(key.program_package_sha256);
    writer.String(key.common_input_sha256);
    writer.String(key.capture_binding_sha256);
    writer.String(key.progress_plan_sha256);
    writer.String(key.canonical_sha256);
    WriteBaseline(writer, definition.baseline);
    WriteCaptureBinding(writer, definition.capture);
    WriteProgressPlan(writer, definition.progress_plan);
    writer.Count(definition.items.size());
    for (const WorksetItemTemplate& item : definition.items)
    {
        writer.U64(item.item_id.value());
        writer.U32(item.ordinal);
        writer.U64(item.execution.execution_id.value());
        writer.U64(item.execution.attempt_id.value());
        writer.Blob(item.execution.input_payload);
        writer.U64(item.declared_terminal_bytes);
        writer.String(item.correlation.durable_job_id);
        writer.String(item.correlation.claim_token);
        writer.String(item.correlation.parent_correlation);
    }
    return writer.Finish(output, kMaximumWorksetWireBytes);
}

WorksetWireCodecResult DecodeWorkerWorksetV4(
    std::span<const std::uint8_t> input,
    WorkerWorksetDefinition& output)
{
    if (input.size() > kMaximumWorksetWireBytes)
        return {false, "Encoded workset exceeds its wire bound"};
    Reader reader(input);
    WorkerWorksetDefinition candidate;
    std::uint32_t version = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t workflow_step_id = 0;
    std::uint64_t root_job_set_id = 0;
    WorkerWorksetExecutionKey& key = candidate.execution_key;
    if (!reader.U32(version) || version != kWorksetWireVersionV4 ||
        !reader.U64(workset_id) ||
        !reader.U64(workflow_step_id) ||
        !reader.U64(root_job_set_id) ||
        !ReadProgramPackage(
            reader, candidate.phase_invocation.program_package) ||
        !ReadCommonInput(
            reader, candidate.phase_invocation.common_input) ||
        !ReadModule(reader, key.module) ||
        !reader.String(key.entrypoint) ||
        !reader.String(key.verified_dependency_sha256) ||
        !reader.String(key.runtime_profile_sha256) ||
        !reader.String(key.baseline.sha256) ||
        !reader.String(key.movie_policy_sha256) ||
        !reader.String(key.service_policy_sha256) ||
        !reader.String(key.program_package_sha256) ||
        !reader.String(key.common_input_sha256) ||
        !reader.String(key.capture_binding_sha256) ||
        !reader.String(key.progress_plan_sha256) ||
        !reader.String(key.canonical_sha256) ||
        !ReadBaseline(reader, candidate.baseline) ||
        !ReadCaptureBinding(reader, candidate.capture) ||
        !ReadProgressPlan(reader, candidate.progress_plan))
    {
        return {false, "WorkerWorkset header is invalid"};
    }
    candidate.workset_id = WorkerWorksetId(workset_id);
    candidate.phase_invocation.invocation_id = {
        .workflow_step_id = workflow_step_id,
        .root_job_set_id = root_job_set_id,
    };
    std::uint32_t item_count = 0;
    if (!reader.Count(item_count, 16))
        return reader.Finish();
    candidate.items.reserve(item_count);
    for (std::uint32_t index = 0; index < item_count; ++index)
    {
        WorksetItemTemplate item;
        std::uint64_t item_id = 0;
        std::uint64_t execution_id = 0;
        std::uint64_t attempt_id = 0;
        std::uint64_t terminal_bytes = 0;
        if (!reader.U64(item_id) ||
            !reader.U32(item.ordinal) ||
            !reader.U64(execution_id) ||
            !reader.U64(attempt_id) ||
            !reader.Blob(item.execution.input_payload) ||
            !reader.U64(terminal_bytes) ||
            !reader.String(item.correlation.durable_job_id) ||
            !reader.String(item.correlation.claim_token) ||
            !reader.String(item.correlation.parent_correlation) ||
            terminal_bytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()))
        {
            return {false, "WorkerWorkset item is invalid"};
        }
        item.item_id = WorkerWorksetItemId(item_id);
        item.execution.execution_id =
            ProgramExecutionId(execution_id);
        item.execution.attempt_id = AttemptId(attempt_id);
        item.declared_terminal_bytes =
            static_cast<std::size_t>(terminal_bytes);
        candidate.items.push_back(std::move(item));
    }
    WorksetWireCodecResult finished = reader.Finish();
    if (!finished)
        return finished;
    candidate.encoded_size_bytes = input.size();
    output = std::move(candidate);
    return {true, {}};
}

WorksetWireCodecResult EncodeWorksetCaptureBindingV1(
    const std::optional<WorksetCaptureBindingV1>& binding,
    std::vector<std::uint8_t>& output)
{
    if (binding &&
        (!static_cast<bool>(*binding) ||
         ComputeWorksetCaptureBindingHashV1(*binding) !=
             binding->content_sha256))
    {
        return {false, "Workset capture binding is not canonical"};
    }
    Writer writer;
    writer.U32(kWorksetCaptureBindingWireVersionV1);
    WriteCaptureBinding(writer, binding);
    return writer.Finish(output, kMaximumWorksetWireBytes);
}

WorksetWireCodecResult DecodeWorksetCaptureBindingV1(
    std::span<const std::uint8_t> input,
    std::optional<WorksetCaptureBindingV1>& output)
{
    if (input.size() > kMaximumWorksetWireBytes)
        return {false, "Encoded workset capture binding exceeds its bound"};
    Reader reader(input);
    std::uint32_t version = 0;
    std::optional<WorksetCaptureBindingV1> candidate;
    if (!reader.U32(version) ||
        version != kWorksetCaptureBindingWireVersionV1 ||
        !ReadCaptureBinding(reader, candidate))
    {
        return {false, "Workset capture binding payload is invalid"};
    }
    WorksetWireCodecResult finished = reader.Finish();
    if (!finished)
        return finished;
    if (candidate &&
        (!static_cast<bool>(*candidate) ||
         ComputeWorksetCaptureBindingHashV1(*candidate) !=
             candidate->content_sha256))
    {
        return {false, "Workset capture binding is not canonical"};
    }
    output = std::move(candidate);
    return {true, {}};
}

WorksetWireCodecResult EncodeProgressPlanV1(
    const progress::ProgressPlanV1& plan,
    std::vector<std::uint8_t>& output)
{
    const progress::ProgressValidationResult validated =
        progress::ValidateProgressPlanV1(plan);
    if (!validated)
    {
        return {
            false,
            validated.message.empty()
                ? "Progress plan is not canonical"
                : validated.message};
    }
    Writer writer;
    writer.U32(kProgressPlanWireVersionV1);
    WriteProgressPlan(writer, plan);
    return writer.Finish(output, kMaximumWorksetWireBytes);
}

WorksetWireCodecResult DecodeProgressPlanV1(
    std::span<const std::uint8_t> input,
    progress::ProgressPlanV1& output)
{
    if (input.size() > kMaximumWorksetWireBytes)
        return {false, "Encoded progress plan exceeds its bound"};
    Reader reader(input);
    std::uint32_t version = 0;
    progress::ProgressPlanV1 candidate;
    if (!reader.U32(version) ||
        version != kProgressPlanWireVersionV1 ||
        !ReadProgressPlan(reader, candidate))
    {
        return {false, "Progress plan payload is invalid"};
    }
    WorksetWireCodecResult finished = reader.Finish();
    if (!finished)
        return finished;
    const progress::ProgressValidationResult validated =
        progress::ValidateProgressPlanV1(candidate);
    if (!validated)
    {
        return {
            false,
            validated.message.empty()
                ? "Progress plan is not canonical"
                : validated.message};
    }
    output = std::move(candidate);
    return {true, {}};
}

WorksetWireCodecResult EncodeWorkerRuntimeContractV1(
    const WorkerRuntimeContractV1& contract,
    std::vector<std::uint8_t>& output)
{
    const WorksetValidationResult validated =
        ValidateWorkerRuntimeContractV1(contract);
    if (!validated.ok)
    {
        return {
            false,
            validated.error.message.empty()
                ? "Worker runtime contract is not canonical"
                : validated.error.message};
    }
    Writer writer;
    writer.U32(kWorkerRuntimeContractWireVersionV1);
    writer.U32(contract.contract_version);
    writer.U32(contract.wrms_protocol_version);
    writer.U32(contract.workset_wire_version);
    writer.U32(contract.program_module_format_version);
    writer.U32(contract.program_invocation_format_version);
    writer.U32(contract.program_result_format_version);
    writer.String(contract.supported_game_id);
    writer.String(contract.executable_identity);
    writer.String(contract.address_map_revision);
    writer.String(contract.emulator_bridge_revision);
    writer.String(contract.build_identity);
    writer.String(contract.static_runtime_abi_sha256);
    WriteLimits(writer, contract.limits);
    writer.String(contract.canonical_sha256);
    return writer.Finish(output, 1024 * 1024);
}

WorksetWireCodecResult DecodeWorkerRuntimeContractV1(
    std::span<const std::uint8_t> input,
    WorkerRuntimeContractV1& output)
{
    Reader reader(input);
    WorkerRuntimeContractV1 candidate;
    std::uint32_t version = 0;
    std::uint32_t wrms = 0;
    if (!reader.U32(version) ||
        version != kWorkerRuntimeContractWireVersionV1 ||
        !reader.U32(candidate.contract_version) ||
        !reader.U32(wrms) ||
        wrms > std::numeric_limits<std::uint16_t>::max() ||
        !reader.U32(candidate.workset_wire_version) ||
        !reader.U32(candidate.program_module_format_version) ||
        !reader.U32(candidate.program_invocation_format_version) ||
        !reader.U32(candidate.program_result_format_version) ||
        !reader.String(candidate.supported_game_id) ||
        !reader.String(candidate.executable_identity) ||
        !reader.String(candidate.address_map_revision) ||
        !reader.String(candidate.emulator_bridge_revision) ||
        !reader.String(candidate.build_identity) ||
        !reader.String(candidate.static_runtime_abi_sha256) ||
        !ReadLimits(reader, candidate.limits) ||
        !reader.String(candidate.canonical_sha256))
    {
        return {false, "Worker runtime contract is invalid"};
    }
    candidate.wrms_protocol_version =
        static_cast<std::uint16_t>(wrms);
    WorksetWireCodecResult finished = reader.Finish();
    if (!finished)
        return finished;
    const WorksetValidationResult validated =
        ValidateWorkerRuntimeContractV1(candidate);
    if (!validated.ok)
    {
        return {
            false,
            validated.error.message.empty()
                ? "Worker runtime contract is not canonical"
                : validated.error.message};
    }
    output = std::move(candidate);
    return {true, {}};
}

} // namespace savor::runtime
