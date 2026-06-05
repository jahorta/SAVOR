#include "BattleContextProbeAdapters.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SimCore/Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../../SimCore/Phases/Programs/BattleContext/BattleContextPayload.h"
#include "../../../../SimCore/Runner/IPC/Wire.h"
#include "../../../../SimCore/Runner/Parallel/PRTypes.h"
#include "../../../../SimCore/Runner/Script/KeyRegistry.h"
#include "../../../../SimCore/Utils/Hash.h"
#include "../../../../SimCore/Utils/Hex.h"
#include "../../../../SimCore/Utils/IniDoc.h"

namespace simcore::db::execution::programdb::battlecontext {
namespace {

constexpr const char* kWaveRefKind = "analysis_battle.turn_wave";
constexpr const char* kContextProbeRefKind = "analysis_battle.context_probe";
constexpr const char* kJobSection = "BattleContextProbe.Job";
constexpr const char* kResultsSection = "BattleContextProbe.Results";
constexpr std::int32_t kProgramVersion = 1;

struct JobIni {
    std::int64_t context_probe_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t source_savestate_id = 0;

    void set_section(IniDoc& ini) const {
        ini.set(kJobSection, "context_probe_id", std::to_string(context_probe_id));
        ini.set(kJobSection, "wave_id", std::to_string(wave_id));
        ini.set(kJobSection, "source_savestate_id", std::to_string(source_savestate_id));
    }

    static JobIni parse(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        JobIni out{};
        out.context_probe_id = ini.get_i64(kJobSection, "context_probe_id", 0);
        out.wave_id = ini.get_i64(kJobSection, "wave_id", 0);
        out.source_savestate_id = ini.get_i64(kJobSection, "source_savestate_id", 0);
        return out;
    }
};

struct ResultsIni {
    std::uint32_t w_err = 0;
    std::uint32_t dw_err = 0;
    std::optional<std::string> context_blob;
    std::optional<std::int64_t> context_blob_len;
    std::optional<int> context_version;

    std::string to_ini() const {
        IniDoc ini;
        ini.set(kResultsSection, "w_err", std::to_string(w_err));
        ini.set(kResultsSection, "dw_err", std::to_string(dw_err));
        if (context_blob_len.has_value()) {
            ini.set(kResultsSection, "context_blob_len", std::to_string(*context_blob_len));
        }
        if (context_blob.has_value()) {
            ini.set(kResultsSection, "context_blob_hex", bytes_to_hex(context_blob->data(), context_blob->size()));
        }
        if (context_version.has_value()) {
            ini.set(kResultsSection, "context_version", std::to_string(*context_version));
        }
        return ini.to_string_sorted();
    }

    static ResultsIni parse(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        ResultsIni out{};
        out.w_err = ini.get_u32(kResultsSection, "w_err", 0);
        out.dw_err = ini.get_u32(kResultsSection, "dw_err", 0);
        if (ini.has(kResultsSection, "context_blob_len")) {
            out.context_blob_len = ini.get_i64(kResultsSection, "context_blob_len", 0);
        }
        if (ini.has(kResultsSection, "context_blob_hex")) {
            const auto bytes = hex_to_bytes(ini.get(kResultsSection, "context_blob_hex", ""));
            out.context_blob = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else if (ini.has(kResultsSection, "context_blob")) {
            out.context_blob = ini.get(kResultsSection, "context_blob", "");
        }
        if (ini.has(kResultsSection, "context_version")) {
            out.context_version = static_cast<int>(ini.get_i64(kResultsSection, "context_version", 0));
        }
        return out;
    }
};

std::string EventId(std::string_view prefix, std::int64_t id, std::string_view suffix) {
    return std::string(prefix) + "-" + std::to_string(id) + "-" + std::string(suffix);
}

std::uint32_t ClampU32(std::int64_t value) {
    if (value <= 0) {
        return 0;
    }
    if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

std::string BuildInputIni(const JobIni& job) {
    IniDoc ini;
    job.set_section(ini);
    return ini.to_string_sorted();
}

std::string FingerprintFor(const JobIni& job) {
    const auto ini = BuildInputIni(job);
    return "PK=" + std::to_string(simcore::PK_BattleContextProbe)
        + ";PV=" + std::to_string(kProgramVersion)
        + ";phase=battle.context_probe;sha=" + hash::sha256(ini.data(), ini.size());
}

bool AppendJobCompleted(
    simcore::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const char* terminal_state,
    std::vector<std::string>* lines) {
    std::ostringstream event;
    event << "[battle-context-probe-job-terminal-state] job=" << job_id
          << " terminal_state=" << terminal_state;
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr || job_id <= 0) {
        event << " ok=false error=execution_db_unavailable";
        if (lines) lines->push_back(event.str());
        return false;
    }
    std::string error;
    const bool ok = execution_db->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = std::string(terminal_state),
            .requested_by = "battle_context_probe_result_mapper",
        },
        &error);
    event << " ok=" << (ok ? "true" : "false");
    if (!ok) event << " error=" << error;
    if (lines) lines->push_back(event.str());
    return ok;
}

std::optional<std::int64_t> ResolveSourceSavestateId(
    simcore::db::IAnalysisDb* analysis_db,
    const simcore::db::BattleTurnWaveSnapshot& wave,
    std::optional<std::int64_t>* parent_exec_job_id_out,
    std::string* error_out) {
    if (analysis_db == nullptr) {
        if (error_out) *error_out = "analysis db unavailable";
        return std::nullopt;
    }
    const auto battle_set = analysis_db->GetBattleSet(wave.battle_set_id);
    if (!battle_set.has_value()) {
        if (error_out) *error_out = "battle set not found";
        return std::nullopt;
    }
    if (!wave.parent_turn_job_id.has_value()) {
        return battle_set->entry_savestate_id;
    }
    const auto parent_jobs = analysis_db->ListBattleTurnJobsForBattleTurn(battle_set->battle_set_id, wave.turn_index - 1);
    const auto parent_it = std::find_if(parent_jobs.begin(), parent_jobs.end(), [&](const auto& row) {
        return row.turn_job_id == *wave.parent_turn_job_id;
    });
    if (parent_it == parent_jobs.end() || !parent_it->output_savestate_id.has_value()) {
        if (error_out) *error_out = "parent turn job output not found";
        return std::nullopt;
    }
    if (parent_exec_job_id_out != nullptr) {
        *parent_exec_job_id_out = parent_it->exec_job_id;
    }
    return *parent_it->output_savestate_id;
}

class BattleContextProbeJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    BattleContextProbeJobPersistenceAdapter(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db) {
    }

    WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const override {
        WorkflowStepScheduleResult scheduled{};
        scheduled.persistence.program_ref_kind = kWaveRefKind;
        scheduled.persistence.program_ref_id = domain_ref_id;
        scheduled.persistence.program_version = kProgramVersion;
        scheduled.persistence.fingerprint = "battle.context_probe.wave." + std::to_string(domain_ref_id);

        if (execution_db_ == nullptr || analysis_db_ == nullptr || domain_ref_id <= 0) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=db_unavailable");
            return scheduled;
        }
        const auto wave = analysis_db_->GetBattleTurnWave(domain_ref_id);
        if (!wave.has_value()) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=wave_not_found");
            return scheduled;
        }
        const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id);
        if (!battle_set.has_value()) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=battle_set_not_found");
            return scheduled;
        }

        std::string error;
        std::optional<std::int64_t> parent_exec_job_id;
        const auto source_savestate_id = ResolveSourceSavestateId(analysis_db_, *wave, &parent_exec_job_id, &error);
        if (!source_savestate_id.has_value()) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=" + error);
            return scheduled;
        }

        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(simcore::PK_BattleContextProbe),
                    .purpose = "Battle Context Probe",
                    .created_by = "battle.context_probe.adapters",
                    .created_at_utc = simcore::db::types::UtcNow().time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = std::string(kWaveRefKind),
                    .domain_ref_id = wave->wave_id,
                    .meta_note = "battle_set=" + std::to_string(battle_set->battle_set_id)
                        + ";turn=" + std::to_string(wave->turn_index),
                },
                &job_set_id,
                &error)
            || job_set_id <= 0) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=" + error);
            return scheduled;
        }
        scheduled.root_job_set_id = job_set_id;

        std::int64_t context_probe_id = 0;
        const auto now = simcore::db::types::UtcNow();
        if (!analysis_db_->CreateBattleContextProbe(
                {
                    .wave_id = wave->wave_id,
                    .source_savestate_id = *source_savestate_id,
                    .probe_status = simcore::db::BattleContextProbeStatus::Queued,
                    .created_at_utc = now,
                    .event_id = EventId("battle.context_probe", wave->wave_id, "created"),
                    .correlation_id = "battle-set-" + std::to_string(battle_set->battle_set_id),
                    .causation_id = "wave-" + std::to_string(wave->wave_id),
                },
                &context_probe_id,
                &error)
            || context_probe_id <= 0) {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=" + error);
            return scheduled;
        }

        JobIni job_ini{};
        job_ini.context_probe_id = context_probe_id;
        job_ini.wave_id = wave->wave_id;
        job_ini.source_savestate_id = *source_savestate_id;

        std::int64_t exec_job_id = 0;
        if (execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .parent_job_id = parent_exec_job_id,
                    .program_kind = static_cast<std::int32_t>(simcore::PK_BattleContextProbe),
                    .program_version = kProgramVersion,
                    .program_ref_kind = kContextProbeRefKind,
                    .program_ref_id = context_probe_id,
                    .savestate_id = *source_savestate_id,
                    .fingerprint = FingerprintFor(job_ini),
                    .priority = wave->turn_index,
                    .max_attempts = 1,
                    .input_ini = BuildInputIni(job_ini),
                },
                &exec_job_id,
                &error)
            && exec_job_id > 0) {
            (void)analysis_db_->SetBattleContextProbeExecJobId(context_probe_id, exec_job_id, &error);
            (void)analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id,
                simcore::db::BattleTurnWaveStatus::ContextProbing,
                std::nullopt,
                nullptr);
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=true wave="
                + std::to_string(wave->wave_id)
                + " context_probe_id=" + std::to_string(context_probe_id)
                + " job=" + std::to_string(exec_job_id));
        } else {
            scheduled.event_lines.push_back("[battle-context-probe-enqueue] ok=false error=" + error);
        }
        return scheduled;
    }

    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
        return persisted.program_ref_id;
    }

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class BattleContextProbeRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    BattleContextProbeRuntimeInitAdapter(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db,
        simcore::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db) {
    }

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = "battle.context_probe";
        request.savestate_ref_kind = "state_savestate";
        request.derived_buffer_type = simcore::DBuf::DK_None;
        request.default_timeout_ms = 10000;
        if (execution_db_ == nullptr) {
            return request;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            return request;
        }
        if (job->savestate_id.has_value()) {
            request.savestate_ref_id = *job->savestate_id;
        }
        if (analysis_db_ != nullptr && authoring_db_ != nullptr) {
            const auto job_ini = JobIni::parse(job->input_ini);
            if (const auto wave = analysis_db_->GetBattleTurnWave(job_ini.wave_id); wave.has_value()) {
                if (const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id); battle_set.has_value()) {
                    if (const auto spec = authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id); spec.has_value() && spec->run_ms > 0) {
                        request.default_timeout_ms = spec->run_ms;
                    }
                }
            }
        }
        return request;
    }

    std::optional<simcore::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
        if (execution_db_ == nullptr) {
            return std::nullopt;
        }
        const auto exec_job = execution_db_->GetJob(job_id);
        if (!exec_job.has_value() || exec_job->program_ref_kind != kContextProbeRefKind) {
            return std::nullopt;
        }
        std::int64_t run_ms = 10000;
        std::int64_t vi_stall_ms = 2000;
        if (analysis_db_ != nullptr && authoring_db_ != nullptr) {
            const auto job_ini = JobIni::parse(exec_job->input_ini);
            if (const auto wave = analysis_db_->GetBattleTurnWave(job_ini.wave_id); wave.has_value()) {
                if (const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id); battle_set.has_value()) {
                    if (const auto spec = authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id); spec.has_value()) {
                        run_ms = spec->run_ms;
                        vi_stall_ms = spec->vi_stall_ms;
                    }
                }
            }
        }

        phase::battle::ctx::EncodeSpec spec{};
        spec.run_ms = ClampU32(run_ms);
        spec.vi_stall_ms = ClampU32(vi_stall_ms);

        simcore::PSJob ps_job{};
        if (!phase::battle::ctx::encode_payload(spec, ps_job.payload)) {
            return std::nullopt;
        }
        return ps_job;
    }

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
    simcore::db::IAuthoringDb* authoring_db_ = nullptr;
};

class BattleContextProbeResultMapper final : public IResultMapper {
public:
    BattleContextProbeResultMapper(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db) {
    }

    std::string BuildResultIniFromPrResult(std::int64_t, const simcore::PRResult& result) const override {
        ResultsIni out{};
        out.w_err = result.ps.w_err;
        if (out.w_err == 0) {
            result.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        }
        std::string context_blob;
        if (result.ps.ctx.get(simcore::keys::battle::CTX_BLOB, context_blob) && !context_blob.empty()) {
            out.context_blob = context_blob;
            out.context_blob_len = static_cast<std::int64_t>(context_blob.size());
            out.context_version = soa::battle::ctx::codec::ver;
        }
        return out.to_ini();
    }

    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override {
        ResultMapPayload payload{};
        payload.result_kind = "analysisbattle.context_probe";
        if (execution_db_ == nullptr || analysis_db_ == nullptr) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            return payload;
        }
        const auto probe = analysis_db_->GetBattleContextProbeForExecJob(job_id);
        if (!probe.has_value()) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "analysisbattle.context_probe.missing";
            return payload;
        }

        const auto parsed = ResultsIni::parse(result_ini);
        const auto parsed_blob_len = parsed.context_blob.has_value()
            ? static_cast<std::int64_t>(parsed.context_blob->size())
            : 0;
        const auto parsed_blob_hex = parsed.context_blob.has_value()
            ? bytes_to_hex(parsed.context_blob->data(), parsed.context_blob->size())
            : std::string{};
        payload.event_lines.push_back("[battle-context-probe-result-blob] job=" + std::to_string(job_id)
            + " context_blob_len_field=" + std::to_string(parsed.context_blob_len.value_or(-1))
            + " parsed_blob_len=" + std::to_string(parsed_blob_len)
            + " parsed_blob_hex=" + parsed_blob_hex);
        const bool failed = parsed.w_err != 0 || parsed.dw_err != 0 || !parsed.context_blob.has_value() || parsed.context_blob->empty();
        std::string error;
        if (!analysis_db_->CompleteBattleContextProbe(
                {
                    .exec_job_id = job_id,
                    .probe_status = failed ? simcore::db::BattleContextProbeStatus::Failed : simcore::db::BattleContextProbeStatus::Succeeded,
                    .context_blob = failed ? std::nullopt : parsed.context_blob,
                    .context_version = failed ? std::nullopt : parsed.context_version,
                    .recorded_at_utc = simcore::db::types::UtcNow(),
                },
                &error)) {
            payload.event_lines.push_back("[battle-context-probe-result] warning=context_probe_update_failed error=" + error);
        }

        AppendJobCompleted(execution_db_, job_id, failed ? "FAILED" : "SUCCEEDED", &payload.event_lines);
        payload.result_ref_id = probe->context_probe_id;
        payload.event_lines.push_back("[battle-context-probe-result] job=" + std::to_string(job_id)
            + " context_probe_id=" + std::to_string(probe->context_probe_id)
            + " status=" + (failed ? "FAILED" : "SUCCEEDED"));
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override {
        return std::nullopt;
    }

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class BattleContextProbeTransitionHandler final : public IWorkflowTransitionHandler {
public:
    explicit BattleContextProbeTransitionHandler(simcore::db::IAnalysisDb* analysis_db)
        : analysis_db_(analysis_db) {
    }

    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (analysis_db_ == nullptr) {
            decision.blocked_reason = "db_unavailable";
            return decision;
        }
        if (!context.input_ref_id.has_value()
            || (context.input_ref_kind.has_value() && *context.input_ref_kind != kWaveRefKind)) {
            decision.blocked_reason = "wave_ref_missing";
            return decision;
        }
        const auto wave = analysis_db_->GetBattleTurnWave(*context.input_ref_id);
        if (!wave.has_value()) {
            decision.blocked_reason = "wave_not_found";
            return decision;
        }
        const auto probe = analysis_db_->GetLatestBattleContextForWave(wave->wave_id);
        if (!probe.has_value()) {
            decision.blocked_reason = "context_probe_missing";
            return decision;
        }

        WorkflowTransitionDecision::DynamicStep step{};
        step.step_key = "BattleTurn/t" + std::to_string(wave->turn_index)
            + "/w" + std::to_string(wave->wave_id);
        step.step_kind = "battle.single_turn";
        step.input_ref_kind = kWaveRefKind;
        step.input_ref_id = wave->wave_id;
        step.priority = wave->turn_index;
        step.max_attempts = 1;
        decision.should_advance = true;
        decision.spawn_steps.push_back(std::move(step));
        return decision;
    }

private:
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

} // namespace

ProgramKindDescriptor BuildBattleContextProbeDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = simcore::PK_BattleContextProbe;
    descriptor.program_name = "BattleContextProbe";
    descriptor.job_persistence = std::make_shared<BattleContextProbeJobPersistenceAdapter>(
        execution_db,
        analysis_db);
    descriptor.runtime_init = std::make_shared<BattleContextProbeRuntimeInitAdapter>(
        execution_db,
        analysis_db,
        config.authoring_db);
    descriptor.result_mapper = std::make_shared<BattleContextProbeResultMapper>(
        execution_db,
        analysis_db);
    descriptor.workflow_transition = std::make_shared<BattleContextProbeTransitionHandler>(
        analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace simcore::db::execution::programdb::battlecontext
