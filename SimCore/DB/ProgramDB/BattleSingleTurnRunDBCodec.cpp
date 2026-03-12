#include "BattleSingleTurnRunDBCodec.h"

#include "../../Utils/Hash.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Runner/Script/KeyRegistry.h"
#include "../../Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "../../Phases/Programs/BattleRunner/BattleOutcome.h"
#include "../../Core/Input/SoaBattle/PlanWriter.h"

#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/TriggersRepo.h"
#include "../ExplorerSettingsPlanLinkRepo.h"
#include "../ExplorerSettingsPredicateRepo.h"
#include "../PredicateSpecRepo.h"
#include "../AddressProgramRepo.h"
#include "../BattlePlanTurnRepo.h"
#include "../BattlePlanAtomRepo.h"
#include "../ExplorerRunRepo.h"
#include "../ExplorerSettingsRepo.h"
#include "../DeltaSeedRepo.h"
#include "../SeedProbeRepo.h"
#include "../SavestateRepo.h"
#include "../DBCore/ObjectStore.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "../Querying/DataService.h"

using BRBp = simcore::db::codec::battle::run::BlueprintIni;
using STJob = simcore::db::codec::battle::singleturn::JobIni;
using STRes = simcore::db::codec::battle::singleturn::ResultsIni;
using STWave = simcore::db::codec::battle::singleturn::WaveIni;

static constexpr int kPK = simcore::PK_BattleSingleTurnRunner;
static constexpr int kPV = phase::battle::turnrunner::PayloadVersion;

namespace {
    struct WaveMetaIni {
        static constexpr const char* SECTION_NAME = "BattleSingleTurn.WaveMeta";
        int64_t root_group_id{-1};
        uint32_t wave_turn{1};
        int64_t settings_id{-1};
        int64_t seed_probe_id{-1};
        std::string settings_name;
        int64_t tas_movie_id{-1};

        static WaveMetaIni from_meta(const std::optional<std::string>& meta_text) {
            WaveMetaIni m{};
            if (!meta_text.has_value() || meta_text->empty()) return m;
            IniDoc ini = IniDoc::parse(*meta_text);
            if (!ini.has_section(SECTION_NAME)) return m;
            auto kv = ini.section_kv(SECTION_NAME);
            m.root_group_id = kv.get_i64("root_group_id", -1);
            m.wave_turn = kv.get_u32("wave_turn", 1);
            m.settings_id = kv.get_i64("settings_id", -1);
            m.seed_probe_id = kv.get_i64("seed_probe_id", -1);
            m.settings_name = kv.get("settings_name", "");
            m.tas_movie_id = kv.get_i64("tas_movie_id", -1);
            return m;
        }

        std::string to_meta_text() const {
            IniDoc ini{};
            ini.ensure_section(SECTION_NAME);
            ini.set(SECTION_NAME, "root_group_id", std::to_string(root_group_id));
            ini.set(SECTION_NAME, "wave_turn", std::to_string(wave_turn));
            ini.set(SECTION_NAME, "settings_id", std::to_string(settings_id));
            ini.set(SECTION_NAME, "seed_probe_id", std::to_string(seed_probe_id));
            ini.set(SECTION_NAME, "settings_name", settings_name);
            ini.set(SECTION_NAME, "tas_movie_id", std::to_string(tas_movie_id));
            return ini.to_string_sorted();
        }
    };

    std::string action_key_for_plan_turn(int64_t plan_id, uint32_t turn_index) {
        auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(plan_id, (int32_t)turn_index);
        if (!actorsR.ok) return "";
        std::string s;
        s.reserve(64);
        for (auto& a : actorsR.value) {
            auto atom = simcore::db::BattlePlanAtomRepo::Get(a.atom_id);
            if (!atom.ok) continue;
            s += std::to_string(atom.value.action_type) + ":" + std::to_string(atom.value.actor_slot) + ":" + std::to_string(atom.value.param_item_id) + ":" + std::to_string(atom.value.target_slot) + "|";
        }
        return hash::sha256(s.data(), s.size());
    }

    struct Survivor {
        int64_t job_id{-1};
        int64_t savestate_id{-1};
        int64_t delta_seed_id{-1};
        uint32_t fake_used{0};
        uint32_t rng_seed{0};
        std::string action_key;
    };

    static std::string get_settings_name(int64_t settings_id) {
        auto s = simcore::db::ExplorerSettingsRepo::Get(settings_id);
        if (!s.ok) return "";
        return s.value.name;
    }

    static DbResult<void> set_wave_meta_for_jobset(int64_t job_set_id, int64_t root_group_id, uint32_t wave_turn, const BRBp& bp) {
        WaveMetaIni meta{};
        meta.root_group_id = root_group_id;
        meta.wave_turn = wave_turn;
        meta.settings_id = bp.settings_id;
        meta.seed_probe_id = bp.seed_probe_id;
        meta.settings_name = get_settings_name(bp.settings_id);
        auto mr = simcore::db::JobSetsRepo::SetMetaText(job_set_id, meta.to_meta_text());
        if (!mr.ok) return DbResult<void>::Err(mr.error);
        return DbResult<void>::Ok();
    }

    static DbResult<int64_t> resolve_root_group_id_for_jobset(int64_t job_set_id) {
        auto js = simcore::db::JobSetsRepo::Get(job_set_id);
        if (!js.ok) return DbResult<int64_t>::Err(js.error);

        WaveMetaIni meta = WaveMetaIni::from_meta(js.value.meta_text);
        if (meta.root_group_id > 0) return DbResult<int64_t>::Ok(meta.root_group_id);

        int64_t cur = job_set_id;
        while (true) {
            auto pr = simcore::db::JobSetsRepo::GetParent(cur);
            if (!pr.ok) return DbResult<int64_t>::Err(pr.error);
            if (!pr.value.has_value()) break;
            cur = *pr.value;
        }
        return DbResult<int64_t>::Ok(cur);
    }
}

// Only used for the first wave
DbResult<int64_t> BattleSingleTurnRunDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    IniDoc ini = IniDoc::parse(blueprint_ini);
    BRBp bp = BRBp::from_section(ini);
    STWave wave = STWave::from_section(ini);

    if (wave.cur_turn != 1) 
    {
        DbError waveError{};
        waveError.message = "Only use encode job into db for the first wave.";
        return DbResult<int64_t>::Err(waveError);
    }

    auto root = resolve_root_group_id_for_jobset(job_set_id);
    if (!root.ok) return DbResult<int64_t>::Err(root.error);
    auto sm = set_wave_meta_for_jobset(job_set_id, root.value, wave.cur_turn, bp);
    if (!sm.ok) return DbResult<int64_t>::Err(sm.error);

    auto plans = simcore::db::ExplorerSettingsPlanLinkRepo::ListBySettings(bp.settings_id);
    if (!plans.ok) return DbResult<int64_t>::Err(plans.error);

    auto deltas = simcore::db::DeltaSeedRepo::ListUniqueForProbe(bp.seed_probe_id);
    if (!deltas.ok) return DbResult<int64_t>::Err(deltas.error);
    auto probe = simcore::db::SeedProbeRepo::Get(bp.seed_probe_id);
    if (!probe.ok) return DbResult<int64_t>::Err(probe.error);


    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> starts;
    for (auto& d : deltas.value) starts.push_back({ probe.value.savestate_id, d.id , probe.value.neutral_seed, d.seed_delta});

    int64_t enqueued = 0;
    IniDoc t_ini{};
    bp.set_section(t_ini);
    wave.set_section(t_ini);

    for (auto& st : starts) {

        std::unordered_set<std::string> seenIt{};

        std::string desc = std::format("Wave {}: {:#X}+({})", wave.cur_turn, std::get<2>(st), std::get<3>(st));
        auto rng = simcore::db::JobSetsRepo::CreateChild(job_set_id, desc, kPK, std::nullopt, std::nullopt, std::nullopt, "", std::nullopt);
        if (!rng.ok) return DbResult<int64_t>::Err(rng.error);

        int64_t enq_start = enqueued;

        for (auto& plan : plans.value) {
            auto turns = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(plan.plan_id);
            if (!turns.ok) return DbResult<int64_t>::Err(turns.error);
            if (wave.cur_turn < 1 || wave.cur_turn > turns.value.size()) continue;


            auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(plan.plan_id, turns.value[wave.cur_turn - 1].turn_index);
            if (!actorsR.ok) return DbResult<int64_t>::Err(actorsR.error);

            std::stringstream ss{};
            for (auto& a : actorsR.value) {
                ss << std::format(":{}", a.atom_id);
            }
            std::string turn_plan = ss.str();

            if (seenIt.contains(turn_plan)) continue;
            seenIt.insert(turn_plan);

            STJob jb{};
            jb.plan_id = plan.plan_id;
            jb.delta_seed_id = std::get<1>(st);
            jb.savestate_id = std::get<0>(st);
            jb.turn_index = wave.cur_turn;
            jb.fake_attacks_used_before = 0;
            jb.fake_attacks_this_turn = 0;
            jb.action_key = action_key_for_plan_turn(plan.plan_id, wave.cur_turn - 1);

            auto run = simcore::db::ExplorerRunRepo::IdempotentCreate(bp.settings_id, plan.plan_id, std::get<1>(st) > 0 ? std::get<1>(st) : 0);
            if (!run.ok) return DbResult<int64_t>::Err(run.error);

            for (uint32_t fake = 0; fake <= bp.max_fake_attacks; ++fake) {
                jb.fake_attacks_this_turn = fake;
                const std::string vm = jb.append_section(t_ini).to_string_sorted();
                const std::string fp = hash::sha256(vm.data(), vm.size());
                auto cj = simcore::db::JobsRepo::CreateOrGetByFingerprint(rng.value, kPK, kPV, run.value, fp, bp.priority, vm, jb.savestate_id);
                if (!cj.ok) return DbResult<int64_t>::Err(cj.error);
                auto ev = simcore::db::JobEventsRepo::Append(cj.value, "ENQUEUED");
                if (!ev.ok) return DbResult<int64_t>::Err(ev.error);
                ++enqueued;
            }

        }

        (void)simcore::db::JobSetsRepo::SetExpectedTotal(rng.value, enqueued - enq_start);
    }

    (void)simcore::db::JobSetsRepo::SetExpectedTotal(job_set_id, enqueued);

    IniKV cond;
    cond.add("type", "ALL_FINISHED");
    auto tr = simcore::db::TriggersRepo::AddForJobSet(job_set_id, kPK, cond.to_string_sorted(), ini.to_string_sorted());
    if (!tr.ok) return DbResult<int64_t>::Err(tr.error);

    return DbResult<int64_t>::Ok(enqueued);
}

DbResult<simcore::PSJob> BattleSingleTurnRunDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "missing vm_kv" });
    IniDoc ini = IniDoc::parse(*jr.value.vm_kv);

    BRBp bp = BRBp::from_section(ini);
    STJob jb = STJob::from_section(ini);

    auto turnsR = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(jb.plan_id);
    if (!turnsR.ok) return DbResult<simcore::PSJob>::Err(turnsR.error);
    if (jb.turn_index < 1 || jb.turn_index > turnsR.value.size()) return DbResult<simcore::PSJob>::Err({ DbErrorKind::InvalidArgument, 0, "turn_index out of range" });

    soa::battle::actions::TurnPlan turn{ .fake_attack_count = jb.fake_attacks_this_turn };
    auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(jb.plan_id, (int32_t)(jb.turn_index - 1));
    if (!actorsR.ok) return DbResult<simcore::PSJob>::Err(actorsR.error);
    for (auto& a : actorsR.value) {
        auto atom = simcore::db::BattlePlanAtomRepo::Get(a.atom_id);
        if (!atom.ok) return DbResult<simcore::PSJob>::Err(atom.error);
        soa::battle::actions::ActionPlan ap{ .actor_slot = (uint8_t)atom.value.actor_slot, .macro = (soa::battle::actions::BattleAction)atom.value.action_type };
        if (atom.value.target_slot >= -1 && atom.value.target_slot < 12) ap.params.target_slot = atom.value.target_slot;
        if (atom.value.param_item_id >= 0) ap.params.item_id = atom.value.param_item_id;
        turn.spec.push_back(std::move(ap));
    }

    std::vector<simcore::pred::Spec> preds;
    auto plist = simcore::db::ExplorerSettingsPredicateRepo::List(bp.settings_id);
    if (!plist.ok) return DbResult<simcore::PSJob>::Err(plist.error);
    for (auto& r : plist.value) {
        auto p = simcore::db::PredicateSpecRepo::Get(r.predicate_id);
        if (!p.ok) return DbResult<simcore::PSJob>::Err(p.error);
        std::vector<uint8_t> lhs_prog, rhs_prog;
        if (p.value.lhs_prog_id.has_value()) {
            auto lhs = simcore::db::AddressProgramRepo::Get(*p.value.lhs_prog_id);
            if (!lhs.ok) return DbResult<simcore::PSJob>::Err(lhs.error);
            lhs_prog = std::move(lhs.value.prog_bytes);
        }
        if (p.value.rhs_prog_id.has_value()) {
            auto rhs = simcore::db::AddressProgramRepo::Get(*p.value.rhs_prog_id);
            if (!rhs.ok) return DbResult<simcore::PSJob>::Err(rhs.error);
            rhs_prog = std::move(rhs.value.prog_bytes);
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
        if (p.value.lhs_key.has_value()) spec.lhs_key = (addr::AddrKey)*p.value.lhs_key;
        if (p.value.rhs_key.has_value()) spec.rhs_key = (addr::AddrKey)*p.value.rhs_key;
        preds.push_back(std::move(spec));
    }

    phase::battle::turnrunner::EncodeSpec spec{};
    spec.run_ms = bp.run_ms;
    spec.vi_stall_ms = bp.vi_stall_ms;
    spec.current_turn = jb.turn_index;
    spec.max_turn = turnsR.value.size();
    spec.turn_plan = std::move(turn);
    spec.predicates = std::move(preds);
    spec.fake_attack_budget_max = bp.max_fake_attacks;
    spec.fake_attacks_used_before_turn = jb.fake_attacks_used_before;

    if (jb.turn_index == 1 && jb.delta_seed_id > 0) {
        auto drow = simcore::db::DeltaSeedRepo::Get(jb.delta_seed_id);
        if (!drow.ok) return DbResult<simcore::PSJob>::Err(drow.error);
        if (!drow.value) return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "delta seed not found" });
        spec.has_initial_input = true;
        spec.initial = drow.value->input;
    }

    auto out_dir = std::filesystem::path(simcore::db::ObjectStore::TmpDir()) / "BattleSingleTurnDerived";
    std::filesystem::create_directories(out_dir);
    spec.output_savestate_path = (out_dir / ("job_" + std::to_string(job_id) + ".sav")).string();

    simcore::PSJob out{};
    if (!phase::battle::turnrunner::encode_payload(spec, out.payload))
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::Unknown, 0, "encode payload failed" });
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> BattleSingleTurnRunDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line) {
    auto r = simcore::db::JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!r.ok) return DbResult<void>::Err(r.error);
    return DbResult<void>::Ok();
}

DbResult<void> BattleSingleTurnRunDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) {
    auto ev = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!ev.ok) return DbResult<void>::Err(ev.error);

    IniDoc ini = IniDoc::parse(results_ini);
    STRes r = STRes::from_section(ini);

    if (success 
        && r.w_err == 0 
        && r.dw_err == 0
        && r.battle_outcome != (uint32_t)simcore::battle::Outcome::PlanMaterializeFailure
        && r.battle_outcome != (uint32_t)simcore::battle::Outcome::Unknown) {
        DbResult<void> save = DbResult<void>::Ok();
        if (r.battle_outcome == (uint32_t)simcore::battle::Outcome::ReachedNextTurn || r.battle_outcome == (uint32_t)simcore::battle::Outcome::Victory) {
            auto obj = simcore::db::ObjectStore::FinalizeFromFile(r.savestate_path, simcore::db::Compression::None, std::filesystem::path(r.savestate_path).filename().string());
            if (!obj.ok) save = DbResult<void>::Err(obj.error);
            else {
                auto plan = simcore::db::SavestateRepo::Plan(simcore::db::SavestateType::BATTLE, "BattleSingleTurnRunner");
                if (!plan.ok) save = DbResult<void>::Err(plan.error);
                else {
                    auto fin = simcore::db::SavestateRepo::Finalize(plan.value, obj.value.id);
                    if (!fin.ok) save =  DbResult<void>::Err(fin.error);
                    else{
                        r.output_savestate_id = plan.value;
                        r.set_section(ini);
                    }
                }
            }
        }

        std::filesystem::remove(r.savestate_path);
        if (!save.ok) return save;

        auto ev2 = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", ini.to_string_sorted());
        if (!ev2.ok) return DbResult<void>::Err(ev2.error);

        auto st = simcore::db::JobsRepo::SetState(job_id, "SUCCEEDED");
        if (!st.ok) return DbResult<void>::Err(st.error);
    }
    else {
        if (!r.savestate_path.empty() && std::filesystem::exists(r.savestate_path)) std::filesystem::remove(r.savestate_path);
        auto st = simcore::db::JobsRepo::SetState(job_id, "FAILED");
        if (!st.ok) return DbResult<void>::Err(st.error);
    }

    return DbResult<void>::Ok();
}

DbResult<std::string> BattleSingleTurnRunDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    ExplorerRunDBCodec c;
    return c.decode_progress_from_db(job_id, job_set_id);
}

DbResult<std::string> BattleSingleTurnRunDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    ExplorerRunDBCodec c;
    return c.decode_results_from_db(job_id, job_set_id);
}

DbResult<std::optional<int64_t>> BattleSingleTurnRunDBCodec::get_required_savestate_id(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::optional<int64_t>>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<std::optional<int64_t>>::Ok(std::nullopt);
    STJob jb = STJob::from_section(IniDoc::parse(*jr.value.vm_kv));
    return DbResult<std::optional<int64_t>>::Ok(jb.savestate_id > 0 ? std::optional<int64_t>(jb.savestate_id) : std::nullopt);
}

DbResult<simcore::PSInit> BattleSingleTurnRunDBCodec::build_psinit_for_job(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSInit>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<simcore::PSInit>::Err({ DbErrorKind::NotFound, 0, "vm_kv missing" });
    STJob jb = STJob::from_section(IniDoc::parse(*jr.value.vm_kv));

    auto ss = simcore::db::SavestateRepo::Get(jb.savestate_id);
    if (!ss.ok) return DbResult<simcore::PSInit>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<simcore::PSInit>::Err({ DbErrorKind::NotFound, 0, "savestate not found" });

    auto temp = simcore::db::ObjectStore::MaterializeToTemp(ss.value->object_ref_id);
    if (!temp.ok) return DbResult<simcore::PSInit>::Err(temp.error);

    simcore::PSInit init{};
    init.savestate_path = temp.value;
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_Battle;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> BattleSingleTurnRunDBCodec::build_results_ini_from_prresult(int64_t, const simcore::PRResult& r) {
    STRes out{};
    out.w_err = r.ps.w_err;
    if (out.w_err == 0) r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, out.dw_err);
    r.ps.ctx.get(simcore::keys::core::VI_FIRST, out.vi_start);
    r.ps.ctx.get(simcore::keys::core::VI_LAST, out.vi_end);
    r.ps.ctx.get(simcore::keys::seed::RNG_SEED, out.rng_seed);
    r.ps.ctx.get(simcore::keys::battle::BATTLE_OUTCOME, out.battle_outcome);
    r.ps.ctx.get(simcore::keys::battle::PLAN_MATERIALIZE_ERR, out.plan_materialize_err);

    uint32_t before = 0, cur = 0;
    r.ps.ctx.get(simcore::keys::battle::FAKE_ATTACK_USED_BEFORE, before);
    r.ps.ctx.get(simcore::keys::battle::FAKE_ATTACK_COUNT_THIS_TURN, cur);
    out.fake_attacks_used = before + cur;
    r.ps.ctx.get(simcore::keys::core::PRED_PASSED, out.pred_passed);
    r.ps.ctx.get(simcore::keys::core::PRED_TOTAL, out.pred_total);
    r.ps.ctx.get(simcore::keys::core::PRED_ABORT_RUN, out.pred_abort_run);
    r.ps.ctx.get(simcore::keys::core::LAST_SAVESTATE_PATH, out.savestate_path);

    IniDoc ini;
    out.set_section(ini);
    return DbResult<std::string>::Ok(ini.to_string_sorted());
}

DbResult<std::string> BattleSingleTurnRunDBCodec::build_artifact_ini_from_db(int64_t job_id) {
    ArtifactIniBuilder artifacts{};

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::string>::Err(jr.error);

    if (!jr.value.vm_kv) return DbResult<std::string>::Err(jr.error);

    IniKV kv = IniDoc::parse(*jr.value.vm_kv).section_kv(STJob::SECTION_NAME);
    const uint64_t savestate_id = kv.get_i64("savestate_id", 0);

    auto ss = SavestateRepo::Get(savestate_id);
    if (!ss.ok) return DbResult<std::string>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
        "No Savestate found/saved..." });

    artifacts.add_artifact("Savestate", ss.value.value().object_ref_id);

    return DbResult<std::string>::Ok(artifacts.to_string());
}

DbResult<void> BattleSingleTurnRunDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {
    IniDoc ini = IniDoc::parse(action_args_ini);
    BRBp bp = BRBp::from_section(ini);
    STWave wave = STWave::from_section(ini);

    auto results = simcore::db::JobEventsRepo::ListByJobSetAndKind(ctx.prev_job_set_id, "RESULTS");
    if (!results.ok) return DbResult<void>::Err(results.error);

    std::unordered_map<std::string, Survivor> best;
    std::unordered_set<int64_t> winner_jobs;
    std::vector<Survivor> all;

    for (auto& e : results.value) {
        if (!e.payload.has_value()) continue;
        IniDoc rdoc = IniDoc::parse(*e.payload);
        if (!rdoc.has_section(STRes::SECTION_NAME)) continue;
        STRes r = STRes::from_section(rdoc);
        if (r.output_savestate_id <= 0) continue;
        if (r.battle_outcome != (uint32_t)simcore::battle::Outcome::ReachedNextTurn) continue;

        auto jr = simcore::db::JobsRepo::Get(e.job_id);
        if (!jr.ok || !jr.value.vm_kv.has_value()) continue;
        STJob jb = STJob::from_section(IniDoc::parse(*jr.value.vm_kv));

        Survivor s{};
        s.job_id = e.job_id;
        s.savestate_id = r.output_savestate_id;
        s.delta_seed_id = jb.delta_seed_id;
        s.fake_used = r.fake_attacks_used;
        s.rng_seed = r.rng_seed;
        s.action_key = jb.action_key;
        all.push_back(s);

        const std::string key = std::to_string(s.rng_seed);
        auto it = best.find(key);
        if (it == best.end() || s.fake_used < it->second.fake_used || (s.fake_used == it->second.fake_used && s.job_id < it->second.job_id)) {
            best[key] = s;
        }
    }

    for (auto& kv : best) winner_jobs.insert(kv.second.job_id);

    for (auto& s : all) {
        if (winner_jobs.count(s.job_id)) {
            (void)simcore::db::JobsRepo::SetState(s.job_id, "SUCCEEDED_WINNER");
        } else {
            (void)simcore::db::JobsRepo::SetState(s.job_id, "SUCCEEDED_DUPLICATE");
            (void)simcore::db::SavestateRepo::Delete(s.savestate_id);
        }
    }

    if (!bp.auto_wave_trigger_enable) return DbResult<void>::Ok();
    if (best.empty()) return DbResult<void>::Ok();

    for (auto& w : winner_jobs) {
        enqueue_next_wave_from_job(w, bp.auto_wave_trigger_enable, std::nullopt);
    }

    return DbResult<void>::Ok();
}


DbResult<int64_t> BattleSingleTurnRunDBCodec::enqueue_next_wave_from_job(int64_t source_job_id, bool auto_wave_trigger_enable, std::optional<uint32_t> max_fake_attacks_override) {
    auto jr = simcore::db::JobsRepo::Get(source_job_id);
    if (!jr.ok) return DbResult<int64_t>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "vm_kv missing" });

    IniDoc job_ini = IniDoc::parse(*jr.value.vm_kv);
    BRBp bp = BRBp::from_section(job_ini);
    STJob jb = STJob::from_section(job_ini);

    auto rr = simcore::db::JobEventsRepo::GetLatestPayload(source_job_id, "RESULTS");
    if (!rr.ok || !rr.value.has_value()) return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "results payload missing" });
    IniDoc rdoc = IniDoc::parse(*rr.value);
    STRes r = STRes::from_section(rdoc);

    if (max_fake_attacks_override.has_value()) {
        if (*max_fake_attacks_override < r.fake_attacks_used) {
            return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "max_fake_attacks_override must be >= already used fake attacks" });
        }
        bp.max_fake_attacks = *max_fake_attacks_override;
    }

    if (r.output_savestate_id <= 0) return DbResult<int64_t>::Err({ DbErrorKind::InvalidState, 0, "source job has no continuation savestate" });
    if (r.battle_outcome != (uint32_t)simcore::battle::Outcome::ReachedNextTurn) return DbResult<int64_t>::Err({ DbErrorKind::InvalidState, 0, "source job did not reach next turn" });

    auto links = simcore::db::ExplorerSettingsPlanLinkRepo::ListBySettings(bp.settings_id);
    if (!links.ok) return DbResult<int64_t>::Err(links.error);

    STWave next{ .cur_turn = jb.turn_index + 1 };
    bool has_next_turn = false;
    for (auto& pl : links.value) {
        auto turns = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(pl.plan_id);
        if (turns.ok && next.cur_turn >= 1 && next.cur_turn <= turns.value.size()) { has_next_turn = true; break; }
    }
    if (!has_next_turn) return DbResult<int64_t>::Err({ DbErrorKind::InvalidState, 0, "no next turn configured for selected run" });

    auto root = resolve_root_group_id_for_jobset(jr.value.job_set_id);
    if (!root.ok) return DbResult<int64_t>::Err(root.error);


    std::string desc = std::format("Wave {}: {}({})", next.cur_turn, jb.delta_seed_id, r.fake_attacks_used);
    auto js = simcore::db::JobSetsRepo::CreateChild(jr.value.job_set_id, desc, kPK, std::nullopt, std::nullopt, std::nullopt, "", std::nullopt);
    if (!js.ok) return DbResult<int64_t>::Err(js.error);

    auto sm = set_wave_meta_for_jobset(js.value, root.value, next.cur_turn, bp);
    if (!sm.ok) return DbResult<int64_t>::Err(sm.error);

    IniDoc t_ini{};
    bp.set_section(t_ini);
    next.set_section(t_ini);

    int64_t enqueued = 0;
    std::unordered_set<std::string> seenIt{};

    for (auto& pl : links.value) {
        auto turns = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(pl.plan_id);
        if (!turns.ok) continue;
        if (next.cur_turn < 1 || next.cur_turn > turns.value.size()) continue;

        auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(pl.plan_id, turns.value[next.cur_turn - 1].turn_index);
        if (!actorsR.ok) return DbResult<int64_t>::Err(actorsR.error);

        std::stringstream ss{};
        for (auto& a : actorsR.value) {
            ss << std::format(":{}", a.atom_id);
        }
        std::string turn_plan = ss.str();

        if (seenIt.contains(turn_plan)) continue;
        seenIt.insert(turn_plan);

        STJob nj{};
        nj.plan_id = pl.plan_id;
        nj.delta_seed_id = jb.delta_seed_id;
        nj.savestate_id = r.output_savestate_id;
        nj.turn_index = next.cur_turn;
        nj.fake_attacks_used_before = r.fake_attacks_used;
        nj.action_key = action_key_for_plan_turn(pl.plan_id, next.cur_turn - 1);

        auto run = simcore::db::ExplorerRunRepo::IdempotentCreate(bp.settings_id, pl.plan_id, (jb.delta_seed_id > 0) ? jb.delta_seed_id : 0);
        if (!run.ok) continue;

        if (r.fake_attacks_used > bp.max_fake_attacks) continue;
        const uint32_t remaining = bp.max_fake_attacks - r.fake_attacks_used;
        for (uint32_t fake = 0; fake <= remaining; ++fake) {
            nj.fake_attacks_this_turn = fake;
            const std::string vm = nj.append_section(t_ini).to_string_sorted();
            const std::string fp = hash::sha256(vm.data(), vm.size());
            auto cj = simcore::db::JobsRepo::CreateOrGetByFingerprint(js.value, kPK, kPV, run.value, fp, bp.priority, vm, nj.savestate_id);
            if (!cj.ok) continue;
            (void)simcore::db::JobEventsRepo::Append(cj.value, "ENQUEUED");
            ++enqueued;
        }
    }

    if (enqueued <= 0) return DbResult<int64_t>::Err({ DbErrorKind::InvalidState, 0, "no jobs enqueued for next wave" });

    (void)simcore::db::JobSetsRepo::SetExpectedTotal(js.value, enqueued);

    IniDoc tr_ini{};
    bp.set_section(tr_ini);
    next.set_section(tr_ini);
    IniKV cond;
    cond.add("type", "ALL_FINISHED");
    auto tr = simcore::db::TriggersRepo::AddForJobSet(js.value, kPK, cond.to_string_sorted(), tr_ini.to_string_sorted());
    if (!tr.ok) return DbResult<int64_t>::Err(tr.error);

    return DbResult<int64_t>::Ok(js.value);
}
