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

void WriteCompatibility(
    Writer& writer,
    const StateCompatibilityToken& value)
{
    writer.String(value.game_id);
    writer.String(value.iso_sha256);
    writer.String(value.emulator_build);
    writer.String(value.runtime_revision);
}

bool ReadCompatibility(
    Reader& reader,
    StateCompatibilityToken& value)
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
    writer.U8(static_cast<std::uint8_t>(baseline.state_kind));
    writer.U8(baseline.current_session ? 1 : 0);
    if (baseline.current_session)
    {
        writer.U64(baseline.current_session->session_id.value());
        writer.U64(baseline.current_session->state_epoch.value());
        writer.U8(
            baseline.current_session->require_clean_idle ? 1 : 0);
    }
    writer.U8(baseline.artifact ? 1 : 0);
    if (baseline.artifact)
    {
        const ProgramBaselineArtifact& artifact = *baseline.artifact;
        writer.String(artifact.state_path.generic_string());
        writer.String(artifact.state_sha256);
        writer.U8(artifact.movie_path ? 1 : 0);
        if (artifact.movie_path)
            writer.String(artifact.movie_path->generic_string());
        writer.String(artifact.movie_sha256);
        writer.U8(static_cast<std::uint8_t>(artifact.movie_mode));
        WriteCompatibility(writer, artifact.compatibility);
        writer.String(artifact.lineage.edge);
        writer.String(artifact.lineage.producer);
    }
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
    std::uint8_t state_kind = 0;
    std::uint8_t has_current_session = 0;
    std::uint8_t has_artifact = 0;
    if (!reader.U8(state_kind) ||
        state_kind >
            static_cast<std::uint8_t>(
                ProgramBaselineStateKind::CurrentSession) ||
        !reader.U8(has_current_session) ||
        has_current_session > 1)
    {
        reader.Fail("Program baseline enum is invalid");
        return false;
    }
    if (has_current_session)
    {
        std::uint64_t session_id = 0;
        std::uint64_t state_epoch = 0;
        std::uint8_t require_clean_idle = 0;
        if (!reader.U64(session_id) ||
            !reader.U64(state_epoch) ||
            !reader.U8(require_clean_idle) ||
            require_clean_idle > 1)
        {
            reader.Fail("Current-session baseline guard is invalid");
            return false;
        }
        baseline.current_session = CurrentSessionBaselineGuard{
            SessionId(session_id),
            StateEpoch(state_epoch),
            require_clean_idle != 0};
    }
    if (!reader.U8(has_artifact) || has_artifact > 1)
    {
        reader.Fail("Program baseline enum is invalid");
        return false;
    }
    baseline.state_kind =
        static_cast<ProgramBaselineStateKind>(state_kind);
    if (has_artifact)
    {
        ProgramBaselineArtifact artifact;
        std::string state_path;
        std::uint8_t has_movie = 0;
        std::string movie_path;
        std::uint8_t movie_mode = 0;
        if (!reader.String(state_path) ||
            !reader.String(artifact.state_sha256) ||
            !reader.U8(has_movie) || has_movie > 1 ||
            (has_movie && !reader.String(movie_path)) ||
            !reader.String(artifact.movie_sha256) ||
            !reader.U8(movie_mode) ||
            movie_mode >
                static_cast<std::uint8_t>(
                    ExternalMovieImportMode::Recording) ||
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
        artifact.movie_mode =
            static_cast<ExternalMovieImportMode>(movie_mode);
        baseline.artifact = std::move(artifact);
    }
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
    writer.U64(limits.maximum_aggregate_active_budget.count());
    writer.U32(limits.maximum_item_credits);
    writer.U32(limits.maximum_active_and_staged_items);
    writer.U32(limits.maximum_state_cache_entries);
    writer.U64(limits.maximum_state_cache_bytes);
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
    std::uint64_t aggregate_ms = 0;
    std::uint64_t cache_bytes = 0;
    std::uint64_t finalizer_bytes = 0;
    std::uint64_t terminal_bytes = 0;
    if (!reader.U32(limits.maximum_items_per_workset) ||
        !reader.U64(encoded_bytes) ||
        !reader.U64(aggregate_ms) ||
        !reader.U32(limits.maximum_item_credits) ||
        !reader.U32(limits.maximum_active_and_staged_items) ||
        !reader.U32(limits.maximum_state_cache_entries) ||
        !reader.U64(cache_bytes) ||
        !reader.U32(limits.finalizer_threads) ||
        !reader.U32(limits.maximum_pending_finalizers) ||
        !reader.U64(finalizer_bytes) ||
        !reader.U32(limits.maximum_retained_terminals) ||
        !reader.U64(terminal_bytes) ||
        !reader.U32(limits.progressive_start_concurrency) ||
        aggregate_ms >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()))
    {
        return false;
    }
    limits.maximum_encoded_workset_bytes =
        static_cast<std::size_t>(encoded_bytes);
    limits.maximum_aggregate_active_budget =
        std::chrono::milliseconds(aggregate_ms);
    limits.maximum_state_cache_bytes =
        static_cast<std::size_t>(cache_bytes);
    limits.maximum_pending_finalizer_bytes =
        static_cast<std::size_t>(finalizer_bytes);
    limits.maximum_retained_terminal_bytes =
        static_cast<std::size_t>(terminal_bytes);
    return true;
}

} // namespace

WorksetWireCodecResult EncodeWorkerWorksetV1(
    const WorkerWorksetDefinition& definition,
    std::vector<std::uint8_t>& output)
{
    Writer writer;
    writer.U32(kWorksetWireVersionV1);
    writer.U64(definition.workset_id.value());
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
        writer.U64(item.invocation.invocation_id.value());
        writer.U64(item.invocation.attempt_id.value());
        WriteModule(writer, item.invocation.module);
        writer.String(item.invocation.entrypoint);
        writer.Blob(item.invocation.template_payload);
        writer.U64(item.declared_active_budget.count());
        writer.U64(item.declared_terminal_bytes);
        writer.String(item.correlation.durable_job_id);
        writer.String(item.correlation.claim_token);
        writer.String(item.correlation.parent_correlation);
    }
    return writer.Finish(output, kMaximumWorksetWireBytes);
}

WorksetWireCodecResult DecodeWorkerWorksetV1(
    std::span<const std::uint8_t> input,
    WorkerWorksetDefinition& output)
{
    if (input.size() > kMaximumWorksetWireBytes)
        return {false, "Encoded workset exceeds its wire bound"};
    Reader reader(input);
    WorkerWorksetDefinition candidate;
    std::uint32_t version = 0;
    std::uint64_t workset_id = 0;
    WorkerWorksetExecutionKey& key = candidate.execution_key;
    if (!reader.U32(version) || version != kWorksetWireVersionV1 ||
        !reader.U64(workset_id) ||
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
    std::uint32_t item_count = 0;
    if (!reader.Count(item_count, 16))
        return reader.Finish();
    candidate.items.reserve(item_count);
    for (std::uint32_t index = 0; index < item_count; ++index)
    {
        WorksetItemTemplate item;
        std::uint64_t item_id = 0;
        std::uint64_t invocation_id = 0;
        std::uint64_t attempt_id = 0;
        std::uint64_t budget_ms = 0;
        std::uint64_t terminal_bytes = 0;
        if (!reader.U64(item_id) ||
            !reader.U32(item.ordinal) ||
            !reader.U64(invocation_id) ||
            !reader.U64(attempt_id) ||
            !ReadModule(reader, item.invocation.module) ||
            !reader.String(item.invocation.entrypoint) ||
            !reader.Blob(item.invocation.template_payload) ||
            !reader.U64(budget_ms) ||
            !reader.U64(terminal_bytes) ||
            !reader.String(item.correlation.durable_job_id) ||
            !reader.String(item.correlation.claim_token) ||
            !reader.String(item.correlation.parent_correlation) ||
            budget_ms >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
            terminal_bytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()))
        {
            return {false, "WorkerWorkset item is invalid"};
        }
        item.item_id = WorkerWorksetItemId(item_id);
        item.invocation.invocation_id =
            InvocationId(invocation_id);
        item.invocation.attempt_id = AttemptId(attempt_id);
        item.declared_active_budget =
            std::chrono::milliseconds(budget_ms);
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
    writer.U32(kWorksetWireVersionV1);
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
    if (!reader.U32(version) || version != kWorksetWireVersionV1 ||
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
