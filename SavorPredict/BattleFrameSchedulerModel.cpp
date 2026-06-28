#include "BattleFrameSchedulerModel.h"

#include "EffectRngModel.h"
#include "RngCore.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

constexpr int kDirectAttackFrames = 4;
constexpr int kFallbackAttackFrames = 2;
constexpr int kPassiveTargetFrames = 3;
constexpr int kPassiveSameSideFrames = 2;
constexpr int kCleanupFrames = 1;
constexpr int kActionViewFrames = 1;
constexpr int kMechanicalAttackFrames = 1;
constexpr int kEffectChunkFrames = 2;
constexpr int kPassiveClashFrames = 1;

BattleFrameVec3 lerp(const BattleFrameVec3& from, const BattleFrameVec3& to, float t) {
    return BattleFrameVec3{
        .x = from.x + (to.x - from.x) * t,
        .y = from.y + (to.y - from.y) * t,
        .z = from.z + (to.z - from.z) * t,
    };
}

int signum(int value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

MovementGridPosition adjacent_grid_toward(
    const BattleFrameCombatantState& actor,
    const BattleFrameCombatantState& target) {
    const int dx = target.grid_position.grid_x - actor.grid_position.grid_x;
    const int dz = target.grid_position.grid_z - actor.grid_position.grid_z;
    MovementGridPosition destination = actor.grid_position;
    if (std::abs(dx) >= std::abs(dz) && dx != 0) {
        destination.grid_x = target.grid_position.grid_x - signum(dx);
        destination.grid_z = target.grid_position.grid_z;
    } else if (dz != 0) {
        destination.grid_x = target.grid_position.grid_x;
        destination.grid_z = target.grid_position.grid_z - signum(dz);
    }
    return destination;
}

BattleFrameWorkerKind active_worker_kind(MovementSelectedWorker worker, bool enemy_owned) {
    switch (worker) {
    case MovementSelectedWorker::PcDirectAttack_80086308:
        return BattleFrameWorkerKind::ActiveDirectAttack;
    case MovementSelectedWorker::PcFallbackAttack_80085ce0:
        return BattleFrameWorkerKind::ActiveFallbackAttack;
    case MovementSelectedWorker::EnemyDirectAttack_80087f6c:
        return BattleFrameWorkerKind::EnemyDirectAttack;
    case MovementSelectedWorker::EnemyFallbackAttack_80087844:
        return BattleFrameWorkerKind::EnemyFallbackAttack;
    case MovementSelectedWorker::None:
        return enemy_owned
            ? BattleFrameWorkerKind::EnemyFallbackAttack
            : BattleFrameWorkerKind::ActiveFallbackAttack;
    }
    return BattleFrameWorkerKind::None;
}

int worker_total_frames(BattleFrameWorkerKind kind) {
    switch (kind) {
    case BattleFrameWorkerKind::ActiveMovement:
        return kDirectAttackFrames;
    case BattleFrameWorkerKind::PassiveMovement:
        return kPassiveTargetFrames;
    case BattleFrameWorkerKind::ActionView:
        return kActionViewFrames;
    case BattleFrameWorkerKind::MechanicalAttack:
        return kMechanicalAttackFrames;
    case BattleFrameWorkerKind::EffectChunk:
        return kEffectChunkFrames;
    case BattleFrameWorkerKind::PassiveClashReaction:
        return kPassiveClashFrames;
    case BattleFrameWorkerKind::Cleanup:
        return kCleanupFrames;
    case BattleFrameWorkerKind::ActiveDirectAttack:
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return kDirectAttackFrames;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return kFallbackAttackFrames;
    case BattleFrameWorkerKind::PassiveTarget:
        return kPassiveTargetFrames;
    case BattleFrameWorkerKind::PassiveSameSide:
        return kPassiveSameSideFrames;
    case BattleFrameWorkerKind::CleanupStanding:
        return kCleanupFrames;
    case BattleFrameWorkerKind::None:
        return 0;
    }
    return 0;
}

std::int16_t action_mode_for_worker(const BattleFrameWorker& worker) {
    if (worker.complete) {
        return BattleFrameActionMode::Standing;
    }
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveMovement:
    case BattleFrameWorkerKind::ActiveDirectAttack:
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return worker.frame + 1 < worker.total_frames
            ? BattleFrameActionMode::ActiveApproach
            : BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::ActionView:
    case BattleFrameWorkerKind::MechanicalAttack:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::EffectChunk:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::PassiveClashReaction:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::PassiveMovement:
        return BattleFrameActionMode::PassiveTarget;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return BattleFrameActionMode::ActiveFallback;
    case BattleFrameWorkerKind::PassiveTarget:
        return BattleFrameActionMode::PassiveTarget;
    case BattleFrameWorkerKind::PassiveSameSide:
        return BattleFrameActionMode::PassiveSameSide;
    case BattleFrameWorkerKind::Cleanup:
    case BattleFrameWorkerKind::CleanupStanding:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::None:
        return BattleFrameActionMode::Standing;
    }
    return BattleFrameActionMode::Standing;
}

std::string callback_for_worker(const BattleFrameWorker& worker) {
    if (!worker.rng_label.empty()) {
        return worker.rng_label;
    }
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveMovement:
        return "ActiveMovement";
    case BattleFrameWorkerKind::PassiveMovement:
        return "PassiveMovement";
    case BattleFrameWorkerKind::ActionView:
        return "ActionView";
    case BattleFrameWorkerKind::MechanicalAttack:
        return "MechanicalAttack";
    case BattleFrameWorkerKind::EffectChunk:
        return "EffectChunk";
    case BattleFrameWorkerKind::PassiveClashReaction:
        return "PassiveClashReaction";
    case BattleFrameWorkerKind::Cleanup:
        return "Cleanup";
    case BattleFrameWorkerKind::ActiveDirectAttack:
        return "PcDirectAttack_80086308";
    case BattleFrameWorkerKind::ActiveFallbackAttack:
        return "PcFallbackAttack_80085ce0";
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return "EnemyDirectAttack_80087f6c";
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return "EnemyFallbackAttack_80087844";
    case BattleFrameWorkerKind::PassiveTarget:
        return "PassiveTargetParticipant";
    case BattleFrameWorkerKind::PassiveSameSide:
        return "PassiveSameSideParticipant";
    case BattleFrameWorkerKind::CleanupStanding:
        return "CleanupStanding";
    case BattleFrameWorkerKind::None:
        return "None";
    }
    return "None";
}

BattleFrameWorker make_worker(
    BattleFrameState& state,
    int slot,
    int target_slot,
    BattleFrameWorkerKind kind,
    MovementSelectedWorker selected_worker) {
    BattleFrameWorker worker;
    worker.slot = slot;
    worker.target_slot = target_slot;
    worker.kind = kind;
    worker.selected_worker = selected_worker;
    worker.total_frames = worker_total_frames(kind);

    auto* combatant = find_frame_combatant(state, slot);
    if (combatant == nullptr) {
        worker.complete = true;
        return worker;
    }
    worker.start_grid = combatant->grid_position;
    worker.destination_grid = combatant->grid_position;
    worker.start_position = combatant->pos_holder;
    worker.destination_position = combatant->pos_holder;

    const auto* target = find_frame_combatant(state, target_slot);
    if (target != nullptr
        && (kind == BattleFrameWorkerKind::ActiveDirectAttack
            || kind == BattleFrameWorkerKind::EnemyDirectAttack)) {
        worker.destination_grid = adjacent_grid_toward(*combatant, *target);
        worker.destination_position = first_battle_grid_to_raw_stage_position(
            worker.destination_grid,
            combatant->width,
            combatant->depth);
    }
    return worker;
}

BattleFrameWorker* find_worker(std::vector<BattleFrameWorker>& workers, int slot) {
    const auto it = std::find_if(
        workers.begin(),
        workers.end(),
        [slot](const BattleFrameWorker& worker) {
            return worker.slot == slot && !worker.complete;
        });
    return it == workers.end() ? nullptr : &*it;
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b) {
    return a.grid_x == b.grid_x && a.grid_z == b.grid_z;
}

bool same_vec(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool is_rng_worker(BattleFrameWorkerKind kind) {
    return kind == BattleFrameWorkerKind::ActionView
        || kind == BattleFrameWorkerKind::EffectChunk
        || kind == BattleFrameWorkerKind::PassiveClashReaction;
}

bool should_move_position(BattleFrameWorkerKind kind) {
    return kind == BattleFrameWorkerKind::ActiveMovement
        || kind == BattleFrameWorkerKind::PassiveMovement
        || kind == BattleFrameWorkerKind::ActiveDirectAttack
        || kind == BattleFrameWorkerKind::ActiveFallbackAttack
        || kind == BattleFrameWorkerKind::EnemyDirectAttack
        || kind == BattleFrameWorkerKind::EnemyFallbackAttack
        || kind == BattleFrameWorkerKind::PassiveTarget
        || kind == BattleFrameWorkerKind::PassiveSameSide
        || kind == BattleFrameWorkerKind::CleanupStanding
        || kind == BattleFrameWorkerKind::Cleanup;
}

void advance_without_rand_values(std::uint32_t& state, int draws) {
    for (int i = 0; i < draws; ++i) {
        state = advance_once(state);
    }
}

void apply_rng_worker(BattleFrameStepEvent& event, BattleFrameWorker& worker, std::uint32_t& rng_state) {
    if (worker.draws_consumed <= 0 || worker.frame != 0) {
        return;
    }

    event.rng_event = true;
    event.rng_label = worker.rng_label;
    event.draws_consumed = worker.draws_consumed;
    event.rng_seed_before = rng_state;
    event.effect_source_key = worker.effect_source_key;
    if (worker.draws_consumed == 1) {
        const auto draw = draw_rand15(rng_state);
        rng_state = draw.next_state;
        event.rand_value = draw.value;
    } else {
        advance_without_rand_values(rng_state, worker.draws_consumed);
    }
    event.rng_seed_after = rng_state;
}

BattleFrameStepEvent execute_worker_frame(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    std::uint32_t* rng_state) {
    auto* combatant = find_frame_combatant(runtime.state, worker.slot);
    BattleFrameStepEvent event;
    event.frame_index = runtime.state.frame_index;
    event.slot = worker.slot;
    event.target_slot = worker.target_slot;
    event.callback = callback_for_worker(worker);
    event.worker_kind = worker.kind;
    event.status = worker.event_status;
    event.rng_label = worker.rng_label;
    event.effect_source_key = worker.effect_source_key;
    event.detail = worker.detail;
    if (combatant == nullptr) {
        worker.complete = true;
        return event;
    }

    event.old_action_mode = combatant->combatant_action_mode;
    event.old_grid = combatant->grid_position;
    event.old_pos_holder = combatant->pos_holder;
    event.old_combatant_position = combatant->combatant_position;

    combatant->combatant_action_mode = action_mode_for_worker(worker);
    if (rng_state != nullptr && is_rng_worker(worker.kind)) {
        apply_rng_worker(event, worker, *rng_state);
    }

    if (should_move_position(worker.kind) && worker.total_frames > 0) {
        const float t = static_cast<float>(worker.frame + 1)
            / static_cast<float>(worker.total_frames);
        combatant->pos_holder = lerp(worker.start_position, worker.destination_position, t);
        if (worker.frame + 1 == worker.total_frames
            && !same_grid(combatant->grid_position, worker.destination_grid)) {
            commit_movement_grid_8008178c(runtime.state, worker.slot, worker.destination_grid);
            combatant = find_frame_combatant(runtime.state, worker.slot);
        }
        if (combatant != nullptr) {
            bridge_pos_holder_to_combatant_8001ab60(runtime.state, worker.slot);
        }
    }

    if (combatant != nullptr) {
        event.new_action_mode = combatant->combatant_action_mode;
        event.new_grid = combatant->grid_position;
        event.new_pos_holder = combatant->pos_holder;
        event.new_combatant_position = combatant->combatant_position;
        event.grid_changed = !same_grid(event.old_grid, event.new_grid);
        event.pos_holder_changed = !same_vec(event.old_pos_holder, event.new_pos_holder);
        event.combatant_position_changed =
            !same_vec(event.old_combatant_position, event.new_combatant_position);
        event.bridged_position = event.combatant_position_changed || event.pos_holder_changed;
    }

    ++worker.frame;
    if (worker.frame >= worker.total_frames) {
        worker.complete = true;
    }
    return event;
}

void append_cleanup_workers(BattleFrameRuntime& runtime) {
    for (const auto& thread : runtime.state.packed_thread_order) {
        const auto* combatant = find_frame_combatant(runtime.state, thread.slot);
        if (combatant == nullptr || !combatant->present || !combatant->alive) {
            continue;
        }
        runtime.workers.push_back(make_worker(
            runtime.state,
            thread.slot,
            -1,
            BattleFrameWorkerKind::CleanupStanding,
            MovementSelectedWorker::None));
    }
}

bool is_passive_reaction_source(BattleFrameWorkerKind kind) {
    return kind == BattleFrameWorkerKind::PassiveMovement
        || kind == BattleFrameWorkerKind::PassiveTarget
        || kind == BattleFrameWorkerKind::PassiveSameSide;
}

} // namespace

std::optional<BattleFrameRuntime> initialize_first_battle_frame_runtime(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots) {
    auto frame_state = initialize_first_battle_frame_state(enemy_event_id, slots);
    if (!frame_state.has_value()) {
        return std::nullopt;
    }

    BattleFrameRuntime runtime;
    runtime.initialized = true;
    runtime.state = std::move(*frame_state);
    runtime.warnings = runtime.state.warnings;
    return runtime;
}

void schedule_first_battle_action_workers(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input) {
    runtime.workers.clear();
    runtime.last_step_events.clear();
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }

    runtime.workers.push_back(make_worker(
        runtime.state,
        input.actor_slot,
        input.target_slot,
        active_worker_kind(input.selected_worker, input.enemy_owned),
        input.selected_worker));

    for (const auto& route : input.passive_routes) {
        if (route.route == PassiveMovementRouteKind::TargetParticipant) {
            runtime.workers.push_back(make_worker(
                runtime.state,
                route.slot,
                input.actor_slot,
                BattleFrameWorkerKind::PassiveTarget,
                route.selected_worker));
        } else if (route.route == PassiveMovementRouteKind::SameSideParticipant
            || route.route == PassiveMovementRouteKind::SpecialParticipant) {
            runtime.workers.push_back(make_worker(
                runtime.state,
                route.slot,
                input.target_slot,
                BattleFrameWorkerKind::PassiveSameSide,
                route.selected_worker));
        }
    }
}

void schedule_first_turn_actor_action(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input) {
    runtime.last_step_events.clear();
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }

    runtime.enable_passive_clash_reaction = true;
    runtime.workers.push_back(make_worker(
        runtime.state,
        input.actor_slot,
        input.target_slot,
        active_worker_kind(input.selected_worker, input.enemy_owned),
        input.selected_worker));

    for (const auto& route : input.passive_routes) {
        if (route.route == PassiveMovementRouteKind::TargetParticipant) {
            runtime.workers.push_back(make_worker(
                runtime.state,
                route.slot,
                input.actor_slot,
                BattleFrameWorkerKind::PassiveTarget,
                route.selected_worker));
        } else if (route.route == PassiveMovementRouteKind::SameSideParticipant
            || route.route == PassiveMovementRouteKind::SpecialParticipant) {
            runtime.workers.push_back(make_worker(
                runtime.state,
                route.slot,
                input.target_slot,
                BattleFrameWorkerKind::PassiveSameSide,
                route.selected_worker));
        }
    }
}

void schedule_first_turn_action_view_rng(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    std::string label,
    int draws_consumed,
    std::string detail,
    BattleFrameEventStatus status) {
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }

    BattleFrameWorker worker = make_worker(
        runtime.state,
        actor_slot,
        target_slot,
        BattleFrameWorkerKind::ActionView,
        MovementSelectedWorker::None);
    worker.draws_consumed = draws_consumed;
    worker.rng_label = std::move(label);
    worker.detail = std::move(detail);
    worker.event_status = status;
    runtime.workers.push_back(std::move(worker));
}

bool schedule_effect_chunks_for_source_key(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    int source_key) {
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return false;
    }

    const auto inputs = first_battle_effect_burst_sequence_for_source_key(source_key);
    if (inputs.empty()) {
        runtime.warnings.push_back(
            "no first-battle effect chunk model for source key " + std::to_string(source_key));
        return false;
    }

    int chunk_index = 0;
    for (const auto& input : inputs) {
        const auto model = model_combat_effect_burst_draws(input);
        BattleFrameWorker worker = make_worker(
            runtime.state,
            actor_slot,
            target_slot,
            BattleFrameWorkerKind::EffectChunk,
            MovementSelectedWorker::None);
        worker.draws_consumed = model.total_draws;
        worker.effect_source_key = source_key;
        worker.effect_chunk_index = chunk_index;
        worker.rng_label = "combat_effect_chunk";
        worker.event_status = BattleFrameEventStatus::Provisional;
        std::ostringstream detail;
        detail << "source_key=" << source_key
               << "; chunk_index=" << chunk_index
               << "; loop_count=" << model.loop_count
               << "; draws=" << model.total_draws
               << "; chunk_timing=provisional; " << combat_effect_burst_rule_detail();
        worker.detail = detail.str();
        runtime.workers.push_back(std::move(worker));
        ++chunk_index;
    }
    return true;
}

std::optional<BattleFrameStepEvent> maybe_emit_passive_clash_reaction(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& completed_worker,
    std::uint32_t& rng_state) {
    if (!runtime.initialized
        || !runtime.enable_passive_clash_reaction
        || !completed_worker.complete
        || completed_worker.clash_emitted
        || !is_passive_reaction_source(completed_worker.kind)) {
        return std::nullopt;
    }

    auto* combatant = find_frame_combatant(runtime.state, completed_worker.slot);
    if (combatant == nullptr
        || !same_vec(combatant->pos_holder, combatant->combatant_position)) {
        return std::nullopt;
    }

    completed_worker.clash_emitted = true;
    BattleFrameStepEvent event;
    event.frame_index = runtime.state.frame_index;
    event.slot = completed_worker.slot;
    event.target_slot = completed_worker.target_slot;
    event.callback = "FUN_8002eb4c";
    event.worker_kind = BattleFrameWorkerKind::PassiveClashReaction;
    event.status = BattleFrameEventStatus::Provisional;
    event.old_action_mode = BattleFrameActionMode::Standing;
    event.old_grid = combatant->grid_position;
    event.old_pos_holder = combatant->pos_holder;
    event.old_combatant_position = combatant->combatant_position;

    const auto seed_before = rng_state;
    const auto draw = draw_rand15(rng_state);
    rng_state = draw.next_state;
    const int selected_index = draw.value % 2;
    const std::int16_t selected_mode = selected_index == 0
        ? BattleFrameActionMode::PassiveDodge
        : BattleFrameActionMode::PassiveBlock;
    combatant->combatant_action_mode = BattleFrameActionMode::Standing;
    event.old_action_mode = BattleFrameActionMode::Standing;
    combatant->combatant_action_mode = selected_mode;

    event.new_action_mode = combatant->combatant_action_mode;
    event.new_grid = combatant->grid_position;
    event.new_pos_holder = combatant->pos_holder;
    event.new_combatant_position = combatant->combatant_position;
    event.rng_event = true;
    event.rng_label = "fun_8002eb4c_passive_clash";
    event.draws_consumed = 1;
    event.rng_seed_before = seed_before;
    event.rand_value = draw.value;
    event.rng_seed_after = rng_state;
    event.passive_clash_selected_index = selected_index;
    event.detail =
        "candidate0=0x0000000D; candidate1=0x0000000C; trigger=passive_destination_reconciled; provisional=1";
    return event;
}

BattleFrameRunResult run_first_turn_frame(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state) {
    BattleFrameRunResult result;
    if (!runtime.initialized) {
        result.ok = false;
        result.ambiguous = true;
        result.warnings.push_back("frame runtime is not initialized");
        return result;
    }

    const bool has_active_worker = std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [](const BattleFrameWorker& worker) {
            return !worker.complete;
        });
    runtime.last_step_events.clear();
    if (!has_active_worker) {
        return result;
    }

    ++runtime.state.frame_index;
    ++result.frames_executed;
    for (const auto& thread : runtime.state.packed_thread_order) {
        auto* worker = find_worker(runtime.workers, thread.slot);
        if (worker == nullptr) {
            continue;
        }
        auto event = execute_worker_frame(runtime, *worker, &rng_state);
        runtime.last_step_events.push_back(event);
        runtime.history.push_back(event);
        result.events.push_back(std::move(event));

        if (worker->complete) {
            if (auto clash = maybe_emit_passive_clash_reaction(runtime, *worker, rng_state);
                clash.has_value()) {
                runtime.last_step_events.push_back(*clash);
                runtime.history.push_back(*clash);
                result.events.push_back(std::move(*clash));
            }
        }
    }

    return result;
}

BattleFrameRunResult run_first_turn_until_idle(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state,
    int max_frames) {
    BattleFrameRunResult combined;
    for (int frame = 0; frame < max_frames; ++frame) {
        const bool has_active_worker = std::any_of(
            runtime.workers.begin(),
            runtime.workers.end(),
            [](const BattleFrameWorker& worker) {
                return !worker.complete;
            });
        if (!has_active_worker) {
            break;
        }
        auto step = run_first_turn_frame(runtime, rng_state);
        combined.frames_executed += step.frames_executed;
        combined.events.insert(combined.events.end(), step.events.begin(), step.events.end());
        combined.warnings.insert(combined.warnings.end(), step.warnings.begin(), step.warnings.end());
        if (!step.ok) {
            combined.ok = false;
            combined.ambiguous = combined.ambiguous || step.ambiguous;
            break;
        }
    }

    const bool unfinished = std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [](const BattleFrameWorker& worker) {
            return !worker.complete;
        });
    if (unfinished) {
        combined.ok = false;
        combined.ambiguous = true;
        combined.warnings.push_back("first-turn frame workers did not finish before max frame budget");
    }
    return combined;
}

BattleFrameRunResult run_scheduled_frame_workers(
    BattleFrameRuntime& runtime,
    int max_frames) {
    BattleFrameRunResult result;
    if (!runtime.initialized) {
        result.ok = false;
        result.ambiguous = true;
        result.warnings.push_back("frame runtime is not initialized");
        return result;
    }

    runtime.last_step_events.clear();
    for (int frame = 0; frame < max_frames; ++frame) {
        const bool has_active_worker = std::any_of(
            runtime.workers.begin(),
            runtime.workers.end(),
            [](const BattleFrameWorker& worker) {
                return !worker.complete;
            });
        if (!has_active_worker) {
            break;
        }

        ++runtime.state.frame_index;
        ++result.frames_executed;
        for (const auto& thread : runtime.state.packed_thread_order) {
            auto* worker = find_worker(runtime.workers, thread.slot);
            if (worker == nullptr) {
                continue;
            }
            auto event = execute_worker_frame(runtime, *worker, nullptr);
            runtime.last_step_events.push_back(event);
            runtime.history.push_back(event);
            result.events.push_back(std::move(event));
        }
    }

    const bool unfinished = std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [](const BattleFrameWorker& worker) {
            return !worker.complete;
        });
    if (unfinished) {
        result.ok = false;
        result.ambiguous = true;
        result.warnings.push_back("frame workers did not finish before max frame budget");
    } else if (!runtime.workers.empty()) {
        append_cleanup_workers(runtime);
        for (auto& worker : runtime.workers) {
            if (worker.kind != BattleFrameWorkerKind::CleanupStanding || worker.complete) {
                continue;
            }
            ++runtime.state.frame_index;
            ++result.frames_executed;
            auto event = execute_worker_frame(runtime, worker, nullptr);
            runtime.last_step_events.push_back(event);
            runtime.history.push_back(event);
            result.events.push_back(std::move(event));
        }
    }

    return result;
}

const char* battle_frame_worker_kind_name(BattleFrameWorkerKind kind) {
    switch (kind) {
    case BattleFrameWorkerKind::None:
        return "None";
    case BattleFrameWorkerKind::ActiveMovement:
        return "ActiveMovement";
    case BattleFrameWorkerKind::PassiveMovement:
        return "PassiveMovement";
    case BattleFrameWorkerKind::ActionView:
        return "ActionView";
    case BattleFrameWorkerKind::MechanicalAttack:
        return "MechanicalAttack";
    case BattleFrameWorkerKind::EffectChunk:
        return "EffectChunk";
    case BattleFrameWorkerKind::PassiveClashReaction:
        return "PassiveClashReaction";
    case BattleFrameWorkerKind::Cleanup:
        return "Cleanup";
    case BattleFrameWorkerKind::ActiveDirectAttack:
        return "ActiveDirectAttack";
    case BattleFrameWorkerKind::ActiveFallbackAttack:
        return "ActiveFallbackAttack";
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return "EnemyDirectAttack";
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return "EnemyFallbackAttack";
    case BattleFrameWorkerKind::PassiveTarget:
        return "PassiveTarget";
    case BattleFrameWorkerKind::PassiveSameSide:
        return "PassiveSameSide";
    case BattleFrameWorkerKind::CleanupStanding:
        return "CleanupStanding";
    }
    return "None";
}

const char* battle_frame_event_status_name(BattleFrameEventStatus status) {
    switch (status) {
    case BattleFrameEventStatus::Matched:
        return "Matched";
    case BattleFrameEventStatus::Provisional:
        return "Provisional";
    case BattleFrameEventStatus::MissingInput:
        return "MissingInput";
    case BattleFrameEventStatus::Unsupported:
        return "Unsupported";
    case BattleFrameEventStatus::Ambiguous:
        return "Ambiguous";
    }
    return "Unsupported";
}

const char* battle_frame_action_mode_name(std::int16_t action_mode) {
    switch (action_mode) {
    case BattleFrameActionMode::Standing:
        return "selected";
    case BattleFrameActionMode::ActiveFallback:
        return "active_fallback";
    case BattleFrameActionMode::ActiveApproach:
        return "active_approach_or_passive_same_side";
    case BattleFrameActionMode::PassiveTarget:
        return "passive_target";
    case BattleFrameActionMode::PassiveBlock:
        return "passive_block";
    case BattleFrameActionMode::PassiveDodge:
        return "passive_dodge";
    default:
        return "unknown";
    }
}

} // namespace savor::predict
