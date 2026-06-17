#include "TasMovieAdapters.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SavorCore/Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Tas/DtmFile.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::tasmovie {
namespace {

constexpr const char* kBlueprintSection = "TasMovie.Blueprint";
constexpr const char* kJobSection = "TasMovie.Job";
constexpr const char* kResultsSection = "TasMovie.Results";
constexpr const char* kVariantRefKind = "state_tas_movie_variant";
constexpr int kProgramVersion = 2;

std::string ToString(std::int64_t value) {
    return std::to_string(value);
}

TasMovieBlueprintConfig ParseBlueprint(const std::string& input_ini) {
    const auto ini = IniDoc::parse(input_ini);
    TasMovieBlueprintConfig cfg{};
    cfg.base_dtm_artifact_id = ini.get_i64(kBlueprintSection, "base_dtm_artifact_id", 0);
    const auto tas_spec_id = ini.get_i64(kBlueprintSection, "tas_spec_id", 0);
    if (tas_spec_id > 0) {
        cfg.tas_spec_id = tas_spec_id;
    }
    cfg.rtc_low = ini.get_i64(kBlueprintSection, "rtc_low", 0);
    cfg.rtc_high = ini.get_i64(kBlueprintSection, "rtc_high", 0);
    cfg.priority = static_cast<int>(ini.get_i64(kBlueprintSection, "priority", 0));
    cfg.run_ms = ini.get_u32(kBlueprintSection, "run_ms", 0);
    cfg.vi_stall_ms = ini.get_u32(kBlueprintSection, "vi_stall_ms", 2000);
    cfg.progress_enable = true;
    cfg.headroom_x10 = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
        ini.get_i64(kBlueprintSection, "headroom_x10", 15),
        0,
        255));
    const auto probe_run_id = ini.get_i64(kBlueprintSection, "bind_seed_probe_run_id", 0);
    if (probe_run_id > 0) {
        cfg.bind_seed_probe_run_id = probe_run_id;
    }
    return cfg;
}

std::int64_t ParseJobRtc(const std::string& input_ini) {
    return IniDoc::parse(input_ini).get_i64(kJobSection, "new_rtc", 0);
}

std::string BuildInputIni(const TasMovieBlueprintConfig& cfg, std::int64_t rtc) {
    IniDoc ini;
    ini.set(kBlueprintSection, "base_dtm_artifact_id", ToString(cfg.base_dtm_artifact_id));
    if (cfg.tas_spec_id.has_value()) {
        ini.set(kBlueprintSection, "tas_spec_id", ToString(*cfg.tas_spec_id));
    }
    ini.set(kBlueprintSection, "rtc_low", ToString(cfg.rtc_low));
    ini.set(kBlueprintSection, "rtc_high", ToString(cfg.rtc_high));
    ini.set(kBlueprintSection, "priority", ToString(cfg.priority));
    ini.set(kBlueprintSection, "run_ms", ToString(cfg.run_ms));
    ini.set(kBlueprintSection, "vi_stall_ms", ToString(cfg.vi_stall_ms));
    ini.set(kBlueprintSection, "progress_enable", "1");
    ini.set(kBlueprintSection, "headroom_x10", ToString(cfg.headroom_x10));
    if (cfg.bind_seed_probe_run_id.has_value()) {
        ini.set(kBlueprintSection, "bind_seed_probe_run_id", ToString(*cfg.bind_seed_probe_run_id));
    }
    ini.set(kJobSection, "new_rtc", ToString(rtc));
    return ini.to_string_sorted();
}

std::string VariantFingerprint(
    const TasMovieBlueprintConfig& cfg,
    std::int64_t job_set_id,
    std::int64_t tas_variant_id,
    std::int64_t rtc) {
    return "PK=2;PV=" + std::to_string(kProgramVersion)
        + ";phase=tasmovie;variant_id=" + std::to_string(tas_variant_id)
        + ";job_set_id=" + std::to_string(job_set_id)
        + (cfg.tas_spec_id.has_value() ? ";tas_spec_id=" + std::to_string(*cfg.tas_spec_id) : "")
        + ";base_dtm_artifact_id=" + std::to_string(cfg.base_dtm_artifact_id)
        + ";rtc=" + std::to_string(rtc)
        + ";run_ms=" + std::to_string(cfg.run_ms)
        + ";vi=" + std::to_string(cfg.vi_stall_ms);
}

std::string VariantIdentitySuffix(const TasMovieBlueprintConfig& cfg, std::int64_t rtc) {
    std::string suffix = "base-" + std::to_string(cfg.base_dtm_artifact_id);
    if (cfg.tas_spec_id.has_value()) {
        suffix += "-tas-spec-" + std::to_string(*cfg.tas_spec_id);
    }
    suffix += "-rtc-" + std::to_string(rtc);
    return suffix;
}

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    if (!configured.empty()) {
        return configured;
    }
    return std::filesystem::temp_directory_path() / "savordb-tasmovie";
}

std::filesystem::path DerivedDtmPath(
    const std::filesystem::path& root,
    std::int64_t job_id,
    std::int64_t variant_id,
    std::int64_t rtc) {
    return root / ("job-" + std::to_string(job_id))
        / ("tas_variant_" + std::to_string(variant_id) + "_rtc" + std::to_string(rtc) + ".dtm");
}

std::int64_t FileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(size);
}

bool AppendJobCompleted(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const char* terminal_state,
    std::vector<std::string>* lines) {
    std::ostringstream event;
    event << "[tasmovie-job-terminal-state] stage=AppendLifecycleEvent"
          << " job=" << job_id
          << " terminal_state=" << terminal_state;
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr || job_id <= 0) {
        event << " ok=false error=execution_db_unavailable";
        if (lines != nullptr) lines->push_back(event.str());
        return false;
    }

    std::string error;
    const bool ok = execution_db->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = std::string(terminal_state),
            .requested_by = "tasmovie_result_mapper",
        },
        &error);
    event << " ok=" << (ok ? "true" : "false");
    if (!ok) {
        event << " error=" << error;
    }
    if (lines != nullptr) lines->push_back(event.str());
    return ok;
}

class TasMovieTransitionHandler final : public IWorkflowTransitionHandler {
public:
    explicit TasMovieTransitionHandler(std::string next_step_key)
        : next_step_key_(std::move(next_step_key)) {
    }

    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.workflow_kind.rfind("workflow_graph", 0) == 0 && context.step_key != "TasMovie") {
            decision.should_advance = true;
            return decision;
        }

        if (context.step_key != "TasMovie") {
            decision.blocked_reason = "unsupported_step_key";
            return decision;
        }
        decision.should_advance = true;
        if (!next_step_key_.empty()) {
            decision.next_step_key = next_step_key_;
        }
        return decision;
    }

private:
    std::string next_step_key_;
};

class TasMovieJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    TasMovieJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        TasMovieBlueprintConfig blueprint)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , blueprint_(std::move(blueprint)) {
    }

    WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const override {
        WorkflowStepScheduleResult scheduled{};
        auto cfg = blueprint_;
        if (cfg.base_dtm_artifact_id <= 0) {
            cfg.base_dtm_artifact_id = domain_ref_id;
        }
        if (cfg.rtc_high < cfg.rtc_low) {
            cfg.rtc_high = cfg.rtc_low;
        }

        savor::db::CreateJobSetCommand set_cmd{};
        set_cmd.program_kind = static_cast<std::int32_t>(savor::PK_TasMovie);
        set_cmd.purpose = "TasMovie";
        set_cmd.created_by = "tasmovie_adapter";
        set_cmd.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
        set_cmd.expected_total = static_cast<int>(cfg.rtc_high - cfg.rtc_low + 1);
        set_cmd.domain_ref_kind = "state_artifact";
        set_cmd.domain_ref_id = cfg.base_dtm_artifact_id;
        set_cmd.meta_note = "phase=TasMovie";

        if (execution_db_ == nullptr || state_db_ == nullptr || cfg.base_dtm_artifact_id <= 0) {
            return scheduled;
        }

        std::string error;
        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(set_cmd, &job_set_id, &error) || job_set_id <= 0) {
            scheduled.event_lines.push_back("[tasmovie-enqueue-summary] ok=false error=" + error);
            return scheduled;
        }
        scheduled.root_job_set_id = job_set_id;

        std::int64_t jobs_enqueued = 0;
        std::int64_t jobs_failed = 0;
        for (std::int64_t rtc = cfg.rtc_low; rtc <= cfg.rtc_high; ++rtc) {
            std::int64_t tas_variant_id = 0;
            const auto identity_suffix = VariantIdentitySuffix(cfg, rtc);
            if (!state_db_->CreateTasVariant(
                    {
                        .name = "tasmovie-" + identity_suffix,
                        .base_dtm_artifact_id = cfg.base_dtm_artifact_id,
                        .mutation_mode = "RTC_OVERRIDE",
                        .rtc_value = rtc,
                        .created_at_utc = savor::db::types::UtcNow(),
                        .correlation_id = "tasmovie-" + identity_suffix,
                        .causation_id = "workflow-input-" + std::to_string(domain_ref_id),
                    },
                    &tas_variant_id,
                    &error)
                || tas_variant_id <= 0) {
                ++jobs_failed;
                continue;
            }

            savor::db::EnqueueJobCommand enqueue{};
            enqueue.job_set_id = job_set_id;
            enqueue.program_kind = static_cast<std::int32_t>(savor::PK_TasMovie);
            enqueue.program_version = kProgramVersion;
            enqueue.program_ref_kind = kVariantRefKind;
            enqueue.program_ref_id = tas_variant_id;
            enqueue.fingerprint = VariantFingerprint(cfg, job_set_id, tas_variant_id, rtc);
            enqueue.priority = cfg.priority;
            enqueue.max_attempts = 1;
            enqueue.input_ini = BuildInputIni(cfg, rtc);
            enqueue.pending_until_workflow_materialized = true;
            if (execution_db_->EnqueueJob(enqueue, nullptr, &error)) {
                ++jobs_enqueued;
            } else {
                ++jobs_failed;
            }
        }

        JobPersistenceRecord persisted{};
        persisted.program_ref_kind = "state_artifact";
        persisted.program_ref_id = cfg.base_dtm_artifact_id;
        persisted.program_version = kProgramVersion;
        persisted.fingerprint = std::string("PK=2;PV=2;phase=tasmovie")
            + ";job_set_id=" + std::to_string(job_set_id)
            + (cfg.tas_spec_id.has_value() ? ";tas_spec_id=" + std::to_string(*cfg.tas_spec_id) : "")
            + ";base_dtm_artifact_id=" + std::to_string(cfg.base_dtm_artifact_id);
        scheduled.persistence = std::move(persisted);

        std::ostringstream event;
        event << "[tasmovie-enqueue-summary] root_job_set=" << job_set_id
              << " rtc_low=" << cfg.rtc_low
              << " rtc_high=" << cfg.rtc_high
              << " jobs_enqueued=" << jobs_enqueued
              << " jobs_failed=" << jobs_failed;
        if (!error.empty()) {
            event << " last_error=" << error;
        }
        scheduled.event_lines.push_back(event.str());
        return scheduled;
    }

    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
        return persisted.program_ref_id;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    TasMovieBlueprintConfig blueprint_;
};

const WorkflowGraphInputBinding* FindBinding(
    const WorkflowGraphStepScheduleContext& context,
    std::string_view input_key,
    std::string_view data_kind) {
    for (const auto& binding : context.input_bindings) {
        if (binding.input_key == input_key && binding.data_kind == data_kind && binding.ref_id > 0) {
            return &binding;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> FindIntegerArgument(
    const WorkflowGraphStepScheduleContext& context,
    std::string_view argument_key) {
    for (const auto& argument : context.arguments) {
        if (argument.argument_key == argument_key
            && argument.value_type == "integer"
            && argument.integer_value.has_value()) {
            return *argument.integer_value;
        }
    }
    return std::nullopt;
}

const savor::db::WorkflowGraphNodeSnapshot* FindNode(
    const savor::db::WorkflowGraphSnapshot& graph,
    std::string_view step_key) {
    for (const auto& node : graph.nodes) {
        if (node.node_key == step_key) {
            return &node;
        }
    }
    return nullptr;
}

class TasMovieGraphJobPersistenceAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    TasMovieGraphJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAuthoringDb* authoring_db,
        TasMovieBlueprintConfig fallback_blueprint)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , authoring_db_(authoring_db)
        , fallback_blueprint_(std::move(fallback_blueprint)) {
    }

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (authoring_db_ == nullptr
            || execution_db_ == nullptr
            || state_db_ == nullptr
            || !context.workflow_graph_revision_id.has_value()
            || context.workflow_instance_id <= 0
            || context.workflow_step_id <= 0) {
            return {};
        }

        const auto graph = authoring_db_->GetWorkflowGraphRevision(*context.workflow_graph_revision_id);
        if (!graph.has_value()) {
            return {};
        }
        const auto* node = FindNode(*graph, context.step_key);
        if (node == nullptr || node->unit_kind != "tas_movie") {
            return {};
        }

        auto cfg = fallback_blueprint_;
        if (node->authored_ref_kind.value_or("") == "tas_spec"
            && node->authored_ref_id.has_value()
            && *node->authored_ref_id > 0) {
            const auto spec = authoring_db_->GetTasSpec(*node->authored_ref_id);
            if (!spec.has_value()) {
                return {};
            }
            cfg.tas_spec_id = *node->authored_ref_id;
            cfg.priority = spec->priority;
            cfg.run_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(0, spec->run_ms));
            cfg.vi_stall_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(0, spec->vi_stall_ms));
            cfg.progress_enable = true;
        }
        const auto headroom_argument = FindIntegerArgument(context, "headroom");
        if (headroom_argument.has_value()) {
            cfg.headroom_x10 = static_cast<std::uint8_t>(std::clamp<std::int64_t>(*headroom_argument, 0, 255));
        }

        const auto* dtm = FindBinding(context, "dtm_artifact", "state_artifact.dtm_artifact_id");
        if (dtm == nullptr) {
            return {};
        }
        cfg.base_dtm_artifact_id = dtm->ref_id;
        const auto rtc_argument = FindIntegerArgument(context, "rtc");
        if (!rtc_argument.has_value()) {
            WorkflowStepScheduleResult scheduled{};
            scheduled.event_lines.push_back(
                "[workflow-graph-tasmovie-bootstrap] ok=false error=missing_required_rtc_argument"
                " workflow_instance_id=" + std::to_string(context.workflow_instance_id)
                + " workflow_step_id=" + std::to_string(context.workflow_step_id)
                + " step=" + context.step_key);
            return scheduled;
        }
        cfg.rtc_low = *rtc_argument;
        cfg.rtc_high = *rtc_argument;

        auto adapter = TasMovieJobPersistenceAdapter(
            execution_db_,
            state_db_,
            cfg);
        auto scheduled = adapter.EncodeForQueueing(cfg.base_dtm_artifact_id);
        scheduled.event_lines.push_back(
            "[workflow-graph-tasmovie-bootstrap] workflow_instance_id="
            + std::to_string(context.workflow_instance_id)
            + " workflow_step_id=" + std::to_string(context.workflow_step_id)
            + " base_dtm_artifact_id=" + std::to_string(cfg.base_dtm_artifact_id)
            + " tas_spec_id=" + (cfg.tas_spec_id.has_value() ? std::to_string(*cfg.tas_spec_id) : "none")
            + " rtc_low=" + std::to_string(cfg.rtc_low)
            + " rtc_high=" + std::to_string(cfg.rtc_high)
            + " rtc_argument=" + (rtc_argument.has_value() ? std::to_string(*rtc_argument) : "none"));
        return scheduled;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    TasMovieBlueprintConfig fallback_blueprint_;
};

class TasMovieRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    TasMovieRuntimeInitAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    RuntimeInitRequest BuildRuntimeInit(std::int64_t) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = "tasmovie.play";
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr) {
            return std::nullopt;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value() || job->program_ref_kind != kVariantRefKind) {
            return std::nullopt;
        }
        const auto variant = state_db_->GetTasVariant(job->program_ref_id);
        if (!variant.has_value()) {
            return std::nullopt;
        }

        const auto cfg = ParseBlueprint(job->input_ini);
        const auto rtc = ParseJobRtc(job->input_ini);
        const auto root = WorkingRoot(working_dir_root_);
        const auto materialized_dir = root / ("job-" + std::to_string(job_id)) / "source";
        std::string error;
        const auto base_path = state_db_->MaterializeArtifactToDirectory(
            variant->base_dtm_artifact_id,
            materialized_dir.string(),
            &error);
        if (!base_path.has_value()) {
            return std::nullopt;
        }

        savor::tas::DtmFile dtm;
        if (!dtm.load(*base_path)) {
            return std::nullopt;
        }
        if (rtc > 0) {
            dtm.set_recording_start_time_unix_seconds(static_cast<std::uint64_t>(rtc));
        }

        const auto derived_path = DerivedDtmPath(root, job_id, variant->tas_variant_id, rtc);
        std::filesystem::create_directories(derived_path.parent_path());
        if (!dtm.save(derived_path.string())) {
            return std::nullopt;
        }

        savor::tasmovie::EncodeSpec spec{};
        spec.dtm_path = derived_path.string();
        spec.run_ms = cfg.run_ms;
        spec.vi_stall_ms = cfg.vi_stall_ms;
        spec.headroom_x10 = cfg.headroom_x10;

        savor::PSJob out{};
        if (!savor::tasmovie::encode_payload(spec, out.payload)) {
            return std::nullopt;
        }
        return out;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

class TasMovieResultMapper final : public IResultMapper {
public:
    TasMovieResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , analysis_db_(analysis_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        TasMovieResultsIni out{};
        out.w_err = result.ps.w_err;
        if (out.w_err == 0) {
            result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        }
        result.ps.ctx.get(savor::context::key::core::RUN_MS, out.run_ms_used);
        result.ps.ctx.get(savor::context::key::core::VI_FIRST, out.vi_start);
        result.ps.ctx.get(savor::context::key::core::VI_LAST, out.vi_end);
        result.ps.ctx.get(savor::context::key::tas::SAVE_PATH, out.savestate_path);
        return out.ToIniText();
    }

    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override {
        ResultMapPayload payload{};
        payload.result_kind = "state.tasmovie.unavailable";

        const auto parsed = TasMovieResultsIni::FromIniText(result_ini);
        const bool failed = parsed.w_err != 0 || parsed.dw_err != 0 || parsed.savestate_path.empty();
        if (failed) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "state.tasmovie.failed";
            std::ostringstream event;
            event << "[tasmovie-result] job=" << job_id
                  << " failed=true"
                  << " w_err=" << parsed.w_err
                  << " dw_err=" << parsed.dw_err
                  << " savestate_path_empty=" << (parsed.savestate_path.empty() ? "true" : "false")
                  << " savestate_path=\"" << parsed.savestate_path << "\"";
            payload.event_lines.push_back(event.str());
            return payload;
        }
        if (execution_db_ == nullptr || state_db_ == nullptr) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            return payload;
        }

        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value() || job->program_ref_kind != kVariantRefKind) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "state.tasmovie.context_missing";
            return payload;
        }

        const std::filesystem::path sav_path(parsed.savestate_path);
        if (!std::filesystem::exists(sav_path)) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "state.tasmovie.savestate_missing";
            return payload;
        }

        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t artifact_id = 0;
        if (!state_db_->StoreArtifact(
                {
                    .sha256 = hash::sha256_of_file(sav_path.string()),
                    .size_bytes = FileSize(sav_path),
                    .compression_kind = 0,
                    .filename = std::filesystem::absolute(sav_path).string(),
                    .file_ext = sav_path.extension().string(),
                    .artifact_kind = "SAV",
                    .created_at_utc = now,
                    .correlation_id = "tasmovie-job-" + std::to_string(job_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &artifact_id,
                &error)
            || artifact_id <= 0) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "state.tasmovie.artifact_store_failed";
            payload.event_lines.push_back("[tasmovie-result] job=" + std::to_string(job_id) + " error=" + error);
            return payload;
        }

        std::int64_t savestate_id = 0;
        if (!state_db_->CreateSavestate(
                {
                    .artifact_id = artifact_id,
                    .savestate_type = "TAS_MOVIE_OUTPUT",
                    .note = "TasMovie produced savestate",
                    .is_complete = true,
                    .created_at_utc = now,
                    .correlation_id = "tasmovie-job-" + std::to_string(job_id),
                    .causation_id = "artifact-" + std::to_string(artifact_id),
                },
                &savestate_id,
                &error)
            || savestate_id <= 0) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "state.tasmovie.savestate_create_failed";
            payload.event_lines.push_back("[tasmovie-result] job=" + std::to_string(job_id) + " error=" + error);
            return payload;
        }

        if (!state_db_->UpdateTasVariantProducedSavestate(
                {
                    .tas_variant_id = job->program_ref_id,
                    .produced_savestate_id = savestate_id,
                    .updated_at_utc = now,
                    .correlation_id = "tasmovie-job-" + std::to_string(job_id),
                    .causation_id = "savestate-" + std::to_string(savestate_id),
                },
                &error)) {
            payload.event_lines.push_back("[tasmovie-result] job=" + std::to_string(job_id) + " warning=variant_update_failed error=" + error);
        }

        const auto cfg = ParseBlueprint(job->input_ini);
        if (cfg.bind_seed_probe_run_id.has_value() && analysis_db_ != nullptr) {
            if (!analysis_db_->SetSeedProbeRunEntrySavestate(
                    {
                        .probe_run_id = *cfg.bind_seed_probe_run_id,
                        .entry_savestate_id = savestate_id,
                        .updated_at_utc = now,
                        .correlation_id = "tasmovie-job-" + std::to_string(job_id),
                        .causation_id = "savestate-" + std::to_string(savestate_id),
                    },
                    &error)) {
                payload.event_lines.push_back("[tasmovie-result] job=" + std::to_string(job_id) + " warning=seedprobe_bind_failed error=" + error);
            }
        }

        AppendJobCompleted(execution_db_, job_id, "SUCCEEDED", &payload.event_lines);
        payload.result_kind = "state.tasmovie.savestate";
        payload.result_ref_id = savestate_id;
        payload.output_key = "savestate";
        payload.output_data_kind = "state.savestate_id";
        payload.output_ref_kind = "state.savestate";
        payload.output_ref_id = savestate_id;
        std::ostringstream line;
        line << "[tasmovie-result] job=" << job_id
             << " savestate_id=" << savestate_id
             << " vi=" << parsed.vi_start << "-" << parsed.vi_end
             << " path=" << sav_path.string();
        payload.event_lines.push_back(line.str());
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override {
        return std::nullopt;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

} // namespace

TasMovieResultsIni TasMovieResultsIni::FromIniText(const std::string& text) {
    const auto ini = IniDoc::parse(text);
    TasMovieResultsIni out{};
    out.w_err = static_cast<int>(ini.get_i64(kResultsSection, "w_err", 0));
    out.dw_err = ini.get_u32(kResultsSection, "dw_err", 0);
    out.run_ms_used = ini.get_u32(kResultsSection, "run_ms_used", 0);
    out.vi_start = ini.get_u32(kResultsSection, "vi_start", 0);
    out.vi_end = ini.get_u32(kResultsSection, "vi_end", 0);
    out.savestate_path = ini.get(kResultsSection, "savestate_path", "");
    return out;
}

std::string TasMovieResultsIni::ToIniText() const {
    IniDoc ini;
    ini.set(kResultsSection, "w_err", std::to_string(w_err));
    ini.set(kResultsSection, "dw_err", std::to_string(dw_err));
    ini.set(kResultsSection, "run_ms_used", std::to_string(run_ms_used));
    ini.set(kResultsSection, "vi_start", std::to_string(vi_start));
    ini.set(kResultsSection, "vi_end", std::to_string(vi_end));
    ini.set(kResultsSection, "savestate_path", savestate_path);
    return ini.to_string_sorted();
}

ProgramKindDescriptor BuildTasMovieDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    TasMoviePhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = savor::PK_TasMovie;
    descriptor.program_name = "TasMovie";
    descriptor.job_persistence = std::make_shared<TasMovieJobPersistenceAdapter>(
        execution_db,
        state_db,
        config.blueprint);
    descriptor.graph_job_persistence = std::make_shared<TasMovieGraphJobPersistenceAdapter>(
        execution_db,
        state_db,
        config.authoring_db,
        config.blueprint);
    descriptor.runtime_init = std::make_shared<TasMovieRuntimeInitAdapter>(
        execution_db,
        state_db,
        config.working_dir_root);
    descriptor.result_mapper = std::make_shared<TasMovieResultMapper>(
        execution_db,
        state_db,
        analysis_db,
        config.working_dir_root);
    descriptor.workflow_transition = std::make_shared<TasMovieTransitionHandler>(std::move(config.next_step_key));
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::tasmovie
