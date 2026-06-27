#include "BattleFrameSchedulerModel.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace savor::predict {
namespace {

constexpr int kDirectAttackFrames = 4;
constexpr int kFallbackAttackFrames = 2;
constexpr int kPassiveTargetFrames = 3;
constexpr int kPassiveSameSideFrames = 2;
constexpr int kCleanupFrames = 1;

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
        return 2;
    }
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveDirectAttack:
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return worker.frame + 1 < worker.total_frames ? 6 : 4;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return 5;
    case BattleFrameWorkerKind::PassiveTarget:
        return 0x0b;
    case BattleFrameWorkerKind::PassiveSameSide:
        return 0x06;
    case BattleFrameWorkerKind::CleanupStanding:
        return 2;
    case BattleFrameWorkerKind::None:
        return 2;
    }
    return 2;
}

std::string callback_for_worker(const BattleFrameWorker& worker) {
    switch (worker.kind) {
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

BattleFrameStepEvent execute_worker_frame(BattleFrameRuntime& runtime, BattleFrameWorker& worker) {
    auto* combatant = find_frame_combatant(runtime.state, worker.slot);
    BattleFrameStepEvent event;
    event.frame_index = runtime.state.frame_index;
    event.slot = worker.slot;
    event.target_slot = worker.target_slot;
    event.callback = callback_for_worker(worker);
    event.worker_kind = worker.kind;
    if (combatant == nullptr) {
        worker.complete = true;
        return event;
    }

    event.old_action_mode = combatant->combatant_action_mode;
    event.old_grid = combatant->grid_position;
    event.old_pos_holder = combatant->pos_holder;
    event.old_combatant_position = combatant->combatant_position;

    combatant->combatant_action_mode = action_mode_for_worker(worker);

    if (worker.total_frames > 0) {
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
            auto event = execute_worker_frame(runtime, *worker);
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
            auto event = execute_worker_frame(runtime, worker);
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

} // namespace savor::predict
