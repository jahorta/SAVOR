#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "../Core/Input/SoaBattle/ActionTypes.h"
#include "../Core/Memory/Soa/Battle/BattleContext.h"
#include "Programs/BattleRunner/BattleOutcome.h"
#include "Programs/BattleRunner/BattleRunnerPayload.h"
// Forward-declare your runner and predicate types to avoid heavy includes.
namespace simcore { class ParallelPhaseScriptRunner; }

namespace simcore::battleexplorer {

    enum class TargetBindingKind { SingleEnemy, MultipleEnemies, AnyEnemy, SameAsOtherPC };

    struct TargetBinding {
        TargetBindingKind kind = TargetBindingKind::SingleEnemy;
        uint32_t mask = 0;        // for SingleEnemy / MultipleEnemies
        uint8_t  var_id = 0xFF;   // for SameAsOtherPC (bind to another actor's decision)
    };

    struct UI_Action {
        uint8_t actor_slot = 0;
        soa::battle::actions::BattleAction macro{};           // Attack/Defend/Focus/FakeAttack...
        soa::battle::actions::ActionParameters params{};      // keep other params (rng_tickle/guards...)
        TargetBinding    target;      // NEW: symbolic target
    };

    using UI_Turn = std::vector<UI_Action>;

    struct UI_Config {
        std::vector<UI_Turn>       turns;            // N == max turns
        int                        fakeattack_budget = 0; // B >= 0
        std::vector<pred::Spec>    predicates;      // enabled predicates (+ params)
        std::vector<GCInputFrame>  initial_frames;
        int                        max_retry_count = 0;
    };

    struct JobResult {
        battle::Outcome outcome;
        uint64_t job_id;
        phase::battle::runner::EncodeSpec spec;
        PRResult pr;
    };

    struct RunResultSummary {
        uint64_t jobs_total = 0;
        uint64_t jobs_success = 0;
        std::vector<JobResult> fails;
        std::vector<JobResult> successes;
    };

    class BattleExplorer {
    public:
        BattleExplorer(std::string savestate_path);

        // 1) Run the BattleContext VM once and decode the context.
        soa::battle::ctx::BattleContext gather_context(ParallelPhaseScriptRunner& runner);

        // 2) Build terminal, non-branching BattlePaths from UI_Config (+ FakeAttack expansion).
        std::vector<soa::battle::actions::BattlePath> enumerate_paths(const soa::battle::ctx::BattleContext& bc,
            const UI_Config& ui) const;

        // 3) Encode and dispatch each BattlePath as a separate job; collate successes.
        RunResultSummary run_paths(const UI_Config& ui,
            const std::vector<soa::battle::actions::BattlePath>& paths,
            ParallelPhaseScriptRunner& runner);

        // Estimators for the CLI footer
        uint64_t estimate_paths_no_fake(const UI_Config& ui, const soa::battle::ctx::BattleContext& ctx) const;
        uint64_t estimate_paths_with_fake(const UI_Config& ui, const uint64_t paths_wo_fake) const; // X * C(B+N, N)

    private:
        bool validate_action_against_context(const soa::battle::ctx::BattleContext& bc,
            const soa::battle::actions::ActionPlan& ap) const;

        static std::vector<std::vector<uint32_t>> enumerate_fakeattack_vectors(std::size_t N, uint32_t B);

        std::string m_savestate_path{""};
    };

} // namespace simcore::battleexplorer
