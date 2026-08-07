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

void WriteLimits(Writer& writer, const WorkerWorksetLimits& limits)
{
    writer.U32(limits.maximum_items_per_workset);
    writer.U64(limits.maximum_encoded_workset_bytes);
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
    std::uint64_t finalizer_bytes = 0;
    std::uint64_t terminal_bytes = 0;
    if (!reader.U32(limits.maximum_items_per_workset) ||
        !reader.U64(encoded_bytes) ||
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
    limits.maximum_pending_finalizer_bytes =
        static_cast<std::size_t>(finalizer_bytes);
    limits.maximum_retained_terminal_bytes =
        static_cast<std::size_t>(terminal_bytes);
    return true;
}

} // namespace

WorksetWireCodecResult EncodeWorkerWorksetV2(
    const WorkerWorksetDefinition& definition,
    std::vector<std::uint8_t>& output)
{
    Writer writer;
    writer.U32(kWorksetWireVersionV2);
    writer.U64(definition.workset_id.value());
    writer.U64(definition.phase_invocation.invocation_id.workflow_step_id);
    writer.U64(definition.phase_invocation.invocation_id.root_job_set_id);
    WriteFullPhaseProgram(
        writer, definition.phase_invocation.program);
    const WorkerWorksetExecutionKey& key = definition.execution_key;
    WriteModule(writer, key.module);
    writer.String(key.entrypoint);
    writer.String(key.verified_dependency_sha256);
    writer.String(key.runtime_profile_sha256);
    writer.String(key.baseline.sha256);
    writer.String(key.movie_policy_sha256);
    writer.String(key.service_policy_sha256);
    writer.String(key.canonical_sha256);
    WriteBaseline(writer, definition.baseline);
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

WorksetWireCodecResult DecodeWorkerWorksetV2(
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
    if (!reader.U32(version) || version != kWorksetWireVersionV2 ||
        !reader.U64(workset_id) ||
        !reader.U64(workflow_step_id) ||
        !reader.U64(root_job_set_id) ||
        !ReadFullPhaseProgram(
            reader, candidate.phase_invocation.program) ||
        !ReadModule(reader, key.module) ||
        !reader.String(key.entrypoint) ||
        !reader.String(key.verified_dependency_sha256) ||
        !reader.String(key.runtime_profile_sha256) ||
        !reader.String(key.baseline.sha256) ||
        !reader.String(key.movie_policy_sha256) ||
        !reader.String(key.service_policy_sha256) ||
        !reader.String(key.canonical_sha256) ||
        !ReadBaseline(reader, candidate.baseline))
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

WorksetWireCodecResult EncodeWorkerRuntimeManifestV1(
    const WorkerRuntimeManifest& manifest,
    std::vector<std::uint8_t>& output)
{
    const WorksetValidationResult validated =
        ValidateWorkerRuntimeManifest(manifest);
    if (!validated.ok)
    {
        return {
            false,
            validated.error.message.empty()
                ? "Worker runtime manifest is not canonical"
                : validated.error.message};
    }
    Writer writer;
    writer.U32(kRuntimeManifestWireVersionV1);
    writer.U32(manifest.wrms_protocol_version);
    writer.U32(manifest.program_module_format_version);
    writer.U32(manifest.program_invocation_format_version);
    writer.U32(manifest.program_result_format_version);
    writer.String(manifest.runtime_profile_sha256);
    writer.String(manifest.dependency_manifest_sha256);
    writer.U8(static_cast<std::uint8_t>(manifest.catalog_status));
    writer.U64(manifest.catalog_generation);
    writer.String(manifest.catalog_sha256);
    writer.Count(manifest.modules.size());
    for (const RuntimeModuleManifestEntry& entry : manifest.modules)
    {
        WriteModule(writer, entry.module);
        writer.Count(entry.entrypoints.size());
        for (const std::string& entrypoint : entry.entrypoints)
            writer.String(entrypoint);
        writer.String(entry.dependency_manifest_sha256);
        writer.U8(entry.development_only ? 1 : 0);
    }
    WriteLimits(writer, manifest.limits);
    return writer.Finish(output, 1024 * 1024);
}

WorksetWireCodecResult DecodeWorkerRuntimeManifestV1(
    std::span<const std::uint8_t> input,
    WorkerRuntimeManifest& output)
{
    Reader reader(input);
    WorkerRuntimeManifest candidate;
    std::uint32_t version = 0;
    std::uint32_t wrms = 0;
    std::uint8_t status = 0;
    if (!reader.U32(version) ||
        version != kRuntimeManifestWireVersionV1 ||
        !reader.U32(wrms) ||
        wrms > std::numeric_limits<std::uint16_t>::max() ||
        !reader.U32(candidate.program_module_format_version) ||
        !reader.U32(candidate.program_invocation_format_version) ||
        !reader.U32(candidate.program_result_format_version) ||
        !reader.String(candidate.runtime_profile_sha256) ||
        !reader.String(candidate.dependency_manifest_sha256) ||
        !reader.U8(status) ||
        status >
            static_cast<std::uint8_t>(
                RuntimeCatalogStatus::CompleteExact) ||
        !reader.U64(candidate.catalog_generation) ||
        !reader.String(candidate.catalog_sha256))
    {
        return {false, "Worker runtime manifest is invalid"};
    }
    candidate.wrms_protocol_version =
        static_cast<std::uint16_t>(wrms);
    candidate.catalog_status =
        static_cast<RuntimeCatalogStatus>(status);
    std::uint32_t module_count = 0;
    if (!reader.Count(module_count, 256))
        return reader.Finish();
    candidate.modules.reserve(module_count);
    for (std::uint32_t index = 0; index < module_count; ++index)
    {
        RuntimeModuleManifestEntry entry;
        std::uint32_t entrypoint_count = 0;
        std::uint8_t development_only = 0;
        if (!ReadModule(reader, entry.module) ||
            !reader.Count(entrypoint_count, 256))
        {
            return {false, "Runtime manifest module is invalid"};
        }
        entry.entrypoints.reserve(entrypoint_count);
        for (std::uint32_t entrypoint = 0;
             entrypoint < entrypoint_count;
             ++entrypoint)
        {
            std::string name;
            if (!reader.String(name))
                return {false, "Runtime manifest entrypoint is invalid"};
            entry.entrypoints.push_back(std::move(name));
        }
        if (!reader.String(entry.dependency_manifest_sha256) ||
            !reader.U8(development_only) ||
            development_only > 1)
        {
            return {false, "Runtime manifest module flags are invalid"};
        }
        entry.development_only = development_only != 0;
        candidate.modules.push_back(std::move(entry));
    }
    if (!ReadLimits(reader, candidate.limits))
        return {false, "Runtime manifest limits are invalid"};
    WorksetWireCodecResult finished = reader.Finish();
    if (!finished)
        return finished;
    const WorksetValidationResult validated =
        ValidateWorkerRuntimeManifest(candidate);
    if (!validated.ok)
    {
        return {
            false,
            validated.error.message.empty()
                ? "Worker runtime manifest is not canonical"
                : validated.error.message};
    }
    output = std::move(candidate);
    return {true, {}};
}

} // namespace savor::runtime
