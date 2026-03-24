// SimCore/DB/ProgramDB/ExplorerRunDBCodec.cpp
#include "ExplorerRunDBCodec.h"
#include "ResultErrorFormatting.h"
#include "../../Utils/IniDoc.h"
#include "../DBCore/CoordinatorClock.h"
#include "../DBCore/DbResult.h"
#include "../DBCore/DbEnv.h"
#include "../DBCore/ObjectStore.h"
#include "SeedProbeDBCodec.h"

#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../ExplorerSettingsPredicateRepo.h"
#include "../PredicateSpecRepo.h"
#include "../BattlePlanRepo.h"
#include "../BattlePlanAtomRepo.h"
#include "../BattlePlanTurnRepo.h"
#include "../AddressProgramRepo.h"
#include "../ExplorerSettingsPlanLinkRepo.h"
#include "../ExplorerRunRepo.h"
#include "../DeltaSeedRepo.h"
#include "../SeedProbeRepo.h"
#include "../SavestateRepo.h"
#include "../Querying/DataService.h"

#include "../../Phases/Programs/ProgramRegistry.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"
#include "../../Core/Input/InputPlanFmt.h"
#include "../../Core/Input/AppliedTurnTapeBlob.h"
#include "../../Runner/Script/PSContext.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Utils/Hash.h"
#include "../../Runner/Parallel/DB/DBTriggerEngine.h"

#include <sqlite3.h>
#include <algorithm>
#include <optional>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>

using simcore::db::BattlePlanAtomRepo;
using simcore::db::BattlePlanTurnRepo;
using simcore::db::PredicateSpecRepo;
using simcore::db::PredicateSpecRow;
using simcore::db::AddressProgramRepo;
using simcore::db::ExplorerRunRepo;
using soa::battle::actions::BattlePath;
using soa::battle::actions::TurnPlan;
using soa::battle::actions::ActionPlan;
using soa::battle::actions::BattleAction;
using simcore::TriggerCtx;
using simcore::db::codec::battle::run::BlueprintIni;
using simcore::db::codec::battle::run::JobIni;
using simcore::db::codec::battle::run::ResultsIni;
using SeedProbeBp = simcore::db::codec::seedprobe::BlueprintIni;

static constexpr int kPK = PK_BattleTurnRunner;                                     // from Wire.h
static constexpr int kProgramVersion = phase::battle::runner::PayloadVersion;       // from BattleRunnerPayload.h

namespace {
    static void dfs_fake_vectors(std::size_t idx, uint32_t remaining, std::vector<uint32_t>& cur, std::vector<std::vector<uint32_t>>& out) {
        if (idx + 1 == cur.size()) {
            cur[idx] = remaining;
            out.push_back(cur);
            return;
        }
        for (uint32_t i = 0; i <= remaining; ++i) {
            cur[idx] = i;
            dfs_fake_vectors(idx + 1, remaining - i, cur, out);
        }
    }

    static std::vector<std::vector<uint32_t>> enumerate_fake_vectors(std::size_t turns, uint32_t min_sum, uint32_t max_sum) {
        if (turns == 0) return {};
        std::vector<std::vector<uint32_t>> out;
        std::vector<uint32_t> cur(turns, 0);
        const uint32_t start_sum = std::min(min_sum, max_sum);
        for (uint32_t sum = start_sum; sum <= max_sum; ++sum) dfs_fake_vectors(0, sum, cur, out);
        return out;
    }

    static std::string to_csv(const std::vector<uint32_t>& vals) {
        std::string out;
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) out.push_back(',');
            out += std::to_string(vals[i]);
        }
        return out;
    }

    static std::vector<uint32_t> parse_csv_u32(const std::string& csv) {
        std::vector<uint32_t> out;
        if (csv.empty()) return out;
        std::stringstream ss(csv);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.empty()) continue;
            out.push_back(static_cast<uint32_t>(std::stoul(tok)));
        }
        return out;
    }

}

DbResult<int64_t> ExplorerRunDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& controls_ini)
{

    IniDoc ini = IniDoc::parse(controls_ini);

    BlueprintIni bp_ini = BlueprintIni::from_section(ini);

    const int64_t settings_id = bp_ini.settings_id;
    const int64_t seed_probe_id = bp_ini.seed_probe_id;
    const int priority = bp_ini.priority;
    const uint32_t run_ms = bp_ini.run_ms;
    const uint32_t vi_stall_ms = bp_ini.vi_stall_ms;

    if (settings_id <= 0 || seed_probe_id <= 0) {
        return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "settings_id and seed_probe_id are required." });
    }

    auto selected_delta_ids = BlueprintIni::parse_ids_csv(bp_ini.delta_seed_ids_csv);
    const bool include_all_deltas = selected_delta_ids.empty() || std::find(selected_delta_ids.begin(), selected_delta_ids.end(), 0) != selected_delta_ids.end();

    auto deltas = DeltaSeedRepo::ListUniqueForProbe(seed_probe_id);
    if (!deltas.ok) return DbResult<int64_t>::Err(deltas.error);

    auto plans = ExplorerSettingsPlanLinkRepo::ListBySettings(settings_id);
    if (!plans.ok) return DbResult<int64_t>::Err(plans.error);

    auto probe = simcore::db::SeedProbeRepo::Get(seed_probe_id);
    if (!probe.ok) return DbResult<int64_t>::Err(probe.error);
    const int64_t savestate_id = probe.value.savestate_id;

    int64_t enqueued = 0;
    IniDoc t_ini = IniDoc();
    bp_ini.set_section(t_ini);

    for (auto delta_row : deltas.value) {
        if (!include_all_deltas && std::find(selected_delta_ids.begin(), selected_delta_ids.end(), delta_row.id) == selected_delta_ids.end()) {
            continue;
        }

        std::string meta = std::format("delta_id={}", delta_row.id);
        auto delta_group = JobSetsRepo::CreateChild(job_set_id, "Explorer Run Delta Group", PK_BattleTurnRunner, std::nullopt, std::nullopt, std::nullopt, meta, std::nullopt);
        if (!delta_group.ok) return DbResult<int64_t>::Err(delta_group.error);

        int64_t delta_enqueued = 0;

        for (auto plan_row : plans.value) {

            JobIni jb_ini = JobIni::from_section(ini);
            jb_ini.savestate_id = savestate_id;
            jb_ini.plan_id = plan_row.plan_id;
            jb_ini.delta_seed_id = delta_row.id;

            auto turns = BattlePlanTurnRepo::LoadTurnsByPlan(plan_row.plan_id);
            if (!turns.ok) return DbResult<int64_t>::Err(turns.error);
            auto fake_vectors = enumerate_fake_vectors(turns.value.size(), bp_ini.min_fake_attacks, bp_ini.max_fake_attacks);

            auto run = ExplorerRunRepo::IdempotentCreate(settings_id, plan_row.plan_id, delta_row.id);
            if (!run.ok) return DbResult<int64_t>::Err(run.error);

            for (const auto& fv : fake_vectors) {
                jb_ini.fake_attacks_by_turn_csv = to_csv(fv);
                std::string to_hash = "ExplorerRun|" + std::to_string(kProgramVersion) + "|" +
                    std::to_string(run.value) + "|" + std::to_string(plan_row.plan_id) + "|" + std::to_string(delta_row.id) + "|" +
                    std::to_string(run_ms) + "|" + std::to_string(vi_stall_ms) + "|" + jb_ini.fake_attacks_by_turn_csv;
                const std::string fingerprint = hash::sha256(to_hash.data(), to_hash.size());

                auto cj = JobsRepo::CreateOrGetByFingerprint(delta_group.value, kPK, kProgramVersion, run.value, fingerprint, priority, jb_ini.append_section(t_ini).to_string_preserve_order(), savestate_id);
                if (!cj.ok) return DbResult<int64_t>::Err(cj.error);

                auto ev = JobEventsRepo::Append(cj.value, "ENQUEUED");
                if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

                enqueued++;
                delta_enqueued++;
            }
        }

        (void)JobSetsRepo::SetExpectedTotal(delta_group.value, delta_enqueued);
    }

    (void)JobSetsRepo::SetExpectedTotal(job_set_id, enqueued);
    return DbResult<int64_t>::Ok(enqueued);
}

DbResult<simcore::PSJob> ExplorerRunDBCodec::decode_job_from_db(int64_t job_id)
{
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    const int64_t run_id = jr.value.program_ref_id;

    if (!jr.value.vm_kv.has_value()) return DbResult<simcore::PSJob>::Err({ .kind = DbErrorKind::NotFound, .message = "job has no vm_kv" });

    IniDoc ini = IniDoc::parse(jr.value.vm_kv.value());
    if (!ini.has_section(BlueprintIni::SECTION_NAME) || !ini.has_section(JobIni::SECTION_NAME))
        return DbResult<simcore::PSJob>::Err({ .kind = DbErrorKind::NotFound, .message = "job vm_kv does not have all necessary sections for blueprint or job" });

    BlueprintIni bp_ini = BlueprintIni::from_section(ini);
    JobIni jb_ini = JobIni::from_section(ini);
    
    const int64_t settings_id = bp_ini.settings_id;
    const int64_t plan_id = jb_ini.plan_id;
    const int64_t delta_seed_id = jb_ini.delta_seed_id;
    const uint32_t run_ms = bp_ini.run_ms;
    const uint32_t vi_stall_ms = bp_ini.vi_stall_ms;
    if (!plan_id || !delta_seed_id) return DbResult<simcore::PSJob>::Err({ .kind = DbErrorKind::NotFound, .message = "plan_id and delta_seed_id required" });

    auto drow = simcore::db::DeltaSeedRepo::Get(delta_seed_id);
    if (!drow.ok) return DbResult<simcore::PSJob>::Err(drow.error);
    if (!drow.value) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::NotFound, 0, "delta_seed_id not found" });

    auto turnsR = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(plan_id);
    if (!turnsR.ok) return DbResult<simcore::PSJob>::Err(turnsR.error);

    auto fake_vec = parse_csv_u32(jb_ini.fake_attacks_by_turn_csv);
    if (fake_vec.size() != turnsR.value.size()) fake_vec.assign(turnsR.value.size(), 0);

    BattlePath path;
    for (size_t i = 0; i < turnsR.value.size(); ++i) {
        auto& t = turnsR.value[i];
        TurnPlan plan{ .fake_attack_count = fake_vec[i] };
        auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(plan_id, t.turn_index);
        if (!actorsR.ok) return DbResult<simcore::PSJob>::Err(actorsR.error);
        for (auto& a : actorsR.value) {
            auto atom = simcore::db::BattlePlanAtomRepo::Get(a.atom_id);
            if (!atom.ok) return DbResult<simcore::PSJob>::Err(atom.error);
            ActionPlan ap{ .actor_slot = (uint8_t)atom.value.actor_slot, .macro = (BattleAction)atom.value.action_type };
            // add target if valid
            if (atom.value.target_slot >= -1 && atom.value.target_slot < 12) ap.params.target_slot = atom.value.target_slot;
            // add item id if valid
            if (atom.value.param_item_id >= 0) ap.params.item_id = atom.value.param_item_id;
            plan.spec.push_back(std::move(ap));
        }
        path.push_back(std::move(plan));
    }

    std::vector<simcore::pred::Spec> preds;
    auto plist = simcore::db::ExplorerSettingsPredicateRepo::List(settings_id);
    if (!plist.ok) return DbResult<simcore::PSJob>::Err(plist.error);
    preds.reserve(plist.value.size());
    for (auto& r : plist.value) {
        auto p = simcore::db::PredicateSpecRepo::Get(r.predicate_id);
        if (!p.ok) return DbResult<simcore::PSJob>::Err(p.error);
        std::vector<uint8_t> lhs_prog, rhs_prog;
        if (p.value.lhs_prog_id.has_value()) {
            auto lhs_prog_row = simcore::db::AddressProgramRepo::Get(p.value.lhs_prog_id.value());
            if (!lhs_prog_row.ok) return DbResult<simcore::PSJob>::Err(lhs_prog_row.error);
            lhs_prog = std::move(lhs_prog_row.value.prog_bytes);
        }
        if (p.value.rhs_prog_id.has_value()) {
            auto rhs_prog_row = simcore::db::AddressProgramRepo::Get(p.value.rhs_prog_id.value());
            if (!rhs_prog_row.ok) return DbResult<simcore::PSJob>::Err(rhs_prog_row.error);
            rhs_prog = std::move(rhs_prog_row.value.prog_bytes);
        }
        simcore::pred::Spec spec{
            .id = (uint16_t)r.ordinal,
            .required_bp = (uint16_t)p.value.required_bp,
            .kind = (simcore::pred::PredKind)p.value.kind,
            .width = (uint8_t)p.value.width,
            .cmp = (simcore::pred::CmpOp)p.value.cmp_op,
            .flags = (uint32_t)p.value.flags,
            .lhs_addr = (uint32_t)p.value.lhs_addr,
            .rhs_value = (uint64_t)p.value.rhs_value,
            .turn_mask = (uint32_t)p.value.turn_mask,
            .lhs_prog = lhs_prog,
            .rhs_prog = rhs_prog,
            .name = p.value.name.size() > 0 ? p.value.name : "unnamed",
            .desc = p.value.description.size() > 0 ? p.value.description : "no description"
        };
        if (p.value.lhs_key.has_value()) spec.lhs_key = (addr::AddrKey)p.value.lhs_key.value();
        if (p.value.rhs_key.has_value()) spec.rhs_key = (addr::AddrKey)p.value.rhs_key.value();
        preds.push_back(std::move(spec));
    }

    phase::battle::runner::EncodeSpec spec;
    spec.run_ms = run_ms;
    spec.vi_stall_ms = vi_stall_ms;
    spec.path = std::move(path);
    spec.predicates = std::move(preds);
    spec.initial = drow.value->input;

    simcore::PSJob out{};
    phase::battle::runner::encode_payload(spec, out.payload);
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> ExplorerRunDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line)
{
    auto r = JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!r.ok) return DbResult<void>::Err(r.error);
    return DbResult<void>::Ok();
}

DbResult<void> ExplorerRunDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success)
{
    IniDoc ini = IniDoc::parse(results_ini);
    ResultsIni results = ResultsIni::from_section(ini);
    std::string persisted_results_ini = results_ini;

    auto st = JobsRepo::SetState(job_id, success ? "SUCCEEDED" : "FAILED");
    if (!st.ok) return DbResult<void>::Err(st.error);

    auto ev = JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!ev.ok) return DbResult<void>::Err(ev.error);

    if (success) {
        if (!results.applied_input_tape_text.empty()) {
            auto art = simcore::db::ObjectStore::PutText(results.applied_input_tape_text);
            if (art.ok) {
                results.applied_input_artifact_id = art.value.id;
                results.applied_input_tape_text.clear();
                results.set_section(ini);
                persisted_results_ini = ini.to_string_sorted();
                auto ev2 = JobEventsRepo::Append(job_id, "RESULTS", persisted_results_ini);
                if (!ev2.ok) return DbResult<void>::Err(ev2.error);
            }
        }

        auto jr = JobsRepo::Get(job_id);
        if (!jr.ok) return DbResult<void>::Err(jr.error);
        const int64_t run_id = jr.value.program_ref_id;

        auto s1 = simcore::db::ExplorerRunRepo::SetResultsIni(run_id, persisted_results_ini);
        if (!s1.ok) return DbResult<void>::Err(s1.error);

        auto lines = JobEventsRepo::ListByJobAndKind(job_id, "PROGRESS");
        if (!lines.ok) return DbResult<void>::Err(lines.error);
        std::string transcript;
        for (size_t i = 0; i < lines.value.size(); ++i) {
            if (i) transcript.push_back('\n');
            if (lines.value[i].payload) transcript.append(*lines.value[i].payload);
        }
        auto art = simcore::db::ObjectStore::PutText(transcript);
        if (!art.ok) return DbResult<void>::Err(art.error);

        auto s2 = simcore::db::ExplorerRunRepo::SetProgressLogArtifactId(run_id, art.value.id);
        if (!s2.ok) return DbResult<void>::Err(s2.error);

        auto md = simcore::db::ExplorerRunRepo::MarkDone(run_id);
        if (!md.ok) return DbResult<void>::Err(md.error);
    }
    return DbResult<void>::Ok();
}

DbResult<std::string> ExplorerRunDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    std::string out;

    if (job_id) {
        auto jr = JobsRepo::Get(*job_id);
        if (!jr.ok) return DbResult<std::string>::Err(jr.error);
        auto run = simcore::db::ExplorerRunRepo::Get(jr.value.program_ref_id);
        if (!run.ok) return DbResult<std::string>::Err(run.error);

        auto rows = JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) {
            if (i) out.push_back('\n');
            if (rows.value[i].payload) out.append(*rows.value[i].payload);
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    if (job_set_id) {
        auto rows = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        int64_t cur_job{ -1 };
        for (auto& e : rows.value) {
            if (e.job_id != cur_job) {
                if (!out.empty()) out.push_back('\n');
                out.append("[job_id ");
                out.append(std::to_string(e.job_id));
                out.append("]");
                cur_job = e.job_id;
            }
            out.push_back('\n');
            if (e.payload) out.append(*e.payload);
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    return DbResult<std::string>::Err({ .kind = DbErrorKind::NotFound, .message = "must provide job_id or job_set_id" });
}

DbResult<std::string> ExplorerRunDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    if (job_id) {
        auto s = JobEventsRepo::GetLatestPayload(*job_id, "RESULTS");
        if (!s.ok) return DbResult<std::string>::Err(s.error);
        return DbResult<std::string>::Ok(simcore::db::codec::HumanizeResultIniErrors(s.value.value_or(std::string{})));
    }

    if (job_set_id) {
        auto rows = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        std::string out;
        int64_t cur_job{ -1 };
        for (auto& e : rows.value) {
            if (e.job_id != cur_job) {
                if (!out.empty()) out.push_back('\n');
                out.append("[job_id ");
                out.append(std::to_string(e.job_id));
                out.append("]");
                cur_job = e.job_id;
            }
            out.push_back('\n');
            if (e.payload) out.append(simcore::db::codec::HumanizeResultIniErrors(*e.payload));
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    return DbResult<std::string>::Err({ .kind = DbErrorKind::NotFound, .message = "must provide job_id or job_set_id" });
}

DbResult<std::optional<int64_t>> ExplorerRunDBCodec::get_required_savestate_id(int64_t job_id) {
    using namespace simcore::db;
    // jobs -> job_set
    auto j = JobsRepo::Get(job_id);
    if (!j.ok) return DbResult<std::optional<int64_t>>::Err(j.error);
    if (!j.value.vm_kv.has_value()) return DbResult<std::optional<int64_t>>::Err({ .kind = DbErrorKind::NotFound, .message = "job has no vm_kv" });

    JobIni jb_ini = JobIni::from_section(IniDoc::parse(j.value.vm_kv.value()));

    return DbResult<std::optional<int64_t>>::Ok(jb_ini.savestate_id);
}

DbResult<simcore::PSInit> ExplorerRunDBCodec::build_psinit_for_job(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSInit>::Err(jr.error);

    IniDoc ini = IniDoc::parse(jr.value.vm_kv.value());
    JobIni jb_ini = JobIni::from_section(ini);

    auto savestate = SavestateRepo::Get(jb_ini.savestate_id);
    if (!savestate.ok) return simcore::db::DbResult<simcore::PSInit>::Err(savestate.error);
    if (!savestate.value.has_value()) return simcore::db::DbResult<simcore::PSInit>::Err({DbErrorKind::NotFound, 0, "Savestate not found"});

    auto temp_savestate_path = ObjectStore::MaterializeToTemp(savestate.value.value().object_ref_id);
    if (!temp_savestate_path.ok) return simcore::db::DbResult<simcore::PSInit>::Err(temp_savestate_path.error);

    simcore::PSInit init{};
    init.savestate_path = temp_savestate_path.value;
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_Battle; 
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> ExplorerRunDBCodec::build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) {
    ResultsIni results{};
    bool success = r.ps.ok ? true : false;
    results.w_err = r.ps.w_err;

    if (results.w_err == 0) r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, results.dw_err);

    if (success) {
        r.ps.ctx.get(simcore::keys::core::VI_FIRST, results.vi_start);
        r.ps.ctx.get(simcore::keys::core::VI_LAST, results.vi_end);

        std::string turn_blob;
        r.ps.ctx.get(simcore::keys::battle::APPLIED_INPUTPLAN_TURN_BLOB, turn_blob);
        if (!turn_blob.empty()) {
            std::vector<simcore::inputtape::TurnChunk> chunks;
            if (simcore::inputtape::decode_turn_chunks(turn_blob, chunks) && !chunks.empty()) {
                results.input_apply_vi_start = chunks.front().vi_start;
                results.input_apply_vi_end = chunks.back().vi_end;
                results.applied_turn_blob_sha256 = hash::sha256(turn_blob.data(), turn_blob.size());
                results.applied_turn_windows_csv = simcore::inputtape::turn_windows_csv(chunks);
                results.applied_input_vi_durations_csv = simcore::inputtape::durations_csv(chunks);
                auto flat = simcore::inputtape::flatten_plan(chunks);
                results.applied_frame_count = static_cast<uint32_t>(flat.size());
                if (!flat.empty()) {
                    results.applied_input_sha256 = hash::sha256(reinterpret_cast<const char*>(flat.data()), flat.size() * sizeof(simcore::GCInputFrame));
                }
                results.applied_input_tape_text = simcore::inputtape::render_text(chunks);
                results.applied_input_summary = simcore::DescribeChosenInputs(flat, "\n");
            }
        }
    }

    IniDoc ini;
    return DbResult<std::string>::Ok(results.append_section(ini).to_string_sorted());
}

DbResult<void> ExplorerRunDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {

    if (ctx.prev_program_kind != PK_SeedProbe) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "previous program kind limited to SeedProbe(" + std::to_string(PK_SeedProbe) + "), pk=" + std::to_string(ctx.prev_program_kind) });
    
    IniDoc ini = IniDoc::parse(action_args_ini);
    if (!ini.has_section(BlueprintIni::SECTION_NAME))return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
        "ExplorerRun Trigger has no BlueprintIni" });

    BlueprintIni bp = BlueprintIni::from_section(ini);

    auto plans = simcore::db::ExplorerSettingsPlanLinkRepo::ListBySettings(bp.settings_id);
    if (!plans.ok) return DbResult<void>::Err(plans.error);
    if (plans.value.size() == 0) return DbResult<void>::Err({DbErrorKind::NotFound, 0, 
        "No plans found for setting_id=" + std::to_string(bp.settings_id) + ". Make sure they were registered in the ExplorerSettingsPlanLink repo."});

    SeedProbeBp spbp = SeedProbeBp::from_section(ini);

    auto unique_count = DeltaSeedRepo::ListUniqueForProbe(spbp.probe_id);
    if (!unique_count.ok) return DbResult<void>::Err(unique_count.error);
    if (unique_count.value.size() == 0) return DbResult<void>::Err({ DbErrorKind::NotFound, 0,
        "No unique seeds found for probe_id=" + std::to_string(spbp.probe_id) + ". Make sure that the SeedProbe is done and that they were registered in the DeltaSeed repo." });


    auto crt = simcore::db::JobSetsRepo::Create("BattleRun", PK_BattleTurnRunner, std::nullopt, std::nullopt, std::nullopt, "", plans.value.size() * unique_count.value.size());
    if (!crt.ok) return DbResult<void>::Err(crt.error);

    bp.seed_probe_id = spbp.probe_id;
    
    encode_job_into_db(crt.value, bp.to_string());

    return DbResult<void>::Ok();
}

DbResult<std::string> ExplorerRunDBCodec::build_artifact_ini_from_db(int64_t job_id)
{
    ArtifactIniBuilder artifacts{};

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::string>::Err(jr.error);

    if (!jr.value.vm_kv) return DbResult<std::string>::Err(jr.error);

    IniKV kv = IniDoc::parse(*jr.value.vm_kv).section_kv(JobIni::SECTION_NAME);
    const uint64_t savestate_id = kv.get_i64("savestate_id", 0);

    auto ss = SavestateRepo::Get(savestate_id);
    if (!ss.ok) return DbResult<std::string>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
        "No Savestate found..." });

    artifacts.add_artifact("Savestate", ss.value.value().object_ref_id);

    auto rr = simcore::db::JobEventsRepo::GetLatestPayload(job_id, "RESULTS");
    if (rr.ok && rr.value.has_value()) {
        IniDoc rdoc = IniDoc::parse(*rr.value);
        if (rdoc.has_section(ResultsIni::SECTION_NAME)) {
            ResultsIni res = ResultsIni::from_section(rdoc);
            if (res.applied_input_artifact_id > 0) {
                artifacts.add_artifact("Applied Input Tape", res.applied_input_artifact_id);
            }
        }
    }
    
    return DbResult<std::string>::Ok(artifacts.to_string());
}
