#include "BattleContextProbeAdapters.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SavorCore/Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../../SavorCore/Phases/Programs/BattleContext/BattleContextPayload.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCore/Utils/Hex.h"
#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::battlecontext {
namespace {

constexpr const char* kWaveRefKind = "analysis_battle.turn_wave";
constexpr const char* kContextProbeRefKind = "analysisbattle.context_probe";
constexpr const char* kGraphContextProbeRefKind = "workflow_graph.battle_context_probe";
constexpr const char* kJobSection = "BattleContextProbe.Job";
constexpr const char* kResultsSection = "BattleContextProbe.Results";
constexpr std::int32_t kProgramVersion = 1;

struct JobIni {
    std::int64_t context_probe_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t source_savestate_id = 0;
    std::string input_set_ref_kind;
    std::int64_t input_set_ref_id = 0;
    std::int64_t battle_chain_spec_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    int fake_attack_min = 0;
    int fake_attack_max = 0;

    void set_section(IniDoc& ini) const {
        ini.set(kJobSection, "context_probe_id", std::to_string(context_probe_id));
        ini.set(kJobSection, "wave_id", std::to_string(wave_id));
        ini.set(kJobSection, "source_savestate_id", std::to_string(source_savestate_id));
        ini.set(kJobSection, "input_set_ref_kind", input_set_ref_kind);
        ini.set(kJobSection, "input_set_ref_id", std::to_string(input_set_ref_id));
        ini.set(kJobSection, "battle_chain_spec_id", std::to_string(battle_chain_spec_id));
        ini.set(kJobSection, "battle_run_spec_id", std::to_string(battle_run_spec_id));
        ini.set(kJobSection, "explorer_settings_id", std::to_string(explorer_settings_id));
        ini.set(kJobSection, "fake_attack_min", std::to_string(fake_attack_min));
        ini.set(kJobSection, "fake_attack_max", std::to_string(fake_attack_max));
    }

    static JobIni parse(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        JobIni out{};
        out.context_probe_id = ini.get_i64(kJobSection, "context_probe_id", 0);
        out.wave_id = ini.get_i64(kJobSection, "wave_id", 0);
        out.source_savestate_id = ini.get_i64(kJobSection, "source_savestate_id", 0);
        out.input_set_ref_kind = ini.get(kJobSection, "input_set_ref_kind", "");
        out.input_set_ref_id = ini.get_i64(kJobSection, "input_set_ref_id", 0);
        out.battle_chain_spec_id = ini.get_i64(kJobSection, "battle_chain_spec_id", 0);
        out.battle_run_spec_id = ini.get_i64(kJobSection, "battle_run_spec_id", 0);
        out.explorer_settings_id = ini.get_i64(kJobSection, "explorer_settings_id", 0);
        out.fake_attack_min = static_cast<int>(ini.get_i64(kJobSection, "fake_attack_min", 0));
        out.fake_attack_max = static_cast<int>(ini.get_i64(kJobSection, "fake_attack_max", 0));
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

struct InputSetCandidate {
    std::optional<std::int64_t> source_probe_result_id;
    std::int64_t source_input_frame_id = 0;
    std::int64_t seed_value = 0;
    savor::db::BattleSeedCandidateSourceKind source_kind = savor::db::BattleSeedCandidateSourceKind::Unknown;
};

std::vector<InputSetCandidate> ResolveInputSetCandidates(
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    const std::string& ref_kind,
    std::int64_t ref_id,
    std::string* error_out);

std::string BuildInputIni(const JobIni& job) {
    IniDoc ini;
    job.set_section(ini);
    return ini.to_string_sorted();
}

std::string FingerprintFor(const JobIni& job) {
    const auto ini = BuildInputIni(job);
    return "PK=" + std::to_string(savor::PK_BattleContextProbe)
        + ";PV=" + std::to_string(kProgramVersion)
        + ";phase=battle.context_probe;sha=" + hash::sha256(ini.data(), ini.size());
}

bool AppendJobCompleted(
    savor::db::IExecutionDb* execution_db,
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
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
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
    savor::db::IAnalysisDb* analysis_db,
    const savor::db::BattleTurnWaveSnapshot& wave,
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
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db) {
    }

    WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const override {
        WorkflowStepScheduleResult scheduled{};
        const std::int64_t domain_ref_id = context.domain_ref_id;
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
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContextProbe),
                    .purpose = "Battle Context Probe",
                    .created_by = "battle.context_probe.adapters",
                    .created_at_utc = savor::db::types::UtcNow().time_since_epoch().count(),
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
        const auto now = savor::db::types::UtcNow();
        if (!analysis_db_->CreateBattleContextProbe(
                {
                    .wave_id = wave->wave_id,
                    .source_savestate_id = *source_savestate_id,
                    .probe_status = savor::db::BattleContextProbeStatus::Queued,
                    .created_at_utc = now,
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
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContextProbe),
                    .program_version = kProgramVersion,
                    .program_ref_kind = kContextProbeRefKind,
                    .program_ref_id = context_probe_id,
                    .savestate_id = *source_savestate_id,
                    .fingerprint = FingerprintFor(job_ini),
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = BuildInputIni(job_ini),
                    .pending_until_workflow_materialized = true,
                },
                &exec_job_id,
                &error)
            && exec_job_id > 0) {
            (void)analysis_db_->SetBattleContextProbeExecJobId(context_probe_id, exec_job_id, &error);
            (void)analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id,
                savor::db::BattleTurnWaveStatus::ContextProbing,
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
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

class BattleContextProbeRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    explicit BattleContextProbeRuntimeInitAdapter(savor::db::IExecutionDb* execution_db)
        : execution_db_(execution_db) {
    }

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = "battle.context_probe";
        request.savestate_ref_kind = "state_savestate";
        request.derived_buffer_type = savor::DBuf::DK_None;
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
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
        if (execution_db_ == nullptr) {
            return std::nullopt;
        }
        const auto exec_job = execution_db_->GetJob(job_id);
        if (!exec_job.has_value()
            || (exec_job->program_ref_kind != kContextProbeRefKind
                && exec_job->program_ref_kind != kGraphContextProbeRefKind)) {
            return std::nullopt;
        }
        phase::battle::ctx::EncodeSpec spec{};

        savor::PSJob ps_job{};
        if (!phase::battle::ctx::encode_payload(spec, ps_job.payload)) {
            return std::nullopt;
        }
        return ps_job;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
};

class BattleContextProbeResultMapper final : public IResultMapper {
public:
    BattleContextProbeResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db) {
    }

    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        ResultsIni out{};
        out.w_err = result.ps.w_err;
        if (out.w_err == 0) {
            result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        }
        std::string context_blob;
        if (result.ps.ctx.get(savor::context::key::battle::CTX_BLOB, context_blob) && !context_blob.empty()) {
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
        const auto exec_job = execution_db_->GetJob(job_id);

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
        const bool graph_bootstrap = exec_job.has_value() && exec_job->program_ref_kind == kGraphContextProbeRefKind;
        std::optional<savor::db::BattleContextProbeSnapshot> probe;
        if (!graph_bootstrap) {
            probe = analysis_db_->GetBattleContextProbeForExecJob(job_id);
            if (!probe.has_value()) {
                AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                payload.result_kind = "analysisbattle.context_probe.missing";
                return payload;
            }
        } else if (failed) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "analysisbattle.context_probe.failed";
            payload.event_lines.push_back("[battle-context-probe-result] graph_bootstrap=true analysis_created=false status=FAILED");
            return payload;
        } else if (authoring_db_ == nullptr || !exec_job.has_value()) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "analysisbattle.context_probe.context_missing";
            return payload;
        } else {
            const auto job_ini = JobIni::parse(exec_job->input_ini);
            std::string input_set_error;
            const auto input_set_candidates = ResolveInputSetCandidates(
                analysis_db_,
                authoring_db_,
                job_ini.input_set_ref_kind,
                job_ini.input_set_ref_id,
                &input_set_error);
            if (job_ini.source_savestate_id <= 0
                || job_ini.input_set_ref_kind.empty()
                || job_ini.input_set_ref_id <= 0
                || job_ini.battle_run_spec_id <= 0
                || job_ini.explorer_settings_id <= 0
                || input_set_candidates.empty()) {
                AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                payload.result_kind = "analysisbattle.context_probe.context_missing";
                if (!input_set_error.empty()) {
                    payload.event_lines.push_back("[battle-context-probe-result] input_set_error=" + input_set_error);
                }
                return payload;
            }

            const auto now = savor::db::types::UtcNow();
            const auto aggregate = "job-" + std::to_string(job_id);
            std::int64_t context_probe_id = 0;
            if (!analysis_db_->CreateBattleContextProbe(
                    {
                        .wave_id = 0,
                        .source_savestate_id = job_ini.source_savestate_id,
                        .probe_status = savor::db::BattleContextProbeStatus::Queued,
                        .created_at_utc = now,
                        .correlation_id = "workflow-battle-" + aggregate,
                        .causation_id = job_ini.input_set_ref_kind + "-" + std::to_string(job_ini.input_set_ref_id),
                    },
                    &context_probe_id,
                    &error)
                || context_probe_id <= 0
                || !analysis_db_->SetBattleContextProbeExecJobId(context_probe_id, job_id, &error)) {
                AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                payload.result_kind = "analysisbattle.context_probe.analysis_create_failed";
                payload.event_lines.push_back("[battle-context-probe-result] context_probe_create_failed error=" + error);
                return payload;
            }
            probe = analysis_db_->GetBattleContextProbeForExecJob(job_id);

            std::int64_t battle_set_id = 0;
            if (!analysis_db_->CreateBattleSet(
                    {
                        .name = "workflow-battle-" + aggregate,
                        .entry_savestate_id = job_ini.source_savestate_id,
                        .battle_run_spec_id = job_ini.battle_run_spec_id,
                        .explorer_settings_id = job_ini.explorer_settings_id,
                        .launch_fake_attack_min = job_ini.fake_attack_min,
                        .launch_fake_attack_max = job_ini.fake_attack_max,
                        .status = savor::db::BattleSetStatus::Active,
                        .created_at_utc = now,
                        .correlation_id = "workflow-battle-" + aggregate,
                        .causation_id = job_ini.input_set_ref_kind + "-" + std::to_string(job_ini.input_set_ref_id),
                    },
                    &battle_set_id,
                    &error)
                || battle_set_id <= 0) {
                AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                payload.result_kind = "analysisbattle.context_probe.analysis_create_failed";
                payload.event_lines.push_back("[battle-context-probe-result] battle_set_create_failed error=" + error);
                return payload;
            }

            std::size_t waves_created = 0;
            for (const auto& candidate : input_set_candidates) {
                std::int64_t seed_candidate_id = 0;
                const auto candidate_suffix = std::to_string(waves_created);
                if (!analysis_db_->AddBattleSeedCandidate(
                        {
                            .battle_set_id = battle_set_id,
                            .source_probe_result_id =
                                candidate.source_probe_result_id,
                            .source_input_frame_id = candidate.source_input_frame_id,
                            .seed_value = candidate.seed_value,
                            .source_kind = candidate.source_kind,
                            .candidate_status = savor::db::BattleSeedCandidateStatus::Ready,
                            .created_at_utc = now,
                            .correlation_id = "workflow-battle-" + aggregate,
                            .causation_id = job_ini.input_set_ref_kind + "-" + std::to_string(job_ini.input_set_ref_id),
                        },
                        &seed_candidate_id,
                        &error)
                    || seed_candidate_id <= 0) {
                    AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                    payload.result_kind = "analysisbattle.context_probe.analysis_create_failed";
                    payload.event_lines.push_back("[battle-context-probe-result] seed_candidate_create_failed error=" + error);
                    return payload;
                }

                std::int64_t wave_id = 0;
                if (!analysis_db_->CreateBattleTurnWave(
                        {
                            .battle_set_id = battle_set_id,
                            .turn_index = 1,
                            .context_probe_id = context_probe_id,
                            .seed_candidate_id = seed_candidate_id,
                            .status = savor::db::BattleTurnWaveStatus::Ready,
                            .created_at_utc = now,
                            .correlation_id = "workflow-battle-" + aggregate,
                            .causation_id = "battle-context-probe-" + std::to_string(context_probe_id),
                        },
                        &wave_id,
                        &error)
                    || wave_id <= 0) {
                    AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                    payload.result_kind = "analysisbattle.context_probe.analysis_create_failed";
                    payload.event_lines.push_back("[battle-context-probe-result] wave_create_failed error=" + error);
                    return payload;
                }
                ++waves_created;
            }

            payload.event_lines.push_back("[battle-context-probe-result] graph_bootstrap=true analysis_created=true battle_set_id="
                + std::to_string(battle_set_id)
                + " context_probe_id=" + std::to_string(context_probe_id)
                + " waves_created=" + std::to_string(waves_created));
        }

        if (!analysis_db_->CompleteBattleContextProbe(
                {
                    .exec_job_id = job_id,
                    .probe_status = failed ? savor::db::BattleContextProbeStatus::Failed : savor::db::BattleContextProbeStatus::Succeeded,
                    .context_blob = failed ? std::nullopt : parsed.context_blob,
                    .context_version = failed ? std::nullopt : parsed.context_version,
                    .recorded_at_utc = savor::db::types::UtcNow(),
                },
                &error)) {
            payload.event_lines.push_back("[battle-context-probe-result] warning=context_probe_update_failed error=" + error);
        }

        AppendJobCompleted(execution_db_, job_id, failed ? "FAILED" : "SUCCEEDED", &payload.event_lines);
        payload.result_ref_id = probe.has_value() ? probe->context_probe_id : 0;
        if (!failed && payload.result_ref_id > 0) {
            payload.output_key = "battle_context";
            payload.output_data_kind = kContextProbeRefKind;
            payload.output_ref_kind = kContextProbeRefKind;
            payload.output_ref_id = payload.result_ref_id;
        }
        payload.event_lines.push_back("[battle-context-probe-result] job=" + std::to_string(job_id)
            + " context_probe_id=" + std::to_string(probe.has_value() ? probe->context_probe_id : 0)
            + " status=" + (failed ? "FAILED" : "SUCCEEDED"));
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override {
        return std::nullopt;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

class BattleContextProbeTransitionHandler final : public IWorkflowTransitionHandler {
public:
    explicit BattleContextProbeTransitionHandler(savor::db::IAnalysisDb* analysis_db)
        : analysis_db_(analysis_db) {
    }

    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (analysis_db_ == nullptr) {
            decision.blocked_reason = "db_unavailable";
            return decision;
        }
        if (context.output_ref_id.has_value()
            && *context.output_ref_id > 0
            && (!context.output_ref_kind.has_value() || *context.output_ref_kind == kContextProbeRefKind)) {
            if (!context.output_ref_id.has_value()
                || *context.output_ref_id <= 0
                || (context.output_ref_kind.has_value() && *context.output_ref_kind != kContextProbeRefKind)) {
                decision.blocked_reason = "battle_chain_context_probe_output_missing";
                return decision;
            }
            const auto probe = analysis_db_->GetBattleContextProbe(*context.output_ref_id);
            if (!probe.has_value()
                || probe->probe_status != savor::db::BattleContextProbeStatus::Succeeded
                || !probe->context_blob.has_value()) {
                decision.blocked_reason = "context_probe_not_ready";
                return decision;
            }
            auto waves = analysis_db_->ListBattleTurnWavesForContextProbe(probe->context_probe_id);
            // Graph bootstrap links generated waves back to a shared probe. Direct probing instead
            // links the probe to its pre-created source wave, so use that relationship only as a fallback.
            if (waves.empty() && probe->wave_id > 0) {
                if (!context.input_ref_id.has_value()
                    || (context.input_ref_kind.has_value() && *context.input_ref_kind != kWaveRefKind)) {
                    decision.blocked_reason = "wave_ref_missing";
                    return decision;
                }
                if (*context.input_ref_id != probe->wave_id) {
                    decision.blocked_reason = "context_probe_wave_mismatch";
                    return decision;
                }
                const auto wave = analysis_db_->GetBattleTurnWave(probe->wave_id);
                if (!wave.has_value()) {
                    decision.blocked_reason = "wave_not_found";
                    return decision;
                }
                waves.push_back(*wave);
            }
            if (waves.empty()) {
                decision.blocked_reason = "context_probe_waves_missing";
                return decision;
            }
            decision.should_advance = true;
            for (const auto& wave : waves) {
                WorkflowTransitionDecision::DynamicStep step{};
                step.step_key = "BattleTurn/t" + std::to_string(wave.turn_index)
                    + "/w" + std::to_string(wave.wave_id);
                step.step_kind = "battle.single_turn";
                step.input_ref_kind = kWaveRefKind;
                step.input_ref_id = wave.wave_id;
                step.priority = 0;
                step.max_attempts = 1;
                decision.spawn_steps.push_back(std::move(step));
            }
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
        step.priority = 0;
        step.max_attempts = 1;
        decision.should_advance = true;
        decision.spawn_steps.push_back(std::move(step));
        return decision;
    }

private:
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

const WorkflowGraphInputBinding* FindGraphBinding(
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

const savor::db::WorkflowGraphNodeSnapshot* FindGraphNode(
    const savor::db::WorkflowGraphSnapshot& graph,
    std::string_view step_key) {
    for (const auto& node : graph.nodes) {
        if (node.node_key == step_key) {
            return &node;
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

std::int64_t AxisXYId(std::int32_t x, std::int32_t y) {
    return (static_cast<std::int64_t>(std::clamp(x, 0, 255)) << 8)
        | static_cast<std::int64_t>(std::clamp(y, 0, 255));
}

std::vector<InputSetCandidate> ResolveInputSetCandidates(
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    const std::string& ref_kind,
    std::int64_t ref_id,
    std::string* error_out) {
    std::vector<InputSetCandidate> out;
    if (ref_id <= 0) {
        if (error_out) *error_out = "input set ref id must be > 0";
        return out;
    }
    if (ref_kind == "an.input_set") {
        if (analysis_db == nullptr) {
            if (error_out) *error_out = "analysis db unavailable";
            return out;
        }
        const auto frames = analysis_db->ListAnalysisInputSetFrames(ref_id);
        out.reserve(frames.size());
        for (const auto& frame : frames) {
            const auto confirmed =
                analysis_db->FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
                    ref_id,
                    frame.input_frame_id);
            out.push_back(InputSetCandidate{
                .source_probe_result_id =
                    confirmed.has_value()
                    ? std::optional<std::int64_t>(
                          confirmed->probe_result_id)
                    : std::nullopt,
                .source_input_frame_id = frame.input_frame_id,
                .seed_value = confirmed.has_value()
                    ? static_cast<std::int64_t>(
                          confirmed->seed_value)
                    : frame.ordinal,
                .source_kind = confirmed.has_value()
                    ? savor::db::BattleSeedCandidateSourceKind::
                          SeedProbeConfirmedResult
                    : savor::db::BattleSeedCandidateSourceKind::
                          Synthetic,
            });
        }
        if (out.empty() && error_out) *error_out = "an.input_set has no frames";
        return out;
    }
    if (ref_kind == "au.input_set") {
        if (analysis_db == nullptr || authoring_db == nullptr) {
            if (error_out) *error_out = "analysis/authoring db unavailable";
            return out;
        }
        const auto frames = authoring_db->ListAuthoringInputSetFrames(ref_id);
        out.reserve(frames.size());
        for (const auto& frame : frames) {
            std::int64_t input_frame_id = 0;
            std::string ensure_error;
            if (!analysis_db->EnsureSeedProbeInputFrame(
                    AxisXYId(frame.main_x, frame.main_y),
                    AxisXYId(frame.cstick_x, frame.cstick_y),
                    AxisXYId(frame.trigger_x, frame.trigger_y),
                    &input_frame_id,
                    &ensure_error)
                || input_frame_id <= 0) {
                if (error_out) *error_out = "failed to materialize au.input_set frame: " + ensure_error;
                out.clear();
                return out;
            }
            out.push_back(InputSetCandidate{
                .source_input_frame_id = input_frame_id,
                .seed_value = frame.ordinal,
                .source_kind = savor::db::BattleSeedCandidateSourceKind::Manual,
            });
        }
        if (out.empty() && error_out) *error_out = "au.input_set has no frames";
        return out;
    }
    if (error_out) *error_out = "initial_input_frames must use an.input_set or au.input_set";
    return out;
}

std::pair<int, int> ResolveFakeAttackRange(
    const WorkflowGraphStepScheduleContext& context,
    std::vector<std::string>* event_lines) {
    const auto min_override = FindIntegerArgument(context, "fake_attack_min");
    const auto max_override = FindIntegerArgument(context, "fake_attack_max");
    const auto min_fake = static_cast<int>(min_override.value_or(0));
    const auto max_fake = static_cast<int>(max_override.value_or(min_fake));
    const auto low = std::min(min_fake, max_fake);
    const auto high = std::max(min_fake, max_fake);

    if (event_lines != nullptr) {
        event_lines->push_back(
            "[workflow-graph-battle-fake-range] fake_attack_min=" + std::to_string(low)
            + " fake_attack_max=" + std::to_string(high));
    }
    return { low, high };
}

class BattleChainGraphJobPersistenceAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    BattleChainGraphJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db)
        {
    }

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (analysis_db_ == nullptr
            || authoring_db_ == nullptr
            || execution_db_ == nullptr
            || !context.workflow_graph_revision_id.has_value()
            || context.workflow_instance_id <= 0
            || context.workflow_step_id <= 0) {
            return {};
        }

        const auto graph = authoring_db_->GetWorkflowGraphRevision(*context.workflow_graph_revision_id);
        if (!graph.has_value()) {
            return {};
        }
        const auto* node = FindGraphNode(*graph, context.step_key);
        if (node == nullptr
            || node->unit_kind != "battle_chain"
            || node->authored_ref_kind.value_or("") != "authoring.battle_chain_spec"
            || !node->authored_ref_id.has_value()
            || *node->authored_ref_id <= 0) {
            return {};
        }
        const auto battle_chain_spec = authoring_db_->GetBattleChainSpec(*node->authored_ref_id);
        if (!battle_chain_spec.has_value()) {
            return {};
        }
        const auto battle_chain_run_spec = authoring_db_->GetBattleRunSpec(battle_chain_spec->battle_run_spec_id);
        if (!battle_chain_run_spec.has_value()) {
            return {};
        }

        const auto* input_frames = FindGraphBinding(context, "initial_input_frames", "analysis.input_frame_set_id");
        if (input_frames == nullptr
            || (input_frames->ref_kind != "an.input_set" && input_frames->ref_kind != "au.input_set")) {
            return {};
        }
        const auto* entry_savestate = FindGraphBinding(
            context,
            "entry_savestate",
            "state.movie_inactive_savestate_id");
        if (entry_savestate == nullptr
            || entry_savestate->ref_kind != "state.savestate"
            || entry_savestate->ref_id <= 0) {
            return {};
        }
        const auto source_savestate_id =
            entry_savestate->ref_id;
        std::string input_set_error;
        const auto input_set_candidates = ResolveInputSetCandidates(
            analysis_db_,
            authoring_db_,
            input_frames->ref_kind,
            input_frames->ref_id,
            &input_set_error);
        if (input_set_candidates.empty()) {
            return {};
        }

        const auto now = savor::db::types::UtcNow();
        const auto aggregate = std::to_string(context.workflow_instance_id)
            + "-" + std::to_string(context.workflow_step_id);
        std::string error;

        WorkflowStepScheduleResult scheduled{};
        scheduled.persistence.program_ref_kind = input_frames->ref_kind;
        scheduled.persistence.program_ref_id = input_frames->ref_id;
        scheduled.persistence.program_version = kProgramVersion;
        scheduled.persistence.fingerprint = "battle.chain.input_set." + input_frames->ref_kind + "." + std::to_string(input_frames->ref_id)
            + ".battle_chain_spec." + std::to_string(*node->authored_ref_id);

        const auto [fake_attack_min, fake_attack_max] = ResolveFakeAttackRange(context, &scheduled.event_lines);

        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContextProbe),
                    .purpose = "Battle Chain Context Probe",
                    .created_by = "battle.chain.adapters",
                    .created_at_utc = now.time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = input_frames->ref_kind,
                    .domain_ref_id = input_frames->ref_id,
                    .meta_note = "phase=battle_chain.context_probe;input_set_ref_kind=" + input_frames->ref_kind
                        + ";battle_chain_spec_id=" + std::to_string(*node->authored_ref_id),
                },
                &job_set_id,
                &error)
            || job_set_id <= 0) {
            scheduled.event_lines.push_back("[workflow-graph-battle-bootstrap] ok=false error=" + error);
            return scheduled;
        }
        scheduled.root_job_set_id = job_set_id;

        JobIni job_ini{};
        job_ini.source_savestate_id = source_savestate_id;
        job_ini.input_set_ref_kind = input_frames->ref_kind;
        job_ini.input_set_ref_id = input_frames->ref_id;
        job_ini.battle_chain_spec_id = *node->authored_ref_id;
        job_ini.battle_run_spec_id = battle_chain_run_spec->battle_run_spec_id;
        job_ini.explorer_settings_id = battle_chain_spec->explorer_settings_id;
        job_ini.fake_attack_min = fake_attack_min;
        job_ini.fake_attack_max = fake_attack_max;

        std::int64_t exec_job_id = 0;
        if (!execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContextProbe),
                    .program_version = kProgramVersion,
                    .program_ref_kind = std::string(kGraphContextProbeRefKind),
                    .program_ref_id = input_frames->ref_id,
                    .savestate_id = source_savestate_id,
                    .fingerprint = FingerprintFor(job_ini),
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = BuildInputIni(job_ini),
                    .pending_until_workflow_materialized = true,
                },
                &exec_job_id,
                &error)
            || exec_job_id <= 0) {
            scheduled.event_lines.push_back("[workflow-graph-battle-bootstrap] ok=false error=" + error);
            return scheduled;
        }

        scheduled.event_lines.push_back(
            "[workflow-graph-battle-bootstrap] workflow_instance_id="
            + std::to_string(context.workflow_instance_id)
            + " workflow_step_id=" + std::to_string(context.workflow_step_id)
            + " input_set_ref_kind=" + input_frames->ref_kind
            + " input_set_ref_id=" + std::to_string(input_frames->ref_id)
            + " input_count=" + std::to_string(input_set_candidates.size())
            + " battle_chain_spec_id=" + std::to_string(*node->authored_ref_id)
            + " battle_run_spec_id=" + std::to_string(job_ini.battle_run_spec_id)
            + " fake_attack_min=" + std::to_string(job_ini.fake_attack_min)
            + " fake_attack_max=" + std::to_string(job_ini.fake_attack_max)
            + " job=" + std::to_string(exec_job_id));
        return scheduled;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

} // namespace

ProgramKindDescriptor BuildBattleContextProbeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = savor::PK_BattleContextProbe;
    descriptor.program_name = "BattleContextProbe";
    descriptor.job_persistence = std::make_shared<BattleContextProbeJobPersistenceAdapter>(
        execution_db,
        analysis_db);
    descriptor.graph_job_persistence = std::make_shared<BattleChainGraphJobPersistenceAdapter>(
        execution_db,
        analysis_db,
        config.authoring_db);
    descriptor.runtime_init = std::make_shared<BattleContextProbeRuntimeInitAdapter>(execution_db);
    descriptor.result_mapper = std::make_shared<BattleContextProbeResultMapper>(
        execution_db,
        analysis_db,
        config.authoring_db);
    descriptor.workflow_transition = std::make_shared<BattleContextProbeTransitionHandler>(
        analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::battlecontext
