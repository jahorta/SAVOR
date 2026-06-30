#include "BattleFrameSchedulerModel.h"

#include "EffectRngModel.h"
#include "RngCore.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

int signum(int value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

MovementGridPosition provisional_path_destination_toward(
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

bool movement_grid_valid(const MovementGridPosition& grid) {
    return grid.grid_x >= 0 && grid.grid_x < 11
        && grid.grid_z >= 0 && grid.grid_z < 11;
}

bool movement_grid_same(const MovementGridPosition& a, const MovementGridPosition& b) {
    return a.grid_x == b.grid_x && a.grid_z == b.grid_z;
}

MovementGridPosition next_direct_path_grid(
    const MovementGridPosition& current,
    const MovementGridPosition& destination) {
    MovementGridPosition next = current;
    const int dx = destination.grid_x - current.grid_x;
    const int dz = destination.grid_z - current.grid_z;
    const int abs_dx = std::abs(dx);
    const int abs_dz = std::abs(dz);

    if (dx == 0 && dz == 0) {
        return next;
    }
    if (dx == 0) {
        next.grid_z += signum(dz);
        return next;
    }
    if (dz == 0) {
        next.grid_x += signum(dx);
        return next;
    }
    if (abs_dx == abs_dz) {
        next.grid_x += signum(dx);
        next.grid_z += signum(dz);
        return next;
    }
    if (abs_dx > abs_dz) {
        next.grid_x += signum(dx);
        return next;
    }

    next.grid_z += signum(dz);
    return next;
}

BattleFrameMovementPathState build_provisional_direct_path(
    const MovementGridPosition& start,
    const MovementGridPosition& destination) {
    BattleFrameMovementPathState path;
    path.available = movement_grid_valid(start) && movement_grid_valid(destination);
    path.zero_distance_target = path.available && movement_grid_same(start, destination);
    path.status_0x16 = path.zero_distance_target ? 1 : 4;
    if (!path.available || path.zero_distance_target) {
        path.terminator_seen = path.available;
        return path;
    }

    MovementGridPosition current = start;
    while (!movement_grid_same(current, destination)
        && path.entry_count < kBattleFrameMovementPathEntryCapacity) {
        current = next_direct_path_grid(current, destination);
        path.entries[path.entry_count++] = current;
    }
    path.dist_to_target_0x14 = static_cast<std::uint8_t>(path.entry_count);
    if (path.entry_count > 0) {
        path.path_index_0x15 = static_cast<std::uint8_t>(path.entry_count - 1);
    }
    path.terminator_seen = path.entry_count < kBattleFrameMovementPathEntryCapacity;
    return path;
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

std::int16_t action_mode_for_worker(const BattleFrameWorker& worker) {
    if (worker.complete) {
        return BattleFrameActionMode::Standing;
    }
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveMovement:
    case BattleFrameWorkerKind::ActiveDirectAttack:
    case BattleFrameWorkerKind::EnemyDirectAttack:
        return BattleFrameActionMode::ActiveApproach;
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
    case BattleFrameWorkerKind::PassiveSpecial:
        return BattleFrameActionMode::PassiveSameSide;
    case BattleFrameWorkerKind::Cleanup:
    case BattleFrameWorkerKind::CleanupStanding:
    case BattleFrameWorkerKind::FrameStartPositionSync:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::None:
        return BattleFrameActionMode::Standing;
    }
    return BattleFrameActionMode::Standing;
}

std::string hex_pc(std::uint32_t pc) {
    if (pc == 0) {
        return "0x00000000";
    }
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << std::nouppercase << pc;
    return out.str();
}

void add_step(
    BattleFrameWorker& worker,
    BattleFrameWorkerStepKind kind,
    std::uint32_t pc,
    std::uint32_t helper_pc,
    std::optional<std::uint32_t> commit_callsite_pc,
    std::int16_t action_mode,
    int worksheet_state_after,
    std::string label,
    std::string detail) {
    worker.program_steps.push_back(BattleFrameWorkerProgramStep{
        .kind = kind,
        .pc = pc,
        .helper_pc = helper_pc,
        .commit_callsite_pc = commit_callsite_pc,
        .action_mode = action_mode,
        .worksheet_state_after = worksheet_state_after,
        .label = std::move(label),
        .detail = std::move(detail),
    });
}

void add_callback_entry_step(BattleFrameWorker& worker) {
    if (worker.callback_pc == 0) {
        return;
    }
    add_step(
        worker,
        BattleFrameWorkerStepKind::CallbackEntry,
        worker.callback_pc,
        0,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "callback_entry",
        "callback_pc=" + hex_pc(worker.callback_pc));
}

void add_path_build_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::PathBuild,
        0x800823e4u,
        0x800823e4u,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "path_build_800823e4",
        "helper_pc=0x800823e4; path timing=provisional");
}

void add_grid_refresh_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::GridRefresh,
        0x80081648u,
        0x80081648u,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "grid_refresh_80081648",
        "helper_pc=0x80081648; grid refresh side effects are provisional");
}

void add_action_motion_setup_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc,
        0x8001fabcu,
        0x8001fabcu,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "action_motion_setup_8001fabc",
        "helper_pc=0x8001fabc; seeds turn fields, pos_to_move_to_0x110, and move_increment_0x104; timing=provisional");
}

void add_rotation_apply_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114,
        0x8001b630u,
        0x80061114u,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "action_motion_rotate_80061114",
        "selector_pc=0x8001b1b0; callsite_pc=0x8001b630; helper_pc=0x80061114; writes CW+0x2c at 0x8001b65c when reached or 0x8001b6cc while rotating; repeats until target reached; timing=provisional");
}

void add_move_increment_apply_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::MoveIncrementApply_80061340,
        0x8001e910u,
        0x80061340u,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "move_increment_apply_80061340",
        "selector_pc=0x8001e910; helper_pc=0x80061340; repeats until target reached; timing=provisional");
}

void add_commit_step(
    BattleFrameWorker& worker,
    std::uint32_t callsite_pc,
    int state_after,
    std::string detail) {
    worker.commit_callsite_pc = callsite_pc;
    add_step(
        worker,
        BattleFrameWorkerStepKind::MovementCommit,
        callsite_pc,
        0x8008178cu,
        callsite_pc,
        action_mode_for_worker(worker),
        state_after,
        "posholder_publish_8008178c",
        std::move(detail));
}

void add_post_commit_step(BattleFrameWorker& worker, int state_after) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::PostCommit,
        0x800810fcu,
        0x800810fcu,
        std::nullopt,
        action_mode_for_worker(worker),
        state_after,
        "post_commit_800810fc",
        "helper_pc=0x800810fc; queued-state update only; post-commit effects remain provisional");
}

void add_mode_helper_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::ModeHelper,
        0x80081134u,
        0x80081134u,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        "mode_helper_80081134",
        "helper_pc=0x80081134; arg=0x0f; mode transition remains provisional");
}

void add_passive_cleanup_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::PassiveCleanup,
        0x80080438u,
        0x80080438u,
        std::nullopt,
        BattleFrameActionMode::Standing,
        0,
        "passive_cleanup_80080438",
        "helper_pc=0x80080438; clears participant bit and stages cleanup");
}

void add_no_commit_step(BattleFrameWorker& worker, std::string detail) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::NoCommit,
        worker.callback_pc,
        0,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        "no_static_commit",
        std::move(detail));
}

void add_rng_step(BattleFrameWorker& worker, std::string detail) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::Rng,
        0,
        0,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        worker.rng_label,
        std::move(detail));
}

void add_marker_step(BattleFrameWorker& worker, std::string label, std::string detail) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::Marker,
        worker.callback_pc,
        0,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        std::move(label),
        std::move(detail));
}

void add_unsupported_step(BattleFrameWorker& worker, std::string detail) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::Unsupported,
        worker.callback_pc,
        0,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        "unsupported_static_worker_path",
        std::move(detail));
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
    case BattleFrameWorkerKind::PassiveSpecial:
        return "PassiveSpecialParticipant";
    case BattleFrameWorkerKind::CleanupStanding:
        return "CleanupStanding";
    case BattleFrameWorkerKind::FrameStartPositionSync:
        return "FrameStartPositionSync";
    case BattleFrameWorkerKind::None:
        return "None";
    }
    return "None";
}

bool is_movement_worker(BattleFrameWorkerKind kind) {
    return kind == BattleFrameWorkerKind::ActiveMovement
        || kind == BattleFrameWorkerKind::PassiveMovement
        || kind == BattleFrameWorkerKind::ActiveDirectAttack
        || kind == BattleFrameWorkerKind::ActiveFallbackAttack
        || kind == BattleFrameWorkerKind::EnemyDirectAttack
        || kind == BattleFrameWorkerKind::EnemyFallbackAttack
        || kind == BattleFrameWorkerKind::PassiveTarget
        || kind == BattleFrameWorkerKind::PassiveSameSide
        || kind == BattleFrameWorkerKind::PassiveSpecial
        || kind == BattleFrameWorkerKind::CleanupStanding
        || kind == BattleFrameWorkerKind::Cleanup;
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b);

std::string movement_grid_detail(const MovementGridPosition& grid) {
    std::ostringstream out;
    out << "(" << grid.grid_x << "," << grid.grid_z << ")";
    return out.str();
}

std::string movement_path_entries_detail(const BattleFrameMovementPathState& path) {
    std::ostringstream out;
    for (std::size_t i = 0; i < path.entry_count; ++i) {
        if (i != 0) {
            out << " ";
        }
        out << i << ":" << movement_grid_detail(path.entries[i]);
    }
    if (path.terminator_seen) {
        if (path.entry_count != 0) {
            out << " ";
        }
        out << "terminator=0xff";
    }
    if (path.zero_distance_target) {
        if (path.entry_count != 0 || path.terminator_seen) {
            out << " ";
        }
        out << "zero_distance_target=1";
    }
    return out.str();
}

bool select_worker_destination_from_path_index(
    BattleFrameState& state,
    BattleFrameWorker& worker,
    std::string* reason) {
    auto* combatant = find_frame_combatant(state, worker.slot);
    if (combatant == nullptr) {
        if (reason != nullptr) {
            *reason = "movement path selection missing combatant slot";
        }
        worker.event_status = BattleFrameEventStatus::MissingInput;
        return false;
    }
    if (!worker.movement_path.available || worker.movement_path.entry_count == 0) {
        if (worker.movement_path.available && worker.movement_path.zero_distance_target) {
            worker.path_index_0x15 = worker.movement_path.path_index_0x15;
            worker.destination_grid = combatant->grid_position;
            worker.destination_position = combatant->pos_holder;
            return true;
        }
        if (reason != nullptr) {
            *reason = "movement path list is missing";
        }
        worker.event_status = BattleFrameEventStatus::MissingInput;
        return false;
    }
    if (worker.movement_path.path_index_0x15 >= worker.movement_path.entry_count) {
        if (reason != nullptr) {
            *reason = "movement path_index_0x15 points past path terminator";
        }
        worker.event_status = BattleFrameEventStatus::Unsupported;
        return false;
    }

    worker.path_index_0x15 = worker.movement_path.path_index_0x15;
    worker.destination_grid =
        worker.movement_path.entries[worker.movement_path.path_index_0x15];
    worker.destination_position = first_battle_grid_to_raw_stage_position(
        worker.destination_grid,
        combatant->width,
        combatant->depth);
    return true;
}

void set_worker_destination_from_target(BattleFrameState& state, BattleFrameWorker& worker) {
    auto* combatant = find_frame_combatant(state, worker.slot);
    const auto* target = find_frame_combatant(state, worker.target_slot);
    if (combatant == nullptr || target == nullptr) {
        worker.event_status = BattleFrameEventStatus::MissingInput;
        worker.detail += "; movement path selection missing actor or target combatant";
        return;
    }

    const auto provisional_destination = provisional_path_destination_toward(*combatant, *target);
    worker.movement_path = build_provisional_direct_path(
        combatant->grid_position,
        provisional_destination);
    std::string reason;
    if (!select_worker_destination_from_path_index(state, worker, &reason)) {
        worker.detail += "; " + reason;
        return;
    }
    worker.detail += "; path_list_source=provisional_direct_path_to_selected_destination";
}

void build_static_worker_program(BattleFrameWorker& worker) {
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveDirectAttack:
        worker.callback_pc = 0x80086308u;
        add_callback_entry_step(worker);
        add_path_build_step(worker);
        add_grid_refresh_step(worker);
        add_action_motion_setup_step(worker);
        add_rotation_apply_step(worker);
        add_move_increment_apply_step(worker);
        add_commit_step(
            worker,
            0x80086480u,
            3,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80086308; candidate_commits=0x80086480,0x80086698; selected=0x80086480; state_after=3");
        add_post_commit_step(worker, 3);
        break;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
        worker.callback_pc = 0x80085ce0u;
        add_callback_entry_step(worker);
        add_no_commit_step(
            worker,
            "caller=0x80085ce0; no static FUN_8008178c commit in current first-turn closure; movement path remains provisional");
        break;
    case BattleFrameWorkerKind::EnemyDirectAttack:
        worker.callback_pc = 0x80087f6cu;
        add_callback_entry_step(worker);
        add_path_build_step(worker);
        add_grid_refresh_step(worker);
        add_action_motion_setup_step(worker);
        add_rotation_apply_step(worker);
        add_move_increment_apply_step(worker);
        add_commit_step(
            worker,
            0x8008816cu,
            2,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80087f6c; candidate_commits=0x8008816c,0x800883cc,0x80088434; selected=0x8008816c; state_after=2");
        add_post_commit_step(worker, 2);
        break;
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        worker.callback_pc = 0x80087844u;
        add_callback_entry_step(worker);
        add_grid_refresh_step(worker);
        add_action_motion_setup_step(worker);
        add_rotation_apply_step(worker);
        add_move_increment_apply_step(worker);
        add_commit_step(
            worker,
            0x800879a8u,
            0x65,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80087844; selected=0x800879a8; state_after=0x65");
        add_post_commit_step(worker, 0x65);
        break;
    case BattleFrameWorkerKind::PassiveTarget:
        worker.callback_pc = 0x8008c21cu;
        add_callback_entry_step(worker);
        add_grid_refresh_step(worker);
        add_step(
            worker,
            BattleFrameWorkerStepKind::CallbackEntry,
            0x8008c4c0u,
            0,
            std::nullopt,
            BattleFrameActionMode::PassiveTarget,
            -1,
            "passive_path_commit_helper_8008c4c0",
            "dispatch=0x8008c21c->0x8008c4c0");
        add_action_motion_setup_step(worker);
        add_rotation_apply_step(worker);
        add_move_increment_apply_step(worker);
        add_commit_step(
            worker,
            0x8008c67cu,
            2,
            "helper_pc=0x8008178c; posHolder publish; caller=0x8008c4c0; selected=0x8008c67c; route=target_participant; state_after=2");
        add_post_commit_step(worker, 2);
        add_passive_cleanup_step(worker);
        break;
    case BattleFrameWorkerKind::PassiveSameSide:
        worker.callback_pc = 0x8008c7b0u;
        add_callback_entry_step(worker);
        add_grid_refresh_step(worker);
        add_action_motion_setup_step(worker);
        add_rotation_apply_step(worker);
        add_move_increment_apply_step(worker);
        add_commit_step(
            worker,
            0x8008c844u,
            -1,
            "helper_pc=0x8008178c; posHolder publish; caller=0x8008c7b0; candidate_commits=0x8008c844,0x8008c920; selected=0x8008c844; route=same_side_participant");
        add_mode_helper_step(worker);
        add_passive_cleanup_step(worker);
        break;
    case BattleFrameWorkerKind::PassiveSpecial:
        worker.callback_pc = 0x8008d960u;
        worker.event_status = BattleFrameEventStatus::Unsupported;
        add_callback_entry_step(worker);
        add_unsupported_step(
            worker,
            "caller=0x8008d960; passive special subdispatch is not classified for first-turn scheduler v1");
        break;
    case BattleFrameWorkerKind::ActionView:
    case BattleFrameWorkerKind::EffectChunk:
        add_rng_step(worker, worker.detail);
        break;
    case BattleFrameWorkerKind::MechanicalAttack:
        worker.callback_pc = 0;
        add_marker_step(
            worker,
            "mechanical_attack",
            "zero-draw mechanical attack marker; attack resolution may run after this frame event");
        break;
    case BattleFrameWorkerKind::CleanupStanding:
    case BattleFrameWorkerKind::Cleanup:
        add_marker_step(
            worker,
            "cleanup_standing",
            "cleanup worker sets selected/standing action mode");
        break;
    case BattleFrameWorkerKind::PassiveMovement:
    case BattleFrameWorkerKind::ActiveMovement:
        add_unsupported_step(worker, "legacy generic worker kind is not used by explicit first-turn scheduler");
        worker.event_status = BattleFrameEventStatus::Unsupported;
        break;
    case BattleFrameWorkerKind::PassiveClashReaction:
        add_rng_step(worker, worker.detail);
        break;
    case BattleFrameWorkerKind::FrameStartPositionSync:
        worker.complete = true;
        break;
    case BattleFrameWorkerKind::None:
        worker.complete = true;
        break;
    }
    worker.total_frames = static_cast<int>(worker.program_steps.size());
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
    if (is_movement_worker(kind) || kind == BattleFrameWorkerKind::MechanicalAttack) {
        worker.event_status = BattleFrameEventStatus::Provisional;
    }

    auto* combatant = find_frame_combatant(state, slot);
    if (combatant == nullptr) {
        worker.complete = true;
        return worker;
    }
    worker.start_grid = combatant->grid_position;
    worker.destination_grid = combatant->grid_position;
    worker.start_position = combatant->pos_holder;
    worker.destination_position = combatant->pos_holder;

    if (is_movement_worker(kind)) {
        set_worker_destination_from_target(state, worker);
    }
    build_static_worker_program(worker);
    return worker;
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b) {
    return a.grid_x == b.grid_x && a.grid_z == b.grid_z;
}

bool same_vec(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

float xz_distance(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dz * dz);
}

BattleFrameVec3 scale_vec(const BattleFrameVec3& value, float scale) {
    return BattleFrameVec3{
        .x = value.x * scale,
        .y = value.y * scale,
        .z = value.z * scale,
    };
}

std::string frame_vec_detail(const BattleFrameVec3& value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << "(" << value.x << "," << value.y << "," << value.z << ")";
    return out.str();
}

std::string angle_detail(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

float selected_motion_speed_for_mode(const BattleFrameCombatantState& combatant) {
    if (combatant.combatant_action_mode == BattleFrameActionMode::ActionMotionAltSpeed) {
        return combatant.motion_alt_speed_0x130;
    }
    return combatant.motion_base_speed_0x12c;
}

void seed_action_motion_8001fabc(
    BattleFrameCombatantState& combatant,
    const BattleFrameWorker& worker,
    BattleFrameStepEvent& event) {
    combatant.pos_to_move_to_0x110 = worker.destination_position;
    combatant.selected_motion_speed = selected_motion_speed_for_mode(combatant);
    event.selected_motion_speed = combatant.selected_motion_speed;
    combatant.turn_current_degrees_0x11c =
        battle_frame_angle_short_to_degrees_8006116c(combatant.combatant_facing_angle_0x2c);
    combatant.turn_target_degrees_0x120 = battle_frame_target_facing_degrees_xz(
        combatant.combatant_cur_pos_0x1c,
        worker.destination_position,
        combatant.turn_current_degrees_0x11c);
    normalize_turn_shortest_path_80061080(
        combatant.turn_current_degrees_0x11c,
        combatant.turn_target_degrees_0x120);
    combatant.turn_step_degrees_0x124 =
        combatant.turn_target_degrees_0x120 - combatant.turn_current_degrees_0x11c >= 0.0f
            ? combatant.turn_speed_degrees_0x128
            : -combatant.turn_speed_degrees_0x128;
    combatant.turn_state_known = combatant.turn_speed_known;

    const float distance = xz_distance(combatant.combatant_cur_pos_0x1c, combatant.pos_to_move_to_0x110);
    if (!combatant.motion_speeds_known || (distance > 0.0f && combatant.selected_motion_speed <= 0.0f)) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.detail += "; missing first-battle motion speed for slot " + std::to_string(combatant.slot);
        combatant.move_increment_0x104 = BattleFrameVec3{};
    } else if (distance == 0.0f) {
        combatant.move_increment_0x104 = BattleFrameVec3{};
    } else {
        const float scale = combatant.selected_motion_speed / distance;
        combatant.move_increment_0x104 = BattleFrameVec3{
            .x = (combatant.pos_to_move_to_0x110.x - combatant.combatant_cur_pos_0x1c.x) * scale,
            .y = 0.0f,
            .z = (combatant.pos_to_move_to_0x110.z - combatant.combatant_cur_pos_0x1c.z) * scale,
        };
    }

    event.action_motion_setup_event = true;
    event.pos_to_move_to_0x110 = combatant.pos_to_move_to_0x110;
    event.move_increment_0x104 = combatant.move_increment_0x104;
    event.turn_current_degrees_0x11c = combatant.turn_current_degrees_0x11c;
    event.turn_target_degrees_0x120 = combatant.turn_target_degrees_0x120;
    event.turn_step_degrees_0x124 = combatant.turn_step_degrees_0x124;
    event.turn_speed_degrees_0x128 = combatant.turn_speed_degrees_0x128;
    event.turn_speed_bits_0x128 = combatant.turn_speed_bits_0x128;
    event.detail += "; target=" + frame_vec_detail(combatant.pos_to_move_to_0x110)
        + "; selected_speed=" + std::to_string(combatant.selected_motion_speed)
        + "; move_increment_0x104=" + frame_vec_detail(combatant.move_increment_0x104)
        + "; turn_current_0x11c=" + angle_detail(combatant.turn_current_degrees_0x11c)
        + "; turn_target_0x120=" + angle_detail(combatant.turn_target_degrees_0x120)
        + "; turn_step_0x124=" + angle_detail(combatant.turn_step_degrees_0x124)
        + "; turn_speed_0x128=" + angle_detail(combatant.turn_speed_degrees_0x128)
        + "; turn_speed_bits_0x128=0x" + [&combatant] {
            std::ostringstream bits;
            bits << std::hex << std::setw(8) << std::setfill('0') << std::nouppercase
                 << combatant.turn_speed_bits_0x128;
            return bits.str();
        }()
        + "; iw_base_speed_0x12c=" + std::to_string(combatant.motion_base_speed_0x12c)
        + "; iw_alt_speed_0x130=" + std::to_string(combatant.motion_alt_speed_0x130)
        + "; selected_action_row_flags=0x" + [&combatant] {
            std::ostringstream flags;
            flags << std::hex << std::nouppercase << combatant.selected_action_row_flags;
            return flags.str();
        }();
}

bool apply_rotation_8001b630(
    BattleFrameCombatantState& combatant,
    BattleFrameStepEvent& event) {
    event.rotation_apply_event = true;
    event.turn_current_degrees_0x11c = combatant.turn_current_degrees_0x11c;
    event.turn_target_degrees_0x120 = combatant.turn_target_degrees_0x120;
    event.turn_step_degrees_0x124 = combatant.turn_step_degrees_0x124;
    event.turn_speed_degrees_0x128 = combatant.turn_speed_degrees_0x128;
    event.turn_speed_bits_0x128 = combatant.turn_speed_bits_0x128;

    if (!combatant.turn_state_known || !combatant.turn_speed_known) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.rotation_reached_target = true;
        event.detail += "; missing first-battle rotation speed for slot " + std::to_string(combatant.slot)
            + "; selector_pc=0x8001b1b0; callsite_pc=0x8001b630; helper_pc=0x80061114";
        return true;
    }

    const bool reached = apply_rotation_increment_80061114(
        combatant.turn_current_degrees_0x11c,
        combatant.turn_target_degrees_0x120,
        combatant.turn_step_degrees_0x124);
    combatant.combatant_facing_angle_0x2c =
        battle_frame_degrees_to_angle_short_8001b1b0(
            reached ? combatant.turn_target_degrees_0x120 : combatant.turn_current_degrees_0x11c);
    combatant.last_written_facing_angle_0x2c = combatant.combatant_facing_angle_0x2c;
    if (reached) {
        combatant.instruction_flags_0xf0 |= 0x200u;
    }

    event.rotation_reached_target = reached;
    event.turn_current_degrees_0x11c = combatant.turn_current_degrees_0x11c;
    event.new_combatant_facing_angle_0x2c = combatant.combatant_facing_angle_0x2c;
    event.detail += "; turn_current_0x11c=" + angle_detail(combatant.turn_current_degrees_0x11c)
        + "; turn_target_0x120=" + angle_detail(combatant.turn_target_degrees_0x120)
        + "; turn_step_0x124=" + angle_detail(combatant.turn_step_degrees_0x124)
        + "; turn_speed_0x128=" + angle_detail(combatant.turn_speed_degrees_0x128)
        + "; turn_speed_bits_0x128=0x" + [&combatant] {
            std::ostringstream bits;
            bits << std::hex << std::setw(8) << std::setfill('0') << std::nouppercase
                 << combatant.turn_speed_bits_0x128;
            return bits.str();
        }()
        + "; facing_write_pc=" + std::string(reached ? "0x8001b65c" : "0x8001b6cc")
        + "; reached=" + std::to_string(reached ? 1 : 0)
        + "; selector_pc=0x8001b1b0; callsite_pc=0x8001b630; helper_pc=0x80061114";
    return reached;
}

bool apply_move_increment_8001e910(
    BattleFrameCombatantState& combatant,
    BattleFrameStepEvent& event) {
    BattleFrameVec3 applied = combatant.move_increment_0x104;
    if (combatant.combatant_action_mode == BattleFrameActionMode::ActionMotionAltSpeed) {
        applied = scale_vec(applied, 0.5f);
    }

    event.move_increment_apply_event = true;
    event.pos_to_move_to_0x110 = combatant.pos_to_move_to_0x110;
    event.move_increment_0x104 = combatant.move_increment_0x104;
    event.applied_move_increment = applied;
    event.selected_motion_speed = combatant.selected_motion_speed;

    if ((combatant.selected_action_row_flags & 0x01000000u) == 0) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.detail += "; FUN_8001e910 movement apply flag not modeled for selected action row";
        event.motion_reached_target = true;
        return true;
    }

    const bool reached = move_combatant_increment_80061340(
        combatant.combatant_cur_pos_0x1c,
        combatant.pos_to_move_to_0x110,
        applied);
    combatant.last_applied_move_increment = applied;
    event.motion_reached_target = reached;
    event.detail += "; target=" + frame_vec_detail(combatant.pos_to_move_to_0x110)
        + "; move_increment_0x104=" + frame_vec_detail(combatant.move_increment_0x104)
        + "; applied_increment=" + frame_vec_detail(applied)
        + "; reached=" + std::to_string(reached ? 1 : 0)
        + "; selector_pc=0x8001e910; helper_pc=0x80061340";
    return reached;
}

bool is_rng_worker(BattleFrameWorkerKind kind) {
    return kind == BattleFrameWorkerKind::ActionView
        || kind == BattleFrameWorkerKind::EffectChunk
        || kind == BattleFrameWorkerKind::PassiveClashReaction;
}

void advance_without_rand_values(std::uint32_t& state, int draws) {
    for (int i = 0; i < draws; ++i) {
        state = advance_once(state);
    }
}

void apply_rng_worker(BattleFrameStepEvent& event, BattleFrameWorker& worker, std::uint32_t& rng_state) {
    if (worker.draws_consumed <= 0) {
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

void reset_worker_queues(BattleFrameRuntime& runtime) {
    runtime.workers.clear();
    for (auto& queue : runtime.slot_worker_queues) {
        queue.clear();
    }
    runtime.next_worker_sequence = 0;
}

void enqueue_worker(BattleFrameRuntime& runtime, BattleFrameWorker worker) {
    if (worker.slot < 0
        || worker.slot >= static_cast<int>(runtime.slot_worker_queues.size())) {
        worker.event_status = BattleFrameEventStatus::Unsupported;
        worker.detail += "; unsupported slot for frame worker queue";
        runtime.workers.push_back(std::move(worker));
        return;
    }
    worker.queue_sequence = runtime.next_worker_sequence++;
    const int index = static_cast<int>(runtime.workers.size());
    runtime.workers.push_back(std::move(worker));
    runtime.slot_worker_queues[static_cast<std::size_t>(runtime.workers.back().slot)].push_back(index);
}

BattleFrameWorker* find_queued_worker(BattleFrameRuntime& runtime, int slot) {
    if (slot < 0 || slot >= static_cast<int>(runtime.slot_worker_queues.size())) {
        return nullptr;
    }
    for (const int index : runtime.slot_worker_queues[static_cast<std::size_t>(slot)]) {
        if (index < 0 || index >= static_cast<int>(runtime.workers.size())) {
            continue;
        }
        auto& worker = runtime.workers[static_cast<std::size_t>(index)];
        if (!worker.complete) {
            return &worker;
        }
    }
    return nullptr;
}

bool has_active_worker(const BattleFrameRuntime& runtime) {
    return std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [](const BattleFrameWorker& worker) {
            return !worker.complete;
        });
}

void append_recorded_event(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameStepEvent event) {
    runtime.last_step_events.push_back(event);
    runtime.history.push_back(event);
    result.events.push_back(std::move(event));
}

void append_frame_start_position_sync_events(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    for (const auto& before : runtime.state.combatants) {
        if (!before.present || !before.alive) {
            continue;
        }
        if (!before.pending_frame_start_position_sync) {
            continue;
        }

        BattleFrameStepEvent event;
        event.frame_index = runtime.state.frame_index;
        event.slot = before.slot;
        event.target_slot = before.slot;
        event.callback = "FrameStartPositionSync";
        event.worker_kind = BattleFrameWorkerKind::FrameStartPositionSync;
        event.step_kind = BattleFrameWorkerStepKind::FrameStartPositionSync;
        event.status = BattleFrameEventStatus::Provisional;
        event.old_action_mode = before.combatant_action_mode;
        event.new_action_mode = before.combatant_action_mode;
        event.old_grid = before.grid_position;
        event.new_grid = before.grid_position;
        event.old_pos_holder = before.pos_holder;
        event.new_pos_holder = before.pos_holder;
        event.old_combatant_cur_pos_0x1c = before.combatant_cur_pos_0x1c;
        event.old_combatant_facing_angle_0x2c = before.combatant_facing_angle_0x2c;
        event.new_combatant_facing_angle_0x2c = before.combatant_facing_angle_0x2c;

        sync_frame_start_position_from_pos_holder(runtime.state, before.slot);
        const auto* after = find_frame_combatant(runtime.state, before.slot);
        if (after == nullptr) {
            event.status = BattleFrameEventStatus::MissingInput;
            event.new_combatant_cur_pos_0x1c = event.old_combatant_cur_pos_0x1c;
        } else {
            event.new_action_mode = after->combatant_action_mode;
            event.new_grid = after->grid_position;
            event.new_pos_holder = after->pos_holder;
            event.new_combatant_cur_pos_0x1c = after->combatant_cur_pos_0x1c;
            event.new_combatant_facing_angle_0x2c = after->combatant_facing_angle_0x2c;
        }
        event.grid_changed = !same_grid(event.old_grid, event.new_grid);
        event.pos_holder_changed = !same_vec(event.old_pos_holder, event.new_pos_holder);
        event.combatant_cur_pos_changed =
            !same_vec(event.old_combatant_cur_pos_0x1c, event.new_combatant_cur_pos_0x1c);
        event.combatant_facing_angle_changed =
            event.old_combatant_facing_angle_0x2c != event.new_combatant_facing_angle_0x2c;
        event.action_motion_position_synced = true;
        event.detail =
            "step_kind=FrameStartPositionSync; frame_phase=top_of_8000a2fc; consumes explicit pending sync flag; copies current slot posHolder into combatant_cur_pos_0x1c and instruction_field_0xf8; function_neutral=1; not_FUN_8001ab60; provisional=1";
        append_recorded_event(runtime, result, std::move(event));
    }
}

void append_event_detail(BattleFrameStepEvent& event, const BattleFrameWorker& worker, const BattleFrameWorkerProgramStep& step) {
    std::ostringstream detail;
    detail << "step_kind=" << battle_frame_worker_step_kind_name(step.kind)
           << "; callback_pc=" << hex_pc(worker.callback_pc)
           << "; step_pc=" << hex_pc(step.pc)
           << "; helper_pc=" << hex_pc(step.helper_pc);
    if (step.commit_callsite_pc.has_value()) {
        detail << "; commit_callsite_pc=" << hex_pc(*step.commit_callsite_pc);
    }
    detail << "; worker_sequence=" << worker.queue_sequence
           << "; program_index=" << worker.program_index
           << "; program_steps=" << worker.program_steps.size();
    if (!worker.detail.empty()) {
        detail << "; " << worker.detail;
    }
    if (!step.detail.empty()) {
        detail << "; " << step.detail;
    }
    event.detail = detail.str();
}

void append_movement_path_event_detail(BattleFrameStepEvent& event, const BattleFrameWorker& worker) {
    if (!worker.movement_path.available) {
        return;
    }

    event.movement_path = worker.movement_path;
    event.selected_path_node = worker.destination_grid;
    event.detail += "; dist_to_target_0x14="
        + std::to_string(worker.movement_path.dist_to_target_0x14);
    event.detail += "; old_path_index_0x15=" + std::to_string(event.old_path_index_0x15);
    event.detail += "; new_path_index_0x15=" + std::to_string(event.new_path_index_0x15);
    event.detail += "; status_0x16=" + std::to_string(worker.movement_path.status_0x16);
    event.detail += "; selected_path_node=" + movement_grid_detail(worker.destination_grid);
    event.detail += "; path_entries=" + movement_path_entries_detail(worker.movement_path);
    event.detail += "; path_target_direct=1";
    if (worker.movement_path.zero_distance_target) {
        event.detail += "; zero_distance_path=1";
    }
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
    if (worker.program_index < worker.program_steps.size()) {
        const auto& step = worker.program_steps[worker.program_index];
        event.step_kind = step.kind;
        event.callback_pc = worker.callback_pc;
        event.step_pc = step.pc;
        event.helper_pc = step.helper_pc;
        event.commit_callsite_pc = step.commit_callsite_pc;
        append_event_detail(event, worker, step);
    } else {
        event.detail = worker.detail;
    }
    if (combatant == nullptr) {
        worker.complete = true;
        return event;
    }

    event.old_action_mode = combatant->combatant_action_mode;
    event.old_grid = combatant->grid_position;
    event.movement_path = worker.movement_path;
    event.old_path_index_0x15 = worker.path_index_0x15;
    if (worker.movement_path.available) {
        event.selected_path_node = worker.destination_grid;
    }
    event.old_pos_holder = combatant->pos_holder;
    event.old_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
    event.old_combatant_facing_angle_0x2c = combatant->combatant_facing_angle_0x2c;
    event.new_combatant_facing_angle_0x2c = combatant->combatant_facing_angle_0x2c;

    if (worker.program_index >= worker.program_steps.size()) {
        worker.complete = true;
    } else {
        const auto& step = worker.program_steps[worker.program_index];
        worker.current_pc = step.pc;
        if (step.action_mode != 0) {
            combatant->combatant_action_mode = step.action_mode;
        } else {
            combatant->combatant_action_mode = action_mode_for_worker(worker);
        }
        if (step.worksheet_state_after >= 0) {
            worker.worksheet_state_0x19 = static_cast<std::uint8_t>(step.worksheet_state_after);
        }

        bool advance_program = true;
        switch (step.kind) {
        case BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc:
            seed_action_motion_8001fabc(*combatant, worker, event);
            break;
        case BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114: {
            const bool reached = apply_rotation_8001b630(*combatant, event);
            advance_program = reached;
            break;
        }
        case BattleFrameWorkerStepKind::ActionMotionMoveStep_8001e910:
        case BattleFrameWorkerStepKind::MoveIncrementApply_80061340: {
            const bool reached = apply_move_increment_8001e910(*combatant, event);
            advance_program = reached;
            break;
        }
        case BattleFrameWorkerStepKind::MovementCommit:
            commit_movement_grid_8008178c(runtime.state, worker.slot, worker.destination_grid);
            combatant = find_frame_combatant(runtime.state, worker.slot);
            if (combatant != nullptr) {
                if (worker.path_index_0x15 < 0xffu) {
                    worker.path_index_0x15 = static_cast<std::uint8_t>(worker.path_index_0x15 + 1);
                    worker.movement_path.path_index_0x15 = worker.path_index_0x15;
                }
                worker.destination_committed = true;
            }
            break;
        case BattleFrameWorkerStepKind::ActionMotionPositionSync:
            sync_action_motion_position_8001ab60(
                runtime.state,
                worker.slot,
                worker.action_motion_position_source_slot);
            combatant = find_frame_combatant(runtime.state, worker.slot);
            break;
        case BattleFrameWorkerStepKind::Rng:
            if (rng_state != nullptr && is_rng_worker(worker.kind)) {
                apply_rng_worker(event, worker, *rng_state);
            }
            break;
        case BattleFrameWorkerStepKind::Unsupported:
            event.status = BattleFrameEventStatus::Unsupported;
            worker.event_status = BattleFrameEventStatus::Unsupported;
            break;
        case BattleFrameWorkerStepKind::CallbackEntry:
        case BattleFrameWorkerStepKind::PathBuild:
        case BattleFrameWorkerStepKind::GridRefresh:
        case BattleFrameWorkerStepKind::PostCommit:
        case BattleFrameWorkerStepKind::ModeHelper:
        case BattleFrameWorkerStepKind::PassiveCleanup:
        case BattleFrameWorkerStepKind::FrameStartPositionSync:
        case BattleFrameWorkerStepKind::Marker:
        case BattleFrameWorkerStepKind::NoCommit:
            break;
        }

        if (advance_program) {
            ++worker.program_index;
        }
        ++worker.frame;
        if (worker.program_index >= worker.program_steps.size()) {
            worker.complete = true;
            if (combatant != nullptr) {
                combatant->combatant_action_mode = BattleFrameActionMode::Standing;
            }
        }
    }

    if (combatant != nullptr) {
        event.new_action_mode = combatant->combatant_action_mode;
        event.new_grid = combatant->grid_position;
        event.new_path_index_0x15 = worker.path_index_0x15;
        event.movement_path = worker.movement_path;
        event.new_pos_holder = combatant->pos_holder;
        event.new_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
        event.new_combatant_facing_angle_0x2c = combatant->combatant_facing_angle_0x2c;
        event.grid_changed = !same_grid(event.old_grid, event.new_grid);
        event.pos_holder_changed = !same_vec(event.old_pos_holder, event.new_pos_holder);
        event.combatant_cur_pos_changed =
            !same_vec(event.old_combatant_cur_pos_0x1c, event.new_combatant_cur_pos_0x1c);
        event.combatant_facing_angle_changed =
            event.old_combatant_facing_angle_0x2c != event.new_combatant_facing_angle_0x2c;
        event.action_motion_position_synced =
            event.step_kind == BattleFrameWorkerStepKind::ActionMotionPositionSync
            || event.step_kind == BattleFrameWorkerStepKind::FrameStartPositionSync;
    }
    append_movement_path_event_detail(event, worker);
    return event;
}

void append_cleanup_workers(BattleFrameRuntime& runtime) {
    for (const auto& thread : runtime.state.packed_thread_order) {
        const auto* combatant = find_frame_combatant(runtime.state, thread.slot);
        if (combatant == nullptr || !combatant->present || !combatant->alive) {
            continue;
        }
        enqueue_worker(runtime, make_worker(
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
        || kind == BattleFrameWorkerKind::PassiveSameSide
        || kind == BattleFrameWorkerKind::PassiveSpecial;
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

bool set_worker_movement_path_from_entries(
    BattleFrameState& state,
    BattleFrameWorker& worker,
    std::uint8_t dist_to_target_0x14,
    std::uint8_t path_index_0x15,
    std::uint8_t status_0x16,
    const std::vector<MovementGridPosition>& entries) {
    BattleFrameMovementPathState path;
    path.available = true;
    path.dist_to_target_0x14 = dist_to_target_0x14;
    path.path_index_0x15 = path_index_0x15;
    path.status_0x16 = status_0x16;
    path.entry_count = std::min(
        entries.size(),
        kBattleFrameMovementPathEntryCapacity);
    for (std::size_t i = 0; i < path.entry_count; ++i) {
        path.entries[i] = entries[i];
    }
    path.terminator_seen = path.entry_count < kBattleFrameMovementPathEntryCapacity;
    worker.movement_path = path;

    std::string reason;
    const bool selected = select_worker_destination_from_path_index(state, worker, &reason);
    if (!selected) {
        if (!worker.detail.empty()) {
            worker.detail += "; ";
        }
        worker.detail += reason;
    }
    return selected;
}

void schedule_first_battle_action_workers(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input) {
    reset_worker_queues(runtime);
    runtime.last_step_events.clear();
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }

    enqueue_worker(runtime, make_worker(
        runtime.state,
        input.actor_slot,
        input.target_slot,
        active_worker_kind(input.selected_worker, input.enemy_owned),
        input.selected_worker));

    for (const auto& route : input.passive_routes) {
        if (route.route == PassiveMovementRouteKind::TargetParticipant) {
            enqueue_worker(runtime, make_worker(
                runtime.state,
                route.slot,
                input.actor_slot,
                BattleFrameWorkerKind::PassiveTarget,
                route.selected_worker));
        } else if (route.route == PassiveMovementRouteKind::SameSideParticipant
            || route.route == PassiveMovementRouteKind::SpecialParticipant) {
            enqueue_worker(runtime, make_worker(
                runtime.state,
                route.slot,
                input.target_slot,
                route.route == PassiveMovementRouteKind::SpecialParticipant
                    ? BattleFrameWorkerKind::PassiveSpecial
                    : BattleFrameWorkerKind::PassiveSameSide,
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
    enqueue_worker(runtime, make_worker(
        runtime.state,
        input.actor_slot,
        input.target_slot,
        active_worker_kind(input.selected_worker, input.enemy_owned),
        input.selected_worker));

    for (const auto& route : input.passive_routes) {
        if (route.route == PassiveMovementRouteKind::TargetParticipant) {
            enqueue_worker(runtime, make_worker(
                runtime.state,
                route.slot,
                input.actor_slot,
                BattleFrameWorkerKind::PassiveTarget,
                route.selected_worker));
        } else if (route.route == PassiveMovementRouteKind::SameSideParticipant
            || route.route == PassiveMovementRouteKind::SpecialParticipant) {
            enqueue_worker(runtime, make_worker(
                runtime.state,
                route.slot,
                input.target_slot,
                route.route == PassiveMovementRouteKind::SpecialParticipant
                    ? BattleFrameWorkerKind::PassiveSpecial
                    : BattleFrameWorkerKind::PassiveSameSide,
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
    worker.program_steps.clear();
    build_static_worker_program(worker);
    enqueue_worker(runtime, std::move(worker));
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
        worker.program_steps.clear();
        build_static_worker_program(worker);
        enqueue_worker(runtime, std::move(worker));
        ++chunk_index;
    }
    return true;
}

void schedule_first_turn_mechanical_attack(
    BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot) {
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }
    enqueue_worker(runtime, make_worker(
        runtime.state,
        actor_slot,
        target_slot,
        BattleFrameWorkerKind::MechanicalAttack,
        MovementSelectedWorker::None));
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
        || combatant->combatant_action_mode != BattleFrameActionMode::Standing
        || !completed_worker.destination_committed) {
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
    event.old_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;

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
    event.new_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
    event.rng_event = true;
    event.rng_label = "fun_8002eb4c_passive_clash";
    event.draws_consumed = 1;
    event.rng_seed_before = seed_before;
    event.rand_value = draw.value;
    event.rng_seed_after = rng_state;
    event.passive_clash_selected_index = selected_index;
    event.detail =
        "candidate0=0x0000000D; candidate1=0x0000000C; trigger=passive_destination_committed; required_action_mode_before=0x0002; provisional=1";
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

    runtime.last_step_events.clear();
    if (!has_active_worker(runtime)) {
        return result;
    }

    ++runtime.state.frame_index;
    ++result.frames_executed;
    append_frame_start_position_sync_events(runtime, result);
    for (const auto& thread : runtime.state.packed_thread_order) {
        auto* worker = find_queued_worker(runtime, thread.slot);
        if (worker == nullptr) {
            continue;
        }
        auto event = execute_worker_frame(runtime, *worker, &rng_state);
        append_recorded_event(runtime, result, std::move(event));

        if (worker->complete) {
            if (auto clash = maybe_emit_passive_clash_reaction(runtime, *worker, rng_state);
                clash.has_value()) {
                append_recorded_event(runtime, result, std::move(*clash));
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
        if (!has_active_worker(runtime)) {
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

    if (has_active_worker(runtime)) {
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
        if (!has_active_worker(runtime)) {
            break;
        }

        ++runtime.state.frame_index;
        ++result.frames_executed;
        append_frame_start_position_sync_events(runtime, result);
        for (const auto& thread : runtime.state.packed_thread_order) {
            auto* worker = find_queued_worker(runtime, thread.slot);
            if (worker == nullptr) {
                continue;
            }
            auto event = execute_worker_frame(runtime, *worker, nullptr);
            append_recorded_event(runtime, result, std::move(event));
        }
    }

    if (has_active_worker(runtime)) {
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
            append_frame_start_position_sync_events(runtime, result);
            auto event = execute_worker_frame(runtime, worker, nullptr);
            append_recorded_event(runtime, result, std::move(event));
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
    case BattleFrameWorkerKind::PassiveSpecial:
        return "PassiveSpecial";
    case BattleFrameWorkerKind::CleanupStanding:
        return "CleanupStanding";
    case BattleFrameWorkerKind::FrameStartPositionSync:
        return "FrameStartPositionSync";
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
    case BattleFrameActionMode::ActionMotionAltSpeed:
        return "action_motion_alt_speed";
    default:
        return "unknown";
    }
}

const char* battle_frame_worker_step_kind_name(BattleFrameWorkerStepKind kind) {
    switch (kind) {
    case BattleFrameWorkerStepKind::CallbackEntry:
        return "CallbackEntry";
    case BattleFrameWorkerStepKind::PathBuild:
        return "PathBuild";
    case BattleFrameWorkerStepKind::GridRefresh:
        return "GridRefresh";
    case BattleFrameWorkerStepKind::MovementCommit:
        return "MovementCommit";
    case BattleFrameWorkerStepKind::PostCommit:
        return "PostCommit";
    case BattleFrameWorkerStepKind::ModeHelper:
        return "ModeHelper";
    case BattleFrameWorkerStepKind::PassiveCleanup:
        return "PassiveCleanup";
    case BattleFrameWorkerStepKind::FrameStartPositionSync:
        return "FrameStartPositionSync";
    case BattleFrameWorkerStepKind::ActionMotionPositionSync:
        return "ActionMotionPositionSync";
    case BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc:
        return "ActionMotionSetup_8001fabc";
    case BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114:
        return "ActionMotionRotateStep_8001b630_80061114";
    case BattleFrameWorkerStepKind::ActionMotionMoveStep_8001e910:
        return "ActionMotionMoveStep_8001e910";
    case BattleFrameWorkerStepKind::MoveIncrementApply_80061340:
        return "MoveIncrementApply_80061340";
    case BattleFrameWorkerStepKind::Rng:
        return "Rng";
    case BattleFrameWorkerStepKind::Marker:
        return "Marker";
    case BattleFrameWorkerStepKind::NoCommit:
        return "NoCommit";
    case BattleFrameWorkerStepKind::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
