#include "BattleRunnerDBSettingsWriter.h"
#include "../../../Utils/Hash.h"
#include "../../../DB/ExplorerSettingsRepo.h"
#include "../../../DB/ExplorerSettingsPlanLinkRepo.h"
#include "../../../DB/ExplorerSettingsPredicateRepo.h"
#include "../../../DB/BattlePlanRepo.h"
#include "../../../DB/BattlePlanTurnRepo.h"
#include "../../../DB/BattlePlanAtomRepo.h"
#include "../../../DB/PredicateSpecRepo.h"
#include "../../../Core/Input/SoaBattle/ActionPlanSerializer.h"
#include "../../../Phases/BattleExplorer.h"

namespace simcore::phases {

    using soa::battle::actions::BattleAction;
    using soa::battle::actions::ActionPlan;
    using soa::battle::actions::TurnPlan;
    using soa::battle::actions::BattlePath;
    using simcore::db::DbResult;
    using simcore::db::BattlePlanTurnRepo;
    using simcore::db::TurnActorBindingByPlan;
    using simcore::db::BattlePlanAtomRepo;

    static inline std::string hash_joined(const std::vector<std::string>& v) {
        std::string cat;
        for (auto& s : v) { cat.append(s); cat.push_back('\n'); }
        return hash::sha256(cat.data(), (uint32_t)cat.size());
    }

    static inline int32_t count_fake_in_turn(const TurnPlan& t) {
        int32_t c = 0;
        for (auto& ap : t.spec) if (ap.macro == BattleAction::FakeAttack) ++c;
        return c;
    }

    static inline DbResult<void> persist_plan_atoms_and_turns(
        int64_t plan_id,
        const BattlePath& bp) {

        for (int32_t turn_idx = 0; turn_idx < (int32_t)bp.size(); ++turn_idx) {
            const TurnPlan& tp = bp[turn_idx];
            std::vector<TurnActorBindingByPlan> actors;
            actors.reserve(tp.spec.size());
            for (const ActionPlan& ap : tp.spec) {
                const int32_t action_type = (int32_t)ap.macro;
                const int32_t actor_slot = (int32_t)ap.actor_slot;
                const int32_t item_id = (int32_t)ap.params.item_id;
                const int32_t target_slot = (int32_t)ap.params.target_slot;
                auto atom = BattlePlanAtomRepo::Ensure(action_type, actor_slot, item_id, target_slot);
                if (!atom.ok) return DbResult<void>::Err(atom.error);
                actors.emplace_back( actor_slot, atom.value );
            }
            const int32_t fake_cnt = tp.fake_attack_count;
            auto rt = BattlePlanTurnRepo::ReplaceTurnByPlan(plan_id, turn_idx, fake_cnt, std::move(actors));
            if (!rt.ok) return DbResult<void>::Err(rt.error);
        }
        return DbResult<void>::Ok();
    }

    db::DbResult<int64_t> BRSettingsWriter::EnsureSettingsWithPredicatesAndPlans(
        int64_t savestate_id,
        const soa::battle::ctx::BattleContext& bc,
        const AuthoringPayload& a) {

        using namespace simcore::db;
        using namespace simcore::battleexplorer;
        using soa::battle::actions::get_battle_path_summary;
        using soa::battle::actions::fingerprint_battle_plan;

        BattleExplorer ex("");
        auto paths = ex.enumerate_paths(bc, a.ui); // deterministic expansion

        std::vector<std::string> plan_fps;
        std::vector<int64_t>     plan_ids;
        plan_fps.reserve(paths.size());
        plan_ids.reserve(paths.size());

        for (const BattlePath& bp : paths) {
            const std::string name = get_battle_path_summary(bp);
            const std::string fp = fingerprint_battle_plan(bp);
            const int32_t num_turns = (int32_t)bp.size();

            auto ep = BattlePlanRepo::Ensure(name, fp, num_turns);
            if (!ep.ok) return DbResult<int64_t>::Err(ep.error);
            const int64_t plan_id = ep.value;

            auto pt = persist_plan_atoms_and_turns(plan_id, bp);
            if (!pt.ok) return DbResult<int64_t>::Err(pt.error);

            plan_fps.emplace_back(fp);
            plan_ids.emplace_back(plan_id);
        }

        std::vector<int64_t> pred_ids;
        std::vector<std::string> pred_fps;
        pred_ids.reserve(a.predicates.size());
        pred_fps.reserve(a.predicates.size());

        for (const auto& p : a.predicates) {
            auto ep = PredicateSpecRepo::EnsureByFingerprint(p);
            if (!ep.ok) return DbResult<int64_t>::Err(ep.error);
            pred_ids.emplace_back(ep.value);
            pred_fps.emplace_back(p.fingerprint);
        }

        const std::string plan_vec_fp = hash_joined(plan_fps);
        const std::string pred_vec_fp = hash_joined(pred_fps);
        const std::string settings_fp = hash::sha256((pred_vec_fp + "|" + plan_vec_fp).c_str(), (uint32_t)(pred_vec_fp.size() + 1 + plan_vec_fp.size()));

        auto es = ExplorerSettingsRepo::EnsureByFingerprint(a.settings_name, a.settings_description, settings_fp);
        if (!es.ok) return DbResult<int64_t>::Err(es.error);
        const int64_t settings_id = es.value;

        // Link predicates (ordinal order preserved)
        std::vector<SettingsPredicateRow> pred_links;
        pred_links.reserve(pred_ids.size());
        for (int i = 0; i < (int)pred_ids.size(); ++i) pred_links.push_back({ settings_id, i, pred_ids[(size_t)i] });
        auto lp = ExplorerSettingsPredicateRepo::ReplaceAll(settings_id, pred_links);
        if (!lp.ok) return DbResult<int64_t>::Err(lp.error);

        // Link plans (ordinal order preserved)
        auto lpl = ExplorerSettingsPlanLinkRepo::ReplaceAll(settings_id, plan_ids);
        if (!lpl.ok) return DbResult<int64_t>::Err(lpl.error);

        // Optional: associate a seed probe later; for now settings are independent
        return DbResult<int64_t>::Ok(settings_id);
    }

} // namespace simcore::phases
