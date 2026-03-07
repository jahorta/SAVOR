#include "DBBattleRunnerCoordinator.h"
#include "../../DB/ProgramDB/ExplorerRunDBCodec.h"
#include "../../DB/ExplorerSettingsRepo.h"
#include "../../DB/ExplorerRunRepo.h"
#include "../../DB/ExplorerSettingsPredicateRepo.h"
#include "../../DB/BattlePlanRepo.h"
#include "../../DB/BattlePlanTurnRepo.h"
#include "../../DB/BattlePlanAtomRepo.h"
#include "../../DB/AddressProgramRepo.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Core/Memory/Soa/SoaAddrProgram.h"


using simcore::db::DbResult;
using simcore::db::PredicateSpecRow;
using simcore::db::SettingsPredicateRow;

namespace simcore {
    namespace phase {

        static inline std::string roll_settings_fp(const std::vector<std::string>& pred_fps,
            const std::vector<std::string>& plan_fps) {
            std::string s;
            s.reserve(32 + pred_fps.size() * 33 + plan_fps.size() * 33);
            s.append("BR|v1|preds|N=");
            s.append(std::to_string(pred_fps.size())); s.push_back('|');
            for (auto& fp : pred_fps) { s.append(fp); s.push_back('|'); }
            s.append("plans|N=");
            s.append(std::to_string(plan_fps.size())); s.push_back('|');
            for (auto& fp : plan_fps) { s.append(fp); s.push_back('|'); }
            return hash::sha256(s.data(), s.size());
        }

        // Build DB rows for a raw pred::Spec (program IDs left null; catalogs are for dedupe only)
        static inline PredicateSpecRow to_row(const simcore::pred::Spec& s) {
            PredicateSpecRow r{};
            r.spec_version = pred::SPEC_VERSION;
            r.required_bp = s.required_bp;
            r.kind = (int32_t)s.kind;
            r.width = s.width ? s.width : 4;
            r.cmp_op = (int32_t)s.cmp;
            r.flags = static_cast<int32_t>(s.flags);
            r.lhs_addr = static_cast<int32_t>(s.lhs_addr);
            r.lhs_key = std::optional<int>((int)s.lhs_key.value());
            r.rhs_value = static_cast<int64_t>(s.rhs_value);
            r.rhs_key = std::optional<int>((int)s.rhs_key.value());
            r.turn_mask = s.turn_mask ? s.turn_mask : 0xFFFFFFFFu;
            r.lhs_prog_id = std::nullopt;
            r.rhs_prog_id = std::nullopt;
            r.description = s.desc;
            r.fingerprint = "";
            return r;
        }

        // === StagePlans: now sourced from BattlePath ===
        DbResult<std::vector<int64_t>>
            BattleRunnerCoordinator::StagePlans(int64_t settings_id, const std::vector<BRPlanSpec>& plans)
        {
            using namespace simcore::db;
            using namespace soa::battle::actions;

            std::vector<int64_t> out; out.reserve(plans.size());
            for (const auto& p : plans) {
                const std::string fp = soa::battle::actions::fingerprint_battle_plan(p.path);

                auto ep = BattlePlanRepo::Ensure(p.name, fp, (int32_t)p.path.size());
                if (!ep.ok) return DbResult<std::vector<int64_t>>::Err(ep.error);
                const int64_t plan_id = ep.value;
                out.push_back(plan_id);

                // materialize per-turn rows
                for (int32_t t = 0; t < (int32_t)p.path.size(); ++t) {
                    const auto& tp = p.path[t];

                    // ensure atoms for each actor action (ordered as given)
                    std::vector<TurnActorBindingByPlan> actors; actors.reserve(tp.spec.size());
                    for (int32_t ai = 0; ai < (int32_t)tp.spec.size(); ++ai) {
                        const auto& ap = tp.spec[ai];
                        const int32_t action_type = static_cast<int32_t>(ap.macro);
                        const int32_t actor_slot = static_cast<int32_t>(ap.actor_slot);
                        const int32_t param_item = static_cast<int32_t>(ap.params.item_id);
                        const int32_t target_slot = static_cast<int32_t>(ap.params.target_slot);

                        auto a = BattlePlanAtomRepo::Ensure(action_type, actor_slot, param_item, target_slot);
                        if (!a.ok) return DbResult<std::vector<int64_t>>::Err(a.error);
                        actors.push_back(TurnActorBindingByPlan{ ai, a.value });
                    }

                    auto rt = BattlePlanTurnRepo::ReplaceTurnByPlan(plan_id, t, (int32_t)tp.fake_attack_count, std::move(actors));
                    if (!rt.ok) return DbResult<std::vector<int64_t>>::Err(rt.error);
                }
            }

            // record ordered plan vector for this settings
            auto rl = ExplorerSettingsPlanLinkRepo::ReplaceAll(settings_id, out);
            if (!rl.ok) return DbResult<std::vector<int64_t>>::Err(rl.error);

            return DbResult<std::vector<int64_t>>::Ok(std::move(out));
        }

        // === EnsureSettingsWithPredicatesAndPlans: raw Spec + BattlePath ===
        DbResult<int64_t>
            BattleRunnerCoordinator::EnsureSettingsWithPredicatesAndPlans(
                const std::string& settings_name,
                const std::string& settings_desc,
                const std::vector<simcore::pred::Spec>& predicates,
                const std::vector<BRPlanSpec>& plans)
        {
            using namespace simcore::db;
            using simcore::pred::fingerprint;
            using soa::battle::actions::fingerprint_battle_plan;

            // 1) predicates -> fps + ids
            std::vector<std::string> pred_fps; pred_fps.reserve(predicates.size());
            std::vector<SettingsPredicateRow> pred_links; pred_links.reserve(predicates.size());
            for (size_t i = 0; i < predicates.size(); ++i) {
                const auto& ps = predicates[i];
                const std::string fp = simcore::pred::fingerprint(ps);
                pred_fps.push_back(fp);

                // Build the base DB row from the raw Spec (width/turn-mask defaults already normalized there)
                PredicateSpecRow row = to_row(ps);
                row.fingerprint = fp;

                // LHS program
                if ((ps.flags & static_cast<uint32_t>(simcore::pred::PredFlag::LhsIsProg)) && !ps.lhs_prog.empty()) {
                    auto lhs_id = simcore::db::AddressProgramRepo::Ensure(
                        /*program_version*/ addrprog::PROG_VERSION,
                        ps.lhs_prog,
                        /*derived_buffer_version*/ std::nullopt,
                        /*derived_buffer_schema_hash*/ std::nullopt,
                        /*soa_structs_hash*/ std::nullopt,
                        /*description*/ ps.lhs_prog_desc);
                    if (!lhs_id.ok) return DbResult<int64_t>::Err(lhs_id.error);
                    row.lhs_prog_id = lhs_id.value;
                }

                // RHS program
                if ((ps.flags & static_cast<uint32_t>(simcore::pred::PredFlag::RhsIsProg)) && !ps.rhs_prog.empty()) {
                    auto rhs_id = simcore::db::AddressProgramRepo::Ensure(
                        /*program_version*/ addrprog::PROG_VERSION,
                        ps.rhs_prog,
                        /*derived_buffer_version*/ std::nullopt,
                        /*derived_buffer_schema_hash*/ std::nullopt,
                        /*soa_structs_hash*/ std::nullopt,
                        /*description*/ ps.rhs_prog_desc);
                    if (!rhs_id.ok) return DbResult<int64_t>::Err(rhs_id.error);
                    row.rhs_prog_id = rhs_id.value;
                }

                // Ensure the predicate spec by fingerprint (now carrying program IDs)
                auto pid = simcore::db::PredicateSpecRepo::EnsureByFingerprint(row);
                if (!pid.ok) return DbResult<int64_t>::Err(pid.error);

                pred_links.push_back(SettingsPredicateRow{ 0, static_cast<int32_t>(i), pid.value });
            }

            // 2) plans -> fps (computed from BattlePath)
            std::vector<std::string> plan_fps; plan_fps.reserve(plans.size());
            for (const auto& p : plans) plan_fps.push_back(soa::battle::actions::fingerprint_battle_plan(p.path));

            // 3) ensure settings row (dedupe by settings_fingerprint)
            const std::string settings_fp = roll_settings_fp(pred_fps, plan_fps);
            auto es = ExplorerSettingsRepo::EnsureByFingerprint(settings_name, settings_desc, settings_fp);
            if (!es.ok) return DbResult<int64_t>::Err(es.error);
            const int64_t settings_id = es.value;

            // 4) link predicates (ordered)
            for (auto& l : pred_links) l.settings_id = settings_id;
            {
                auto r = ExplorerSettingsPredicateRepo::ReplaceAll(settings_id, pred_links);
                if (!r.ok) return DbResult<int64_t>::Err(r.error);
            }

            // 5) stage plans from raw BattlePath (persist + link ordered)
            auto staged = StagePlans(settings_id, plans);
            if (!staged.ok) return DbResult<int64_t>::Err(staged.error);

            return DbResult<int64_t>::Ok(settings_id);
        }

        DbResult<int64_t> BattleRunnerCoordinator::CreateGroupAndQueueRuns(
            int64_t settings_id,
            int64_t seed_probe_id,
            const BROptions& opt,
            std::optional<std::string> group_name,
            std::optional<std::string> group_desc)
        {
            using namespace simcore::db;

            // Ensure settings points at this probe (idempotent)
            {
                auto ss = ExplorerSettingsRepo::SetSeedProbeId(settings_id, seed_probe_id);
                if (!ss.ok) return DbResult<int64_t>::Err(ss.error);
            }

            // Load ordered plan ids for settings
            std::vector<int64_t> plan_ids;
            {
                auto links = ExplorerSettingsPlanLinkRepo::ListBySettings(settings_id);
                if (!links.ok) return DbResult<int64_t>::Err(links.error);
                plan_ids.reserve(links.value.size());
                for (auto& l : links.value) plan_ids.push_back(l.plan_id);
            }

            // Load unique delta-seed inputs for probe
            std::vector<DeltaSeedRow> uniques;
            {
                auto lu = DeltaSeedRepo::ListUniqueForProbe(seed_probe_id);
                if (!lu.ok) return DbResult<int64_t>::Err(lu.error);
                uniques = std::move(lu.value);
            }

            // 2) Create a job set bound to this run with expected_total=1
            auto js = JobSetsRepo::Create(
                /*purpose*/"BattleRunner",
                /*program_kind*/PK_BattleTurnRunner,
                /*created_by*/std::nullopt,
                /*domain_ref_kind*/std::nullopt,
                /*domain_ref_id*/std::nullopt,
                /*meta_text*/std::nullopt,
                /*expected_total*/std::optional<int64_t>(1));
            if (!js.ok) return DbResult<int64_t>::Err(js.error);
            const int64_t job_set_id = js.value;

            ExplorerRunDBCodec codec;

            // For each (plan x delta), create one run and one job in a one-item job set.
            for (int64_t plan_id : plan_ids) {
                for (const auto& d : uniques) {
                    // 1) Create run (planned) and attach to group
                    auto run = ExplorerRunRepo::IdempotentCreate(settings_id, plan_id, d.id);
                    if (!run.ok) return DbResult<int64_t>::Err(run.error);
                    const int64_t run_id = run.value;

                    // 3) Build blueprint INI and enqueue exactly one job in the set
                    IniKV kv;
                    kv.add("plan_id", std::to_string(plan_id));
                    kv.add("delta_seed_id", std::to_string(d.id));
                    kv.add("run_ms", std::to_string(opt.run_ms));
                    kv.add("vi_stall_ms", std::to_string(opt.vi_stall_ms));
                    if (opt.priority.has_value())     kv.add("priority", std::to_string(opt.priority.value()));
                    if (opt.max_attempts.has_value()) kv.add("max_attempts", std::to_string(opt.max_attempts.value()));

                    auto enq = codec.encode_job_into_db(job_set_id, kv.to_string_sorted());
                    if (!enq.ok) return DbResult<int64_t>::Err(enq.error);
                }
            }

            return DbResult<int64_t>::Ok(0);
        }

        DbResult<std::string> BattleRunnerCoordinator::PollProgress_JobSet(int64_t job_set_id)
        {
            ExplorerRunDBCodec codec;
            return codec.decode_progress_from_db(std::nullopt, job_set_id);
        }

        DbResult<std::string> BattleRunnerCoordinator::RetrieveResults_JobSet(int64_t job_set_id)
        {
            ExplorerRunDBCodec codec;
            return codec.decode_results_from_db(std::nullopt, job_set_id);
        }

    }
} // namespace
