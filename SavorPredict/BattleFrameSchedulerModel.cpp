#include "BattleFrameSchedulerModel.h"

#include "ActionMotionTargetModel.h"
#include "ActionViewPathingTailModel.h"
#include "CombatantInstructionModeModel.h"
#include "EffectRngModel.h"
#include "RngCore.h"
#include "ViewPlacementGeometryModel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <utility>

namespace savor::predict {
namespace {

bool has_active_thread_for_slot(
    const BattleFrameRuntime& runtime,
    BattleFrameThreadNodeKind kind,
    int slot) {
    return std::any_of(
        runtime.thread_list.nodes.begin(),
        runtime.thread_list.nodes.end(),
        [kind, slot](const BattleFrameThreadNode& node) {
            return node.active && node.kind == kind && node.owner_slot == slot;
        });
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

BattleFrameWorkerKind frame_worker_kind(
    BattleMovementInvocationWorkerKind kind) {
    switch (kind) {
    case BattleMovementInvocationWorkerKind::ActivePcDirect:
        return BattleFrameWorkerKind::ActiveDirectAttack;
    case BattleMovementInvocationWorkerKind::ActivePcFallback:
        return BattleFrameWorkerKind::ActiveFallbackAttack;
    case BattleMovementInvocationWorkerKind::EnemyDirect:
        return BattleFrameWorkerKind::EnemyDirectAttack;
    case BattleMovementInvocationWorkerKind::EnemyFallback:
        return BattleFrameWorkerKind::EnemyFallbackAttack;
    case BattleMovementInvocationWorkerKind::PassiveController:
        return BattleFrameWorkerKind::PassiveController;
    case BattleMovementInvocationWorkerKind::None:
        return BattleFrameWorkerKind::None;
    }
    return BattleFrameWorkerKind::None;
}

BattleFrameMovementLegPolicy frame_leg_policy(
    BattleMovementInvocationLegPolicy policy) {
    switch (policy) {
    case BattleMovementInvocationLegPolicy::RebuildPath:
        return BattleFrameMovementLegPolicy::RebuildPath;
    case BattleMovementInvocationLegPolicy::AdvanceExistingPath:
        return BattleFrameMovementLegPolicy::AdvanceExistingPath;
    case BattleMovementInvocationLegPolicy::CompleteAfterLeg:
        return BattleFrameMovementLegPolicy::CompleteAfterLeg;
    }
    return BattleFrameMovementLegPolicy::CompleteAfterLeg;
}

BattleFrameEventStatus frame_event_status(
    BattleMovementInvocationStatus status) {
    switch (status) {
    case BattleMovementInvocationStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case BattleMovementInvocationStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case BattleMovementInvocationStatus::Skipped:
        return BattleFrameEventStatus::Skipped;
    case BattleMovementInvocationStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case BattleMovementInvocationStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    case BattleMovementInvocationStatus::Ambiguous:
        return BattleFrameEventStatus::Ambiguous;
    }
    return BattleFrameEventStatus::Ambiguous;
}

BattleFrameEventStatus frame_event_status(
    CombatantInstructionStdRowProducerStatus status) {
    switch (status) {
    case CombatantInstructionStdRowProducerStatus::Published:
    case CombatantInstructionStdRowProducerStatus::Unchanged:
        return BattleFrameEventStatus::Matched;
    case CombatantInstructionStdRowProducerStatus::DeferredState0:
        return BattleFrameEventStatus::Provisional;
    case CombatantInstructionStdRowProducerStatus::Idle:
        return BattleFrameEventStatus::Skipped;
    case CombatantInstructionStdRowProducerStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case CombatantInstructionStdRowProducerStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(ActionMotionPlaybackStatus status) {
    switch (status) {
    case ActionMotionPlaybackStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case ActionMotionPlaybackStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case ActionMotionPlaybackStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case ActionMotionPlaybackStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(ActionMotionInvocationStatus status) {
    switch (status) {
    case ActionMotionInvocationStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case ActionMotionInvocationStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case ActionMotionInvocationStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case ActionMotionInvocationStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(BattleTargetReactionStatus status) {
    switch (status) {
    case BattleTargetReactionStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case BattleTargetReactionStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case BattleTargetReactionStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case BattleTargetReactionStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(
    DirectInstructionTransitionStatus status) {
    switch (status) {
    case DirectInstructionTransitionStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case DirectInstructionTransitionStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case DirectInstructionTransitionStatus::Skipped:
        return BattleFrameEventStatus::Skipped;
    case DirectInstructionTransitionStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case DirectInstructionTransitionStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(BattleCollisionModelStatus status) {
    switch (status) {
    case BattleCollisionModelStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case BattleCollisionModelStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case BattleCollisionModelStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case BattleCollisionModelStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

CombatantInstructionActionKind instruction_action_kind(
    BattleMovementActionKind action_kind) {
    switch (action_kind) {
    case BattleMovementActionKind::BasicAttack:
        return CombatantInstructionActionKind::BasicAttack;
    case BattleMovementActionKind::Guard:
        return CombatantInstructionActionKind::Guard;
    case BattleMovementActionKind::Unknown:
        return CombatantInstructionActionKind::Unknown;
    }
    return CombatantInstructionActionKind::Unknown;
}

CombatantInstructionProducerFamily instruction_producer_family(
    BattleMovementControllerFamily family) {
    switch (family) {
    case BattleMovementControllerFamily::ActivePcDirect:
        return CombatantInstructionProducerFamily::ActivePcDirect;
    case BattleMovementControllerFamily::ActivePcFallback:
        return CombatantInstructionProducerFamily::ActivePcFallback;
    case BattleMovementControllerFamily::EnemyDirect:
    case BattleMovementControllerFamily::EnemyHandler:
        return CombatantInstructionProducerFamily::EnemyDirect;
    case BattleMovementControllerFamily::EnemyFallback:
        return CombatantInstructionProducerFamily::EnemyFallback;
    case BattleMovementControllerFamily::AmbientPursuit:
        return CombatantInstructionProducerFamily::AmbientPursuit;
    case BattleMovementControllerFamily::AmbientFormation:
        return CombatantInstructionProducerFamily::AmbientFormation;
    case BattleMovementControllerFamily::AffectedTargetReaction:
        return CombatantInstructionProducerFamily::AffectedTargetReaction;
    default:
        return CombatantInstructionProducerFamily::Unknown;
    }
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
    case BattleFrameWorkerKind::ViewPlacement:
    case BattleFrameWorkerKind::MechanicalAttack:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::EffectChunk:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::PassiveMovement:
        return BattleFrameActionMode::ActiveApproach;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        return BattleFrameActionMode::ActiveFallback;
    case BattleFrameWorkerKind::PassiveController:
        return worker.controller_family == BattleMovementControllerFamily::AmbientPursuit
            ? BattleFrameActionMode::ActiveApproach
            : worker.controller_family == BattleMovementControllerFamily::AmbientFormation
                ? BattleFrameActionMode::AmbientFormation
                : BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::VisualController:
    case BattleFrameWorkerKind::VisualActionService:
    case BattleFrameWorkerKind::VisualCollisionBox:
    case BattleFrameWorkerKind::VisualActionViewRecord:
    case BattleFrameWorkerKind::VisualUnsupportedCommand:
    case BattleFrameWorkerKind::CombatantInstruction:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::Cleanup:
    case BattleFrameWorkerKind::CleanupStanding:
    case BattleFrameWorkerKind::FrameStartPositionSync:
        return BattleFrameActionMode::Standing;
    case BattleFrameWorkerKind::None:
        return BattleFrameActionMode::Standing;
    }
    return BattleFrameActionMode::Standing;
}

std::int16_t action_motion_mode_for_worker(
    const BattleFrameWorker& worker,
    std::int16_t published_controller_mode) {
    if (!worker.complete
        && (worker.kind == BattleFrameWorkerKind::ActiveFallbackAttack
            || worker.kind == BattleFrameWorkerKind::EnemyFallbackAttack)
        && published_controller_mode == BattleFrameActionMode::ActiveFallback) {
        return BattleFrameActionMode::ActiveApproach;
    }
    return published_controller_mode;
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
    worker.movement_loop_start_index = worker.program_steps.size();
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

void add_path_node_selection_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::PathNodeSelection,
        0x8007fe0cu,
        0x8007fe0cu,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "path_node_selection_8007fe0c",
        "helper_pc=0x8007fe0c; straight-run selection from rebuilt path state");
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

void add_combatant_instruction_steps(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::CombatantInstructionPublish,
        worker.callback_pc,
        0x800221fcu,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "combatant_instruction_publish",
        "movement controller publishes mode/target state; persistent FUN_80022850 owns action-motion execution");
    add_step(
        worker,
        BattleFrameWorkerStepKind::CombatantInstructionWait,
        worker.callback_pc,
        0x80022850u,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "combatant_instruction_wait",
        "movement controller polls the persistent combatant instruction runtime before grid refresh or next-leg handling");
}

void add_next_leg_decision_step(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::NextLegOrRebuildDecision,
        0x8001b778u,
        0,
        std::nullopt,
        action_mode_for_worker(worker),
        -1,
        "movement_next_leg_or_rebuild",
        "rebuilds from current grid state; stable path indexes are not advanced blindly");
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

void add_active_fallback_lifecycle_steps(BattleFrameWorker& worker) {
    add_step(
        worker,
        BattleFrameWorkerStepKind::FallbackSetupWait,
        0x80085DA8u,
        0x8007FAB0u,
        std::nullopt,
        BattleFrameActionMode::ActiveFallback,
        1,
        "fallback_setup_wait_80085da8",
        "state=1; FUN_8007FAB0 target-readiness poll; provisional semantic gate uses completed passive family dispatch");
    add_step(
        worker,
        BattleFrameWorkerStepKind::FallbackMode7Publish,
        0x80085EA0u,
        0x80081168u,
        std::nullopt,
        BattleFrameActionMode::ActiveFallback,
        0x0B,
        "fallback_mode7_publish_80085ea0",
        "state=0x64 publishes actor instruction mode 7 and advances callback state to 0x0b");
    add_step(
        worker,
        BattleFrameWorkerStepKind::FallbackAttackResolutionWait,
        0x80085F0Cu,
        0x8007FFE8u,
        std::nullopt,
        BattleFrameActionMode::ActiveFallback,
        0x0B,
        "fallback_attack_resolution_wait_80085f0c",
        "state=0x0b polls mode 7; the frame model yields this slot to the mechanical attack worker until resolution is available");
    add_step(
        worker,
        BattleFrameWorkerStepKind::FallbackVisualCompletionWait,
        0x8008609Cu,
        0x8007FFE8u,
        std::nullopt,
        BattleFrameActionMode::ActiveFallback,
        0x0F,
        "fallback_visual_completion_wait_8008609c",
        "state=0x0f polls actor and target mode 0x16 until the post-attack completion boundary opens");
    add_step(
        worker,
        BattleFrameWorkerStepKind::FallbackTerminalHandoff,
        0x80086124u,
        0x8007FFE8u,
        std::nullopt,
        BattleFrameActionMode::Standing,
        0x10,
        "fallback_terminal_handoff_80086124",
        "state=0x10 completes the active callback and hands the thread to 0x80086c48");
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

void add_view_placement_step(BattleFrameWorker& worker) {
    const bool direct_view = worker.view_placement_publisher_source_id
        == ViewPlacementCacheSemanticSource::DirectViewPublication;
    add_step(
        worker,
        BattleFrameWorkerStepKind::ViewPlacementResolve,
        direct_view ? 0x800145bcu : 0x80012308u,
        direct_view ? 0x80014474u : 0x800121d8u,
        std::nullopt,
        BattleFrameActionMode::Standing,
        -1,
        direct_view
            ? "view_placement_direct_view"
            : "view_placement_end_turn",
        worker.view_placement_provenance);
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
    case BattleFrameWorkerKind::ViewPlacement:
        return "ViewPlacement";
    case BattleFrameWorkerKind::MechanicalAttack:
        return "MechanicalAttack";
    case BattleFrameWorkerKind::EffectChunk:
        return "EffectChunk";
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
    case BattleFrameWorkerKind::PassiveController:
        return battle_movement_controller_family_name(worker.controller_family);
    case BattleFrameWorkerKind::CombatantInstruction:
        return "FUN_80022850";
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
        || kind == BattleFrameWorkerKind::PassiveController
        || kind == BattleFrameWorkerKind::CleanupStanding
        || kind == BattleFrameWorkerKind::Cleanup;
}

bool continue_worker_in_same_thread_visit(
    const BattleFrameWorker& worker,
    BattleFrameWorkerStepKind executed_step) {
    if (worker.complete || worker.program_index >= worker.program_steps.size()) {
        return false;
    }
    const auto next_step = worker.program_steps[worker.program_index].kind;
    switch (executed_step) {
    case BattleFrameWorkerStepKind::CallbackEntry:
    case BattleFrameWorkerStepKind::PathBuild:
    case BattleFrameWorkerStepKind::PathNodeSelection:
    case BattleFrameWorkerStepKind::GridRefresh:
    case BattleFrameWorkerStepKind::NextLegOrRebuildDecision:
    case BattleFrameWorkerStepKind::ModeHelper:
        return true;
    case BattleFrameWorkerStepKind::MovementCommit:
        return next_step == BattleFrameWorkerStepKind::PostCommit;
    default:
        return false;
    }
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b);
std::optional<int> nearest_opposing_slot(
    const BattleFrameRuntime& runtime,
    int slot,
    int excluded_actor_slot,
    int excluded_target_slot);
std::vector<int> ordered_opposing_slots(
    const BattleFrameRuntime& runtime,
    int slot,
    int excluded_actor_slot,
    int excluded_target_slot);
std::optional<MovementGridPosition> provisional_formation_destination(
    const BattleFrameRuntime& runtime,
    int slot,
    std::string* diagnostic = nullptr);

std::string movement_grid_detail(const MovementGridPosition& grid) {
    std::ostringstream out;
    out << "(" << grid.grid_x << "," << grid.grid_z << ")";
    return out.str();
}

std::string active_grid_occupancy_detail(const BattleFrameState& state) {
    std::ostringstream out;
    bool first = true;
    for (std::size_t index = 0; index < state.active_grid.size(); ++index) {
        if (state.active_grid[index] == state.base_grid[index]) {
            continue;
        }
        if (!first) {
            out << " ";
        }
        first = false;
        out << "(" << (index % kBattleMovementGridWidth)
            << "," << (index / kBattleMovementGridWidth) << ")=0x"
            << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(state.active_grid[index]) << std::dec;
    }
    return first ? "none" : out.str();
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

BattleFrameEventStatus frame_event_status_from_path_model(
    BattleMovementPathModelStatus status) {
    switch (status) {
    case BattleMovementPathModelStatus::Exact:
        return BattleFrameEventStatus::Matched;
    case BattleMovementPathModelStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case BattleMovementPathModelStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case BattleMovementPathModelStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    case BattleMovementPathModelStatus::Unreachable:
        return BattleFrameEventStatus::Provisional;
    }
    return BattleFrameEventStatus::Ambiguous;
}

BattleMovementPathResult model_runtime_movement_path(
    const BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot,
    BattleMovementPathSelectionPolicy selection_policy) {
    const auto* actor = find_frame_combatant(runtime.state, actor_slot);
    const auto* target = find_frame_combatant(runtime.state, target_slot);
    if (actor == nullptr || target == nullptr) {
        return {};
    }
    return model_battle_movement_path(BattleMovementPathInput{
        .base_grid = runtime.state.base_grid,
        .active_grid = runtime.state.active_grid,
        .actor = BattleMovementPathCombatantInput{
            .slot = actor->slot,
            .present = actor->present,
            .alive = actor->alive,
            .movement_flags = actor->movement_flags,
            .width = actor->width,
            .depth = actor->depth,
            .current_grid = actor->grid_position,
            .previous_grid = actor->previous_grid_position,
            .queued_controller_state = actor->queued_controller_state,
        },
        .target = BattleMovementPathCombatantInput{
            .slot = target->slot,
            .present = target->present,
            .alive = target->alive,
            .movement_flags = target->movement_flags,
            .width = target->width,
            .depth = target->depth,
            .current_grid = target->grid_position,
            .previous_grid = target->previous_grid_position,
            .queued_controller_state = target->queued_controller_state,
        },
        .initial_path_index = 0,
        .selection_policy = selection_policy,
    });
}

void publish_worker_path_to_runtime_worksheet(
    BattleFrameRuntime& runtime,
    const BattleFrameWorker& worker) {
    if (worker.slot < 0
        || worker.slot >= static_cast<int>(runtime.movement_worksheets.size())
        || !worker.movement_path.available) {
        return;
    }
    auto& worksheet = runtime.movement_worksheets[
        static_cast<std::size_t>(worker.slot)];
    worksheet.initialized = true;
    worksheet.dist_to_target_0x14 = worker.movement_path.dist_to_target_0x14;
    worksheet.path_index_0x15 = worker.movement_path.path_index_0x15;
    worksheet.status_0x16 = worker.movement_path.status_0x16;
    for (std::size_t index = 0;
         index < worker.movement_path.entry_count
            && index < worksheet.raw_path_entries.size();
         ++index) {
        worksheet.raw_path_entries[index] = worker.movement_path.entries[index];
    }
    if (worker.controller_family != BattleMovementControllerFamily::AmbientFormation
        && worker.movement_path.terminator_seen
        && worker.movement_path.entry_count < worksheet.raw_path_entries.size()) {
        worksheet.raw_path_entries[worker.movement_path.entry_count].grid_x = -1;
    }
    worksheet.provenance =
        "frame-backed path publication retained as the slot-local movement worksheet state";
}

bool rebuild_worker_movement_path(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    std::string* detail) {
    auto& state = runtime.state;
    auto* combatant = find_frame_combatant(state, worker.slot);
    if (combatant == nullptr) {
        worker.event_status = BattleFrameEventStatus::MissingInput;
        if (detail != nullptr) {
            *detail = "movement path selection missing actor combatant";
        }
        return false;
    }

    const auto model_for_target = [&](int target_slot) {
        return model_runtime_movement_path(
            runtime,
            worker.slot,
            target_slot,
            worker.path_selection_policy);
    };

    std::optional<BattleMovementPathResult> selected_model;
    if (worker.controller_family == BattleMovementControllerFamily::AmbientPursuit) {
        const auto* locked_target = find_frame_combatant(state, worker.target_slot);
        if (worker.semantic_target_locked && locked_target != nullptr
            && locked_target->present && locked_target->alive) {
            selected_model = model_for_target(worker.target_slot);
        } else {
            worker.semantic_target_locked = false;
            const auto candidates = ordered_opposing_slots(
                runtime,
                worker.slot,
                runtime.active_action.has_value() ? runtime.active_action->actor_slot : -1,
                runtime.active_action.has_value() ? runtime.active_action->target_slot : -1);
            for (const int candidate_slot : candidates) {
                auto candidate = model_for_target(candidate_slot);
                if (candidate.status == BattleMovementPathModelStatus::Exact
                    || candidate.status == BattleMovementPathModelStatus::Provisional) {
                    worker.target_slot = candidate_slot;
                    worker.semantic_target_locked = true;
                    selected_model = std::move(candidate);
                    break;
                }
            }
        }
    } else if (find_frame_combatant(state, worker.target_slot) != nullptr) {
        selected_model = model_for_target(worker.target_slot);
    }
    if (!selected_model.has_value()) {
        worker.event_status = worker.controller_family
                == BattleMovementControllerFamily::AmbientPursuit
            ? BattleFrameEventStatus::Provisional
            : BattleFrameEventStatus::MissingInput;
        if (detail != nullptr) {
            *detail = worker.controller_family
                    == BattleMovementControllerFamily::AmbientPursuit
                ? "no reachable pursuit target; provisional FUN_8008BFDC handoff"
                : "movement path selection found no reachable target candidate";
        }
        return false;
    }
    const auto& modeled = *selected_model;

    worker.event_status = frame_event_status_from_path_model(modeled.status);
    worker.movement_path = BattleFrameMovementPathState{
        .available = modeled.status == BattleMovementPathModelStatus::Exact
            || modeled.status == BattleMovementPathModelStatus::Provisional,
        .dist_to_target_0x14 = modeled.distance,
        .path_index_0x15 = modeled.selected_path_index,
        .status_0x16 = modeled.reachability_status_0x16,
        .entry_count = modeled.entry_count,
        .terminator_seen = modeled.terminator_written,
        .zero_distance_target = modeled.reachability == MovementReachabilityStatus::Adjacent1,
    };
    for (std::size_t i = 0; i < modeled.entry_count; ++i) {
        worker.movement_path.entries[i] = modeled.path_entries[i];
    }
    worker.path_index_0x15 = modeled.selected_path_index;
    worker.destination_source = modeled.destination_source;

    std::ostringstream out;
    out << "path_model_status=" << battle_movement_path_model_status_name(modeled.status)
        << "; selected_target_slot=" << worker.target_slot
        << "; queued_controller_state=" << combatant->queued_controller_state
        << "; reachability_status_0x16=" << static_cast<int>(modeled.reachability_status_0x16)
        << "; distance=" << static_cast<int>(modeled.distance)
        << "; selected_index=" << static_cast<int>(modeled.selected_path_index)
        << "; destination_source="
        << movement_commit_destination_source_name(modeled.destination_source)
        << "; confidence=" << modeled.confidence
        << "; provenance=" << modeled.provenance;
    if (detail != nullptr) {
        *detail = out.str();
    }

    if (!modeled.selected_path_node.has_value()) {
        worker.destination_grid = combatant->grid_position;
        worker.destination_position = combatant->pos_holder;
        return false;
    }

    worker.destination_grid = *modeled.selected_path_node;
    worker.destination_position = first_battle_grid_to_raw_stage_position(
        worker.destination_grid,
        combatant->width,
        combatant->depth);
    return true;
}

void set_worker_commit_callsite(BattleFrameWorker& worker, std::uint32_t callsite_pc) {
    worker.commit_callsite_pc = callsite_pc;
    const auto commit = std::find_if(
        worker.program_steps.begin(),
        worker.program_steps.end(),
        [](const BattleFrameWorkerProgramStep& step) {
            return step.kind == BattleFrameWorkerStepKind::MovementCommit;
        });
    if (commit == worker.program_steps.end()) {
        return;
    }
    commit->pc = callsite_pc;
    commit->commit_callsite_pc = callsite_pc;
}

std::optional<std::size_t> path_node_selection_step_index(
    const BattleFrameWorker& worker) {
    const auto selected = std::find_if(
        worker.program_steps.begin(),
        worker.program_steps.end(),
        [](const BattleFrameWorkerProgramStep& step) {
            return step.kind == BattleFrameWorkerStepKind::PathNodeSelection;
        });
    if (selected == worker.program_steps.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(worker.program_steps.begin(), selected));
}

void build_static_worker_program(BattleFrameWorker& worker) {
    switch (worker.kind) {
    case BattleFrameWorkerKind::ActiveDirectAttack:
        worker.movement_leg_policy =
            BattleFrameMovementLegPolicy::AdvanceExistingPath;
        worker.callback_pc = 0x80086308u;
        add_callback_entry_step(worker);
        add_path_build_step(worker);
        add_path_node_selection_step(worker);
        add_commit_step(
            worker,
            0x80086480u,
            3,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80086308; candidate_commits=0x80086480,0x80086698; selected=0x80086480; state_after=3");
        add_post_commit_step(worker, 3);
        add_combatant_instruction_steps(worker);
        add_grid_refresh_step(worker);
        add_next_leg_decision_step(worker);
        break;
    case BattleFrameWorkerKind::ActiveFallbackAttack:
        worker.callback_pc = 0x80085ce0u;
        add_callback_entry_step(worker);
        add_active_fallback_lifecycle_steps(worker);
        break;
    case BattleFrameWorkerKind::EnemyDirectAttack:
        worker.movement_leg_policy =
            BattleFrameMovementLegPolicy::AdvanceExistingPath;
        worker.callback_pc = 0x80087f6cu;
        add_callback_entry_step(worker);
        add_path_build_step(worker);
        add_path_node_selection_step(worker);
        add_commit_step(
            worker,
            0x8008816cu,
            2,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80087f6c; candidate_commits=0x8008816c,0x800883cc,0x80088434; selected=0x8008816c; state_after=2");
        add_post_commit_step(worker, 2);
        add_combatant_instruction_steps(worker);
        add_grid_refresh_step(worker);
        add_next_leg_decision_step(worker);
        break;
    case BattleFrameWorkerKind::EnemyFallbackAttack:
        worker.callback_pc = 0x80087844u;
        add_callback_entry_step(worker);
        add_path_build_step(worker);
        add_path_node_selection_step(worker);
        add_commit_step(
            worker,
            0x800879a8u,
            0x65,
            "helper_pc=0x8008178c; posHolder publish; caller=0x80087844; selected=0x800879a8; state_after=0x65");
        add_post_commit_step(worker, 0x65);
        add_combatant_instruction_steps(worker);
        add_grid_refresh_step(worker);
        add_next_leg_decision_step(worker);
        break;
    case BattleFrameWorkerKind::PassiveController:
        if (worker.controller_family == BattleMovementControllerFamily::AmbientPursuit) {
            worker.path_selection_policy =
                BattleMovementPathSelectionPolicy::NextPathingGridSquare;
            worker.movement_leg_policy = BattleFrameMovementLegPolicy::RebuildPath;
            worker.callback_pc = 0x8008c21cu;
            add_callback_entry_step(worker);
            add_step(
                worker,
                BattleFrameWorkerStepKind::CallbackEntry,
                0x8008c4c0u,
                0,
                std::nullopt,
                BattleFrameActionMode::ActiveApproach,
                -1,
                "ambient_pursuit_commit_helper_8008c4c0",
                "dispatch=0x8008c21c->0x8008c4c0");
            add_path_build_step(worker);
            add_path_node_selection_step(worker);
            add_commit_step(
                worker,
                0x8008c67cu,
                2,
                "helper_pc=0x8008178c; caller=0x8008c4c0; family=ambient_pursuit; state_after=2");
            add_post_commit_step(worker, 2);
            add_combatant_instruction_steps(worker);
            add_grid_refresh_step(worker);
            add_next_leg_decision_step(worker);
            add_passive_cleanup_step(worker);
        } else if (worker.controller_family == BattleMovementControllerFamily::AmbientFormation) {
            worker.path_selection_policy =
                BattleMovementPathSelectionPolicy::NextPathingGridSquare;
            worker.movement_leg_policy = BattleFrameMovementLegPolicy::RebuildPath;
            worker.callback_pc = 0x8008c7b0u;
            add_callback_entry_step(worker);
            add_path_build_step(worker);
            add_path_node_selection_step(worker);
            add_commit_step(
                worker,
                0x8008c844u,
                -1,
                "helper_pc=0x8008178c; caller=0x8008c7b0; candidate_commits=0x8008c844,0x8008c920; family=ambient_formation");
            add_combatant_instruction_steps(worker);
            add_grid_refresh_step(worker);
            add_next_leg_decision_step(worker);
            add_mode_helper_step(worker);
            add_passive_cleanup_step(worker);
        } else {
            add_callback_entry_step(worker);
            add_marker_step(
                worker,
                "passive_family_wait",
                "callback family has no movement primitive; completion remains action-local and RNG-free");
            add_passive_cleanup_step(worker);
        }
        break;
    case BattleFrameWorkerKind::ActionView:
        worker.callback_pc = 0x80014474u;
        add_view_placement_step(worker);
        add_rng_step(worker, worker.detail);
        break;
    case BattleFrameWorkerKind::ViewPlacement:
        worker.callback_pc = 0x800121d8u;
        add_view_placement_step(worker);
        break;
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
    case BattleFrameWorkerKind::FrameStartPositionSync:
        worker.complete = true;
        break;
    case BattleFrameWorkerKind::CombatantInstruction:
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

    build_static_worker_program(worker);
    return worker;
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b) {
    return a.grid_x == b.grid_x && a.grid_z == b.grid_z;
}

bool movement_footprints_adjacent(
    const BattleFrameCombatantState& actor,
    const BattleFrameCombatantState& target) {
    const int actor_left = actor.grid_position.grid_x;
    const int actor_right = actor_left + std::max(1, actor.width) - 1;
    const int actor_top = actor.grid_position.grid_z;
    const int actor_bottom = actor_top + std::max(1, actor.depth) - 1;
    const int target_left = target.grid_position.grid_x;
    const int target_right = target_left + std::max(1, target.width) - 1;
    const int target_top = target.grid_position.grid_z;
    const int target_bottom = target_top + std::max(1, target.depth) - 1;
    const int gap_x = std::max({
        target_left - actor_right - 1,
        actor_left - target_right - 1,
        0});
    const int gap_z = std::max({
        target_top - actor_bottom - 1,
        actor_top - target_bottom - 1,
        0});
    return gap_x == 0 && gap_z == 0;
}

bool same_vec(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool fun_80080438_allows_completion(const BattleFrameActionRuntime& action) {
    return action.completion_override
        || action.completion_turn_phase == 3
        || action.completion_turn_phase == 5
        || action.completion_turn_phase == 6;
}

bool passive_dispatch_ready_for_active_fallback(
    const BattleFrameRuntime& runtime,
    int action_ordinal) {
    return std::all_of(
        runtime.passive_participants.begin(),
        runtime.passive_participants.end(),
        [action_ordinal](const BattleFramePassiveParticipantRuntime& participant) {
            if (!participant.active || participant.action_ordinal != action_ordinal) {
                return true;
            }
            switch (participant.phase) {
            case BattleFramePassiveParticipantPhase::FamilyPending:
            case BattleFramePassiveParticipantPhase::FamilyActive:
            case BattleFramePassiveParticipantPhase::CompletionDeferred:
            case BattleFramePassiveParticipantPhase::Cleared:
            case BattleFramePassiveParticipantPhase::Removed:
                return true;
            default:
                return false;
            }
        });
}

std::optional<std::size_t> passive_cleanup_step_index(
    const BattleFrameWorker& worker) {
    const auto cleanup = std::find_if(
        worker.program_steps.begin(),
        worker.program_steps.end(),
        [](const BattleFrameWorkerProgramStep& step) {
            return step.kind == BattleFrameWorkerStepKind::PassiveCleanup;
        });
    if (cleanup == worker.program_steps.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(worker.program_steps.begin(), cleanup));
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
    const BattleFrameVec3& provisional_fallback_target,
    BattleFrameStepEvent& event) {
    const auto target_result = select_action_motion_target(ActionMotionTargetInput{
        .actor_slot = combatant.slot,
        .action_mode = combatant.combatant_action_mode,
        .own_pos_holder = combatant.pos_holder,
        .provisional_fallback = provisional_fallback_target,
    });
    if (target_result.target.has_value()) {
        combatant.pos_to_move_to_0x110 = *target_result.target;
    }
    if (target_result.status == ActionMotionTargetStatus::MissingInput) {
        event.status = BattleFrameEventStatus::MissingInput;
    } else if (target_result.status == ActionMotionTargetStatus::Unsupported) {
        event.status = BattleFrameEventStatus::Unsupported;
    } else if (target_result.status == ActionMotionTargetStatus::Provisional
        && event.status == BattleFrameEventStatus::Matched) {
        event.status = BattleFrameEventStatus::Provisional;
    }
    combatant.selected_motion_speed = selected_motion_speed_for_mode(combatant);
    event.selected_motion_speed = combatant.selected_motion_speed;
    combatant.turn_current_degrees_0x11c =
        battle_frame_angle_short_to_degrees_8006116c(combatant.combatant_facing_angle_0x2c);
    combatant.turn_target_degrees_0x120 = battle_frame_target_facing_degrees_xz(
        combatant.combatant_cur_pos_0x1c,
        combatant.pos_to_move_to_0x110,
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
        + "; action_motion_target_status="
        + action_motion_target_status_name(target_result.status)
        + "; action_motion_target_source="
        + action_motion_target_source_name(target_result.source)
        + "; action_motion_target_confidence=" + target_result.confidence
        + "; action_motion_target_provenance=" + target_result.provenance
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
        || kind == BattleFrameWorkerKind::EffectChunk;
}

ViewPlacementGeometryInput provisional_view_placement_geometry(
    const BattleFrameState& state) {
    ViewPlacementGeometryInput input;
    for (const auto& combatant : state.combatants) {
        if (!combatant.present
            || combatant.slot < 0
            || combatant.slot >= static_cast<int>(input.combatants.size())) {
            continue;
        }
        input.combatants[static_cast<std::size_t>(combatant.slot)] =
            ViewPlacementGeometryCombatantState{
                .instruction_flags = combatant.instruction_flags_0xec,
                .current_position = ViewPlacementGeometryVectorBits{
                    .x_bits = std::bit_cast<std::uint32_t>(combatant.combatant_cur_pos_0x1c.x),
                    .y_bits = std::bit_cast<std::uint32_t>(combatant.combatant_cur_pos_0x1c.y),
                    .z_bits = std::bit_cast<std::uint32_t>(combatant.combatant_cur_pos_0x1c.z),
                },
                .geometry_extent_bits = std::bit_cast<std::uint32_t>(15.0f),
                .mld_slot_result = static_cast<std::int8_t>(0),
            };
    }
    return input;
}

const char* view_placement_read_status_name(ViewPlacementCacheReadStatus status) {
    switch (status) {
    case ViewPlacementCacheReadStatus::Hit:
        return "Hit";
    case ViewPlacementCacheReadStatus::Miss:
        return "Miss";
    case ViewPlacementCacheReadStatus::Unknown:
        return "Unknown";
    case ViewPlacementCacheReadStatus::MissingInput:
        return "MissingInput";
    case ViewPlacementCacheReadStatus::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

void apply_view_placement_worker(
    BattleFrameRuntime& runtime,
    BattleFrameStepEvent& event,
    BattleFrameWorker& worker,
    std::uint32_t& rng_state) {
    bool bootstrapped_zero_cache = false;
    if (runtime.view_placement_cache.state.knowledge
        == ViewPlacementCacheKnowledge::Uninitialized) {
        const auto reset = reset_active_record_view_placement_cache(
            runtime.view_placement_cache,
            ViewPlacementCacheEventContext{
                .frame_index = static_cast<std::uint64_t>(runtime.state.frame_index),
                .worker_sequence = worker.queue_sequence,
            },
            "provisional frame predictor coherent-zero initial cache assumption");
        bootstrapped_zero_cache =
            reset.status == ViewPlacementCacheMutationStatus::Applied;
    }

    const auto geometry = model_view_placement_geometry(
        provisional_view_placement_geometry(runtime.state));
    const auto resolved = resolve_geometry_backed_view_placement(
        runtime.view_placement_cache,
        rng_state,
        geometry,
        GeometryBackedViewPlacementRequest{
            .readiness = ViewPlacementReadiness::Ready,
            .publisher_source_id = worker.view_placement_publisher_source_id,
            .context = ViewPlacementCacheEventContext{
                .frame_index = static_cast<std::uint64_t>(runtime.state.frame_index),
                .worker_sequence = worker.queue_sequence,
            },
            .provenance = worker.view_placement_provenance,
        });

    event.rng_seed_before = resolved.seed_before;
    event.rng_seed_after = resolved.seed_after;
    event.draws_consumed = resolved.draws_consumed;
    event.rand_value = resolved.rand_value;
    event.rng_event = resolved.draws_consumed > 0;
    event.rng_label = worker.kind == BattleFrameWorkerKind::ActionView
        ? "view_placement_direct_view"
        : "view_placement_end_turn";
    if (resolved.status == ViewPlacementCacheReadStatus::MissingInput) {
        event.status = BattleFrameEventStatus::MissingInput;
    } else if (resolved.status == ViewPlacementCacheReadStatus::Unsupported) {
        event.status = BattleFrameEventStatus::Unsupported;
    } else if (resolved.status == ViewPlacementCacheReadStatus::Unknown) {
        event.status = BattleFrameEventStatus::Ambiguous;
    } else {
        event.status = BattleFrameEventStatus::Provisional;
    }

    std::ostringstream detail;
    detail << event.detail
           << "; view_placement_status=" << view_placement_read_status_name(resolved.status)
           << "; view_placement_source=" << worker.view_placement_publisher_source_id
           << "; cache_revision=" << resolved.revision_before << "->" << resolved.revision_after
           << "; readiness_assumption=ready"
           << "; geometry_position_source=current"
           << "; geometry_extent_assumption=15.0"
           << "; mld_slot_assumption=included"
           << "; initial_zero_cache_assumption=" << (bootstrapped_zero_cache ? 1 : 0);
    if (resolved.angle_bits.has_value()) {
        detail << "; angle_bits=0x" << std::hex << std::setw(8)
               << std::setfill('0') << std::nouppercase << *resolved.angle_bits;
    }
    if (!resolved.provenance.empty()) {
        detail << "; provenance=" << resolved.provenance;
    }
    event.detail = detail.str();
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
    runtime.pending_movement_invocations.clear();
    runtime.next_worker_sequence = 0;
    runtime.next_action_ordinal = 0;
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

int find_queued_worker_index(const BattleFrameRuntime& runtime, int slot) {
    if (slot < 0 || slot >= static_cast<int>(runtime.slot_worker_queues.size())) {
        return -1;
    }
    int selected_index = -1;
    int selected_sequence = 0;
    int waiting_index = -1;
    int waiting_sequence = 0;
    const bool resolution_available = runtime.active_action.has_value()
        && runtime.active_action->action_resolution_available;
    const bool completion_gate_open = runtime.active_action.has_value()
        && runtime.active_action->completion_gate_open;
    for (const int index : runtime.slot_worker_queues[static_cast<std::size_t>(slot)]) {
        if (index < 0 || index >= static_cast<int>(runtime.workers.size())) {
            continue;
        }
        const auto& worker = runtime.workers[static_cast<std::size_t>(index)];
        if (worker.complete) {
            continue;
        }
        const bool waiting =
            (worker.waiting_for_action_resolution && !resolution_available)
            || (worker.waiting_for_action_completion && !completion_gate_open);
        if (waiting) {
            if (waiting_index < 0 || worker.queue_sequence < waiting_sequence) {
                waiting_index = index;
                waiting_sequence = worker.queue_sequence;
            }
            continue;
        }
        if (selected_index < 0 || worker.queue_sequence < selected_sequence) {
            selected_index = index;
            selected_sequence = worker.queue_sequence;
        }
    }
    return selected_index >= 0 ? selected_index : waiting_index;
}

BattleFrameWorker* find_queued_worker(BattleFrameRuntime& runtime, int slot) {
    const int index = find_queued_worker_index(runtime, slot);
    return index < 0 ? nullptr : &runtime.workers[static_cast<std::size_t>(index)];
}

bool has_active_worker(const BattleFrameRuntime& runtime) {
    const bool resolution_available = runtime.active_action.has_value()
        && runtime.active_action->action_resolution_available;
    const bool completion_gate_open = runtime.active_action.has_value()
        && runtime.active_action->completion_gate_open;
    return std::any_of(
        runtime.workers.begin(),
        runtime.workers.end(),
        [resolution_available, completion_gate_open](const BattleFrameWorker& worker) {
            return !worker.complete
                && (!worker.waiting_for_action_resolution || resolution_available)
                && (!worker.waiting_for_action_completion || completion_gate_open);
        });
}

bool has_active_combatant_instruction(const BattleFrameRuntime& runtime) {
    return std::any_of(
        runtime.combatant_instructions.begin(),
        runtime.combatant_instructions.end(),
        [](const BattleFrameCombatantInstructionRuntime& instruction) {
            return instruction.active
                && instruction.phase != BattleFrameCombatantInstructionPhase::Complete
                && instruction.phase != BattleFrameCombatantInstructionPhase::Removed
                && instruction.phase != BattleFrameCombatantInstructionPhase::Unsupported;
        });
}

bool is_visual_step_kind(BattleFrameWorkerStepKind kind) {
    switch (kind) {
    case BattleFrameWorkerStepKind::VisualControllerVisit:
    case BattleFrameWorkerStepKind::VisualInstructionDecision:
    case BattleFrameWorkerStepKind::VisualInstructionStatePublish:
    case BattleFrameWorkerStepKind::TargetReactionPublish:
    case BattleFrameWorkerStepKind::DirectTransitionSelect:
    case BattleFrameWorkerStepKind::CollisionOccupancyRefresh:
    case BattleFrameWorkerStepKind::CollisionProbe:
    case BattleFrameWorkerStepKind::InstructionCallbackControlReset:
    case BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume:
    case BattleFrameWorkerStepKind::ActionMotionInvocationDecision:
    case BattleFrameWorkerStepKind::ActionMotionPlaybackInstall:
    case BattleFrameWorkerStepKind::ActionMotionRendererAdvance:
    case BattleFrameWorkerStepKind::ActionMotionState6Poll:
    case BattleFrameWorkerStepKind::ActionMotionPostState6Delay:
    case BattleFrameWorkerStepKind::ActionMotionPublicationRelease:
    case BattleFrameWorkerStepKind::VisualStdRowProducerVisit:
    case BattleFrameWorkerStepKind::VisualInstructionInstall:
    case BattleFrameWorkerStepKind::VisualAuxiliaryPublication:
    case BattleFrameWorkerStepKind::VisualCommandPublish:
    case BattleFrameWorkerStepKind::VisualChildState0:
    case BattleFrameWorkerStepKind::VisualChildDelay:
    case BattleFrameWorkerStepKind::VisualChildNested:
    case BattleFrameWorkerStepKind::VisualChildCleanup:
    case BattleFrameWorkerStepKind::VisualMode0Rewrite:
    case BattleFrameWorkerStepKind::VisualMode0eCamera:
    case BattleFrameWorkerStepKind::VisualMode1Pathing:
    case BattleFrameWorkerStepKind::VisualUnsupportedWait:
        return true;
    default:
        return false;
    }
}

void append_recorded_event(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameStepEvent event) {
    runtime.last_step_events.push_back(event);
    runtime.history.push_back(event);
    if (is_visual_step_kind(event.step_kind)) {
        runtime.visual.history.push_back(event);
    }
    result.events.push_back(std::move(event));
}

BattleFrameEventStatus frame_event_status(CombatantVisualModelStatus status) {
    switch (status) {
    case CombatantVisualModelStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case CombatantVisualModelStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case CombatantVisualModelStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case CombatantVisualModelStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(
    CombatantAuxiliaryPublicationStatus status) {
    switch (status) {
    case CombatantAuxiliaryPublicationStatus::Matched:
        return BattleFrameEventStatus::Matched;
    case CombatantAuxiliaryPublicationStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case CombatantAuxiliaryPublicationStatus::Skipped:
        return BattleFrameEventStatus::Skipped;
    case CombatantAuxiliaryPublicationStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case CombatantAuxiliaryPublicationStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

BattleFrameEventStatus frame_event_status(ActionViewPathingTailStatus status) {
    switch (status) {
    case ActionViewPathingTailStatus::Exact:
        return BattleFrameEventStatus::Matched;
    case ActionViewPathingTailStatus::Provisional:
        return BattleFrameEventStatus::Provisional;
    case ActionViewPathingTailStatus::Skipped:
        return BattleFrameEventStatus::Skipped;
    case ActionViewPathingTailStatus::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    case ActionViewPathingTailStatus::Ambiguous:
        return BattleFrameEventStatus::Ambiguous;
    case ActionViewPathingTailStatus::Unsupported:
        return BattleFrameEventStatus::Unsupported;
    }
    return BattleFrameEventStatus::Unsupported;
}

const CombatantVisualResource* visual_resource_for(
    const BattleFrameRuntime& runtime,
    int slot) {
    if (slot < 0 || slot >= static_cast<int>(runtime.visual.resources.size())) {
        return nullptr;
    }
    const auto& resource = runtime.visual.resources[static_cast<std::size_t>(slot)];
    return resource.has_value() ? &*resource : nullptr;
}

CombatantVisualResource* visual_resource_for(
    BattleFrameRuntime& runtime,
    int slot) {
    if (slot < 0 || slot >= static_cast<int>(runtime.visual.resources.size())) {
        return nullptr;
    }
    auto& resource = runtime.visual.resources[static_cast<std::size_t>(slot)];
    return resource.has_value() ? &*resource : nullptr;
}

std::optional<CombatantStdActionRow> selected_action_row_for(
    const CombatantVisualResource* resource,
    const BattleFrameCombatantState& combatant) {
    if (!combatant.selected_action_row_known
        || combatant.selected_action_row_index < 0) {
        return std::nullopt;
    }
    if (resource != nullptr) {
        const auto found = std::find_if(
            resource->action_rows.begin(),
            resource->action_rows.end(),
            [&](const CombatantStdActionRow& row) {
                return row.index == combatant.selected_action_row_index;
            });
        if (found != resource->action_rows.end()) {
            auto row = *found;
            if (combatant.selected_action_row_duration_known
                && combatant.selected_action_row_action_id == row.action_id) {
                row.transition_gate_divisor_bits =
                    combatant.selected_action_row_duration_bits;
            }
            if (combatant.selected_action_row_callback_index >= 0) {
                row.callback_index =
                    combatant.selected_action_row_callback_index;
            }
            if (combatant.selected_action_row_callback_ordinal >= 0) {
                row.callback_ordinal =
                    combatant.selected_action_row_callback_ordinal;
            }
            return row;
        }
    }
    return CombatantStdActionRow{
        .index = combatant.selected_action_row_index,
        .action_id = combatant.selected_action_row_action_id,
        .callback_index = combatant.selected_action_row_callback_index,
        .callback_ordinal = combatant.selected_action_row_callback_ordinal,
        .flags = combatant.selected_action_row_flags,
        .transition_gate_divisor_bits =
            combatant.selected_action_row_duration_bits,
    };
}

void apply_selected_action_row(
    BattleFrameCombatantState& combatant,
    const CombatantStdActionRow& row,
    std::int16_t selected_action_id,
    bool duration_known) {
    combatant.selected_action_row_index = row.index;
    combatant.selected_action_row_flags = row.flags;
    combatant.selected_action_row_action_id = selected_action_id;
    combatant.selected_action_row_callback_index = row.callback_index;
    combatant.selected_action_row_callback_ordinal = row.callback_ordinal;
    combatant.selected_action_row_known = true;
    combatant.selected_action_row_duration_bits =
        row.transition_gate_divisor_bits;
    combatant.selected_action_row_duration_known = duration_known;
}

const char* callback_publication_source_name(
    BattleFrameInstructionCallbackPublicationSource source) {
    switch (source) {
    case BattleFrameInstructionCallbackPublicationSource::State0Initialization:
        return "State0Initialization";
    case BattleFrameInstructionCallbackPublicationSource::State1CurrentInstruction:
        return "State1CurrentInstruction";
    case BattleFrameInstructionCallbackPublicationSource::State1QueuedTransition:
        return "State1QueuedTransition";
    case BattleFrameInstructionCallbackPublicationSource::ExplicitModeledTransition:
        return "ExplicitModeledTransition";
    }
    return "State1CurrentInstruction";
}

ActionMotionDelayTable action_motion_delay_table_for(
    const CombatantVisualResource* resource) {
    ActionMotionDelayTable table;
    if (resource == nullptr) {
        return table;
    }
    table.table_known = true;
    table.includes_sentinel = resource->includes_sentinel;
    table.descriptors.reserve(resource->records.size());
    for (const auto& record : resource->records) {
        table.descriptors.push_back({
            .record_index = record.index,
            .location_code = record.location_code,
            .combined_type = record.combined_type,
            .payload_size = record.payload_size,
            .payload_in_bounds = record.payload_in_bounds,
            .payload_bytes = record.payload_bytes,
        });
    }
    return table;
}

ActionMotionInstructionGateInput action_motion_delay_gate_input_for(
    const BattleFrameCombatantState* combatant) {
    ActionMotionInstructionGateInput input;
    if (combatant == nullptr
        || combatant->visual_instruction_knowledge
            == CombatantVisualInstructionKnowledge::Unknown) {
        return input;
    }
    input.current_action_key = combatant->visual_instruction_mode_0x6;
    input.current_secondary_key = combatant->visual_instruction_subtype_0x8;
    input.instruction_flags_0xec = combatant->instruction_flags_0xec;
    input.alternate_a_action_key =
        combatant->visual_instruction_alternate_a_mode_0x4a;
    input.alternate_a_secondary_key =
        combatant->visual_instruction_alternate_a_subtype_0x4c;
    input.alternate_b_action_key =
        combatant->visual_instruction_alternate_b_mode_0x56;
    input.alternate_b_secondary_key =
        combatant->visual_instruction_alternate_b_subtype_0x58;
    return input;
}

int normalized_camera_duration(const CombatantVisualSystemCameraPayload& camera) {
    std::uint32_t duration = camera.end_frame;
    if (duration >= 16U && (duration % 16U) == 0U) {
        duration /= 16U;
    }
    return static_cast<int>(std::clamp<std::uint32_t>(duration == 0 ? 1U : duration, 1U, 256U));
}

BattleFrameStepEvent make_visual_event(
    const BattleFrameRuntime& runtime,
    const BattleFrameVisualChildTask* task,
    BattleFrameWorkerStepKind step_kind,
    std::string callback,
    BattleFrameEventStatus status,
    std::string detail) {
    BattleFrameStepEvent event;
    event.frame_index = runtime.state.frame_index;
    event.step_kind = step_kind;
    event.callback = std::move(callback);
    event.status = status;
    if (task != nullptr) {
        event.action_ordinal = task->action_ordinal;
        event.slot = task->origin_slot;
        event.target_slot = task->target_slot;
        switch (task->kind) {
        case BattleFrameVisualChildKind::ActionService:
            event.worker_kind = BattleFrameWorkerKind::VisualActionService;
            break;
        case BattleFrameVisualChildKind::CollisionBox:
            event.worker_kind = BattleFrameWorkerKind::VisualCollisionBox;
            break;
        case BattleFrameVisualChildKind::ActionViewRecord:
            event.worker_kind = BattleFrameWorkerKind::VisualActionViewRecord;
            break;
        case BattleFrameVisualChildKind::UnsupportedCommand:
            event.worker_kind = BattleFrameWorkerKind::VisualUnsupportedCommand;
            break;
        }
        event.visual_command_kind = task->command_kind;
        event.visual_resource = task->resource_stem;
        event.visual_record_index = task->record_index;
        event.visual_epoch = task->publication_epoch;
        event.visual_task_sequence = task->sequence;
        event.visual_payload_mode = task->payload_mode;
        event.visual_effective_mode = task->effective_mode;
        event.visual_child_kind = battle_frame_visual_child_kind_name(task->kind);
    } else {
        event.worker_kind = BattleFrameWorkerKind::VisualController;
    }
    if (runtime.active_action.has_value()) {
        event.action_phase = runtime.active_action->phase;
        event.passive_completion_mask_before =
            runtime.active_action->passive_completion_mask;
        event.passive_completion_mask_after =
            runtime.active_action->passive_completion_mask;
    }
    event.detail = std::move(detail);
    return event;
}

BattleFrameThreadCallbackIdentity visual_child_callback_identity(
    CombatantVisualCommandKind kind) {
    switch (kind) {
    case CombatantVisualCommandKind::SetCommand:
        return BattleFrameThreadCallbackIdentity::VisualSetCommand;
    case CombatantVisualCommandKind::MoveModel:
        return BattleFrameThreadCallbackIdentity::VisualMoveModel;
    case CombatantVisualCommandKind::PutModel:
        return BattleFrameThreadCallbackIdentity::VisualPutModel;
    case CombatantVisualCommandKind::HitWeapon:
        return BattleFrameThreadCallbackIdentity::VisualHitWeapon;
    case CombatantVisualCommandKind::CollisionBox:
        return BattleFrameThreadCallbackIdentity::VisualCollisionBox;
    case CombatantVisualCommandKind::MotionPause:
        return BattleFrameThreadCallbackIdentity::VisualMotionPause;
    case CombatantVisualCommandKind::PointLight:
        return BattleFrameThreadCallbackIdentity::VisualPointLight;
    case CombatantVisualCommandKind::SystemCamera:
    case CombatantVisualCommandKind::SyntheticActionView:
        return BattleFrameThreadCallbackIdentity::VisualSystemCamera;
    case CombatantVisualCommandKind::Unknown:
        return BattleFrameThreadCallbackIdentity::Unknown;
    }
    return BattleFrameThreadCallbackIdentity::Unknown;
}

const char* visual_child_handler_name(CombatantVisualCommandKind kind) {
    switch (kind) {
    case CombatantVisualCommandKind::SetCommand:
        return "SetCommandHandler_8004281C";
    case CombatantVisualCommandKind::MoveModel:
        return "MoveModelHandler_80044B24";
    case CombatantVisualCommandKind::PutModel:
        return "PutmodelHandler_80045CD8";
    case CombatantVisualCommandKind::HitWeapon:
        return "HitWeaponHandler_8004A40C";
    case CombatantVisualCommandKind::CollisionBox:
        return "CollisionBoxHandler_8004B7C4";
    case CombatantVisualCommandKind::MotionPause:
        return "MotionPauseHandler_8004C070";
    case CombatantVisualCommandKind::PointLight:
        return "PointLightHandler_8004F868";
    case CombatantVisualCommandKind::SystemCamera:
    case CombatantVisualCommandKind::SyntheticActionView:
        return "SystemCameraHandler_80051264";
    case CombatantVisualCommandKind::Unknown:
        return "DispatchVisualCommand_800367E8_unsupported";
    }
    return "DispatchVisualCommand_800367E8_unsupported";
}

int enqueue_visual_child(
    BattleFrameRuntime& runtime,
    const CombatantVisualPublication& publication,
    std::optional<int> origin_parent_node_id = std::nullopt) {
    if (publication.record == nullptr) {
        return -1;
    }
    BattleFrameVisualChildTask task;
    task.sequence = runtime.visual.next_child_sequence++;
    task.action_ordinal = publication.slot >= 0
            && publication.slot < static_cast<int>(runtime.visual.timeline_action_ordinals.size())
        ? runtime.visual.timeline_action_ordinals[static_cast<std::size_t>(publication.slot)]
        : -1;
    task.origin_slot = publication.slot;
    task.target_slot = publication.target_slot;
    task.record_index = publication.record_index;
    task.publication_epoch = publication.epoch;
    task.command_kind = publication.kind;
    task.publication_frame = runtime.state.frame_index;
    task.first_eligible_frame = runtime.state.frame_index;
    task.publication_visit_cursor = runtime.visual.current_visit_cursor;
    task.participates_in_action_barrier = true;
    task.status = publication.status;
    task.provenance = publication.provenance;
    if (const auto* resource = visual_resource_for(runtime, publication.slot)) {
        task.resource_stem = resource->binding.resource_stem;
    }
    if (publication.kind == CombatantVisualCommandKind::SetCommand) {
        task.kind = BattleFrameVisualChildKind::ActionService;
        task.set_command = publication.record->set_command;
        if (task.set_command.has_value()) {
            task.initial_delay = std::max(0, static_cast<int>(task.set_command->delay));
            task.delay_remaining = task.initial_delay;
            task.maximum_visits = std::min(256, task.initial_delay + 4);
        }
        task.derived_mode = publication.action_key;
    } else if (publication.kind == CombatantVisualCommandKind::CollisionBox) {
        task.kind = BattleFrameVisualChildKind::CollisionBox;
        task.collision_box = publication.record->collision_box;
        if (task.collision_box.has_value()) {
            const auto& payload = *task.collision_box;
            task.collision_current = {
                .x = std::bit_cast<float>(payload.current_x_bits),
                .y = std::bit_cast<float>(payload.current_y_bits),
                .z = std::bit_cast<float>(payload.current_z_bits),
            };
            task.collision_velocity = {
                .x = std::bit_cast<float>(payload.velocity_x_bits),
                .y = std::bit_cast<float>(payload.velocity_y_bits),
                .z = std::bit_cast<float>(payload.velocity_z_bits),
            };
            task.maximum_visits = std::clamp(
                static_cast<int>(payload.end_counter) + 3,
                4,
                1200);
            if (payload.behavior_flags != 5U
                || payload.trailing_flags != 0U
                || payload.start_counter < 0
                || payload.end_counter < 0) {
                task.status = CombatantVisualModelStatus::Unsupported;
            }
        } else {
            task.status = CombatantVisualModelStatus::MissingInput;
            task.maximum_visits = 4;
        }
    } else if (publication.kind == CombatantVisualCommandKind::SystemCamera) {
        task.kind = BattleFrameVisualChildKind::ActionViewRecord;
        task.system_camera = publication.record->system_camera;
        if (task.system_camera.has_value()) {
            task.payload_mode = task.system_camera->mode;
            task.effective_mode = task.payload_mode;
            task.maximum_visits = normalized_camera_duration(*task.system_camera) + 2;
        }
    } else {
        task.kind = BattleFrameVisualChildKind::UnsupportedCommand;
        task.maximum_visits = 1;
        task.status = CombatantVisualModelStatus::Provisional;
    }
    const int sequence = task.sequence;
    if (origin_parent_node_id.has_value()) {
        const auto callback = visual_child_callback_identity(publication.kind);
        const auto created = create_battle_frame_thread(
            runtime.thread_list,
            BattleFrameThreadCreateRequest{
                .kind = BattleFrameThreadNodeKind::AuxiliaryVisualChild,
                .owner_slot = publication.slot,
                .semantic_instance_id =
                    static_cast<std::uint64_t>(sequence),
                .callback = callback,
                .active = true,
                .insertion =
                    BattleFrameThreadInsertionKind::AfterCurrentCursor,
                .parent_node_id = origin_parent_node_id,
                .semantic_source_id =
                    "combatant.auxiliary_visual.child",
                .provenance = publication.provenance,
                .frame_index = runtime.state.frame_index,
            });
        if (created.status == BattleFrameThreadMutationStatus::Applied) {
            task.thread_node_id = created.node_id;
        } else {
            task.complete = true;
            runtime.warnings.push_back(
                "auxiliary visual child thread creation failed for record "
                + std::to_string(publication.record_index) + ": "
                + created.detail);
        }
    }
    runtime.visual.child_tasks.push_back(std::move(task));
    return sequence;
}

int enqueue_synthetic_action_view_child(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    int origin_slot,
    int target_slot,
    std::uint64_t epoch,
    int mode,
    std::string provenance) {
    BattleFrameVisualChildTask task;
    task.sequence = runtime.visual.next_child_sequence++;
    task.action_ordinal = action_ordinal;
    task.origin_slot = origin_slot;
    task.target_slot = target_slot;
    task.resource_stem = "synthetic";
    task.record_index = -1;
    task.publication_epoch = epoch;
    task.command_kind = CombatantVisualCommandKind::SyntheticActionView;
    task.kind = BattleFrameVisualChildKind::ActionViewRecord;
    task.publication_frame = runtime.state.frame_index;
    task.first_eligible_frame = runtime.state.frame_index;
    task.publication_visit_cursor = runtime.visual.current_visit_cursor;
    task.participates_in_action_barrier = false;
    task.synthetic = true;
    task.maximum_visits = 2;
    task.payload_mode = mode;
    task.effective_mode = mode;
    task.status = CombatantVisualModelStatus::Provisional;
    task.provenance = std::move(provenance);
    const int sequence = task.sequence;
    runtime.visual.child_tasks.push_back(std::move(task));
    return sequence;
}

bool has_active_visual_task(const BattleFrameRuntime& runtime) {
    const bool active_child = std::any_of(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [](const BattleFrameVisualChildTask& task) { return !task.complete; });
    if (active_child) {
        return true;
    }
    return std::any_of(
        runtime.visual.action_motion_playbacks.begin(),
        runtime.visual.action_motion_playbacks.end(),
        [](const ActionMotionPlaybackRuntime& playback) {
            return action_motion_playback_blocks_publication(playback);
        });
}

bool has_action_visual_barrier(
    const BattleFrameRuntime& runtime,
    int action_ordinal) {
    const bool active_playback = std::any_of(
        runtime.visual.action_motion_playbacks.begin(),
        runtime.visual.action_motion_playbacks.end(),
        [action_ordinal](const ActionMotionPlaybackRuntime& playback) {
            return playback.action_ordinal == action_ordinal
                && action_motion_playback_blocks_publication(playback);
        });
    if (active_playback) {
        return true;
    }
    const bool active_child = std::any_of(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [action_ordinal](const BattleFrameVisualChildTask& task) {
            return !task.complete
                && task.participates_in_action_barrier
                && task.action_ordinal == action_ordinal;
        });
    if (active_child) {
        return true;
    }
    for (const auto& callback :
         runtime.visual.persistent_instruction_callbacks) {
        if (callback.action_ordinal == action_ordinal
            && callback.auxiliary_publication_pending) {
            return true;
        }
    }
    for (const auto& combatant : runtime.state.combatants) {
        if (!combatant.present
            || combatant.visual_instruction_action_ordinal != action_ordinal
            || combatant.visual_instruction_revision == 0
            || combatant.slot < 0
            || combatant.slot
                >= static_cast<int>(runtime.visual.std_row_producers.size())) {
            continue;
        }
        const auto& producer = runtime.visual.std_row_producers[
            static_cast<std::size_t>(combatant.slot)];
        if (producer.last_instruction_state_revision
            != combatant.visual_instruction_revision) {
            return true;
        }
    }
    return false;
}

void flush_pending_visual_events(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    for (auto& event : runtime.visual.pending_events) {
        event.frame_index = runtime.state.frame_index;
        append_recorded_event(runtime, result, std::move(event));
    }
    runtime.visual.pending_events.clear();
}

void visit_persistent_action_view_controller(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    std::uint32_t& rng_state) {
    if (!runtime.active_action.has_value()) {
        return;
    }
    const auto& action = *runtime.active_action;
    if (action.phase == BattleFrameActionPhase::Complete) {
        return;
    }
    if (action.actor_slot < 0
        || action.actor_slot >= static_cast<int>(runtime.visual.timelines.size())) {
        return;
    }
    const auto* resource = visual_resource_for(runtime, action.actor_slot);
    auto& controller = runtime.visual.controller;
    if (resource != nullptr
        && std::find(
            controller.direct_view_action_ordinals.begin(),
            controller.direct_view_action_ordinals.end(),
            action.action_ordinal) == controller.direct_view_action_ordinals.end()) {
        BattleFrameWorker placement = make_worker(
            runtime.state,
            action.actor_slot,
            action.target_slot,
            BattleFrameWorkerKind::ActionView,
            MovementSelectedWorker::None);
        placement.queue_sequence = action.action_ordinal;
        placement.view_placement_publisher_source_id =
            ViewPlacementCacheSemanticSource::DirectViewPublication;
        placement.view_placement_provenance =
            "provisional persistent-controller FUN_80014474 request at action entry";
        auto placement_event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::ViewPlacementResolve,
            "FUN_800136DC/FUN_80014474",
            BattleFrameEventStatus::Provisional,
            "controller-owned direct-view placement request; action_ordinal="
                + std::to_string(action.action_ordinal));
        placement_event.action_ordinal = action.action_ordinal;
        placement_event.slot = action.actor_slot;
        placement_event.target_slot = action.target_slot;
        apply_view_placement_worker(runtime, placement_event, placement, rng_state);
        append_recorded_event(runtime, result, std::move(placement_event));
        controller.direct_view_action_ordinals.push_back(action.action_ordinal);
    }
    const auto& timeline = runtime.visual.timelines[static_cast<std::size_t>(action.actor_slot)];
    if (!timeline.installed || resource == nullptr || resource->records.empty()) {
        return;
    }
    const auto& persistent_callback =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(action.actor_slot)];
    if (persistent_callback.installed
        && persistent_callback.callback_family
            == ActionMotionPersistentCallbackFamily::
                ActionMotionBasic_8001B1B0) {
        // This callback's action-view children are owned by
        // FUN_8001B1B0 state 10. Running the provisional selector publisher
        // here would create a second child before the authoritative boundary.
        return;
    }

    const auto key = resolve_combatant_visual_action_key(timeline.instruction);
    if (!key.action_key.has_value()) {
        return;
    }
    ActionViewSelectorInput input;
    input.instruction_field6_0x6 = *key.action_key;
    input.instruction_field8_0x8 = timeline.instruction.subtype.value_or(-1);
    input.previous_effective_mode_0x2f = controller.effective_mode_0x2f;
    input.previous_selector_state_0x30 = controller.selector_state_0x30;
    input.previous_actor_slot_0x2 = controller.actor_slot_0x2;
    input.current_actor_slot = static_cast<std::int16_t>(action.actor_slot);
    input.current_secondary_slot = static_cast<std::int16_t>(action.target_slot);
    input.instruction_flags_bit6_set =
        timeline.instruction.instruction_flags.value_or(0) & 0x40U;
    input.selected_aux_table = combatant_visual_selector_table(*resource);

    const auto selected = select_action_view_mode(input);
    controller.initialized = true;
    controller.effective_mode_0x2f = selected.dispatch_effective_mode_0x2f;
    controller.selector_state_0x30 = selected.selector_state_0x30;
    controller.actor_slot_0x2 = static_cast<std::int16_t>(action.actor_slot);
    controller.target_slot_0x4 = static_cast<std::int16_t>(action.target_slot);

    std::ostringstream detail;
    detail << "persistent_selector=FUN_80012F58"
           << "; requested_mode=" << static_cast<int>(selected.requested_mode)
           << "; effective_mode=" << static_cast<int>(selected.dispatch_effective_mode_0x2f)
           << "; selector_state=" << selected.selector_state_0x30
           << "; instruction_epoch=" << timeline.epoch
           << "; direct_view_requests_not_duplicated=1";
    if (selected.mode0e_count.has_value()) {
        detail << "; mode0e_count=" << selected.mode0e_count->count;
    }
    auto event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualControllerVisit,
        "FUN_800136DC/FUN_80012F58",
        selected.unsupported_without_aux_table
            ? BattleFrameEventStatus::MissingInput
            : BattleFrameEventStatus::Provisional,
        detail.str());
    event.action_ordinal = action.action_ordinal;
    event.slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.visual_resource = resource->binding.resource_stem;
    event.visual_epoch = timeline.epoch;
    append_recorded_event(runtime, result, std::move(event));

    if (!selected.spawned_action_view_record_mode_if_known.has_value()) {
        return;
    }
    const int mode = *selected.spawned_action_view_record_mode_if_known;
    const auto signature = std::to_string(action.action_ordinal)
        + ":" + std::to_string(action.actor_slot)
        + ":" + std::to_string(timeline.epoch)
        + ":" + std::to_string(mode)
        + ":" + std::to_string(selected.selector_state_0x30);
    if (std::find(
            controller.published_signatures.begin(),
            controller.published_signatures.end(),
            signature) != controller.published_signatures.end()) {
        return;
    }
    controller.published_signatures.push_back(signature);
    const int task_sequence = enqueue_synthetic_action_view_child(
        runtime,
        action.action_ordinal,
        action.actor_slot,
        action.target_slot,
        timeline.epoch,
        mode,
        "SpawnSyntheticActionViewRecord_80053F38; selector_signature=" + signature);
    const auto task = std::find_if(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [task_sequence](const BattleFrameVisualChildTask& candidate) {
            return candidate.sequence == task_sequence;
        });
    if (task != runtime.visual.child_tasks.end()) {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &*task,
                BattleFrameWorkerStepKind::VisualCommandPublish,
                "SpawnSyntheticActionViewRecord_80053F38",
                BattleFrameEventStatus::Provisional,
                "synthetic action-view record publication; selector_signature="
                    + signature
                    + "; publication_cursor="
                    + std::to_string(runtime.visual.current_visit_cursor)
                    + "; same_frame_child_eligible=1"));
    }
}

bool publish_state10_auxiliary_for_slot(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameRunResult& result,
    std::string boundary_provenance) {
    if (slot < 0
        || slot >= static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        return false;
    }
    auto* combatant = find_frame_combatant(runtime.state, slot);
    const auto* resource = visual_resource_for(runtime, slot);
    auto& callback = runtime.visual.persistent_instruction_callbacks[
        static_cast<std::size_t>(slot)];
    auto& timeline =
        runtime.visual.timelines[static_cast<std::size_t>(slot)];

    if (combatant == nullptr || resource == nullptr || !timeline.installed) {
        callback.auxiliary_publication_pending = false;
        auto event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::VisualAuxiliaryPublication,
            "FUN_8001B1B0_state10_FUN_8001CAA8",
            BattleFrameEventStatus::MissingInput,
            "state-10 publication has no combatant, parsed resource, or installed instruction epoch; draws=0; provenance="
                + boundary_provenance);
        event.action_ordinal = callback.action_ordinal;
        event.slot = slot;
        runtime.visual.pending_events.push_back(std::move(event));
        return false;
    }

    const auto flags_before = combatant->instruction_flags_0xf0;
    combatant->instruction_flags_0xf0 |= 0x10000000u;
    callback.callback_state = 11;
    callback.auxiliary_publication_pending = false;
    ++callback.auxiliary_publication_revision;
    callback.last_auxiliary_instruction_revision =
        callback.instruction_state_revision;

    const auto parent_node_id = runtime.thread_list.current_node_id;
    const auto published = publish_combatant_auxiliary_commands(
        CombatantAuxiliaryPublicationRequest{
            .action_ordinal = callback.action_ordinal,
            .slot = slot,
            .target_slot = combatant->instruction_target_slot_0x4,
            .instruction_revision = callback.instruction_state_revision,
            .publication_epoch = timeline.epoch,
            .owning_thread_visit = timeline.owning_thread_visits,
            .instruction_mode = combatant->visual_instruction_mode_0x6,
            .instruction_subtype =
                combatant->visual_instruction_subtype_0x8,
            .instruction_flags_0xec = combatant->instruction_flags_0xec,
            .instruction_flags_0xf0 = combatant->instruction_flags_0xf0,
            .gate_input = action_motion_delay_gate_input_for(combatant),
            .current_resource = resource,
            .readiness_uses_static_resource = false,
            .current_range_policy =
                CombatantAuxiliaryRangePolicy::FullTable,
            .selector_state = std::nullopt,
            .already_dispatched_record_indices =
                timeline.published_record_indices,
            .provenance =
                "current-resource full-table dispatch is provisional until "
                "FUN_8001C1F0, IW+0x298 ranges, and DAT_8030A1FC are modeled; "
                + boundary_provenance,
        });
    ++timeline.owning_thread_visits;

    std::ostringstream boundary_detail;
    boundary_detail
        << "callback_state=10->11"
        << "; flags_0xf0=0x" << std::hex << std::setw(8)
        << std::setfill('0') << flags_before
        << "->0x" << std::setw(8) << combatant->instruction_flags_0xf0
        << std::dec
        << "; publication_status="
        << combatant_auxiliary_publication_status_name(published.status)
        << "; command_decisions=" << published.decisions.size()
        << "; child_creations=" << published.publications.size()
        << "; auxiliary_revision="
        << callback.auxiliary_publication_revision
        << "; draws=0; provenance=" << published.provenance;
    auto boundary_event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualAuxiliaryPublication,
        "FUN_8001B1B0_state10_FUN_8001CAA8",
        frame_event_status(published.status),
        boundary_detail.str());
    boundary_event.action_ordinal = callback.action_ordinal;
    boundary_event.slot = slot;
    boundary_event.target_slot = combatant->instruction_target_slot_0x4;
    boundary_event.action_motion_callback_family = callback.callback_family;
    boundary_event.action_motion_callback_state_before = 10;
    boundary_event.action_motion_callback_state_after = 11;
    boundary_event.visual_epoch = timeline.epoch;
    boundary_event.visual_resource = resource->binding.resource_stem;
    append_recorded_event(runtime, result, std::move(boundary_event));

    for (const auto& publication : published.publications) {
        const int task_sequence =
            enqueue_visual_child(runtime, publication, parent_node_id);
        if (task_sequence < 0) {
            continue;
        }
        timeline.published_record_indices.push_back(
            publication.record_index);
        const auto task = std::find_if(
            runtime.visual.child_tasks.begin(),
            runtime.visual.child_tasks.end(),
            [task_sequence](const BattleFrameVisualChildTask& candidate) {
                return candidate.sequence == task_sequence;
            });
        if (task == runtime.visual.child_tasks.end()) {
            continue;
        }
        std::ostringstream detail;
        detail
            << "visual_command="
            << combatant_visual_command_kind_name(publication.kind)
            << "; action_key=" << publication.action_key
            << "; epoch=" << publication.epoch
            << "; owning_thread_visit="
            << publication.owning_thread_visit
            << "; parent_node_id="
            << parent_node_id.value_or(-1)
            << "; child_node_id=" << task->thread_node_id
            << "; same_frame_child_eligible="
            << (task->thread_node_id >= 0 ? 1 : 0)
            << "; draws=0; provenance=" << publication.provenance;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &*task,
                BattleFrameWorkerStepKind::VisualCommandPublish,
                visual_child_handler_name(publication.kind),
                frame_event_status(publication.status),
                detail.str()));
    }
    return published.status !=
        CombatantAuxiliaryPublicationStatus::MissingInput;
}

bool visit_action_motion_playback_for_slot(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameRunResult& result) {
    if (slot < 0
        || slot >= static_cast<int>(runtime.visual.action_motion_playbacks.size())) {
        return true;
    }
    auto& playback = runtime.visual.action_motion_playbacks[
        static_cast<std::size_t>(slot)];
    if (!action_motion_playback_blocks_publication(playback)) {
        return true;
    }

    const auto phase_before = playback.phase;
    auto* combatant = find_frame_combatant(runtime.state, slot);
    ActionMotionPlaybackVisitInput visit_input;
    if (phase_before == ActionMotionPlaybackPhase::State6Satisfied) {
        visit_input.post_state6_delay = resolve_action_motion_post_state6_delay(
            action_motion_delay_table_for(visual_resource_for(runtime, slot)),
            action_motion_delay_gate_input_for(combatant));
    }
    const auto visited = visit_action_motion_playback(playback, visit_input);
    playback = visited.runtime;
    bool release_auxiliary_publication = false;
    if (combatant != nullptr) {
        combatant->instruction_flags_0xec = visited.flags_after;
    }
    if (visited.publication_released_this_visit
        && slot < static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        auto& callback_runtime =
            runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(slot)];
        if (callback_runtime.installed
            && callback_runtime.action_ordinal == playback.action_ordinal) {
            switch (playback.continuation) {
            case ActionMotionPlaybackContinuation::State5LoadLookedUpTo4:
                callback_runtime.callback_state = 4;
                break;
            case ActionMotionPlaybackContinuation::State6PostDelayTo11:
                callback_runtime.callback_state = 11;
                release_auxiliary_publication =
                    callback_runtime.callback_family
                    == ActionMotionPersistentCallbackFamily::
                        ActionMotionBasic_8001B1B0;
                break;
            case ActionMotionPlaybackContinuation::State7LoadLookedUpTo14:
                callback_runtime.callback_state = 14;
                break;
            case ActionMotionPlaybackContinuation::GenericRelease:
                break;
            }
        }
    }

    BattleFrameWorkerStepKind step_kind =
        BattleFrameWorkerStepKind::ActionMotionRendererAdvance;
    std::string callback = "FUN_80018CBC";
    switch (visited.kind) {
    case ActionMotionPlaybackVisitKind::InitialRendererAdvance:
        break;
    case ActionMotionPlaybackVisitKind::State6Deferred:
    case ActionMotionPlaybackVisitKind::State6Satisfied:
        step_kind = BattleFrameWorkerStepKind::ActionMotionState6Poll;
        callback = "FUN_80075D64";
        break;
    case ActionMotionPlaybackVisitKind::PostState6DelayDeferred:
    case ActionMotionPlaybackVisitKind::PostState6DelayUnavailable:
        step_kind = BattleFrameWorkerStepKind::ActionMotionPostState6Delay;
        callback = "FUN_8001DDE0/FUN_8001B6F8_states8_9";
        break;
    case ActionMotionPlaybackVisitKind::PublicationReleased:
        step_kind = BattleFrameWorkerStepKind::ActionMotionPublicationRelease;
        callback = "FUN_8001B6D4_states8_9_10";
        break;
    case ActionMotionPlaybackVisitKind::None:
        return !action_motion_playback_blocks_publication(playback);
    }

    std::ostringstream detail;
    detail << "playback_visit="
           << action_motion_playback_visit_kind_name(visited.kind)
           << "; phase_before="
           << action_motion_playback_phase_name(phase_before)
           << "; progress_before_bits=0x" << std::hex
           << std::setw(8) << std::setfill('0') << visited.progress_before
           << "; progress_after_bits=0x" << std::setw(8)
           << visited.progress_after
           << "; increment_bits=0x" << std::setw(8)
           << playback.increment_bits_0x6c
           << "; flags_before=0x" << std::setw(8) << visited.flags_before
           << "; flags_after=0x" << std::setw(8) << visited.flags_after
           << std::dec
           << "; control=" << visited.control_state_before
           << "->" << visited.control_state_after
           << "; renderer_visits=" << playback.renderer_visits
           << "; state6_polls=" << playback.state6_polls;
    if (visited.gate_result.has_value()) {
        detail << "; gate_result=" << (*visited.gate_result ? 1 : 0);
    }
    if (visited.post_state6_delay_lookup_performed
        || phase_before == ActionMotionPlaybackPhase::WaitingForPostState6Delay) {
        detail << "; post_state6_delay_status="
               << action_motion_delay_status_name(visited.post_state6_delay_status)
               << "; post_state6_delay_descriptor_record="
               << visited.post_state6_delay_descriptor_record_index
               << "; post_state6_delay=" << visited.post_state6_delay_before
               << "->" << visited.post_state6_delay_after
               << "; delay_lookup="
               << (visited.post_state6_delay_lookup_performed ? 1 : 0);
    }
    detail << "; draws=0; " << visited.detail;

    auto event = make_visual_event(
        runtime,
        nullptr,
        step_kind,
        callback,
        playback.status == ActionMotionPlaybackStatus::Matched
            ? BattleFrameEventStatus::Provisional
            : frame_event_status(playback.status),
        detail.str()
            + "; primitive=validated; frame-thread invocation=provisional");
    event.action_ordinal = playback.action_ordinal;
    event.slot = slot;
    event.target_slot = combatant != nullptr
        ? combatant->instruction_target_slot_0x4
        : -1;
    event.action_motion_playback_phase_before = phase_before;
    event.action_motion_playback_phase_after = playback.phase;
    event.action_motion_duration_bits = playback.raw_duration_bits;
    event.action_motion_effective_duration_bits = playback.effective_duration_bits;
    event.action_motion_progress_before_bits = visited.progress_before;
    event.action_motion_progress_after_bits = visited.progress_after;
    event.action_motion_increment_bits = playback.increment_bits_0x6c;
    event.action_motion_flags_before = visited.flags_before;
    event.action_motion_flags_after = visited.flags_after;
    event.action_motion_control_before = visited.control_state_before;
    event.action_motion_control_after = visited.control_state_after;
    event.action_motion_renderer_advanced = visited.renderer_advanced;
    event.action_motion_gate_polled = visited.gate_polled;
    event.action_motion_gate_result = visited.gate_result;
    event.action_motion_delay_lookup_performed =
        visited.post_state6_delay_lookup_performed;
    event.action_motion_delay_status = visited.post_state6_delay_status;
    event.action_motion_delay_descriptor_record_index =
        visited.post_state6_delay_descriptor_record_index;
    event.action_motion_delay_before = visited.post_state6_delay_before;
    event.action_motion_delay_after = visited.post_state6_delay_after;
    append_recorded_event(runtime, result, std::move(event));
    if (release_auxiliary_publication) {
        (void)publish_state10_auxiliary_for_slot(
            runtime,
            slot,
            result,
            "ActionMotionPlaybackModel completed the captured state-6/post-delay "
            "path and entered FUN_8001B1B0 state 10 in the same instruction visit");
    }
    return !action_motion_playback_blocks_publication(playback);
}

bool visit_persistent_instruction_callback_for_slot(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameRunResult& result) {
    if (slot < 0
        || slot >= static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        return true;
    }
    auto& invocation = runtime.visual.persistent_instruction_callbacks[
        static_cast<std::size_t>(slot)];
    if (!invocation.installed || invocation.thread_state_0x19 != 1) {
        return true;
    }
    auto* combatant = find_frame_combatant(runtime.state, slot);
    const auto* resource = visual_resource_for(runtime, slot);
    if (combatant == nullptr || resource == nullptr) {
        invocation.status = ActionMotionInvocationStatus::MissingInput;
        return true;
    }
    if (invocation.instruction_state_revision
        != combatant->visual_instruction_revision) {
        invocation.status = ActionMotionInvocationStatus::Unsupported;
        return true;
    }

    const int callback_state_before = invocation.callback_state;
    std::optional<bool> rotation_complete;
    if (combatant->turn_state_known) {
        rotation_complete =
            combatant->turn_current_degrees_0x11c
            == combatant->turn_target_degrees_0x120;
    }
    std::optional<int> state8_descriptor_delay;
    if (invocation.callback_family
            == ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0
        && invocation.callback_state == 8) {
        const auto delay = resolve_action_motion_post_state6_delay(
            action_motion_delay_table_for(resource),
            action_motion_delay_gate_input_for(combatant));
        if ((delay.status == ActionMotionDelayStatus::Matched
                || delay.status == ActionMotionDelayStatus::NoMatch)
            && delay.delay.has_value()) {
            state8_descriptor_delay = *delay.delay;
        }
    }
    const auto decision = resolve_action_motion_invocation(
        resource->action_rows,
        ActionMotionInvocationRequest{
            .callback_family = invocation.callback_family,
            .callback_state = invocation.callback_state,
            .instruction_mode = combatant->visual_instruction_mode_0x6,
            .instruction_subtype = combatant->visual_instruction_subtype_0x8 >= 0
                ? std::optional<std::int16_t>{
                    combatant->visual_instruction_subtype_0x8}
                : std::nullopt,
            .instruction_flags_0xf0 = combatant->instruction_flags_0xf0,
            .rotation_complete = rotation_complete,
            .state8_descriptor_delay = state8_descriptor_delay,
            .state8_delay_remaining = invocation.state8_delay_remaining,
            .ranged_flag_0x2_set =
                (combatant->instruction_flags_0xec & 0x2u) != 0,
            .current_motion_resource_present =
                invocation.current_motion_resource_present,
            .current_motion_id = invocation.current_motion_id,
            .selected_instruction_row = invocation.current_instruction_row,
        });
    ++invocation.visits;
    invocation.status = decision.status;
    invocation.callback_state = decision.callback_state_after;
    invocation.state8_delay_remaining = decision.state8_delay_remaining;
    if (invocation.callback_family
        == ActionMotionPersistentCallbackFamily::
            ActionMotionBasic_8001B1B0) {
        invocation.auxiliary_publication_pending =
            !decision.auxiliary_publication_requested
            && decision.callback_state_after >= 5
            && decision.callback_state_after <= 10;
    }

    std::ostringstream detail;
    detail << "persistent_callback="
           << action_motion_callback_family_name(invocation.callback_family)
           << "; callback_index=" << invocation.callback_index
           << "; callback_state=" << callback_state_before
           << "->" << decision.callback_state_after
           << "; decision="
           << action_motion_invocation_decision_name(decision.decision)
           << "; resolver_called=" << (decision.resolver_called ? 1 : 0)
           << "; resolver_result="
           << action_motion_resolver_code_name(decision.resolver.code)
           << "; resolver_callsite=" << decision.resolver_callsite
           << "; operation_callsite=" << decision.operation_callsite
           << "; row_source="
           << action_motion_invocation_row_source_name(decision.row_source)
           << "; output_row="
           << (decision.operation_row.has_value()
               ? decision.operation_row->index
               : -1)
           << "; resolved_motion_id="
           << (decision.resolver.resolved_motion_id.has_value()
               ? *decision.resolver.resolved_motion_id
               : -1)
           << "; state8_delay_remaining="
           << decision.state8_delay_remaining
           << "; auxiliary_publication="
           << (decision.auxiliary_publication_requested ? 1 : 0)
           << "; state10_fallthrough="
           << (decision.entered_state10_via_fallthrough ? 1 : 0)
           << "; visits=" << invocation.visits
           << "; draws=0; provenance=" << decision.provenance;

    auto decision_event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::ActionMotionInvocationDecision,
        "FUN_80022850_persistent_callback_visit",
        frame_event_status(decision.status),
        detail.str());
    decision_event.action_ordinal = invocation.action_ordinal;
    decision_event.slot = slot;
    decision_event.target_slot = combatant->instruction_target_slot_0x4;
    decision_event.action_motion_callback_family = invocation.callback_family;
    decision_event.action_motion_invocation_decision = decision.decision;
    decision_event.action_motion_invocation_status = decision.status;
    decision_event.action_motion_resolver_code = decision.resolver.code;
    decision_event.action_motion_callback_state_before = callback_state_before;
    decision_event.action_motion_callback_state_after =
        decision.callback_state_after;
    decision_event.action_motion_resolver_row =
        decision.operation_row.has_value() ? decision.operation_row->index : -1;
    decision_event.action_motion_resolved_motion_id =
        decision.resolver.resolved_motion_id.value_or(-1);
    append_recorded_event(runtime, result, std::move(decision_event));

    if (decision.auxiliary_publication_requested) {
        (void)publish_state10_auxiliary_for_slot(
            runtime,
            slot,
            result,
            decision.entered_state10_via_fallthrough
                ? "FUN_8001B1B0 state 8/9 reached zero delay and fell through "
                  "state 10 in the same persistent callback visit"
                : "FUN_8001B1B0 entered state 10 directly in the persistent "
                  "callback visit");
    }

    switch (decision.decision) {
    case ActionMotionInvocationDecisionKind::InstallPlayback: {
        const auto duration_bits = decision.operation_row.has_value()
            ? std::optional<std::uint32_t>{
                decision.operation_row->transition_gate_divisor_bits}
            : std::nullopt;
        const auto playback_install = install_action_motion_playback(
            ActionMotionPlaybackInstallRequest{
                .action_ordinal = invocation.action_ordinal,
                .slot = slot,
                .instruction_state_revision =
                    invocation.instruction_state_revision,
                .selected_action_row_index = decision.operation_row.has_value()
                    ? decision.operation_row->index
                    : -1,
                .selected_action_row_duration_bits = duration_bits,
                .instruction_flags_0xec = combatant->instruction_flags_0xec,
                .instruction_flags_0xf0 = combatant->instruction_flags_0xf0,
                .continuation = decision.playback_continuation,
                .provenance = invocation.provenance
                    + "; " + decision.provenance,
            });
        runtime.visual.action_motion_playbacks[
            static_cast<std::size_t>(slot)] = playback_install.runtime;
        if (playback_install.installed) {
            combatant->instruction_flags_0xec = playback_install.flags_after;
            invocation.current_motion_resource_present = true;
            invocation.current_motion_id =
                decision.resolver.resolved_motion_id;
            ++invocation.installs;
        }

        auto playback_event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::ActionMotionPlaybackInstall,
            "FUN_8001EBA4/FUN_80076170",
            playback_install.runtime.status == ActionMotionPlaybackStatus::Matched
                ? frame_event_status(decision.status)
                : frame_event_status(playback_install.runtime.status),
            playback_install.detail
                + "; persistent_callback="
                + action_motion_callback_family_name(invocation.callback_family)
                + "; resolver_callsite=" + decision.resolver_callsite
                + "; install_callsite=" + decision.operation_callsite
                + "; resolver_result="
                + action_motion_resolver_code_name(decision.resolver.code)
                + "; instruction_state_revision="
                + std::to_string(invocation.instruction_state_revision)
                + "; publication_blocked="
                + std::to_string(playback_install.blocks_publication ? 1 : 0)
                + "; primitive=validated; invocation_policy="
                + action_motion_invocation_status_name(decision.status)
                + "; provenance=" + playback_install.runtime.provenance);
        playback_event.action_ordinal = invocation.action_ordinal;
        playback_event.slot = slot;
        playback_event.target_slot = combatant->instruction_target_slot_0x4;
        playback_event.action_motion_callback_family = invocation.callback_family;
        playback_event.action_motion_invocation_decision = decision.decision;
        playback_event.action_motion_invocation_status = decision.status;
        playback_event.action_motion_resolver_code = decision.resolver.code;
        playback_event.action_motion_callback_state_before =
            callback_state_before;
        playback_event.action_motion_callback_state_after =
            decision.callback_state_after;
        playback_event.action_motion_resolver_row =
            decision.operation_row.has_value()
                ? decision.operation_row->index
                : -1;
        playback_event.action_motion_resolved_motion_id =
            decision.resolver.resolved_motion_id.value_or(-1);
        playback_event.action_motion_playback_phase_before =
            ActionMotionPlaybackPhase::Inactive;
        playback_event.action_motion_playback_phase_after =
            playback_install.runtime.phase;
        playback_event.action_motion_duration_bits =
            playback_install.runtime.raw_duration_bits;
        playback_event.action_motion_effective_duration_bits =
            playback_install.runtime.effective_duration_bits;
        playback_event.action_motion_progress_after_bits =
            playback_install.runtime.progress_bits_0x68;
        playback_event.action_motion_increment_bits =
            playback_install.runtime.increment_bits_0x6c;
        playback_event.action_motion_flags_before = playback_install.flags_before;
        playback_event.action_motion_flags_after = playback_install.flags_after;
        playback_event.action_motion_control_before = callback_state_before;
        playback_event.action_motion_control_after =
            playback_install.runtime.callback_control_state;
        append_recorded_event(runtime, result, std::move(playback_event));
        return !playback_install.blocks_publication;
    }
    case ActionMotionInvocationDecisionKind::LoadSelected:
    case ActionMotionInvocationDecisionKind::LoadLookedUp:
        if (decision.operation_row.has_value()) {
            invocation.current_motion_resource_present = true;
            invocation.current_motion_id =
                decision.operation_row->callback_ordinal;
            ++invocation.loads;
        }
        break;
    case ActionMotionInvocationDecisionKind::Restore:
        if (decision.callback_state_after == 15) {
            invocation.current_motion_resource_present = false;
            invocation.current_motion_id.reset();
        }
        break;
    case ActionMotionInvocationDecisionKind::Release:
        // Playback and callback-local action completion do not uninstall IW+0xE0.
        break;
    case ActionMotionInvocationDecisionKind::Unsupported:
        // Unknown callback families remain installed and consume no RNG.
        break;
    case ActionMotionInvocationDecisionKind::Wait:
        break;
    }
    return true;
}

void visit_std_row_producer_for_slot(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameRunResult& result) {
    if (slot < 0
        || slot >= static_cast<int>(runtime.visual.std_row_producers.size())) {
        return;
    }
    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant == nullptr || !combatant->present) {
        return;
    }

    const auto& instruction_runtime = runtime.combatant_instructions[
        static_cast<std::size_t>(slot)];
    const auto selected_key = combatant->selected_action_row_known
            && combatant->selected_action_row_action_id >= 0
        ? std::optional<std::int16_t>{combatant->selected_action_row_action_id}
        : std::nullopt;
    const auto runtime_mode = combatant->visual_instruction_knowledge
            != CombatantVisualInstructionKnowledge::Unknown
        ? std::optional<std::int16_t>{combatant->visual_instruction_mode_0x6}
        : std::nullopt;
    auto& cursor = runtime.visual.std_row_producers[
        static_cast<std::size_t>(slot)];
    const auto produced = visit_combatant_instruction_std_row_producer(
        cursor,
        CombatantInstructionStdRowProducerRequest{
            .action_ordinal = combatant->visual_instruction_action_ordinal,
            .slot = slot,
            .instruction_revision = instruction_runtime.revision,
            .instruction_state_revision = combatant->visual_instruction_revision,
            .selected_action_row_known = combatant->selected_action_row_known,
            .selected_action_row_index = combatant->selected_action_row_index,
            .selected_action_key = selected_key,
            .runtime_instruction_mode = runtime_mode,
            .subtype = combatant->visual_instruction_subtype_0x8 >= 0
                ? std::optional<std::int16_t>{
                    combatant->visual_instruction_subtype_0x8}
                : std::nullopt,
            .target_slot = combatant->instruction_target_slot_0x4 >= 0
                ? std::optional<int>{combatant->instruction_target_slot_0x4}
                : std::nullopt,
            .instruction_flags = combatant->instruction_flags_0xec,
            .knowledge = combatant->visual_instruction_knowledge,
            .provenance = combatant->visual_instruction_provenance,
        });
    cursor = produced.cursor_after;

    if (produced.status
        == CombatantInstructionStdRowProducerStatus::Unchanged) {
        return;
    }

    auto producer_event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualStdRowProducerVisit,
        "CombatantInstructionStdRowProducer",
        produced.status == CombatantInstructionStdRowProducerStatus::Published
            ? frame_event_status(produced.visual_status)
            : frame_event_status(produced.status),
        "producer_status="
            + std::string(combatant_instruction_std_row_producer_status_name(
                produced.status))
            + "; thread_state_0x19="
            + std::to_string(cursor.thread_state_0x19)
            + "; instruction_revision="
            + std::to_string(instruction_runtime.revision)
            + "; instruction_state_revision="
            + std::to_string(combatant->visual_instruction_revision)
            + "; selected_action_row="
            + std::to_string(combatant->selected_action_row_index)
            + "; selected_action_key="
            + (selected_key.has_value()
                ? std::to_string(*selected_key)
                : std::string("missing"))
            + "; installs_epoch="
            + std::to_string(produced.install_epoch ? 1 : 0)
            + "; provenance=" + produced.provenance);
    producer_event.action_ordinal = combatant->visual_instruction_action_ordinal;
    producer_event.slot = slot;
    producer_event.target_slot = combatant->instruction_target_slot_0x4;
    producer_event.combatant_instruction_revision = instruction_runtime.revision;
    append_recorded_event(runtime, result, std::move(producer_event));

    if (!produced.install_epoch || !produced.instruction.has_value()) {
        return;
    }

    auto& timeline = runtime.visual.timelines[static_cast<std::size_t>(slot)];
    install_combatant_visual_instruction(timeline, *produced.instruction);
    runtime.visual.timeline_action_ordinals[static_cast<std::size_t>(slot)] =
        combatant->visual_instruction_action_ordinal;
    const auto key = resolve_combatant_visual_action_key(timeline.instruction);
    auto install_event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualInstructionInstall,
        "CombatantInstructionStdRowEpochInstall",
        frame_event_status(key.status),
        "action_key="
            + (key.action_key.has_value()
                ? std::to_string(*key.action_key)
                : std::string("missing"))
            + "; key_source=" + combatant_visual_key_source_name(key.source)
            + "; epoch=" + std::to_string(timeline.epoch)
            + "; instruction_state_revision="
            + std::to_string(combatant->visual_instruction_revision)
            + "; source_context_only=1"
            + "; publication_owner=FUN_8001B1B0_state10_FUN_8001CAA8"
            + "; provenance="
            + produced.provenance);
    install_event.action_ordinal = combatant->visual_instruction_action_ordinal;
    install_event.slot = slot;
    install_event.target_slot = combatant->instruction_target_slot_0x4;
    install_event.visual_epoch = timeline.epoch;
    if (const auto* resource = visual_resource_for(runtime, slot);
        resource != nullptr) {
        install_event.visual_resource = resource->binding.resource_stem;
    }
    append_recorded_event(runtime, result, std::move(install_event));
}

void append_visual_cleanup(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::string reason) {
    task.complete = true;
    task.phase = BattleFrameVisualChildPhase::Complete;
    task.thread_state_0x19 = 3;
    append_recorded_event(
        runtime,
        result,
        make_visual_event(
            runtime,
            &task,
            BattleFrameWorkerStepKind::VisualChildCleanup,
            std::string(visual_child_handler_name(task.command_kind))
                + "_cleanup",
            BattleFrameEventStatus::Provisional,
            std::move(reason)));
    if (task.thread_node_id >= 0) {
        const auto removed = remove_battle_frame_thread(
            runtime.thread_list,
            task.thread_node_id,
            "combatant.auxiliary_visual.child.complete",
            "visual child reached its callback-specific completion boundary",
            runtime.state.frame_index,
            static_cast<std::uint64_t>(task.sequence));
        if (removed.status != BattleFrameThreadMutationStatus::Applied) {
            runtime.warnings.push_back(
                "completed auxiliary visual child thread could not be removed: "
                + removed.detail);
        }
    }
}

std::vector<std::int16_t> target_action_ids_for_selector(
    const BattleFrameRuntime& runtime,
    int target_slot) {
    std::vector<std::int16_t> action_ids;
    const auto* resource = visual_resource_for(runtime, target_slot);
    if (resource == nullptr) {
        return action_ids;
    }
    for (const auto& row : resource->action_rows) {
        if (row.row_type == 3) {
            break;
        }
        if (row.action_id >= 0) {
            action_ids.push_back(row.action_id);
        }
    }
    return action_ids;
}

DirectInstructionTransitionResult run_direct_transition_selector(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    DirectInstructionTransitionProducer producer,
    BattleFrameWorkerStepKind step_kind,
    std::string callback,
    std::string rng_label,
    std::uint32_t& rng_state) {
    auto* origin = find_frame_combatant(runtime.state, task.origin_slot);
    std::optional<std::uint32_t> target_flags_0xf0;
    std::optional<std::uint32_t> target_flags_0xf4;
    if (task.target_slot >= 0
        && task.target_slot < static_cast<int>(runtime.target_reactions.size())) {
        const auto& staged = runtime.target_reactions[
            static_cast<std::size_t>(task.target_slot)];
        if (staged.available && staged.action_ordinal == task.action_ordinal) {
            target_flags_0xf0 = staged.reaction.selector_flags_0xf0;
            target_flags_0xf4 = staged.reaction.selector_flags_0xf4;
        }
    }
    const auto selected = select_direct_instruction_transition({
        .producer = producer,
        .origin_slot = task.origin_slot,
        .target_slot = task.target_slot,
        .origin_mode = origin != nullptr
            ? std::optional<std::int16_t>{origin->visual_instruction_mode_0x6}
            : std::nullopt,
        .origin_flags_0xec = origin != nullptr
            ? std::optional<std::uint32_t>{origin->instruction_flags_0xec}
            : std::nullopt,
        .target_flags_0xf0 = target_flags_0xf0,
        .target_flags_0xf4 = target_flags_0xf4,
        .available_target_action_ids =
            target_action_ids_for_selector(runtime, task.target_slot),
        .rng_seed_before = rng_state,
    });

    auto event = make_visual_event(
        runtime,
        &task,
        step_kind,
        std::move(callback),
        frame_event_status(selected.status),
        "producer="
            + std::string(direct_instruction_transition_producer_name(producer))
            + "; branch="
            + direct_instruction_transition_branch_name(selected.branch)
            + "; selected_mode="
            + (selected.selected_mode.has_value()
                ? std::to_string(*selected.selected_mode)
                : std::string("missing"))
            + "; should_reset=" + (selected.should_reset ? "1" : "0")
            + "; provenance=" + selected.provenance);
    event.visual_effective_mode = selected.selected_mode.value_or(-1);
    event.draws_consumed = selected.draws_consumed;
    event.rand_value = selected.rand_value;
    event.visual_candidate_selected_index = selected.candidate_index;
    if (selected.draws_consumed != 0) {
        event.rng_event = true;
        event.rng_label = std::move(rng_label);
        event.rng_seed_before = selected.rng_seed_before;
        event.rng_seed_after = selected.rng_seed_after;
        if (selected.rng_seed_after.has_value()) {
            rng_state = *selected.rng_seed_after;
        }
    }
    if (selected.clear_origin_random_gate && origin != nullptr) {
        origin->instruction_flags_0xec &= ~0x00100000U;
    }
    if (selected.should_reset && selected.selected_mode.has_value()) {
        const bool staged = stage_battle_frame_validated_instruction_transition(
            runtime,
            task.action_ordinal,
            task.target_slot,
            task.origin_slot,
            *selected.selected_mode,
            selected.provenance,
            task.sequence);
        if (!staged) {
            event.status = BattleFrameEventStatus::MissingInput;
            event.detail +=
                "; selected target row could not be published through the validated direct-reset boundary";
        }
    }
    append_recorded_event(runtime, result, std::move(event));
    return selected;
}

void advance_action_service_child(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::uint32_t& rng_state) {
    auto* origin = find_frame_combatant(runtime.state, task.origin_slot);
    if (task.phase == BattleFrameVisualChildPhase::Published) {
        task.phase = BattleFrameVisualChildPhase::Delay;
        task.thread_state_0x19 = 1;
        task.derived_mode = origin != nullptr
                && origin->visual_instruction_knowledge
                    != CombatantVisualInstructionKnowledge::Unknown
            ? origin->visual_instruction_mode_0x6
            : task.derived_mode;
        task.derived_subtype = origin != nullptr
            ? origin->visual_instruction_subtype_0x8
            : -1;
        if (origin != nullptr && origin->instruction_target_slot_0x4 >= 0) {
            task.target_slot = origin->instruction_target_slot_0x4;
        }
        std::ostringstream detail;
        detail << "state0_derived_mode=" << task.derived_mode
               << "; state0_derived_subtype=" << task.derived_subtype
               << "; origin_slot=" << task.origin_slot
               << "; selected_target_slot=" << task.target_slot
               << "; delay_initial=" << task.initial_delay
               << "; origin_and_target_distinct="
               << (task.origin_slot != task.target_slot ? 1 : 0)
               << "; provenance=" << task.provenance;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualChildState0,
                "FUN_8004281C_state0",
                frame_event_status(task.status),
                detail.str()));
        return;
    }

    if (task.delay_remaining > 0) {
        const int before = task.delay_remaining;
        --task.delay_remaining;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualChildDelay,
                "FUN_8004281C_state1_delay",
                BattleFrameEventStatus::Matched,
                "delay=" + std::to_string(before) + "->"
                    + std::to_string(task.delay_remaining)
                    + "; zero visit falls through on the next packed-thread visit"));
        return;
    }

    task.phase = BattleFrameVisualChildPhase::Active;
    task.thread_state_0x19 = 2;
    task.nested_call_complete = true;
    (void)run_direct_transition_selector(
        runtime,
        result,
        task,
        DirectInstructionTransitionProducer::ActionService,
        BattleFrameWorkerStepKind::VisualChildNested,
        "FUN_8004281C/FUN_80020B8C/FUN_8002E5D0",
        "fun_8002eb4c_action_service",
        rng_state);
    append_visual_cleanup(
        runtime,
        result,
        task,
        "normal action-service cleanup; nested return and child-count decrement share the zero-delay visit");
}

BattleCollisionVec3 collision_vec(const BattleFrameVec3& value) {
    return {.x = value.x, .y = value.y, .z = value.z};
}

BattleCollisionOccupancyRefreshResult refresh_collision_occupancy_for_slot(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameRunResult* result) {
    const auto* combatant = find_frame_combatant(runtime.state, slot);
    const auto refreshed = refresh_battle_collision_occupancy(
        runtime.collision_occupancy,
        BattleCollisionOccupancyRefreshRequest{
            .slot = slot,
            .present = combatant != nullptr && combatant->present,
            .alive = combatant != nullptr && combatant->alive,
            .position = combatant != nullptr
                ? collision_vec(combatant->combatant_cur_pos_0x1c)
                : BattleCollisionVec3{},
            .instruction_flags_0xec = combatant != nullptr
                ? combatant->instruction_flags_0xec
                : 0U,
            .mld_slot_valid = combatant != nullptr
                ? combatant->collision_mld_slot_valid
                : std::nullopt,
        });
    if (result != nullptr) {
        auto event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::CollisionOccupancyRefresh,
            "FUN_80018CBC/FUN_800184F0",
            frame_event_status(refreshed.status),
            "slot=" + std::to_string(slot)
                + "; grid=" + std::to_string(refreshed.grid_x) + ','
                + std::to_string(refreshed.grid_z)
                + "; cells_written=" + std::to_string(refreshed.cells_written)
                + "; occupancy_revision=" + std::to_string(refreshed.revision)
                + "; provenance=" + refreshed.provenance);
        event.slot = slot;
        append_recorded_event(runtime, *result, std::move(event));
    }
    return refreshed;
}

void append_collision_probe_event(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    const BattleCollisionVec3& local,
    const BattleCollisionTransformResult& transformed,
    int candidate,
    bool reverse_probe) {
    std::ostringstream detail;
    detail << "state=2; counter=" << task.collision_counter
           << "; probe=" << (reverse_probe ? "reverse_half_step" : "primary")
           << "; local_bits="
           << std::hex << std::showbase
           << std::bit_cast<std::uint32_t>(local.x) << ','
           << std::bit_cast<std::uint32_t>(local.y) << ','
           << std::bit_cast<std::uint32_t>(local.z)
           << "; world_bits="
           << transformed.world_x_bits << ','
           << transformed.world_y_bits << ','
           << transformed.world_z_bits
           << std::dec << std::noshowbase
           << "; candidate=" << candidate
           << "; occupancy_revision=" << runtime.collision_occupancy.revision
           << "; provenance=" << transformed.provenance;
    append_recorded_event(
        runtime,
        result,
        make_visual_event(
            runtime,
            &task,
            BattleFrameWorkerStepKind::CollisionProbe,
            reverse_probe
                ? "FUN_8004BA88_reverse_occupancy"
                : "FUN_8004B9CC_primary_occupancy",
            frame_event_status(transformed.status),
            detail.str()));
}

void advance_collision_box_child(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::uint32_t& rng_state) {
    auto* origin = find_frame_combatant(runtime.state, task.origin_slot);
    if (!task.collision_box.has_value() || origin == nullptr) {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualUnsupportedWait,
                "FUN_8004B7C4_missing_input",
                BattleFrameEventStatus::MissingInput,
                "collision child requires a decoded payload and live origin combatant"));
        append_visual_cleanup(
            runtime,
            result,
            task,
            "missing CollisionBox input ended the child without RNG or a reset");
        return;
    }
    const auto& payload = *task.collision_box;
    if (payload.behavior_flags != 5U || payload.trailing_flags != 0U
        || payload.start_counter < 0 || payload.end_counter < 0) {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualUnsupportedWait,
                "FUN_8004B7C4_unsupported_payload",
                BattleFrameEventStatus::Unsupported,
                "only behavior_flags=5, trailing_flags=0, and nonnegative counters are live-validated"));
        append_visual_cleanup(
            runtime,
            result,
            task,
            "unsupported CollisionBox variant ended without RNG or a reset");
        return;
    }

    if (task.collision_state == 0) {
        task.phase = BattleFrameVisualChildPhase::Active;
        task.thread_state_0x19 = 1;
        task.collision_state = 1;
        task.derived_mode = origin->visual_instruction_mode_0x6;
        task.derived_subtype = origin->visual_instruction_subtype_0x8;
        if (origin->instruction_target_slot_0x4 >= 0) {
            task.target_slot = origin->instruction_target_slot_0x4;
        }
        const int before = task.collision_counter;
        ++task.collision_counter;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualChildState0,
                "FUN_8004B7C4_state0",
                frame_event_status(task.status),
                "state=0->1; counter=" + std::to_string(before) + "->"
                    + std::to_string(task.collision_counter)
                    + "; start=" + std::to_string(payload.start_counter)
                    + "; end=" + std::to_string(payload.end_counter)
                    + "; object_id=" + std::to_string(payload.object_id)
                    + "; target=" + std::to_string(task.target_slot)));
        return;
    }

    if (task.collision_state == 0xFA) {
        append_visual_cleanup(
            runtime,
            result,
            task,
            "CollisionBox state 0xFA was removed on the visit after selector service");
        return;
    }

    if (task.collision_state == 1) {
        const int state_before = task.collision_state;
        const int counter_before = task.collision_counter;
        if (task.collision_counter >= payload.start_counter) {
            task.collision_state = 2;
        }
        ++task.collision_counter;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualChildDelay,
                "FUN_8004B7C4_state1_wait",
                BattleFrameEventStatus::Matched,
                "state=" + std::to_string(state_before) + "->"
                    + std::to_string(task.collision_state)
                    + "; counter=" + std::to_string(counter_before) + "->"
                    + std::to_string(task.collision_counter)
                    + "; the transition visit performs no candidate query"));
        return;
    }

    if (task.collision_state != 2) {
        append_visual_cleanup(
            runtime,
            result,
            task,
            "unknown CollisionBox state ended without RNG or a reset");
        return;
    }
    if (!origin->collision_xz_rotation_known) {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualUnsupportedWait,
                "FUN_80292080_missing_rotation",
                BattleFrameEventStatus::MissingInput,
                "collision X/Z worksheet rotation producer is unavailable"));
        append_visual_cleanup(
            runtime,
            result,
            task,
            "missing collision rotation ended the child without RNG or a reset");
        return;
    }

    const BattleCollisionRotationRaw rotation{
        .x = origin->collision_rotation_x_0x28,
        .y = std::bit_cast<std::int32_t>(origin->combatant_facing_angle_0x2c),
        .z = origin->collision_rotation_z_0x30,
    };
    const auto probe = [&](const BattleCollisionVec3& local, bool reverse) {
        const auto transformed = transform_battle_collision_point({
            .local = local,
            .origin = collision_vec(origin->combatant_cur_pos_0x1c),
            .rotation = rotation,
        });
        const int candidate = transformed.status == BattleCollisionModelStatus::Matched
            ? static_cast<int>(lookup_battle_collision_occupancy(
                runtime.collision_occupancy,
                transformed.world.x,
                transformed.world.z))
            : -1;
        append_collision_probe_event(
            runtime, result, task, local, transformed, candidate, reverse);
        return std::pair{candidate, transformed.status};
    };

    auto [candidate, transform_status] = probe(task.collision_current, false);
    if (candidate == -1 && transform_status == BattleCollisionModelStatus::Matched) {
        const auto reverse = reverse_half_step_battle_collision_vector(
            task.collision_current,
            task.collision_velocity);
        std::tie(candidate, transform_status) = probe(reverse, true);
    }

    if (transform_status == BattleCollisionModelStatus::Matched
        && candidate >= 0
        && candidate < static_cast<int>(task.collision_visited.size())
        && candidate != task.origin_slot) {
        const auto* candidate_combatant =
            find_frame_combatant(runtime.state, candidate);
        if (candidate_combatant != nullptr
            && candidate_combatant->present
            && candidate_combatant->alive
            && !task.collision_visited[static_cast<std::size_t>(candidate)]) {
            task.collision_visited[static_cast<std::size_t>(candidate)] = true;
            if (candidate == task.target_slot) {
                task.collision_selector_invoked = true;
                task.collision_state = 0xFA;
                (void)run_direct_transition_selector(
                    runtime,
                    result,
                    task,
                    DirectInstructionTransitionProducer::CollisionBox,
                    BattleFrameWorkerStepKind::DirectTransitionSelect,
                    "FUN_8004BB9C/FUN_8002E5D0",
                    "fun_8002eb4c_collision_box",
                    rng_state);
            }
        }
    }

    if (payload.end_counter == 0 || task.collision_counter < payload.end_counter) {
        task.collision_current = advance_battle_collision_vector(
            task.collision_current,
            task.collision_velocity);
    }
    ++task.collision_counter;
}

bool mode0_rewrite_gate_for_task(
    const BattleFrameRuntime& runtime,
    const BattleFrameVisualChildTask& task) {
    const auto* resource = visual_resource_for(runtime, task.origin_slot);
    return resource != nullptr
        && resource->binding.mode0_rewrite_gate.value_or(false);
}

void consume_mode0e_draw(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::uint32_t& rng_state) {
    if (task.mode0e_draw_consumed) {
        return;
    }
    const auto draw = draw_rand15(rng_state);
    auto event = make_visual_event(
        runtime,
        &task,
        BattleFrameWorkerStepKind::VisualMode0eCamera,
        "FUN_80052B24",
        BattleFrameEventStatus::Provisional,
        "effective_mode=0x0E; exactly one 0x80052BF0 draw for this record lifetime");
    event.rng_event = true;
    event.rng_label = "mode0e_action_view_camera";
    event.draws_consumed = 1;
    event.rng_seed_before = rng_state;
    event.rng_seed_after = draw.next_state;
    event.rand_value = draw.value;
    event.visual_effective_mode = 0x0e;
    rng_state = draw.next_state;
    task.mode0e_draw_consumed = true;
    append_recorded_event(runtime, result, std::move(event));
}

void consume_mode1_pathing(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::uint32_t& rng_state) {
    if (task.mode1_pathing_consumed) {
        return;
    }
    std::optional<bool> attack_landed;
    if (runtime.active_action.has_value()
        && runtime.active_action->action_ordinal == task.action_ordinal
        && runtime.active_action->action_resolution_available) {
        attack_landed = runtime.active_action->attack_landed;
    }
    const auto tail = model_first_battle_action_view_pathing_tail({
        .profile_name = runtime.visual.pathing_profile_name,
        .actor_slot = task.origin_slot,
        .target_slot = task.target_slot,
        .combatant_action_mode = task.derived_mode,
        .attack_landed = attack_landed,
        .counter_follow_up = false,
        .rng_seed_before = rng_state,
        .frame_state = &runtime.state,
    });
    for (const auto& step : tail.steps) {
        auto event = make_visual_event(
            runtime,
            &task,
            BattleFrameWorkerStepKind::VisualMode1Pathing,
            "FUN_8005259C/" + step.label,
            frame_event_status(step.status),
            step.detail + "; invoked_from_action_view_record_callback=1"
                + (attack_landed.has_value()
                    ? "; action_result_known=1"
                    : "; action_result_known=0; dispatch_is_record_visit_driven=1"));
        if (!attack_landed.has_value()
            && event.status == BattleFrameEventStatus::Matched) {
            event.status = BattleFrameEventStatus::Provisional;
        }
        event.rng_label = step.label;
        event.draws_consumed = step.draws_consumed;
        if (step.draws_consumed > 0) {
            event.rng_event = true;
            event.rng_seed_before = rng_state;
            advance_without_rand_values(rng_state, step.draws_consumed);
            event.rng_seed_after = rng_state;
        }
        append_recorded_event(runtime, result, std::move(event));
    }
    task.mode1_pathing_consumed = true;
}

void advance_action_view_child(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    BattleFrameVisualChildTask& task,
    std::uint32_t& rng_state) {
    if (task.phase == BattleFrameVisualChildPhase::Published) {
        task.phase = BattleFrameVisualChildPhase::Active;
        task.thread_state_0x19 = 2;
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualChildState0,
                "UpdateActionViewRecord_80051264_state0",
                frame_event_status(task.status),
                "record state-0 setup; payload_mode=" + std::to_string(task.payload_mode)
                    + "; publication_frame=" + std::to_string(task.publication_frame)
                    + "; provenance=" + task.provenance));

        auto rng_plan = combatant_visual_action_view_rng_plan(
            static_cast<std::int16_t>(task.payload_mode),
            static_cast<std::int16_t>(task.effective_mode));
        if (rng_plan.mode0_rewrite_draw) {
            if (mode0_rewrite_gate_for_task(runtime, task)) {
                const auto draw = draw_rand15(rng_state);
                task.mode0_draw_consumed = true;
                task.effective_mode = (draw.value % 2U) == 0U ? 0x0e : 0;
                auto event = make_visual_event(
                    runtime,
                    &task,
                    BattleFrameWorkerStepKind::VisualMode0Rewrite,
                    "UpdateActionViewRecord_80051264",
                    BattleFrameEventStatus::Provisional,
                    "mode-0 rewrite gate; even rand and zero camera override select mode 0x0E"
                    "; gate supplied by explicit instruction/resource producer state");
                event.rng_event = true;
                event.rng_label = "action_view_record_mode0";
                event.draws_consumed = 1;
                event.rng_seed_before = rng_state;
                event.rng_seed_after = draw.next_state;
                event.rand_value = draw.value;
                event.visual_effective_mode = task.effective_mode;
                rng_state = draw.next_state;
                append_recorded_event(runtime, result, std::move(event));
                rng_plan = combatant_visual_action_view_rng_plan(
                    static_cast<std::int16_t>(task.payload_mode),
                    static_cast<std::int16_t>(task.effective_mode));
            } else {
                append_recorded_event(
                    runtime,
                    result,
                    make_visual_event(
                        runtime,
                        &task,
                        BattleFrameWorkerStepKind::VisualUnsupportedWait,
                        "UpdateActionViewRecord_80051264",
                        BattleFrameEventStatus::Provisional,
                        "mode-0 rewrite gate is unknown; no RNG draw was invented"));
            }
        }
        if (rng_plan.mode0e_camera_draw) {
            consume_mode0e_draw(runtime, result, task, rng_state);
        }
        if (rng_plan.mode1_pathing_callback) {
            consume_mode1_pathing(runtime, result, task, rng_state);
        }
    } else if (combatant_visual_action_view_rng_plan(
                   static_cast<std::int16_t>(task.payload_mode),
                   static_cast<std::int16_t>(task.effective_mode))
                   .mode1_pathing_callback
               && !task.mode1_pathing_consumed) {
        consume_mode1_pathing(runtime, result, task, rng_state);
    }

    if (task.synthetic && task.visits >= task.maximum_visits) {
        append_visual_cleanup(
            runtime,
            result,
            task,
            "bounded synthetic action-view record completion");
        return;
    }
    if (!task.synthetic && task.visits >= task.maximum_visits) {
        append_visual_cleanup(
            runtime,
            result,
            task,
            "decoded camera timing reached bounded record completion");
        return;
    }
    if (task.effective_mode != 0
        && task.effective_mode != 1
        && task.effective_mode != 0x0e
        && task.visits == 1) {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualUnsupportedWait,
                "UpdateActionViewRecord_80051264_unsupported_mode",
                BattleFrameEventStatus::Provisional,
                "unsupported record mode consumes no RNG and uses bounded completion wait"));
    }
}

void advance_visual_child_task(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    std::uint32_t& rng_state,
    BattleFrameVisualChildTask& task) {
    ++task.visits;
    if (task.visits > task.maximum_visits + 2) {
        append_visual_cleanup(
            runtime,
            result,
            task,
            "bounded diagnostic completion prevented a visual child deadlock");
        return;
    }
    if (task.kind == BattleFrameVisualChildKind::ActionService) {
        advance_action_service_child(runtime, result, task, rng_state);
    } else if (task.kind == BattleFrameVisualChildKind::CollisionBox) {
        advance_collision_box_child(runtime, result, task, rng_state);
    } else if (task.kind == BattleFrameVisualChildKind::ActionViewRecord) {
        advance_action_view_child(runtime, result, task, rng_state);
    } else {
        append_recorded_event(
            runtime,
            result,
            make_visual_event(
                runtime,
                &task,
                BattleFrameWorkerStepKind::VisualUnsupportedWait,
                visual_child_handler_name(task.command_kind),
                BattleFrameEventStatus::Provisional,
                "child creation and list position are evidence-backed; callback lifetime is provisional and consumes zero RNG"));
        append_visual_cleanup(
            runtime,
            result,
            task,
            "unsupported auxiliary child completed after one typed zero-draw visit");
    }
    flush_pending_visual_events(runtime, result);
}

void advance_visual_children_at_cursor(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    std::uint32_t& rng_state,
    int visit_cursor) {
    const auto task_count = runtime.visual.child_tasks.size();
    for (std::size_t index = 0; index < task_count; ++index) {
        auto& task = runtime.visual.child_tasks[index];
        if (task.thread_node_id >= 0
            || task.complete
            || runtime.state.frame_index < task.first_eligible_frame
            || task.publication_visit_cursor != visit_cursor) {
            continue;
        }
        advance_visual_child_task(runtime, result, rng_state, task);
    }
}

bool advance_visual_child_thread(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result,
    std::uint32_t& rng_state,
    int thread_node_id) {
    const auto found = std::find_if(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [thread_node_id](const BattleFrameVisualChildTask& task) {
            return task.thread_node_id == thread_node_id;
        });
    if (found == runtime.visual.child_tasks.end() || found->complete) {
        return false;
    }
    if (runtime.state.frame_index < found->first_eligible_frame) {
        return true;
    }
    advance_visual_child_task(runtime, result, rng_state, *found);
    return true;
}

BattleFramePendingMovementInvocation* find_pending_invocation(
    BattleFrameRuntime& runtime,
    int worker_index) {
    const auto found = std::find_if(
        runtime.pending_movement_invocations.begin(),
        runtime.pending_movement_invocations.end(),
        [worker_index](const BattleFramePendingMovementInvocation& pending) {
            return pending.worker_index == worker_index
                && pending.lifecycle == BattleFrameMovementInvocationLifecycle::Pending;
        });
    return found == runtime.pending_movement_invocations.end() ? nullptr : &*found;
}

void append_invocation_history(
    BattleFrameRuntime& runtime,
    const BattleFramePendingMovementInvocation& pending,
    BattleFrameMovementInvocationLifecycle lifecycle,
    BattleMovementControllerState old_state,
    BattleMovementControllerState new_state,
    std::string provenance) {
    runtime.movement_invocation_history.push_back(
        BattleFrameMovementInvocationHistoryEvent{
            .frame_index = runtime.state.frame_index,
            .action_ordinal = pending.decision.action_ordinal,
            .slot = pending.decision.slot,
            .semantic_target_slot = pending.decision.semantic_target_slot.value_or(-1),
            .thread_order_index = pending.decision.thread_order_index,
            .controller_family = pending.decision.controller_family,
            .relation_route = pending.decision.relation_route,
            .activation_timing = pending.decision.activation_timing,
            .lifecycle = lifecycle,
            .old_controller_state = old_state,
            .new_controller_state = new_state,
            .draws_consumed = 0,
            .provenance = std::move(provenance),
        });
}

BattleFrameStepEvent make_invocation_event(
    BattleFrameRuntime& runtime,
    const BattleFramePendingMovementInvocation& pending,
    BattleFrameWorkerStepKind step_kind,
    const char* callback,
    BattleFrameEventStatus status,
    BattleMovementControllerState old_state,
    BattleMovementControllerState new_state,
    std::string detail) {
    BattleFrameStepEvent event;
    event.action_ordinal = pending.decision.action_ordinal;
    event.frame_index = runtime.state.frame_index;
    event.slot = pending.decision.slot;
    event.target_slot = pending.decision.semantic_target_slot.value_or(-1);
    event.callback = callback;
    event.worker_kind = frame_worker_kind(pending.decision.worker_kind);
    event.step_kind = step_kind;
    event.status = status;
    event.controller_family = pending.decision.controller_family;
    event.relation_route = pending.decision.relation_route;
    event.activation_timing = pending.decision.activation_timing;
    event.thread_order_index = pending.decision.thread_order_index;
    event.old_controller_state = old_state;
    event.new_controller_state = new_state;
    event.invocation_confidence = pending.decision.confidence;
    event.invocation_provenance = pending.decision.provenance;
    if (runtime.active_action.has_value()) {
        event.action_phase = runtime.active_action->phase;
        event.passive_completion_mask_before =
            runtime.active_action->passive_completion_mask;
        event.passive_completion_mask_after =
            runtime.active_action->passive_completion_mask;
        event.completion_turn_phase =
            runtime.active_action->completion_turn_phase;
        event.completion_override = runtime.active_action->completion_override;
    }
    event.detail = std::move(detail);

    const auto* combatant = find_frame_combatant(runtime.state, event.slot);
    if (combatant != nullptr) {
        event.combatant_state_available = true;
        event.old_action_mode = combatant->combatant_action_mode;
        event.new_action_mode = combatant->combatant_action_mode;
        event.old_grid = combatant->grid_position;
        event.new_grid = combatant->grid_position;
        event.old_pos_holder = combatant->pos_holder;
        event.new_pos_holder = combatant->pos_holder;
        event.old_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
        event.new_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
        event.old_combatant_facing_angle_0x2c =
            combatant->combatant_facing_angle_0x2c;
        event.new_combatant_facing_angle_0x2c =
            combatant->combatant_facing_angle_0x2c;
    }
    return event;
}

std::string invocation_detail(
    const BattleFramePendingMovementInvocation& pending,
    std::string transition) {
    std::ostringstream detail;
    detail << "action_ordinal=" << pending.decision.action_ordinal
           << "; controller_family="
           << battle_movement_controller_family_name(
               pending.decision.controller_family)
           << "; activation_timing="
           << battle_movement_activation_timing_name(
               pending.decision.activation_timing)
           << "; semantic_target_slot="
           << pending.decision.semantic_target_slot.value_or(-1)
           << "; relation_route="
           << battle_movement_relation_route_name(pending.decision.relation_route)
           << "; thread_order_index="
           << pending.decision.thread_order_index
           << "; invocation_status="
           << battle_movement_invocation_status_name(
               pending.decision.status)
           << "; draws=0; transition=" << transition
           << "; confidence=" << pending.decision.confidence
           << "; provenance=" << pending.decision.provenance;
    return detail.str();
}

void activate_pending_invocation(
    BattleFrameRuntime& runtime,
    int worker_index,
    BattleFrameRunResult& result) {
    auto* pending = find_pending_invocation(runtime, worker_index);
    if (pending == nullptr) {
        runtime.workers[static_cast<std::size_t>(worker_index)].activation_pending = false;
        runtime.warnings.push_back(
            "movement worker had no matching pending invocation record");
        return;
    }

    auto& worker = runtime.workers[static_cast<std::size_t>(worker_index)];
    const auto slot_index = static_cast<std::size_t>(worker.slot);
    if (worker.kind == BattleFrameWorkerKind::PassiveController) {
        if (auto* combatant = find_frame_combatant(runtime.state, worker.slot);
            combatant != nullptr) {
            combatant->queued_controller_state = 0;
        }
    }
    const auto old_state = runtime.movement_controller_states[slot_index];
    runtime.movement_controller_states[slot_index] =
        pending->decision.activation_controller_state;
    append_recorded_event(
        runtime,
        result,
        make_invocation_event(
            runtime,
            *pending,
            BattleFrameWorkerStepKind::MovementInvocationActivate,
            "MovementInvocationActivate",
            frame_event_status(pending->decision.status),
            old_state,
            pending->decision.activation_controller_state,
            invocation_detail(*pending, "activate")));
    append_invocation_history(
        runtime,
        *pending,
        BattleFrameMovementInvocationLifecycle::Activated,
        old_state,
        pending->decision.activation_controller_state,
        pending->decision.provenance);

    if (pending->decision.activation_timing
        == BattleMovementActivationTiming::SameThreadVisitAfterHandoff) {
        const auto handler_state = runtime.movement_controller_states[slot_index];
        runtime.movement_controller_states[slot_index] =
            pending->decision.worker_controller_state;
        append_recorded_event(
            runtime,
            result,
            make_invocation_event(
                runtime,
                *pending,
                BattleFrameWorkerStepKind::MovementControllerHandoff,
                "EnemyMovementHandlerHandoff",
                BattleFrameEventStatus::Provisional,
                handler_state,
                pending->decision.worker_controller_state,
                invocation_detail(*pending, "enemy_handler_to_direct_same_visit")));
        append_invocation_history(
            runtime,
            *pending,
            BattleFrameMovementInvocationLifecycle::Handoff,
            handler_state,
            pending->decision.worker_controller_state,
            pending->decision.provenance);
    } else {
        runtime.movement_controller_states[slot_index] =
            pending->decision.worker_controller_state;
    }

    pending->lifecycle = BattleFrameMovementInvocationLifecycle::Activated;
    worker.activation_pending = false;
}

void skip_unvisitable_pending_invocations(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    for (auto& pending : runtime.pending_movement_invocations) {
        if (pending.lifecycle != BattleFrameMovementInvocationLifecycle::Pending
            || pending.worker_index < 0
            || pending.worker_index >= static_cast<int>(runtime.workers.size())) {
            continue;
        }
        auto& worker = runtime.workers[static_cast<std::size_t>(pending.worker_index)];
        const auto* combatant = find_frame_combatant(runtime.state, worker.slot);
        const bool has_thread = has_active_thread_for_slot(
            runtime, BattleFrameThreadNodeKind::MovementController, worker.slot);
        if (combatant != nullptr && combatant->present && combatant->alive && has_thread) {
            continue;
        }

        const auto slot_index = static_cast<std::size_t>(worker.slot);
        const auto old_state = runtime.movement_controller_states[slot_index];
        runtime.movement_controller_states[slot_index] =
            BattleMovementControllerState::Idle;
        const auto status = !has_thread
            ? BattleFrameEventStatus::MissingInput
            : BattleFrameEventStatus::Skipped;
        append_recorded_event(
            runtime,
            result,
            make_invocation_event(
                runtime,
                pending,
                BattleFrameWorkerStepKind::MovementInvocationSkipped,
                "MovementInvocationSkipped",
                status,
                old_state,
                BattleMovementControllerState::Idle,
                invocation_detail(
                    pending,
                    has_thread ? "combatant_absent_or_dead" : "packed_thread_missing")));
        append_invocation_history(
            runtime,
            pending,
            BattleFrameMovementInvocationLifecycle::Skipped,
            old_state,
            BattleMovementControllerState::Idle,
            pending.decision.provenance);
        pending.lifecycle = BattleFrameMovementInvocationLifecycle::Skipped;
        worker.activation_pending = false;
        worker.complete = true;
    }
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
        event.combatant_state_available = true;
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
    if (worker.action_ordinal >= 0) {
        detail << "; action_ordinal=" << worker.action_ordinal
               << "; controller_family="
               << battle_movement_controller_family_name(worker.controller_family)
               << "; activation_timing="
               << battle_movement_activation_timing_name(worker.activation_timing)
               << "; semantic_target_slot=" << worker.target_slot
               << "; thread_order_index=" << worker.thread_order_index
               << "; invocation_confidence=" << worker.invocation_confidence
               << "; invocation_provenance=" << worker.invocation_provenance;
    }
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
    event.destination_source = worker.destination_source;
    if (worker.destination_source != MovementCommitDestinationSource::Unknown) {
        event.selected_path_node = worker.destination_grid;
    }
    event.detail += "; dist_to_target_0x14="
        + std::to_string(worker.movement_path.dist_to_target_0x14);
    event.detail += "; old_path_index_0x15=" + std::to_string(event.old_path_index_0x15);
    event.detail += "; new_path_index_0x15=" + std::to_string(event.new_path_index_0x15);
    event.detail += "; status_0x16=" + std::to_string(worker.movement_path.status_0x16);
    event.detail += "; selected_path_node=" + movement_grid_detail(worker.destination_grid);
    event.detail += "; path_entries=" + movement_path_entries_detail(worker.movement_path);
    event.detail += "; destination_source="
        + std::string(movement_commit_destination_source_name(worker.destination_source));
    if (worker.movement_path.zero_distance_target) {
        event.detail += "; zero_distance_path=1";
    }
}

void restore_grid_footprint_from_base(
    BattleFrameState& state,
    const BattleFrameCombatantState& combatant,
    const MovementGridPosition& position) {
    for (int dz = 0; dz < std::max(1, combatant.depth); ++dz) {
        for (int dx = 0; dx < std::max(1, combatant.width); ++dx) {
            const int x = position.grid_x + dx;
            const int z = position.grid_z + dz;
            if (x >= 0 && x < 11 && z >= 0 && z < 11) {
                const auto index = static_cast<std::size_t>(z * 11 + x);
                state.active_grid[index] = state.base_grid[index];
            }
        }
    }
}

void publish_grid_footprint(
    BattleFrameState& state,
    const BattleFrameCombatantState& combatant,
    const MovementGridPosition& position) {
    const auto marker = static_cast<std::uint8_t>(combatant.slot + 0x50);
    for (int dz = 0; dz < std::max(1, combatant.depth); ++dz) {
        for (int dx = 0; dx < std::max(1, combatant.width); ++dx) {
            const int x = position.grid_x + dx;
            const int z = position.grid_z + dz;
            if (x >= 0 && x < 11 && z >= 0 && z < 11) {
                state.active_grid[static_cast<std::size_t>(z * 11 + x)] = marker;
            }
        }
    }
}

void refresh_completed_route_grid_80081648(
    BattleFrameState& state,
    const BattleFrameWorker& worker) {
    const auto* combatant = find_frame_combatant(state, worker.slot);
    if (combatant == nullptr) {
        return;
    }
    const int delta_x = combatant->previous_grid_position.grid_x
        - combatant->grid_position.grid_x;
    const int delta_z = combatant->previous_grid_position.grid_z
        - combatant->grid_position.grid_z;
    const int distance = std::max(std::abs(delta_x), std::abs(delta_z));
    int traversed_x = 0;
    int traversed_z = 0;
    for (int i = 0; i < distance; ++i) {
        restore_grid_footprint_from_base(
            state,
            *combatant,
            MovementGridPosition{
                .grid_x = combatant->previous_grid_position.grid_x - traversed_x,
                .grid_z = combatant->previous_grid_position.grid_z - traversed_z,
            });
        traversed_x += delta_x / distance;
        traversed_z += delta_z / distance;
    }
    for (const auto& current : state.combatants) {
        if (current.present && current.alive) {
            publish_grid_footprint(state, current, current.grid_position);
        }
    }
}

void discard_uncommitted_route_grid(
    BattleFrameState& state,
    const BattleFrameWorker& worker) {
    (void)state;
    (void)worker;
    // Route markers remain private to the movement worksheet until a grid commit.
}

bool publish_combatant_instruction(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    BattleFrameStepEvent& event) {
    if (worker.slot < 0
        || worker.slot >= static_cast<int>(runtime.combatant_instructions.size())) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.detail += "; combatant instruction publication has no valid slot";
        return false;
    }
    if (!has_active_thread_for_slot(
            runtime,
            BattleFrameThreadNodeKind::CombatantInstruction,
            worker.slot)) {
        auto& instruction = runtime.combatant_instructions[
            static_cast<std::size_t>(worker.slot)];
        instruction.active = false;
        instruction.phase = BattleFrameCombatantInstructionPhase::Unsupported;
        worker.waiting_for_combatant_instruction = false;
        worker.event_status = BattleFrameEventStatus::MissingInput;
        worker.complete = true;
        event.status = BattleFrameEventStatus::MissingInput;
        event.instruction_phase_before = instruction.phase;
        event.instruction_phase_after = instruction.phase;
        event.detail +=
            "; no producer-created combatant instruction thread exists for this slot"
            "; STD resource publication order is required before motion can continue";
        return false;
    }
    auto& instruction = runtime.combatant_instructions[
        static_cast<std::size_t>(worker.slot)];
    event.instruction_phase_before = instruction.phase;
    if (instruction.active
        && instruction.phase != BattleFrameCombatantInstructionPhase::Complete
        && instruction.phase != BattleFrameCombatantInstructionPhase::Removed
        && instruction.phase != BattleFrameCombatantInstructionPhase::Unsupported) {
        event.status = BattleFrameEventStatus::Provisional;
        event.combatant_instruction_revision = instruction.revision;
        event.instruction_phase_after = instruction.phase;
        event.detail += "; prior persistent combatant instruction remains active; publication deferred";
        worker.waiting_for_combatant_instruction = true;
        return false;
    }

    const int revision = instruction.revision + 1;
    auto* combatant = find_frame_combatant(runtime.state, worker.slot);
    const auto published_controller_mode = combatant != nullptr
        ? combatant->combatant_action_mode
        : action_mode_for_worker(worker);
    const auto motion_action_mode = action_motion_mode_for_worker(
        worker,
        published_controller_mode);
    if (combatant == nullptr) {
        instruction.active = false;
        instruction.phase = BattleFrameCombatantInstructionPhase::Unsupported;
        worker.waiting_for_combatant_instruction = false;
        worker.event_status = BattleFrameEventStatus::MissingInput;
        worker.complete = true;
        event.status = BattleFrameEventStatus::MissingInput;
        event.instruction_phase_after = instruction.phase;
        event.detail += "; selected-action-row producer has no combatant state";
        return false;
    }

    const int selected_action_mode = motion_action_mode;

    CombatantStdActionRowSelectionResult selected_row;
    const auto* resource = visual_resource_for(runtime, worker.slot);
    if (resource != nullptr) {
        selected_row = select_combatant_std_action_row(
            resource->action_rows,
            CombatantStdActionRowSelectionRequest{
                .action_id = static_cast<std::int16_t>(selected_action_mode),
                .secondary_key = combatant->visual_instruction_subtype_0x8 >= 0
                    ? std::optional<std::int16_t>{
                        combatant->visual_instruction_subtype_0x8}
                    : std::nullopt,
                .allow_transition_fallback = true,
            });
    } else if (combatant->selected_action_row_known) {
        selected_row.status = CombatantStdActionRowSelectionStatus::Matched;
        selected_row.requested_action_id = selected_action_mode;
        selected_row.selected_action_id =
            combatant->selected_action_row_action_id >= 0
            ? combatant->selected_action_row_action_id
            : static_cast<std::int16_t>(selected_action_mode);
        selected_row.row = CombatantStdActionRow{
            .index = combatant->selected_action_row_index,
            .action_id = selected_row.selected_action_id,
            .callback_index = combatant->selected_action_row_callback_index,
            .callback_ordinal = combatant->selected_action_row_callback_ordinal,
            .flags = combatant->selected_action_row_flags,
            .transition_gate_divisor_bits =
                combatant->selected_action_row_duration_bits,
        };
        selected_row.provenance =
            "selected action row was explicitly supplied by a low-level runtime fixture";
    } else {
        selected_row.status = CombatantStdActionRowSelectionStatus::MissingInput;
        selected_row.provenance =
            "selected-action-row producer has no published STD resource";
    }
    if (!selected_row.row.has_value()) {
        instruction.active = false;
        instruction.phase = BattleFrameCombatantInstructionPhase::Unsupported;
        worker.waiting_for_combatant_instruction = false;
        worker.event_status = selected_row.status
                == CombatantStdActionRowSelectionStatus::MissingInput
            ? BattleFrameEventStatus::MissingInput
            : BattleFrameEventStatus::Unsupported;
        worker.complete = true;
        event.status = worker.event_status;
        event.instruction_phase_after = instruction.phase;
        event.detail += "; selected_action_row_status="
            + std::string(combatant_std_action_row_selection_status_name(
                selected_row.status))
            + "; " + selected_row.provenance
            + "; no movement or RNG was synthesized";
        return false;
    }
    apply_selected_action_row(
        *combatant,
        *selected_row.row,
        selected_row.selected_action_id,
        resource != nullptr);
    if (selected_row.status == CombatantStdActionRowSelectionStatus::Provisional
        && event.status == BattleFrameEventStatus::Matched) {
        event.status = BattleFrameEventStatus::Provisional;
    }
    instruction = BattleFrameCombatantInstructionRuntime{
        .active = true,
        .revision = revision,
        .action_ordinal = worker.action_ordinal,
        .slot = worker.slot,
        .target_slot = worker.target_slot,
        .owner_worker_queue_sequence = worker.queue_sequence,
        .controller_worker_kind = worker.kind,
        .controller_family = worker.controller_family,
        .relation_route = worker.relation_route,
        .thread_order_index = worker.thread_order_index,
        .action_mode = motion_action_mode,
        .provisional_fallback_target = worker.destination_position,
        .destination_grid = worker.destination_grid,
        .destination_source = worker.destination_source,
        .movement_path = worker.movement_path,
        .status = worker.event_status,
        .phase = BattleFrameCombatantInstructionPhase::SetupPending,
        .provenance =
            "movement controller staged mode/row inputs; the next persistent FUN_80022850 state-1 visit owns FUN_800221FC and IW+0xE0 publication",
    };
    worker.combatant_instruction_revision = revision;
    worker.waiting_for_combatant_instruction = true;
    CombatantVisualInstructionSnapshot visual_instruction;
    visual_instruction.slot = worker.slot;
    visual_instruction.runtime_instruction_mode = static_cast<std::int16_t>(
        motion_action_mode);
    visual_instruction.selected_std_action_key = selected_row.selected_action_id;
    visual_instruction.subtype = combatant->visual_instruction_subtype_0x8 >= 0
        ? std::optional<std::int16_t>{
            combatant->visual_instruction_subtype_0x8}
        : std::nullopt;
    visual_instruction.target_slot = worker.target_slot;
    visual_instruction.instruction_flags = combatant->instruction_flags_0xec;
    visual_instruction.knowledge =
        selected_row.status == CombatantStdActionRowSelectionStatus::Matched
        && worker.event_status == BattleFrameEventStatus::Matched
        ? CombatantVisualInstructionKnowledge::Known
        : CombatantVisualInstructionKnowledge::Provisional;
    visual_instruction.provenance =
        "movement controller staged the action-motion mode and selected row without publishing IW+0xE0; "
        + selected_row.provenance;
    (void)stage_battle_frame_visual_instruction_state(
        runtime,
        worker.action_ordinal,
        std::move(visual_instruction));
    event.combatant_instruction_revision = revision;
    event.instruction_phase_after = instruction.phase;
    event.detail += "; published combatant_instruction_revision="
        + std::to_string(revision)
        + "; motion_action_mode=" + std::to_string(motion_action_mode)
        + "; selected_action_row=" + std::to_string(selected_row.row->index)
        + "; selected_action_id=" + std::to_string(selected_row.selected_action_id)
        + "; selected_action_row_status="
        + combatant_std_action_row_selection_status_name(selected_row.status)
        + "; selected_action_row_provenance=" + selected_row.provenance
        + "; consumer=FUN_80022850; ownership=controller_stage_then_instruction_thread_publish";
    return true;
}

bool combatant_instruction_completed_for_worker(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    BattleFrameStepEvent& event) {
    if (worker.slot < 0
        || worker.slot >= static_cast<int>(runtime.combatant_instructions.size())) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.detail += "; combatant instruction wait has no valid slot";
        return false;
    }
    auto& instruction = runtime.combatant_instructions[
        static_cast<std::size_t>(worker.slot)];
    event.combatant_instruction_revision = instruction.revision;
    event.instruction_phase_before = instruction.phase;
    event.instruction_phase_after = instruction.phase;
    if (instruction.revision != worker.combatant_instruction_revision
        || instruction.owner_worker_queue_sequence != worker.queue_sequence) {
        event.status = BattleFrameEventStatus::MissingInput;
        event.detail += "; combatant instruction ownership/revision mismatch";
        return false;
    }
    if (instruction.phase == BattleFrameCombatantInstructionPhase::Complete) {
        instruction.active = false;
        worker.waiting_for_combatant_instruction = false;
        ++worker.completed_motion_legs;
        event.detail += "; persistent combatant instruction completed; controller resumes";
        return true;
    }
    if (instruction.phase == BattleFrameCombatantInstructionPhase::Removed
        || instruction.phase == BattleFrameCombatantInstructionPhase::Unsupported) {
        instruction.active = false;
        worker.waiting_for_combatant_instruction = false;
        event.status = instruction.phase == BattleFrameCombatantInstructionPhase::Removed
            ? BattleFrameEventStatus::Skipped
            : BattleFrameEventStatus::Unsupported;
        event.detail += "; persistent combatant instruction terminated without normal completion";
        return true;
    }
    worker.waiting_for_combatant_instruction = true;
    event.detail += "; persistent combatant instruction phase="
        + std::string(battle_frame_combatant_instruction_phase_name(instruction.phase))
        + "; controller remains parked";
    return false;
}

std::optional<BattleFrameStepEvent> advance_combatant_instruction(
    BattleFrameRuntime& runtime,
    int slot) {
    if (slot < 0 || slot >= static_cast<int>(runtime.combatant_instructions.size())) {
        return std::nullopt;
    }
    auto& instruction = runtime.combatant_instructions[static_cast<std::size_t>(slot)];
    if (!instruction.active
        || instruction.phase == BattleFrameCombatantInstructionPhase::Complete
        || instruction.phase == BattleFrameCombatantInstructionPhase::Removed
        || instruction.phase == BattleFrameCombatantInstructionPhase::Unsupported) {
        return std::nullopt;
    }

    BattleFrameStepEvent event;
    event.action_ordinal = instruction.action_ordinal;
    event.frame_index = runtime.state.frame_index;
    event.slot = instruction.slot;
    event.target_slot = instruction.target_slot;
    event.callback = "FUN_80022850";
    event.worker_kind = BattleFrameWorkerKind::CombatantInstruction;
    event.status = instruction.status;
    event.callback_pc = 0x80022850u;
    event.controller_family = instruction.controller_family;
    event.relation_route = instruction.relation_route;
    event.thread_order_index = instruction.thread_order_index;
    event.destination_source = instruction.destination_source;
    event.movement_path = instruction.movement_path;
    event.combatant_instruction_revision = instruction.revision;
    event.instruction_phase_before = instruction.phase;
    if (instruction.destination_source != MovementCommitDestinationSource::Unknown) {
        event.selected_path_node = instruction.destination_grid;
    }
    if (runtime.active_action.has_value()
        && runtime.active_action->action_ordinal == instruction.action_ordinal) {
        event.action_phase = runtime.active_action->phase;
        event.passive_completion_mask_before =
            runtime.active_action->passive_completion_mask;
        event.passive_completion_mask_after =
            runtime.active_action->passive_completion_mask;
    }

    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant == nullptr || !combatant->present || !combatant->alive) {
        instruction.phase = BattleFrameCombatantInstructionPhase::Removed;
        instruction.active = false;
        event.status = BattleFrameEventStatus::Skipped;
        event.step_kind = BattleFrameWorkerStepKind::CombatantInstructionWait;
        event.instruction_phase_after = instruction.phase;
        event.detail = "persistent combatant thread was removed before instruction completion";
        return event;
    }

    event.old_action_mode = combatant->combatant_action_mode;
    event.combatant_state_available = true;
    event.old_grid = combatant->grid_position;
    event.new_grid = combatant->grid_position;
    event.old_path_index_0x15 = instruction.movement_path.path_index_0x15;
    event.new_path_index_0x15 = instruction.movement_path.path_index_0x15;
    event.old_pos_holder = combatant->pos_holder;
    event.new_pos_holder = combatant->pos_holder;
    event.old_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
    event.old_combatant_facing_angle_0x2c = combatant->combatant_facing_angle_0x2c;
    combatant->combatant_action_mode = instruction.action_mode;

    switch (instruction.phase) {
    case BattleFrameCombatantInstructionPhase::SetupPending:
        event.step_kind = BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc;
        event.step_pc = 0x8001fabcu;
        event.helper_pc = 0x8001fabcu;
        seed_action_motion_8001fabc(
            *combatant, instruction.provisional_fallback_target, event);
        instruction.phase = BattleFrameCombatantInstructionPhase::Rotating;
        break;
    case BattleFrameCombatantInstructionPhase::Rotating:
        event.step_kind =
            BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114;
        event.step_pc = 0x8001b630u;
        event.helper_pc = 0x80061114u;
        if (apply_rotation_8001b630(*combatant, event)) {
            instruction.phase = BattleFrameCombatantInstructionPhase::Moving;
        }
        break;
    case BattleFrameCombatantInstructionPhase::Moving:
        event.step_kind = BattleFrameWorkerStepKind::MoveIncrementApply_80061340;
        event.step_pc = 0x8001e910u;
        event.helper_pc = 0x80061340u;
        if (apply_move_increment_8001e910(*combatant, event)) {
            instruction.phase = BattleFrameCombatantInstructionPhase::StopPending;
        }
        break;
    case BattleFrameCombatantInstructionPhase::StopPending:
        event.step_kind = BattleFrameWorkerStepKind::MotionStopResult_8001eb54;
        event.step_pc = 0x8001eb54u;
        event.helper_pc = 0x80061340u;
        event.motion_reached_target = true;
        event.detail += "; final_position="
            + frame_vec_detail(combatant->combatant_cur_pos_0x1c)
            + "; target=" + frame_vec_detail(combatant->pos_to_move_to_0x110);
        instruction.phase = BattleFrameCombatantInstructionPhase::Complete;
        break;
    case BattleFrameCombatantInstructionPhase::Idle:
    case BattleFrameCombatantInstructionPhase::Complete:
    case BattleFrameCombatantInstructionPhase::Removed:
    case BattleFrameCombatantInstructionPhase::Unsupported:
        return std::nullopt;
    }

    event.instruction_phase_after = instruction.phase;
    event.new_action_mode = combatant->combatant_action_mode;
    event.new_grid = combatant->grid_position;
    event.new_pos_holder = combatant->pos_holder;
    event.new_combatant_cur_pos_0x1c = combatant->combatant_cur_pos_0x1c;
    event.new_combatant_facing_angle_0x2c = combatant->combatant_facing_angle_0x2c;
    event.combatant_cur_pos_changed =
        !same_vec(event.old_combatant_cur_pos_0x1c, event.new_combatant_cur_pos_0x1c);
    event.combatant_facing_angle_changed =
        event.old_combatant_facing_angle_0x2c
        != event.new_combatant_facing_angle_0x2c;
    event.detail += "; instruction_revision=" + std::to_string(instruction.revision)
        + "; phase="
        + battle_frame_combatant_instruction_phase_name(event.instruction_phase_before)
        + "->"
        + battle_frame_combatant_instruction_phase_name(event.instruction_phase_after)
        + "; owner_worker_queue_sequence="
        + std::to_string(instruction.owner_worker_queue_sequence)
        + "; " + instruction.provenance;
    if (instruction.movement_path.available) {
        event.detail += "; dist_to_target_0x14="
            + std::to_string(instruction.movement_path.dist_to_target_0x14)
            + "; old_path_index_0x15="
            + std::to_string(event.old_path_index_0x15)
            + "; new_path_index_0x15="
            + std::to_string(event.new_path_index_0x15)
            + "; status_0x16="
            + std::to_string(instruction.movement_path.status_0x16)
            + "; selected_path_node="
            + movement_grid_detail(instruction.destination_grid)
            + "; path_entries="
            + movement_path_entries_detail(instruction.movement_path)
            + "; destination_source="
            + movement_commit_destination_source_name(instruction.destination_source);
    }
    return event;
}

BattleFrameStepEvent execute_worker_frame(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    std::uint32_t* rng_state) {
    auto* combatant = find_frame_combatant(runtime.state, worker.slot);
    BattleFrameStepEvent event;
    event.action_ordinal = worker.action_ordinal;
    event.frame_index = runtime.state.frame_index;
    event.slot = worker.slot;
    event.target_slot = worker.target_slot;
    event.callback = callback_for_worker(worker);
    event.worker_kind = worker.kind;
    event.status = worker.event_status;
    event.rng_label = worker.rng_label;
    event.effect_source_key = worker.effect_source_key;
    event.destination_source = worker.destination_source;
    event.controller_family = worker.controller_family;
    event.relation_route = worker.relation_route;
    event.activation_timing = worker.activation_timing;
    event.thread_order_index = worker.thread_order_index;
    event.invocation_confidence = worker.invocation_confidence;
    event.invocation_provenance = worker.invocation_provenance;
    if (runtime.active_action.has_value()
        && runtime.active_action->action_ordinal == worker.action_ordinal) {
        event.action_phase = runtime.active_action->phase;
        event.passive_completion_mask_before =
            runtime.active_action->passive_completion_mask;
        event.passive_completion_mask_after =
            runtime.active_action->passive_completion_mask;
    }
    if (worker.slot >= 0
        && worker.slot < static_cast<int>(runtime.movement_controller_states.size())) {
        event.old_controller_state = runtime.movement_controller_states[
            static_cast<std::size_t>(worker.slot)];
        event.new_controller_state = event.old_controller_state;
    }
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
    event.combatant_state_available = true;
    event.old_grid = combatant->grid_position;
    event.movement_path = worker.movement_path;
    event.old_path_index_0x15 = worker.path_index_0x15;
    if (worker.movement_path.available
        && worker.destination_source != MovementCommitDestinationSource::Unknown) {
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
        case BattleFrameWorkerStepKind::PathBuild: {
            const auto active_grid_before = active_grid_occupancy_detail(runtime.state);
            std::string path_detail;
            bool selected = false;
            if (worker.use_supplied_movement_path_once) {
                worker.use_supplied_movement_path_once = false;
                selected = worker.movement_path.available
                    && worker.movement_path.entry_count != 0
                    && worker.path_index_0x15 < worker.movement_path.entry_count;
                path_detail = "path_source=supplied_captured_vector";
            } else if (worker.controller_family
                == BattleMovementControllerFamily::AmbientFormation) {
                const auto destination = provisional_formation_destination(
                    runtime,
                    worker.slot,
                    &path_detail);
                selected = destination.has_value()
                    && set_worker_movement_path_from_entries(
                        runtime.state,
                        worker,
                        1,
                        0,
                        2,
                        {*destination});
                worker.use_supplied_movement_path_once = false;
                worker.destination_source =
                    MovementCommitDestinationSource::GeneratedSingleSquare;
                path_detail = (selected
                    ? "path_source=provisional_FUN_80080710_generated_single_square; "
                    : "formation candidate generation found no open square; ")
                    + path_detail;
            } else {
                selected = rebuild_worker_movement_path(
                    runtime,
                    worker,
                    &path_detail);
            }
            if (worker.movement_path.available) {
                publish_worker_path_to_runtime_worksheet(runtime, worker);
            }
            event.status = worker.event_status;
            event.detail += "; active_grid_occupancy=" + active_grid_before
                + "; " + path_detail;
            event.destination_source = worker.destination_source;
            if (!selected) {
                if (worker.controller_family
                    == BattleMovementControllerFamily::AmbientPursuit) {
                    runtime.movement_controller_states[
                        static_cast<std::size_t>(worker.slot)] =
                        BattleMovementControllerState::PursuitCoordination;
                    event.new_controller_state =
                        BattleMovementControllerState::PursuitCoordination;
                    event.detail +=
                        "; pursuit_handoff=8008D6B4_or_8008C124_or_8008BFDC; typed_provisional=1";
                }
                const auto completion = std::find_if(
                    worker.program_steps.begin(),
                    worker.program_steps.end(),
                    [](const BattleFrameWorkerProgramStep& candidate) {
                        return candidate.kind == BattleFrameWorkerStepKind::PassiveCleanup
                            || candidate.kind == BattleFrameWorkerStepKind::ModeHelper;
                    });
                if (worker.movement_path.zero_distance_target
                    && completion != worker.program_steps.end()) {
                    worker.program_index = static_cast<std::size_t>(
                        std::distance(worker.program_steps.begin(), completion));
                    advance_program = false;
                    event.detail += "; already adjacent; advancing to controller cleanup";
                } else {
                    worker.complete = true;
                    advance_program = false;
                    event.detail += worker.movement_path.zero_distance_target
                        ? "; already adjacent; movement controller complete"
                        : "; no modeled commit destination; movement controller stopped";
                }
            }
            break;
        }
        case BattleFrameWorkerStepKind::PathNodeSelection:
            event.destination_source = worker.destination_source;
            event.detail += "; selected_index=" + std::to_string(worker.path_index_0x15)
                + "; destination=" + movement_grid_detail(worker.destination_grid)
                + "; source="
                + movement_commit_destination_source_name(worker.destination_source);
            break;
        case BattleFrameWorkerStepKind::CombatantInstructionPublish:
            advance_program = publish_combatant_instruction(runtime, worker, event);
            break;
        case BattleFrameWorkerStepKind::CombatantInstructionWait:
            advance_program = combatant_instruction_completed_for_worker(
                runtime, worker, event);
            break;
        case BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc:
        case BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114:
        case BattleFrameWorkerStepKind::ActionMotionMoveStep_8001e910:
        case BattleFrameWorkerStepKind::MoveIncrementApply_80061340:
        case BattleFrameWorkerStepKind::MotionStopResult_8001eb54:
            event.status = BattleFrameEventStatus::Unsupported;
            worker.event_status = BattleFrameEventStatus::Unsupported;
            worker.complete = true;
            advance_program = false;
            event.detail +=
                "; legacy controller-owned action-motion step rejected; persistent combatant instruction runtime owns this operation";
            break;
        case BattleFrameWorkerStepKind::MovementCommit:
            if (worker.controller_family
                == BattleMovementControllerFamily::AmbientPursuit) {
                const auto* target = find_frame_combatant(runtime.state, worker.target_slot);
                if (target != nullptr
                    && target->present
                    && target->alive
                    && movement_footprints_adjacent(*combatant, *target)) {
                    discard_uncommitted_route_grid(runtime.state, worker);
                    worker.movement_path.zero_distance_target = true;
                    runtime.movement_controller_states[
                        static_cast<std::size_t>(worker.slot)] =
                        BattleMovementControllerState::PursuitCoordination;
                    event.new_controller_state =
                        BattleMovementControllerState::PursuitCoordination;
                    const auto completion = std::find_if(
                        worker.program_steps.begin(),
                        worker.program_steps.end(),
                        [](const BattleFrameWorkerProgramStep& candidate) {
                            return candidate.kind
                                == BattleFrameWorkerStepKind::PassiveCleanup;
                        });
                    if (completion != worker.program_steps.end()) {
                        worker.program_index = static_cast<std::size_t>(
                            std::distance(worker.program_steps.begin(), completion));
                    } else {
                        worker.complete = true;
                    }
                    advance_program = false;
                    event.detail +=
                        "; stale pursuit route invalidated after target moved adjacent"
                        "; pursuit_handoff=8008D6B4_or_8008C124_or_8008BFDC"
                        "; grid_refresh=FUN_80081648_route_restore_and_footprint_publish";
                    break;
                }
            }
            if (!commit_movement_grid_8008178c(
                    runtime.state,
                    worker.slot,
                    worker.destination_grid)) {
                event.status = BattleFrameEventStatus::Unsupported;
                worker.event_status = BattleFrameEventStatus::Unsupported;
                worker.complete = true;
                advance_program = false;
                event.detail += "; movement commit rejected destination";
                break;
            }
            combatant = find_frame_combatant(runtime.state, worker.slot);
            if (combatant != nullptr) {
                worker.destination_committed = true;
            }
            break;
        case BattleFrameWorkerStepKind::GridRefresh:
            refresh_completed_route_grid_80081648(runtime.state, worker);
            event.detail +=
                "; grid_refresh=FUN_80081648_route_restore_and_footprint_publish";
            break;
        case BattleFrameWorkerStepKind::NextLegOrRebuildDecision:
            ++worker.completed_movement_legs;
            if (worker.kind == BattleFrameWorkerKind::PassiveController
                && runtime.active_action.has_value()
                && runtime.active_action->action_ordinal == worker.action_ordinal
                && fun_80080438_allows_completion(*runtime.active_action)) {
                const auto cleanup = passive_cleanup_step_index(worker);
                if (cleanup.has_value()) {
                    worker.program_index = *cleanup;
                    advance_program = false;
                    event.detail +=
                        "; FUN_80080438 accepted completion after leg"
                        "; turn_phase="
                        + std::to_string(runtime.active_action->completion_turn_phase)
                        + "; completion_override="
                        + std::to_string(runtime.active_action->completion_override ? 1 : 0);
                    break;
                }
            }
            if (worker.movement_leg_policy
                == BattleFrameMovementLegPolicy::AdvanceExistingPath) {
                const auto next_index = static_cast<std::size_t>(
                    worker.path_index_0x15) + 1u;
                const auto selection_step = path_node_selection_step_index(worker);
                if (next_index < worker.movement_path.entry_count
                    && selection_step.has_value()) {
                    worker.path_index_0x15 = static_cast<std::uint8_t>(next_index);
                    worker.movement_path.path_index_0x15 = worker.path_index_0x15;
                    worker.destination_grid = worker.movement_path.entries[next_index];
                    worker.destination_position = first_battle_grid_to_raw_stage_position(
                        worker.destination_grid,
                        combatant->width,
                        combatant->depth);
                    worker.destination_source =
                        MovementCommitDestinationSource::SelectedPathNode;
                    if (worker.kind == BattleFrameWorkerKind::ActiveDirectAttack) {
                        set_worker_commit_callsite(worker, 0x80086698u);
                    } else if (worker.kind
                        == BattleFrameWorkerKind::EnemyDirectAttack) {
                        set_worker_commit_callsite(worker, 0x800883ccu);
                    }
                    worker.program_index = *selection_step;
                    advance_program = false;
                    event.detail += "; advancing existing path to index="
                        + std::to_string(worker.path_index_0x15)
                        + "; destination="
                        + movement_grid_detail(worker.destination_grid);
                } else {
                    worker.complete = true;
                    advance_program = false;
                    event.detail += "; existing path exhausted; movement controller complete";
                }
            } else if (worker.movement_leg_policy
                == BattleFrameMovementLegPolicy::RebuildPath) {
                if (worker.completed_movement_legs >= worker.maximum_movement_legs) {
                    event.status = BattleFrameEventStatus::Provisional;
                    worker.event_status = BattleFrameEventStatus::Provisional;
                    worker.complete = true;
                    if (worker.controller_family
                        == BattleMovementControllerFamily::AmbientPursuit) {
                        runtime.movement_controller_states[
                            static_cast<std::size_t>(worker.slot)] =
                            BattleMovementControllerState::PursuitCoordination;
                        event.new_controller_state =
                            BattleMovementControllerState::PursuitCoordination;
                        event.detail +=
                            "; pursuit_handoff=8008D6B4_or_8008C124_or_8008BFDC";
                    }
                    advance_program = false;
                    event.detail += "; provisional movement leg cap reached";
                    break;
                }
                if (worker.controller_family
                        == BattleMovementControllerFamily::AmbientFormation
                    && worker.completed_movement_legs == 1) {
                    set_worker_commit_callsite(worker, 0x8008c920u);
                }
                worker.movement_path = {};
                worker.path_index_0x15 = 0;
                worker.destination_source = MovementCommitDestinationSource::Unknown;
                worker.program_index = worker.movement_loop_start_index;
                advance_program = false;
                event.detail += "; rebuilding path from committed grid; completed_legs="
                    + std::to_string(worker.completed_movement_legs);
            } else {
                worker.complete = true;
                advance_program = false;
                event.detail += "; movement controller completes after one leg";
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
        case BattleFrameWorkerStepKind::ViewPlacementResolve:
            if (rng_state != nullptr) {
                apply_view_placement_worker(runtime, event, worker, *rng_state);
            } else {
                event.status = BattleFrameEventStatus::MissingInput;
                event.detail += "; view-placement frame step requires RNG state";
            }
            break;
        case BattleFrameWorkerStepKind::FallbackSetupWait:
            if (!runtime.active_action.has_value()
                || runtime.active_action->action_ordinal != worker.action_ordinal) {
                event.status = BattleFrameEventStatus::MissingInput;
                worker.event_status = BattleFrameEventStatus::MissingInput;
                advance_program = false;
                event.detail += "; active action runtime is unavailable";
            } else {
                advance_program = passive_dispatch_ready_for_active_fallback(
                    runtime,
                    worker.action_ordinal);
                if (!advance_program) {
                    event.detail +=
                        "; FUN_8007FAB0 readiness remains false while passive dispatch relays are pending";
                }
            }
            break;
        case BattleFrameWorkerStepKind::FallbackMode7Publish:
            worker.waiting_for_action_resolution = false;
            event.detail +=
                "; modeled instruction producer boundary publishes mode 7 before state 0x0b";
            break;
        case BattleFrameWorkerStepKind::FallbackAttackResolutionWait:
            if (!runtime.active_action.has_value()
                || runtime.active_action->action_ordinal != worker.action_ordinal) {
                event.status = BattleFrameEventStatus::MissingInput;
                worker.event_status = BattleFrameEventStatus::MissingInput;
                advance_program = false;
                event.detail += "; active action resolution state is unavailable";
            } else if (!runtime.active_action->action_resolution_available) {
                worker.waiting_for_action_resolution = true;
                advance_program = false;
                event.detail +=
                    "; yielded packed-thread ownership to a queued mechanical attack worker";
            } else {
                worker.waiting_for_action_resolution = false;
                runtime.active_action->completion_override = true;
                worker.worksheet_state_0x19 = 0x0F;
                event.completion_override = true;
                event.completion_turn_phase =
                    runtime.active_action->completion_turn_phase;
                event.detail +=
                    "; state 0x0b observed action resolution; 0x80085f34 completion override published; next_state=0x0f";
            }
            break;
        case BattleFrameWorkerStepKind::FallbackVisualCompletionWait:
            if (!runtime.active_action.has_value()
                || runtime.active_action->action_ordinal != worker.action_ordinal
                || !runtime.active_action->completion_gate_open) {
                worker.waiting_for_action_completion = true;
                advance_program = false;
                event.detail +=
                    "; actor/target mode-0x16 completion boundary remains pending";
            } else {
                worker.waiting_for_action_completion = false;
                worker.worksheet_state_0x19 = 0x10;
                event.detail += "; post-attack visual completion boundary reached; next_state=0x10";
            }
            break;
        case BattleFrameWorkerStepKind::FallbackTerminalHandoff:
            worker.callback_pc = 0x80086C48u;
            worker.waiting_for_action_resolution = false;
            worker.waiting_for_action_completion = false;
            worker.complete = true;
            advance_program = false;
            event.detail += "; terminal_callback=0x80086c48";
            break;
        case BattleFrameWorkerStepKind::Unsupported:
            event.status = BattleFrameEventStatus::Unsupported;
            worker.event_status = BattleFrameEventStatus::Unsupported;
            break;
        case BattleFrameWorkerStepKind::MovementInvocationActivate:
        case BattleFrameWorkerStepKind::MovementControllerHandoff:
        case BattleFrameWorkerStepKind::MovementInvocationSkipped:
        case BattleFrameWorkerStepKind::ActionPhaseTransition:
        case BattleFrameWorkerStepKind::PassiveRelayPublish:
        case BattleFrameWorkerStepKind::PassiveRelayAdvance:
        case BattleFrameWorkerStepKind::PassiveDispatchPublish:
        case BattleFrameWorkerStepKind::PassiveFamilySelect:
        case BattleFrameWorkerStepKind::PassiveCompletionDeferred:
        case BattleFrameWorkerStepKind::PassiveCompletionClear:
        case BattleFrameWorkerStepKind::PassiveDeathClear:
        case BattleFrameWorkerStepKind::ActionComplete:
        case BattleFrameWorkerStepKind::CallbackEntry:
        case BattleFrameWorkerStepKind::ModeHelper:
        case BattleFrameWorkerStepKind::PassiveCleanup:
        case BattleFrameWorkerStepKind::FrameStartPositionSync:
        case BattleFrameWorkerStepKind::Marker:
        case BattleFrameWorkerStepKind::NoCommit:
            break;
        case BattleFrameWorkerStepKind::PostCommit:
            combatant->queued_controller_state = 4;
            break;
        }

        if (advance_program) {
            ++worker.program_index;
        }
        ++worker.frame;
        if (worker.complete && combatant != nullptr) {
            combatant->combatant_action_mode = BattleFrameActionMode::Standing;
        } else if (worker.program_index >= worker.program_steps.size()) {
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
    for (const auto* thread : active_battle_frame_threads(
             runtime.thread_list,
             BattleFrameThreadNodeKind::MovementController)) {
        const auto* combatant = find_frame_combatant(runtime.state, thread->owner_slot);
        if (combatant == nullptr || !combatant->present || !combatant->alive) {
            continue;
        }
        enqueue_worker(runtime, make_worker(
            runtime.state,
            thread->owner_slot,
            -1,
            BattleFrameWorkerKind::CleanupStanding,
            MovementSelectedWorker::None));
    }
}

template <typename InvocationInput>
void populate_invocation_runtime_state(
    const BattleFrameRuntime& runtime,
    InvocationInput& invocation) {
    invocation.slots.reserve(runtime.state.combatants.size());
    for (const auto& combatant : runtime.state.combatants) {
        invocation.slots.push_back(BattleMovementInvocationSlotState{
            .slot = combatant.slot,
            .present = combatant.present,
            .alive = combatant.alive,
            .is_player = combatant.is_player,
            .status_flags = combatant.status_flags,
            .movement_flags = combatant.movement_flags,
        });
    }
    const auto movement_threads = active_battle_frame_threads(
        runtime.thread_list,
        BattleFrameThreadNodeKind::MovementController);
    invocation.packed_thread_order.reserve(movement_threads.size());
    for (const auto* thread : movement_threads) {
        invocation.packed_thread_order.push_back(BattleMovementInvocationThreadState{
            .slot = thread->owner_slot,
            .active = thread->active,
        });
    }
    for (std::size_t slot = 0; slot < runtime.movement_controller_states.size(); ++slot) {
        invocation.prior_controller_states.push_back(
            BattleMovementPriorControllerState{
                .slot = static_cast<int>(slot),
                .state = runtime.movement_controller_states[slot],
            });
    }
}

BattleMovementActiveInvocationInput make_active_invocation_input(
    const BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input,
    int action_ordinal) {
    BattleMovementActiveInvocationInput invocation;
    invocation.action_ordinal = action_ordinal;
    invocation.actor_slot = input.actor_slot;
    invocation.final_target_slot = input.target_slot;
    invocation.enemy_owned = input.enemy_owned;
    invocation.selected_active_worker = input.selected_worker;
    populate_invocation_runtime_state(runtime, invocation);
    return invocation;
}

BattleMovementPassiveDispatchInput make_passive_dispatch_input(
    const BattleFrameRuntime& runtime,
    const BattleFrameActionRuntime& action) {
    BattleMovementPassiveDispatchInput invocation;
    invocation.action_ordinal = action.action_ordinal;
    invocation.actor_slot = action.actor_slot;
    invocation.final_target_slot = action.target_slot;
    invocation.action_kind = action.action_kind;
    invocation.relation_scope = action.relation_scope;
    invocation.turn_type = action.turn_type;
    populate_invocation_runtime_state(runtime, invocation);
    return invocation;
}

std::optional<int> nearest_opposing_slot(
    const BattleFrameRuntime& runtime,
    int slot,
    int excluded_actor_slot,
    int excluded_target_slot) {
    const auto ordered = ordered_opposing_slots(
        runtime, slot, excluded_actor_slot, excluded_target_slot);
    return ordered.empty() ? std::nullopt : std::optional<int>{ordered.front()};
}

std::vector<int> ordered_opposing_slots(
    const BattleFrameRuntime& runtime,
    int slot,
    int excluded_actor_slot,
    int excluded_target_slot) {
    const auto* source = find_frame_combatant(runtime.state, slot);
    if (source == nullptr) {
        return {};
    }
    std::vector<int> result;
    const auto append_pass = [&](bool apply_exclusions) {
        std::vector<std::pair<int, int>> candidates;
        for (const auto& candidate : runtime.state.combatants) {
            if (!candidate.present || !candidate.alive || candidate.slot == slot
                || candidate.is_player == source->is_player
                || (apply_exclusions
                    && (candidate.slot == excluded_actor_slot
                        || candidate.slot == excluded_target_slot))) {
                continue;
            }
            const int dx = std::abs(
                candidate.grid_position.grid_x - source->grid_position.grid_x);
            const int dz = std::abs(
                candidate.grid_position.grid_z - source->grid_position.grid_z);
            const int distance = std::max(dx, dz) + std::min(dx, dz) / 2;
            candidates.emplace_back(distance, candidate.slot);
        }
        std::sort(candidates.begin(), candidates.end());
        for (const auto& [distance, candidate_slot] : candidates) {
            (void)distance;
            if (std::find(result.begin(), result.end(), candidate_slot) == result.end()) {
                result.push_back(candidate_slot);
            }
        }
    };
    append_pass(true);
    append_pass(false);
    return result;
}

bool grid_is_open(const BattleFrameState& state, const MovementGridPosition& grid) {
    const int x = grid.grid_x;
    const int z = grid.grid_z;
    return x >= 0 && x < 11 && z >= 0 && z < 11
        && state.active_grid[static_cast<std::size_t>(z * 11 + x)] == 0;
}

std::optional<MovementGridPosition> provisional_formation_destination(
    const BattleFrameRuntime& runtime,
    int slot,
    std::string* diagnostic) {
    const auto* source = find_frame_combatant(runtime.state, slot);
    if (source == nullptr) {
        if (diagnostic != nullptr) {
            *diagnostic = "formation source combatant missing";
        }
        return std::nullopt;
    }
    const BattleFrameCombatantState* nearest = nullptr;
    int nearest_distance = 0;
    const auto formation_distance = [](const MovementGridPosition& left,
                                       const MovementGridPosition& right) {
        const int dx = std::abs(left.grid_x - right.grid_x);
        const int dz = std::abs(left.grid_z - right.grid_z);
        return std::max(dx, dz) + std::min(dx, dz) / 2;
    };
    for (const auto& candidate : runtime.state.combatants) {
        if (!candidate.present || !candidate.alive || candidate.slot == slot
            || candidate.is_player == source->is_player) {
            continue;
        }
        const int distance = formation_distance(
            candidate.grid_position, source->grid_position);
        if (nearest == nullptr || distance < nearest_distance) {
            nearest = &candidate;
            nearest_distance = distance;
        }
    }

    if (nearest == nullptr) {
        if (diagnostic != nullptr) {
            *diagnostic = "formation opposing group has no live candidate";
        }
        return std::nullopt;
    }

    const int x = source->grid_position.grid_x;
    const int z = source->grid_position.grid_z;
    const int dx = x - nearest->grid_position.grid_x;
    const int dz = z - nearest->grid_position.grid_z;
    std::vector<MovementGridPosition> candidates;
    const auto push = [&](int candidate_x, int candidate_z) {
        candidates.push_back({.grid_x = candidate_x, .grid_z = candidate_z});
    };
    const auto push_x_side_fallback = [&]() {
        if (x < 6) {
            push(x + 1, z);
            push(x - 1, z);
        } else {
            push(x - 1, z);
            push(x + 1, z);
        }
    };
    const auto push_z_side_fallback = [&](int step_x) {
        if (z < 6) {
            push(x + step_x, z + 1);
            push(x + step_x, z - 1);
        } else {
            push(x + step_x, z - 1);
            push(x + step_x, z + 1);
        }
    };

    if (dx == 0) {
        push(x, z + (dz > 0 ? 1 : -1));
        push_x_side_fallback();
    } else {
        const int step_x = dx > 0 ? 1 : -1;
        if (dz == 0) {
            push(x + step_x, z);
            push_z_side_fallback(step_x);
        } else {
            const int step_z = dz > 0 ? 1 : -1;
            push(x + step_x, z + step_z);
            if (std::abs(dz) < std::abs(dx)) {
                push(x + step_x, z);
                push(x, z + step_z);
            } else {
                push(x, z + step_z);
                push(x + step_x, z);
            }
        }
    }

    const auto found = std::find_if(candidates.begin(), candidates.end(),
        [&](const MovementGridPosition& candidate) {
            return grid_is_open(runtime.state, candidate);
        });
    if (found == candidates.end()) {
        if (diagnostic != nullptr) {
            std::ostringstream out;
            out << "formation_source=" << movement_grid_detail(source->grid_position)
                << "; nearest_slot=" << nearest->slot
                << "; candidates=";
            for (const auto& candidate : candidates) {
                const int index = candidate.grid_z * 11 + candidate.grid_x;
                out << movement_grid_detail(candidate) << ":"
                    << (index >= 0 && index < 121
                        ? static_cast<int>(runtime.state.active_grid[
                            static_cast<std::size_t>(index)])
                        : -1)
                    << " ";
            }
            *diagnostic = out.str();
        }
        return std::nullopt;
    }

    int moved_nearest_distance = 0;
    bool moved_nearest_known = false;
    for (const auto& candidate : runtime.state.combatants) {
        if (!candidate.present || !candidate.alive || candidate.slot == slot
            || candidate.is_player == source->is_player) {
            continue;
        }
        const int distance = formation_distance(candidate.grid_position, *found);
        if (!moved_nearest_known || distance < moved_nearest_distance) {
            moved_nearest_distance = distance;
            moved_nearest_known = true;
        }
    }
    const bool accepted = moved_nearest_known
        && moved_nearest_distance > nearest_distance;
    if (diagnostic != nullptr) {
        std::ostringstream out;
        out << "formation_source=" << movement_grid_detail(source->grid_position)
            << "; nearest_slot=" << nearest->slot
            << "; selected=" << movement_grid_detail(*found)
            << "; distance=" << nearest_distance << "->" << moved_nearest_distance
            << "; accepted=" << (accepted ? 1 : 0);
        *diagnostic = out.str();
    }
    return accepted ? std::optional<MovementGridPosition>{*found} : std::nullopt;
}

void append_noninvoked_decision_history(
    BattleFrameRuntime& runtime,
    const BattleMovementInvocationDecision& decision) {
    BattleFramePendingMovementInvocation pending{
        .decision = decision,
        .worker_index = -1,
        .queue_sequence = -1,
        .lifecycle = BattleFrameMovementInvocationLifecycle::Skipped,
    };
    const auto old_state = decision.slot >= 0
            && decision.slot < static_cast<int>(runtime.movement_controller_states.size())
        ? runtime.movement_controller_states[static_cast<std::size_t>(decision.slot)]
        : BattleMovementControllerState::Unknown;
    append_invocation_history(
        runtime,
        pending,
        BattleFrameMovementInvocationLifecycle::Skipped,
        old_state,
        old_state,
        decision.provenance);
}

void enqueue_pending_movement_invocation(
    BattleFrameRuntime& runtime,
    const BattleMovementInvocationDecision& decision) {
    if (decision.slot < 0
        || decision.slot >= static_cast<int>(runtime.slot_worker_queues.size())) {
        runtime.warnings.push_back(
            "movement invocation slot is outside the frame worker queue range");
        append_noninvoked_decision_history(runtime, decision);
        return;
    }

    BattleFrameWorker worker = make_worker(
        runtime.state,
        decision.slot,
        decision.semantic_target_slot.value_or(-1),
        frame_worker_kind(decision.worker_kind),
        decision.selected_worker);
    worker.action_ordinal = decision.action_ordinal;
    worker.path_selection_policy = decision.path_selection_policy;
    worker.movement_leg_policy = frame_leg_policy(decision.leg_policy);
    worker.activation_pending = true;
    worker.controller_family = decision.controller_family;
    worker.relation_route = decision.relation_route;
    worker.activation_timing = decision.activation_timing;
    worker.thread_order_index = decision.thread_order_index;
    worker.activation_controller_state = decision.activation_controller_state;
    worker.worker_controller_state = decision.worker_controller_state;
    worker.invocation_confidence = decision.confidence;
    worker.invocation_provenance = decision.provenance;
    worker.callback_pc = decision.callback_pc;
    worker.program_steps.clear();
    worker.program_index = 0;
    build_static_worker_program(worker);
    if (decision.controller_family == BattleMovementControllerFamily::AmbientPursuit) {
        const auto target = nearest_opposing_slot(
            runtime,
            decision.slot,
            runtime.active_action.has_value() ? runtime.active_action->actor_slot : -1,
            runtime.active_action.has_value() ? runtime.active_action->target_slot : -1);
        if (target.has_value()) {
            worker.target_slot = *target;
            worker.invocation_provenance +=
                "; dynamic nearest opposing target selected at family activation";
        } else {
            worker.event_status = BattleFrameEventStatus::MissingInput;
            worker.detail += "no live opposing pursuit target; ";
        }
    } else if (decision.controller_family
        == BattleMovementControllerFamily::AmbientFormation) {
        std::string formation_detail;
        const auto destination = provisional_formation_destination(
            runtime,
            decision.slot,
            &formation_detail);
        if (destination.has_value()) {
            set_worker_movement_path_from_entries(
                runtime.state,
                worker,
                1,
                0,
                2,
                {*destination});
            worker.destination_source =
                MovementCommitDestinationSource::GeneratedSingleSquare;
            worker.invocation_provenance +=
                "; provisional FUN_80080710 formation candidate generated without action target; "
                + formation_detail;
        } else {
            worker.event_status = BattleFrameEventStatus::MissingInput;
            worker.detail += "no open formation candidate; " + formation_detail + "; ";
        }
    }
    if (decision.status != BattleMovementInvocationStatus::Matched) {
        worker.event_status = frame_event_status(decision.status);
    }
    if (!worker.detail.empty()) {
        worker.detail += "; ";
    }
    worker.detail += "invocation_family="
        + std::string(battle_movement_controller_family_name(
            decision.controller_family));

    const int worker_index = static_cast<int>(runtime.workers.size());
    enqueue_worker(runtime, std::move(worker));
    auto& queued_worker = runtime.workers[static_cast<std::size_t>(worker_index)];
    runtime.pending_movement_invocations.push_back(
        BattleFramePendingMovementInvocation{
            .decision = decision,
            .worker_index = worker_index,
            .queue_sequence = queued_worker.queue_sequence,
            .lifecycle = BattleFrameMovementInvocationLifecycle::Pending,
        });
    append_invocation_history(
        runtime,
        runtime.pending_movement_invocations.back(),
        BattleFrameMovementInvocationLifecycle::Pending,
        decision.prior_controller_state,
        decision.prior_controller_state,
        decision.provenance);
}

std::uint16_t participant_bit(int slot) {
    return slot >= 0 && slot < 16
        ? static_cast<std::uint16_t>(1u << slot)
        : 0;
}

BattleFrameStepEvent make_action_lifecycle_event(
    const BattleFrameRuntime& runtime,
    const BattleFramePassiveParticipantRuntime* participant,
    BattleFrameWorkerStepKind step_kind,
    std::string callback,
    std::uint32_t callback_pc,
    std::uint16_t mask_before,
    std::uint16_t mask_after,
    std::string reason) {
    BattleFrameStepEvent event;
    if (runtime.active_action.has_value()) {
        event.action_ordinal = runtime.active_action->action_ordinal;
        event.action_phase = runtime.active_action->phase;
        event.target_slot = runtime.active_action->target_slot;
        event.completion_turn_phase =
            runtime.active_action->completion_turn_phase;
        event.completion_override = runtime.active_action->completion_override;
    }
    event.frame_index = runtime.state.frame_index;
    event.step_kind = step_kind;
    event.callback = std::move(callback);
    event.callback_pc = callback_pc;
    event.status = BattleFrameEventStatus::Provisional;
    event.worker_kind = participant == nullptr
        ? BattleFrameWorkerKind::None
        : BattleFrameWorkerKind::PassiveController;
    event.passive_completion_mask_before = mask_before;
    event.passive_completion_mask_after = mask_after;
    event.completion_reason = reason;
    if (participant != nullptr) {
        event.slot = participant->slot;
        event.target_slot = participant->semantic_target_slot.value_or(event.target_slot);
        event.controller_family = participant->controller_family;
        event.relation_route = participant->relation_route;
        event.deferred_callback_pc = participant->deferred_callback_pc;
        event.invocation_confidence = participant->confidence;
        event.invocation_provenance = participant->provenance;
        event.old_controller_state = runtime.movement_controller_states[
            static_cast<std::size_t>(participant->slot)];
        event.new_controller_state = event.old_controller_state;
    }
    std::ostringstream detail;
    detail << "action_phase=" << battle_frame_action_phase_name(event.action_phase)
           << "; relation_route="
           << battle_movement_relation_route_name(event.relation_route)
           << "; callback_family="
           << battle_movement_controller_family_name(event.controller_family)
           << "; callback_pc=" << hex_pc(callback_pc)
           << "; deferred_callback_pc=" << hex_pc(event.deferred_callback_pc)
           << "; completion_mask_before=0x" << std::hex << mask_before
           << "; completion_mask_after=0x" << mask_after << std::dec
           << "; completion_turn_phase="
           << static_cast<int>(event.completion_turn_phase)
           << "; completion_override=" << (event.completion_override ? 1 : 0)
           << "; completion_reason=" << reason;
    if (participant != nullptr) {
        detail << "; thread_state="
               << static_cast<int>(participant->thread_state_0x19)
               << "; participant_phase="
               << battle_frame_passive_participant_phase_name(participant->phase)
               << "; provenance=" << participant->provenance;
    }
    event.detail = detail.str();
    return event;
}

bool action_lifecycle_has_runnable_work(const BattleFrameRuntime& runtime) {
    if (!runtime.visual.pending_instruction_control_resets.empty()) {
        return true;
    }
    if (has_active_visual_task(runtime)) {
        return true;
    }
    if (!runtime.active_action.has_value()
        || runtime.active_action->phase == BattleFrameActionPhase::Complete) {
        return false;
    }
    if (runtime.active_action->phase == BattleFrameActionPhase::HandoffPending) {
        return true;
    }
    if (runtime.active_action->completion_gate_open
        && runtime.active_action->passive_completion_mask == 0) {
        return true;
    }
    if (runtime.active_action->completion_gate_open
        && !runtime.active_action->completion_override
        && runtime.active_action->passive_completion_mask != 0) {
        return true;
    }
    return std::any_of(
        runtime.passive_participants.begin(),
        runtime.passive_participants.end(),
        [&](const BattleFramePassiveParticipantRuntime& participant) {
            if (!participant.active) {
                return false;
            }
            if (participant.phase == BattleFramePassiveParticipantPhase::CompletionDeferred) {
                return fun_80080438_allows_completion(*runtime.active_action);
            }
            if (participant.phase == BattleFramePassiveParticipantPhase::FamilyActive
                && participant.controller_family
                    == BattleMovementControllerFamily::AffectedTargetReaction
                && !runtime.active_action->action_resolution_available) {
                return false;
            }
            return participant.phase != BattleFramePassiveParticipantPhase::WaitingForDispatch
                && participant.phase != BattleFramePassiveParticipantPhase::Cleared
                && participant.phase != BattleFramePassiveParticipantPhase::Removed;
        });
}

std::uint32_t active_completion_override_publisher_pc(
    const BattleFrameRuntime& runtime) {
    if (!runtime.active_action.has_value()) {
        return 0;
    }
    const int index = runtime.active_action->active_worker_index;
    if (index < 0 || index >= static_cast<int>(runtime.workers.size())) {
        return 0;
    }
    switch (runtime.workers[static_cast<std::size_t>(index)].controller_family) {
    case BattleMovementControllerFamily::ActivePcFallback: return 0x80085F34u;
    case BattleMovementControllerFamily::ActivePcDirect: return 0x80086870u;
    case BattleMovementControllerFamily::EnemyFallback: return 0x80087BA4u;
    case BattleMovementControllerFamily::EnemyDirect: return 0x80088658u;
    default: return 0;
    }
}

bool passive_workers_ready_for_completion_override(
    const BattleFrameRuntime& runtime) {
    if (!runtime.active_action.has_value()) {
        return false;
    }
    const auto action_ordinal = runtime.active_action->action_ordinal;
    for (const auto& participant : runtime.passive_participants) {
        if (!participant.active || !participant.completion_bit_set
            || participant.action_ordinal != action_ordinal) {
            continue;
        }
        if (participant.controller_family != BattleMovementControllerFamily::AmbientPursuit
            && participant.controller_family
                != BattleMovementControllerFamily::AmbientFormation) {
            continue;
        }
        if (participant.worker_index < 0
            || participant.worker_index >= static_cast<int>(runtime.workers.size())) {
            return false;
        }
        const auto& worker = runtime.workers[
            static_cast<std::size_t>(participant.worker_index)];
        if (participant.controller_family
                == BattleMovementControllerFamily::AmbientPursuit
            && !worker.complete) {
            return false;
        }
        if (participant.controller_family
                == BattleMovementControllerFamily::AmbientFormation
            && !worker.complete
            && worker.completed_movement_legs < 1) {
            return false;
        }
    }
    return true;
}

void maybe_publish_completion_override(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    if (!runtime.active_action.has_value()) {
        return;
    }
    auto& action = *runtime.active_action;
    if (!action.completion_gate_open || action.completion_override
        || !action.action_resolution_available
        || !passive_workers_ready_for_completion_override(runtime)) {
        return;
    }
    action.completion_override = true;
    const auto publisher_pc = active_completion_override_publisher_pc(runtime);
    append_recorded_event(
        runtime,
        result,
        make_action_lifecycle_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::ActionPhaseTransition,
            "movement_completion_override_publish",
            publisher_pc,
            action.passive_completion_mask,
            action.passive_completion_mask,
            "provisional active-callback publisher after pursuits enter handoff and formations finish their first leg"));
}

const BattleMovementInvocationDecision* find_dispatch_decision(
    const BattleMovementInvocationPlan& plan,
    int slot) {
    const auto found = std::find_if(
        plan.decisions.begin(),
        plan.decisions.end(),
        [slot](const BattleMovementInvocationDecision& decision) {
            return decision.slot == slot;
        });
    return found == plan.decisions.end() ? nullptr : &*found;
}

bool clear_passive_participant_completion(
    BattleFrameRuntime& runtime,
    BattleFramePassiveParticipantRuntime& participant,
    BattleFrameRunResult& result,
    std::string reason) {
    if (!runtime.active_action.has_value() || !participant.completion_bit_set) {
        return false;
    }
    auto& action = *runtime.active_action;
    const auto before = action.passive_completion_mask;
    action.passive_completion_mask = static_cast<std::uint16_t>(
        action.passive_completion_mask & ~participant_bit(participant.slot));
    participant.completion_bit_set = false;
    participant.phase = BattleFramePassiveParticipantPhase::Cleared;
    participant.active = false;
    runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
        BattleMovementControllerState::Idle;
    append_recorded_event(
        runtime,
        result,
        make_action_lifecycle_event(
            runtime,
            &participant,
            BattleFrameWorkerStepKind::PassiveCompletionClear,
            "FUN_80080438_completion_clear",
            0x80080438u,
            before,
            action.passive_completion_mask,
            std::move(reason)));
    if (action.completion_gate_open && action.completion_override
        && action.completion_turn_phase == 4) {
        action.completion_turn_phase = 5;
    }
    return true;
}

bool advance_passive_participant(
    BattleFrameRuntime& runtime,
    BattleFramePassiveParticipantRuntime& participant,
    BattleFrameRunResult& result) {
    if (!participant.active || !runtime.active_action.has_value()) {
        return false;
    }
    auto& action = *runtime.active_action;
    const auto mask = action.passive_completion_mask;
    const auto emit = [&](BattleFrameWorkerStepKind kind,
                          const char* callback,
                          std::uint32_t pc,
                          const char* reason) {
        append_recorded_event(
            runtime,
            result,
            make_action_lifecycle_event(
                runtime, &participant, kind, callback, pc, mask,
                action.passive_completion_mask, reason));
    };

    const auto* combatant = find_frame_combatant(runtime.state, participant.slot);
    const bool has_thread = has_active_thread_for_slot(
        runtime,
        BattleFrameThreadNodeKind::MovementController,
        participant.slot);
    const bool removed = combatant == nullptr || !combatant->present || !combatant->alive
        || !has_thread
        || (participant.slot == action.target_slot && action.target_dead);
    if (removed
        && participant.slot >= 0
        && participant.slot < static_cast<int>(runtime.combatant_instructions.size())) {
        auto& instruction = runtime.combatant_instructions[
            static_cast<std::size_t>(participant.slot)];
        if (instruction.active) {
            instruction.phase = BattleFrameCombatantInstructionPhase::Removed;
            instruction.active = false;
        }
    }
    if (removed && participant.completion_bit_set) {
        const auto before = action.passive_completion_mask;
        action.passive_completion_mask = static_cast<std::uint16_t>(
            action.passive_completion_mask & ~participant_bit(participant.slot));
        participant.completion_bit_set = false;
        participant.phase = BattleFramePassiveParticipantPhase::Removed;
        participant.active = false;
        if (participant.worker_index >= 0
            && participant.worker_index < static_cast<int>(runtime.workers.size())) {
            runtime.workers[static_cast<std::size_t>(participant.worker_index)].complete = true;
        }
        runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
            BattleMovementControllerState::Removed;
        append_recorded_event(
            runtime,
            result,
            make_action_lifecycle_event(
                runtime,
                &participant,
                BattleFrameWorkerStepKind::PassiveDeathClear,
                "FUN_8008D36C_death_clear",
                0x8008D36Cu,
                before,
                action.passive_completion_mask,
                "combatant removal cleared the action-local completion bit"));
        return true;
    }

    if (participant.phase == BattleFramePassiveParticipantPhase::InitialRelayPending
        || participant.phase == BattleFramePassiveParticipantPhase::DispatchRelayPending) {
        const bool dispatch_relay = participant.phase
            == BattleFramePassiveParticipantPhase::DispatchRelayPending;
        if (participant.actual_callback_pc == 0x800804B8u) {
            participant.actual_callback_pc = 0x800801A8u;
            participant.thread_state_0x19 = 0;
            runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
                BattleMovementControllerState::Relay801A8;
            emit(
                dispatch_relay ? BattleFrameWorkerStepKind::PassiveRelayAdvance
                               : BattleFrameWorkerStepKind::PassiveRelayPublish,
                "FUN_800804B8_to_800801A8",
                0x800804B8u,
                dispatch_relay ? "dispatch relay entered state 0"
                               : "initial nonactor relay entered state 0");
        } else if (participant.thread_state_0x19 == 0) {
            participant.thread_state_0x19 = 1;
            emit(
                BattleFrameWorkerStepKind::PassiveRelayAdvance,
                "FUN_800801A8_state_0_to_1",
                0x800801A8u,
                "relay advanced one packed-thread visit");
        } else {
            participant.actual_callback_pc = dispatch_relay
                ? participant.deferred_callback_pc
                : 0x800804B8u;
            participant.thread_state_0x19 = 0;
            participant.phase = dispatch_relay
                ? BattleFramePassiveParticipantPhase::Dispatching
                : BattleFramePassiveParticipantPhase::WaitingForDispatch;
            runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
                dispatch_relay
                    ? BattleMovementControllerState::Dispatch8DEEC
                    : BattleMovementControllerState::Relay804B8;
            emit(
                BattleFrameWorkerStepKind::PassiveRelayAdvance,
                "FUN_800801A8_publish_deferred",
                0x800801A8u,
                dispatch_relay ? "relay published deferred FUN_8008DEEC"
                               : "initial relay returned to staged callback");
        }
        return true;
    }

    if (participant.phase == BattleFramePassiveParticipantPhase::Dispatching) {
        const auto plan = model_passive_movement_dispatch(
            make_passive_dispatch_input(runtime, action));
        const auto* decision = find_dispatch_decision(plan, participant.slot);
        if (decision == nullptr) {
            participant.controller_family = BattleMovementControllerFamily::Unsupported;
            participant.status = BattleMovementInvocationStatus::Unsupported;
            participant.phase = BattleFramePassiveParticipantPhase::CompletionDeferred;
            runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
                BattleMovementControllerState::CompletionDeferred;
            emit(
                BattleFrameWorkerStepKind::PassiveFamilySelect,
                "FUN_8008DEEC_unsupported",
                0x8008DEECu,
                "missing family decision entered provisional completion wait");
            return true;
        }
        participant.actual_callback_pc = decision->callback_pc;
        runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
            decision->activation_controller_state;
        if (decision->controller_family
            == BattleMovementControllerFamily::AffectedTargetReaction) {
            participant.phase = BattleFramePassiveParticipantPhase::FamilyActive;
            emit(
                BattleFrameWorkerStepKind::PassiveFamilySelect,
                battle_movement_controller_family_name(decision->controller_family),
                0x8008DEECu,
                "FUN_8008D3B0 selected; affected target waits for action result");
            return true;
        }
        participant.worker_index = static_cast<int>(runtime.workers.size());
        enqueue_pending_movement_invocation(runtime, *decision);
        participant.phase = BattleFramePassiveParticipantPhase::FamilyPending;
        emit(
            BattleFrameWorkerStepKind::PassiveFamilySelect,
            battle_movement_controller_family_name(decision->controller_family),
            0x8008DEECu,
            "FUN_8008DEEC selected the passive callback family; family starts next visit");
        return true;
    }

    if (participant.phase == BattleFramePassiveParticipantPhase::FamilyPending) {
        participant.phase = BattleFramePassiveParticipantPhase::FamilyActive;
        return false;
    }

    if (participant.phase == BattleFramePassiveParticipantPhase::FamilyActive
        && participant.controller_family
            == BattleMovementControllerFamily::AffectedTargetReaction
        && action.action_resolution_available) {
        participant.actual_callback_pc = 0x8008CDA8u;
        participant.phase = BattleFramePassiveParticipantPhase::CompletionDeferred;
        runtime.movement_controller_states[static_cast<std::size_t>(participant.slot)] =
            BattleMovementControllerState::CompletionDeferred;
        emit(
            BattleFrameWorkerStepKind::PassiveCompletionDeferred,
            "FUN_8008D3B0_to_8008CDA8",
            0x8008D3B0u,
            "surviving affected target observed action result and entered deferred completion");
        return true;
    }

    if (participant.phase == BattleFramePassiveParticipantPhase::CompletionDeferred
        && fun_80080438_allows_completion(action)) {
        return clear_passive_participant_completion(
            runtime,
            participant,
            result,
            "FUN_80080438 accepted the current turn phase or completion override");
    }
    return false;
}

void clear_unvisitable_passive_participants(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    if (!runtime.active_action.has_value()) {
        return;
    }
    for (auto& participant : runtime.passive_participants) {
        if (!participant.active || !participant.completion_bit_set) {
            continue;
        }
        const auto* combatant = find_frame_combatant(runtime.state, participant.slot);
        const bool has_thread = has_active_thread_for_slot(
            runtime,
            BattleFrameThreadNodeKind::MovementController,
            participant.slot);
        if (combatant != nullptr && combatant->present && combatant->alive
            && has_thread
            && !(participant.slot == runtime.active_action->target_slot
                && runtime.active_action->target_dead)) {
            continue;
        }
        (void)advance_passive_participant(runtime, participant, result);
    }
}

void mark_passive_worker_complete(
    BattleFrameRuntime& runtime,
    BattleFrameWorker& worker,
    BattleFrameRunResult& result) {
    if (worker.kind != BattleFrameWorkerKind::PassiveController
        || worker.slot < 0
        || worker.slot >= static_cast<int>(runtime.passive_participants.size())
        || !runtime.active_action.has_value()) {
        return;
    }
    auto& participant = runtime.passive_participants[
        static_cast<std::size_t>(worker.slot)];
    if (!participant.active
        || participant.action_ordinal != runtime.active_action->action_ordinal) {
        return;
    }
    if (fun_80080438_allows_completion(*runtime.active_action)) {
        (void)clear_passive_participant_completion(
            runtime,
            participant,
            result,
            "completed passive callback was accepted by FUN_80080438");
        return;
    }
    participant.phase = BattleFramePassiveParticipantPhase::CompletionDeferred;
    runtime.movement_controller_states[static_cast<std::size_t>(worker.slot)] =
        BattleMovementControllerState::CompletionDeferred;
    append_recorded_event(
        runtime,
        result,
        make_action_lifecycle_event(
            runtime,
            &participant,
            BattleFrameWorkerStepKind::PassiveCompletionDeferred,
            "FUN_80080438_completion_deferred",
            0x80080438u,
            runtime.active_action->passive_completion_mask,
            runtime.active_action->passive_completion_mask,
            "FUN_80080438 rejected turn phase 4 without the completion override; retained action-local bit"));
}

void complete_action_if_drained(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    if (!runtime.active_action.has_value()) {
        return;
    }
    auto& action = *runtime.active_action;
    const bool active_worker_complete = action.active_worker_index < 0
        || action.active_worker_index >= static_cast<int>(runtime.workers.size())
        || runtime.workers[static_cast<std::size_t>(action.active_worker_index)].complete;
    if (!action.completion_gate_open || action.passive_completion_mask != 0
        || action.phase == BattleFrameActionPhase::Complete
        || !active_worker_complete
        || has_action_visual_barrier(runtime, action.action_ordinal)) {
        return;
    }
    const auto before = action.passive_completion_mask;
    action.completion_turn_phase = 5;
    action.phase = BattleFrameActionPhase::Complete;
    action.active = false;
    append_recorded_event(
        runtime,
        result,
        make_action_lifecycle_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::ActionComplete,
            "BattleActionComplete",
            0,
            before,
            action.passive_completion_mask,
            "active callback and all action-local passive participants completed or were removed"));
}

const BattleFrameThreadNode* active_instruction_thread_for_slot(
    const BattleFrameThreadListRuntime& runtime,
    int slot) {
    const auto found = std::find_if(
        runtime.nodes.begin(),
        runtime.nodes.end(),
        [slot](const BattleFrameThreadNode& node) {
            return node.active
                && node.kind == BattleFrameThreadNodeKind::CombatantInstruction
                && node.owner_slot == slot;
        });
    return found == runtime.nodes.end() ? nullptr : &*found;
}

BattleFrameEventStatus instruction_control_reset_event_status(
    BattleFrameInstructionControlResetLifecycle lifecycle) {
    switch (lifecycle) {
    case BattleFrameInstructionControlResetLifecycle::Applied:
    case BattleFrameInstructionControlResetLifecycle::Consumed:
        return BattleFrameEventStatus::Matched;
    case BattleFrameInstructionControlResetLifecycle::TargetRemoved:
    case BattleFrameInstructionControlResetLifecycle::TargetReplaced:
    case BattleFrameInstructionControlResetLifecycle::Superseded:
        return BattleFrameEventStatus::Skipped;
    case BattleFrameInstructionControlResetLifecycle::MissingInput:
        return BattleFrameEventStatus::MissingInput;
    }
    return BattleFrameEventStatus::Unsupported;
}

void record_instruction_control_reset(
    BattleFrameRuntime& runtime,
    const BattleFramePendingInstructionControlReset& reset,
    BattleFrameInstructionControlResetLifecycle lifecycle,
    int callback_state_before,
    int callback_state_after,
    std::string provenance,
    BattleFrameRunResult* result = nullptr) {
    runtime.visual.instruction_control_reset_history.push_back(
        BattleFrameInstructionControlResetHistoryEvent{
            .sequence = reset.sequence,
            .frame_index = runtime.state.frame_index,
            .action_ordinal = reset.action_ordinal,
            .target_slot = reset.target_slot,
            .producer_node_id = reset.producer_node_id,
            .producer_visual_task_sequence =
                reset.producer_visual_task_sequence,
            .target_node_id = reset.target_node_id,
            .producer_node_index = reset.producer_node_index,
            .target_node_index = reset.target_node_index,
            .traversal_generation = runtime.thread_list.traversal_generation,
            .callback_publication_revision =
                reset.callback_publication_revision,
            .callback_state_before = callback_state_before,
            .callback_state_after = callback_state_after,
            .source = reset.source,
            .lifecycle = lifecycle,
            .timing = reset.timing,
            .provenance = provenance,
        });

    const auto step_kind = lifecycle
            == BattleFrameInstructionControlResetLifecycle::Consumed
        || lifecycle == BattleFrameInstructionControlResetLifecycle::TargetRemoved
        || lifecycle == BattleFrameInstructionControlResetLifecycle::TargetReplaced
        || lifecycle == BattleFrameInstructionControlResetLifecycle::Superseded
        ? BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume
        : BattleFrameWorkerStepKind::InstructionCallbackControlReset;
    std::ostringstream detail;
    detail << "reset_source="
           << battle_frame_instruction_control_reset_source_name(reset.source)
           << "; reset_lifecycle="
           << battle_frame_instruction_control_reset_lifecycle_name(lifecycle)
           << "; reset_timing="
           << battle_frame_instruction_control_reset_timing_name(reset.timing)
           << "; reset_sequence=" << reset.sequence
           << "; producer_node_id=" << reset.producer_node_id
           << "; producer_visual_task_sequence="
           << reset.producer_visual_task_sequence
           << "; target_node_id=" << reset.target_node_id
           << "; producer_node_index=" << reset.producer_node_index
           << "; target_node_index=" << reset.target_node_index
           << "; staged_frame=" << reset.staged_frame_index
           << "; staged_traversal_generation="
           << reset.staged_traversal_generation
           << "; eligible_traversal_generation="
           << reset.eligible_traversal_generation
           << "; callback_publication_revision="
           << reset.callback_publication_revision
           << "; callback_state=" << callback_state_before
           << "->" << callback_state_after
           << "; draws=0; provenance=" << provenance;
    auto event = make_visual_event(
        runtime,
        nullptr,
        step_kind,
        reset.source == BattleFrameInstructionControlResetSource::QueuedTransition
            ? "ResolveQueuedStdActionTransition_800227AC"
            : "FUN_80020250_80020310",
        instruction_control_reset_event_status(lifecycle),
        detail.str());
    event.action_ordinal = reset.action_ordinal;
    event.slot = reset.target_slot;
    event.instruction_control_reset_sequence = reset.sequence;
    event.instruction_control_reset_source = reset.source;
    event.instruction_control_reset_lifecycle = lifecycle;
    event.instruction_control_reset_timing = reset.timing;
    event.instruction_control_reset_producer_node_id = reset.producer_node_id;
    event.instruction_control_reset_target_node_id = reset.target_node_id;
    event.instruction_control_reset_traversal_generation =
        runtime.thread_list.traversal_generation;
    event.action_motion_callback_state_before = callback_state_before;
    event.action_motion_callback_state_after = callback_state_after;
    event.persistent_callback_publication_revision =
        reset.callback_publication_revision;
    if (result != nullptr) {
        append_recorded_event(runtime, *result, std::move(event));
    } else {
        runtime.visual.pending_events.push_back(std::move(event));
    }
}

bool apply_queued_instruction_control_reset(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    int slot,
    std::string provenance) {
    if (slot < 0
        || slot >= static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        return false;
    }
    auto& callback = runtime.visual.persistent_instruction_callbacks[
        static_cast<std::size_t>(slot)];
    if (!callback.installed) {
        return false;
    }
    const auto* target = active_instruction_thread_for_slot(
        runtime.thread_list, slot);
    BattleFramePendingInstructionControlReset reset;
    reset.sequence = runtime.visual.next_instruction_control_reset_sequence++;
    reset.action_ordinal = action_ordinal;
    reset.target_slot = slot;
    reset.producer_node_id = runtime.thread_list.current_node_id.value_or(-1);
    reset.target_node_id = target != nullptr ? target->node_id : -1;
    reset.target_node_creation_sequence = target != nullptr
        ? target->creation_sequence
        : 0;
    reset.producer_node_index = runtime.thread_list.current_node_index.has_value()
        ? static_cast<int>(*runtime.thread_list.current_node_index)
        : -1;
    reset.target_node_index = reset.producer_node_index;
    reset.staged_frame_index = runtime.state.frame_index;
    reset.staged_traversal_generation = runtime.thread_list.traversal_generation;
    reset.eligible_traversal_generation = runtime.thread_list.traversal_generation;
    reset.callback_publication_revision = callback.publication_revision;
    reset.source = BattleFrameInstructionControlResetSource::QueuedTransition;
    reset.timing =
        BattleFrameInstructionControlResetTiming::SameInstructionVisit;
    reset.provenance = std::move(provenance);

    const int before = callback.callback_state;
    callback.callback_state = reset.reset_value;
    callback.state8_delay_remaining = -1;
    record_instruction_control_reset(
        runtime,
        reset,
        target != nullptr
            ? BattleFrameInstructionControlResetLifecycle::Applied
            : BattleFrameInstructionControlResetLifecycle::MissingInput,
        before,
        callback.callback_state,
        reset.provenance);
    return target != nullptr;
}

bool apply_direct_instruction_control_reset(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    int slot,
    int producer_visual_task_sequence,
    std::string provenance) {
    if (slot < 0
        || slot >= static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        return false;
    }
    auto& callback = runtime.visual.persistent_instruction_callbacks[
        static_cast<std::size_t>(slot)];
    if (!callback.installed) {
        return false;
    }

    const auto* target = active_instruction_thread_for_slot(
        runtime.thread_list, slot);
    BattleFramePendingInstructionControlReset reset;
    reset.sequence = runtime.visual.next_instruction_control_reset_sequence++;
    reset.action_ordinal = action_ordinal;
    reset.target_slot = slot;
    reset.producer_node_id = runtime.thread_list.current_node_id.value_or(-1);
    reset.producer_visual_task_sequence = producer_visual_task_sequence;
    reset.target_node_id = target != nullptr ? target->node_id : -1;
    reset.target_node_creation_sequence = target != nullptr
        ? target->creation_sequence
        : 0;
    reset.producer_node_index = runtime.thread_list.current_node_index.has_value()
        ? static_cast<int>(*runtime.thread_list.current_node_index)
        : -1;
    reset.staged_frame_index = runtime.state.frame_index;
    reset.callback_publication_revision = callback.publication_revision;
    reset.source = BattleFrameInstructionControlResetSource::DirectTransition;
    reset.provenance = std::move(provenance);

    BattleFrameThreadDeliveryDecision delivery;
    if (target != nullptr) {
        delivery = plan_battle_frame_thread_delivery(
            runtime.thread_list, target->node_id);
        reset.target_node_index = delivery.target_node_index;
        reset.staged_traversal_generation =
            delivery.staged_traversal_generation;
        reset.eligible_traversal_generation =
            delivery.eligible_traversal_generation;
        if (delivery.status == BattleFrameThreadDeliveryStatus::SameTraversal) {
            reset.timing =
                BattleFrameInstructionControlResetTiming::SameFrameLaterVisit;
        } else if (runtime.thread_list.traversal_active
            && runtime.thread_list.current_node_id.has_value()) {
            reset.timing =
                BattleFrameInstructionControlResetTiming::NextFrameVisit;
        }
        reset.provenance += "; " + delivery.provenance;
    }

    const int before = callback.callback_state;
    callback.callback_state = reset.reset_value;
    callback.state8_delay_remaining = -1;
    const bool schedulable = target != nullptr
        && (delivery.status == BattleFrameThreadDeliveryStatus::SameTraversal
            || delivery.status == BattleFrameThreadDeliveryStatus::NextTraversal);
    record_instruction_control_reset(
        runtime,
        reset,
        schedulable
            ? BattleFrameInstructionControlResetLifecycle::Applied
            : BattleFrameInstructionControlResetLifecycle::MissingInput,
        before,
        callback.callback_state,
        reset.provenance);
    if (!schedulable) {
        return false;
    }
    runtime.visual.pending_instruction_control_resets.push_back(
        std::move(reset));
    return true;
}

void resolve_unvisitable_instruction_control_resets(
    BattleFrameRuntime& runtime,
    BattleFrameRunResult& result) {
    auto& pending = runtime.visual.pending_instruction_control_resets;
    for (auto it = pending.begin(); it != pending.end();) {
        const auto* target = find_battle_frame_thread(
            runtime.thread_list, it->target_node_id);
        if (target != nullptr
            && target->active
            && target->creation_sequence == it->target_node_creation_sequence) {
            ++it;
            continue;
        }
        const auto* replacement = active_instruction_thread_for_slot(
            runtime.thread_list, it->target_slot);
        const auto lifecycle = replacement != nullptr
            ? BattleFrameInstructionControlResetLifecycle::TargetReplaced
            : BattleFrameInstructionControlResetLifecycle::TargetRemoved;
        const auto& callback = runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(it->target_slot)];
        record_instruction_control_reset(
            runtime,
            *it,
            lifecycle,
            callback.callback_state,
            callback.callback_state,
            replacement != nullptr
                ? "the captured target instruction node was replaced before its visit"
                : "the captured target instruction node was removed before its visit",
            &result);
        it = pending.erase(it);
    }
}

bool consume_instruction_control_resets_for_thread(
    BattleFrameRuntime& runtime,
    const BattleFrameThreadNode& thread,
    BattleFrameRunResult& result) {
    bool consumed_any = false;
    auto& pending = runtime.visual.pending_instruction_control_resets;
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->target_node_id != thread.node_id
            || runtime.thread_list.traversal_generation
                < it->eligible_traversal_generation) {
            ++it;
            continue;
        }
        auto& callback = runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(thread.owner_slot)];
        const auto lifecycle = callback.publication_revision
                > it->callback_publication_revision
            ? BattleFrameInstructionControlResetLifecycle::Superseded
            : BattleFrameInstructionControlResetLifecycle::Consumed;
        record_instruction_control_reset(
            runtime,
            *it,
            lifecycle,
            callback.callback_state,
            callback.callback_state,
            lifecycle == BattleFrameInstructionControlResetLifecycle::Consumed
                ? "the exact target instruction node consumed the direct reset before callback invocation"
                : "a later direct transition publication superseded this reset before the target visit",
            &result);
        consumed_any = true;
        it = pending.erase(it);
    }
    return consumed_any;
}

} // namespace

std::optional<BattleFrameRuntime> initialize_first_battle_frame_runtime(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots,
    const std::optional<std::array<std::uint8_t, 81>>& terrain_source_9x9,
    soa::battle::TurnType initial_turn_type) {
    auto frame_state = initialize_first_battle_frame_state(
        enemy_event_id,
        slots,
        terrain_source_9x9,
        initial_turn_type);
    if (!frame_state.has_value()) {
        return std::nullopt;
    }

    BattleFrameRuntime runtime;
    runtime.initialized = true;
    runtime.state = std::move(*frame_state);
    runtime.collision_occupancy = make_battle_collision_occupancy_runtime();
    runtime.view_placement_cache = make_default_view_placement_cache_runtime();
    runtime.visual.timeline_action_ordinals.fill(-1);
    runtime.movement_controller_states.fill(
        BattleMovementControllerState::Unknown);
    for (const auto& combatant : runtime.state.combatants) {
        if (combatant.slot >= 0
            && combatant.slot < static_cast<int>(runtime.movement_controller_states.size())
            && combatant.present) {
            const auto slot_index = static_cast<std::size_t>(combatant.slot);
            runtime.movement_controller_states[slot_index] =
                BattleMovementControllerState::Idle;
            auto& worksheet = runtime.movement_worksheets[slot_index];
            worksheet.initialized = true;
            worksheet.raw_path_entries.fill(MovementGridPosition{
                .grid_x = 0,
                .grid_z = 0,
            });
            worksheet.provenance =
                "movement worksheet initializer clears the new runtime-local workspace; later path publications mutate it";
            auto& instruction = runtime.combatant_instructions[slot_index];
            instruction.slot = combatant.slot;
            instruction.provenance =
                "persistent Thread_BattleCombatant runtime initialized without an active instruction";
            const auto created = create_battle_frame_thread(
                runtime.thread_list,
                BattleFrameThreadCreateRequest{
                    .kind = BattleFrameThreadNodeKind::MovementController,
                    .owner_slot = combatant.slot,
                    .callback = BattleFrameThreadCallbackIdentity::MovementController,
                    .active = true,
                    .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
                    .semantic_source_id = "battle.setup.movement_controller",
                    .provenance =
                        "ascending live-slot setup loop created one movement controller through the live thread cursor",
                    .frame_index = 0,
                });
            if (created.status != BattleFrameThreadMutationStatus::Applied) {
                runtime.warnings.push_back(
                    "movement-controller thread creation failed for slot "
                    + std::to_string(combatant.slot) + ": " + created.detail);
            }
            (void)refresh_collision_occupancy_for_slot(
                runtime, combatant.slot, nullptr);
        }
    }
    const auto resource_worker = create_battle_frame_thread(
        runtime.thread_list,
        BattleFrameThreadCreateRequest{
            .kind = BattleFrameThreadNodeKind::ResourceWorker,
            .owner_slot = -1,
            .callback = BattleFrameThreadCallbackIdentity::ResourceQueue,
            .active = true,
            .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
            .semantic_source_id = "battle.resource_queue.std_loader",
            .provenance =
                "battle resource producer created the STD loader after movement-controller setup",
            .frame_index = 0,
        });
    if (resource_worker.status == BattleFrameThreadMutationStatus::Applied) {
        runtime.std_resource_worker_node_id = resource_worker.node_id;
    } else {
        runtime.warnings.push_back(
            "STD resource-worker thread creation failed: " + resource_worker.detail);
    }
    runtime.warnings.insert(
        runtime.warnings.begin(),
        runtime.state.warnings.begin(),
        runtime.state.warnings.end());
    return runtime;
}

MovementWorksheetSnapshot project_battle_frame_movement_worksheet_snapshot(
    const BattleFrameRuntime& runtime,
    int actor_slot,
    int target_slot) {
    MovementWorksheetSnapshot snapshot;
    if (!runtime.initialized
        || actor_slot < 0
        || actor_slot >= static_cast<int>(runtime.movement_worksheets.size())) {
        return snapshot;
    }
    const auto* actor = find_frame_combatant(runtime.state, actor_slot);
    const auto* target = find_frame_combatant(runtime.state, target_slot);
    const auto& worksheet = runtime.movement_worksheets[
        static_cast<std::size_t>(actor_slot)];
    if (actor == nullptr || target == nullptr || !worksheet.initialized) {
        return snapshot;
    }

    const auto modeled = model_runtime_movement_path(
        runtime,
        actor_slot,
        target_slot,
        BattleMovementPathSelectionPolicy::StraightRunPathIndex);
    if (modeled.status == BattleMovementPathModelStatus::MissingInput
        || modeled.status == BattleMovementPathModelStatus::Unsupported) {
        return snapshot;
    }

    const auto to_raw = [](const BattleFrameVec3& position) {
        return MovementRawStagePosition{
            .raw_x = static_cast<int>(position.x),
            .raw_y = static_cast<int>(position.y),
            .raw_z = static_cast<int>(position.z),
        };
    };
    snapshot.available = true;
    snapshot.actor_grid_position = actor->grid_position;
    snapshot.target_grid_position = target->grid_position;
    snapshot.actor_raw_stage_position = to_raw(actor->pos_holder);
    snapshot.target_raw_stage_position = to_raw(target->pos_holder);
    snapshot.target_adjacent = modeled.reachability
        == MovementReachabilityStatus::Adjacent1;
    snapshot.reachability_result = modeled.reachability_status_0x16;
    snapshot.path_shape_forces_fallback = model_pc_path_shape_80082340(
        actor->grid_position,
        worksheet.raw_path_entries);
    snapshot.dist_to_target = modeled.distance;
    snapshot.helper_8008a174_result = modeled.reachability_status_0x16 == 0 ? 0 : 1;
    snapshot.helper_80082340_result = snapshot.path_shape_forces_fallback.has_value()
        ? (*snapshot.path_shape_forces_fallback ? 1 : 0)
        : std::optional<int>{};
    snapshot.source =
        "frame_runtime_FUN_80083728_reachability_plus_persistent_FUN_80082340_path_workspace";
    return snapshot;
}

bool configure_battle_frame_visual_resource(
    BattleFrameRuntime& runtime,
    CombatantVisualResource resource) {
    const int slot = resource.binding.slot;
    if (!runtime.initialized
        || slot < 0
        || slot >= static_cast<int>(runtime.visual.resources.size())) {
        runtime.warnings.push_back(
            "visual resource binding has an invalid combatant slot");
        return false;
    }
    if (resource.selector_table.entries.empty()) {
        resource.selector_table = combatant_visual_selector_table(resource);
    }
    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant != nullptr) {
        const auto motion = initialize_combatant_std_motion_state(
            resource.action_rows);
        if (motion.status == CombatantStdMotionInitializationStatus::Matched) {
            combatant->motion_base_speed_0x12c = motion.base_speed;
            combatant->motion_alt_speed_0x130 = motion.alternate_speed;
            combatant->motion_speeds_known = true;
            combatant->turn_speed_degrees_0x128 = motion.turn_speed_degrees;
            combatant->turn_speed_bits_0x128 = motion.turn_speed_bits;
            combatant->turn_speed_known = true;
        } else {
            combatant->motion_speeds_known = false;
            combatant->turn_speed_known = false;
            runtime.warnings.push_back(
                "primary STD action rows do not establish motion state for slot "
                + std::to_string(slot) + ": " + motion.provenance);
        }
    }
    runtime.visual.resources[static_cast<std::size_t>(slot)] = std::move(resource);
    return true;
}

BattleFrameThreadMutationResult publish_battle_frame_std_resource(
    BattleFrameRuntime& runtime,
    CombatantVisualResource resource,
    std::string provenance,
    std::optional<int> parent_thread_node_id) {
    const int slot = resource.binding.slot;
    if (!configure_battle_frame_visual_resource(runtime, std::move(resource))) {
        return {
            .status = BattleFrameThreadMutationStatus::MissingInput,
            .detail = "STD resource could not be configured",
        };
    }

    auto created = create_battle_frame_thread(
        runtime.thread_list,
        BattleFrameThreadCreateRequest{
            .kind = BattleFrameThreadNodeKind::CombatantInstruction,
            .owner_slot = slot,
            .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
            .active = true,
            .insertion = BattleFrameThreadInsertionKind::AfterCurrentCursor,
            .parent_node_id = parent_thread_node_id,
            .semantic_source_id = "battle.std_resource.publication",
            .provenance = std::move(provenance)
                + "; mkChild insertion uses the live runner cursor; parent is metadata only",
            .frame_index = runtime.state.frame_index,
        });
    if (created.status == BattleFrameThreadMutationStatus::Applied) {
        const auto slot_index = static_cast<std::size_t>(slot);
        runtime.visual.std_row_producers[slot_index] = {};
        runtime.visual.timelines[slot_index] = {};
        runtime.visual.action_motion_playbacks[slot_index] = {};
        runtime.visual.persistent_instruction_callbacks[slot_index] = {};
        runtime.visual.timeline_action_ordinals[slot_index] = -1;
    }
    return created;
}

BattleFrameStdResourcePublicationResult
publish_configured_battle_frame_std_resources(
    BattleFrameRuntime& runtime,
    std::string provenance) {
    BattleFrameStdResourcePublicationResult result;
    if (!runtime.initialized || !runtime.std_resource_worker_node_id.has_value()) {
        result.provenance =
            "STD resource publication requires the producer-created resource worker";
        return result;
    }
    const auto* resource_worker = find_battle_frame_thread(
        runtime.thread_list,
        *runtime.std_resource_worker_node_id);
    if (resource_worker == nullptr || !resource_worker->active) {
        result.provenance =
            "STD resource publication cannot reset the cursor to an active resource worker";
        return result;
    }

    struct ResourceGroup {
        std::string stem;
        std::vector<int> slots;
    };
    std::vector<ResourceGroup> groups;
    for (int slot = 0; slot < kBattleFrameCombatantSlotCapacity; ++slot) {
        const auto* combatant = find_frame_combatant(runtime.state, slot);
        if (combatant == nullptr || !combatant->present) {
            continue;
        }
        const auto* resource = visual_resource_for(runtime, slot);
        if (resource == nullptr
            || resource->binding.resource_stem.empty()
            || resource->action_rows.empty()) {
            result.missing_slots.push_back(slot);
            continue;
        }
        auto group = std::find_if(
            groups.begin(),
            groups.end(),
            [&](const ResourceGroup& candidate) {
                return candidate.stem == resource->binding.resource_stem;
            });
        if (group == groups.end()) {
            groups.push_back(ResourceGroup{
                .stem = resource->binding.resource_stem,
                .slots = {slot},
            });
        } else {
            group->slots.push_back(slot);
        }
    }

    bool mutation_failed = false;
    for (const auto& group : groups) {
        result.resource_group_order.push_back(group.stem);
        const auto cursor = set_battle_frame_thread_cursor(
            runtime.thread_list,
            *runtime.std_resource_worker_node_id,
            "battle.resource_queue.group_visit",
            provenance + "; resource_group=" + group.stem,
            runtime.state.frame_index);
        if (cursor.status != BattleFrameThreadMutationStatus::Applied) {
            mutation_failed = true;
            continue;
        }
        for (const int slot : group.slots) {
            const auto* resource = visual_resource_for(runtime, slot);
            const auto movement_thread = std::find_if(
                runtime.thread_list.nodes.begin(),
                runtime.thread_list.nodes.end(),
                [slot](const BattleFrameThreadNode& node) {
                    return node.active
                        && node.kind == BattleFrameThreadNodeKind::MovementController
                        && node.owner_slot == slot;
                });
            const auto parent = movement_thread == runtime.thread_list.nodes.end()
                ? std::optional<int>{}
                : std::optional<int>{movement_thread->node_id};
            if (resource == nullptr || !parent.has_value()) {
                result.missing_slots.push_back(slot);
                mutation_failed = true;
                continue;
            }
            const auto publication = publish_battle_frame_std_resource(
                runtime,
                *resource,
                provenance + "; resource_group=" + group.stem
                    + "; slot_scan=ascending",
                parent);
            if (publication.status != BattleFrameThreadMutationStatus::Applied) {
                mutation_failed = true;
                continue;
            }
            result.publication_order.push_back(slot);
        }
    }

    std::sort(result.missing_slots.begin(), result.missing_slots.end());
    result.missing_slots.erase(
        std::unique(result.missing_slots.begin(), result.missing_slots.end()),
        result.missing_slots.end());
    if (mutation_failed || !result.missing_slots.empty() || groups.empty()) {
        result.status = BattleFrameEventStatus::MissingInput;
    } else {
        result.status = BattleFrameEventStatus::Provisional;
    }
    std::ostringstream detail;
    detail << provenance
           << "; resource_groups=" << result.resource_group_order.size()
           << "; publication_order=";
    for (std::size_t index = 0; index < result.publication_order.size(); ++index) {
        if (index != 0) {
            detail << ',';
        }
        detail << result.publication_order[index];
    }
    detail << "; group order follows the producer's first-seen pending-resource order"
              "; same-resource slots follow the ascending movement-root scan";
    if (!result.missing_slots.empty()) {
        detail << "; missing_slots=";
        for (std::size_t index = 0; index < result.missing_slots.size(); ++index) {
            if (index != 0) {
                detail << ',';
            }
            detail << result.missing_slots[index];
        }
    }
    result.provenance = detail.str();
    return result;
}

void configure_battle_frame_visual_pathing_profile(
    BattleFrameRuntime& runtime,
    std::string profile_name) {
    runtime.visual.pathing_profile_name = std::move(profile_name);
}

bool stage_battle_frame_visual_instruction_state(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    CombatantVisualInstructionSnapshot instruction) {
    if (!runtime.initialized
        || instruction.slot < 0
        || instruction.slot >= static_cast<int>(runtime.visual.timelines.size())) {
        runtime.warnings.push_back(
            "visual instruction-state staging has an invalid combatant slot");
        return false;
    }

    auto* combatant = find_frame_combatant(runtime.state, instruction.slot);
    const auto* resource = visual_resource_for(runtime, instruction.slot);
    if (combatant == nullptr) {
        runtime.warnings.push_back(
            "visual instruction-state staging has no combatant state");
        return false;
    }
    if (!instruction.target_slot.has_value()
        && combatant->instruction_target_slot_0x4 >= 0) {
        instruction.target_slot = combatant->instruction_target_slot_0x4;
    }
    if (!instruction.subtype.has_value()
        && combatant->visual_instruction_subtype_0x8 >= 0) {
        instruction.subtype = combatant->visual_instruction_subtype_0x8;
    }
    if (!instruction.instruction_flags.has_value()) {
        instruction.instruction_flags = combatant->instruction_flags_0xec;
    }

    const auto key = resolve_combatant_visual_action_key(instruction);
    combatant->instruction_target_slot_0x4 = instruction.target_slot.value_or(-1);
    combatant->visual_instruction_subtype_0x8 = instruction.subtype.value_or(-1);
    combatant->visual_instruction_knowledge = instruction.knowledge;
    combatant->visual_instruction_provenance = instruction.provenance;
    combatant->visual_instruction_action_ordinal = action_ordinal;
    ++combatant->visual_instruction_revision;
    if (instruction.runtime_instruction_mode.has_value()) {
        combatant->visual_instruction_mode_0x6 =
            *instruction.runtime_instruction_mode;
    } else if (instruction.validated_transition_mode.has_value()) {
        combatant->visual_instruction_mode_0x6 =
            *instruction.validated_transition_mode;
    } else if (key.action_key.has_value()) {
        combatant->visual_instruction_mode_0x6 = *key.action_key;
    }

    const auto& callback_runtime =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(instruction.slot)];

    BattleFrameStepEvent event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualInstructionDecision,
        "CombatantVisualInstructionStateStage",
        frame_event_status(key.status),
        "action_key="
            + (key.action_key.has_value() ? std::to_string(*key.action_key) : "missing")
            + "; key_source=" + combatant_visual_key_source_name(key.source)
            + "; instruction_state_revision="
            + std::to_string(combatant->visual_instruction_revision)
            + "; selected_action_row="
            + std::to_string(combatant->selected_action_row_index)
            + "; installed_callback_index="
            + std::to_string(callback_runtime.callback_index)
            + "; installed_persistent_callback="
            + action_motion_callback_family_name(
                callback_runtime.callback_family)
            + "; callback_publication=deferred_to_param4_0_instruction_thread_publisher"
            + "; installs_epoch=0; provenance="
            + key.provenance);
    event.action_ordinal = action_ordinal;
    event.slot = instruction.slot;
    event.target_slot = instruction.target_slot.value_or(-1);
    event.visual_resource = resource != nullptr
        ? resource->binding.resource_stem
        : std::string{};
    event.visual_epoch = runtime.visual.timelines[
        static_cast<std::size_t>(instruction.slot)].epoch;
    runtime.visual.pending_events.push_back(std::move(event));
    return true;
}

bool stage_battle_frame_validated_instruction_transition(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    int slot,
    int target_slot,
    std::int16_t instruction_mode,
    std::string provenance,
    int producer_visual_task_sequence) {
    auto* combatant = find_frame_combatant(runtime.state, slot);
    const auto* resource = visual_resource_for(runtime, slot);
    if (combatant == nullptr || resource == nullptr) {
        return false;
    }
    const auto selected_row = select_combatant_std_action_row(
        resource->action_rows,
        CombatantStdActionRowSelectionRequest{
            .action_id = instruction_mode,
            .secondary_key = combatant->visual_instruction_subtype_0x8 >= 0
                ? std::optional<std::int16_t>{
                    combatant->visual_instruction_subtype_0x8}
                : std::nullopt,
            .allow_transition_fallback = true,
        });
    if (!selected_row.row.has_value()) {
        return false;
    }
    const auto* target_instruction_thread = active_instruction_thread_for_slot(
        runtime.thread_list, slot);
    const auto& callback_before_commit =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(slot)];
    if (target_instruction_thread == nullptr
        || !callback_before_commit.installed
        || callback_before_commit.thread_state_0x19 != 1
        || selected_row.row->callback_index < 0) {
        return false;
    }
    apply_selected_action_row(
        *combatant,
        *selected_row.row,
        selected_row.selected_action_id,
        true);

    CombatantVisualInstructionSnapshot instruction;
    instruction.slot = slot;
    instruction.target_slot = target_slot;
    instruction.validated_transition_mode = instruction_mode;
    instruction.selected_std_action_key = selected_row.selected_action_id;
    instruction.subtype = combatant->visual_instruction_subtype_0x8 >= 0
        ? std::optional<std::int16_t>{combatant->visual_instruction_subtype_0x8}
        : std::nullopt;
    instruction.knowledge =
        selected_row.status == CombatantStdActionRowSelectionStatus::Matched
        ? CombatantVisualInstructionKnowledge::Known
        : CombatantVisualInstructionKnowledge::Provisional;
    const std::string transition_provenance = std::move(provenance)
        + "; FUN_800214FC selected action row "
        + std::to_string(selected_row.row->index)
        + " and executed the direct param4=0 publisher";
    instruction.provenance = transition_provenance;
    const bool staged = stage_battle_frame_visual_instruction_state(
        runtime,
        action_ordinal,
        std::move(instruction));
    if (!staged) {
        return false;
    }
    if (!publish_battle_frame_persistent_instruction_callback(
            runtime,
            slot,
            BattleFrameInstructionCallbackPublicationSource::
                ExplicitModeledTransition,
            transition_provenance
                + "; FUN_80020250 published IW+0xE0 before resetting IW+0x12")) {
        return false;
    }
    return apply_direct_instruction_control_reset(
        runtime,
        action_ordinal,
        slot,
        producer_visual_task_sequence,
        transition_provenance
            + "; FUN_80020250 committed IW+0x12=0 at 0x80020310");
}

bool stage_battle_frame_queued_std_action_transition(
    BattleFrameRuntime& runtime,
    int action_ordinal) {
    if (!runtime.initialized
        || !runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != action_ordinal) {
        return false;
    }

    auto& action = *runtime.active_action;
    if (!action.action_resolution_available
        || !action.queued_state_transition_pending
        || action.queued_state_transition_published
        || !action.queued_state_transition.has_value()) {
        return false;
    }
    auto* combatant = find_frame_combatant(runtime.state, action.actor_slot);
    if (combatant == nullptr || !combatant->present) {
        return false;
    }

    const auto& transition = *action.queued_state_transition;
    if (!transition.instruction_mode.has_value()) {
        action.queued_state_transition_pending = false;
        auto event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::VisualInstructionDecision,
            "QueuedStdActionTransitionUnsupported",
            transition.status == QueuedInstructionParamStatus::MissingInput
                ? BattleFrameEventStatus::MissingInput
                : BattleFrameEventStatus::Unsupported,
            "route=" + std::string(basic_attack_execution_route_name(action.execution_route))
                + "; transition_status="
                + queued_instruction_param_status_name(transition.status)
                + "; provenance=" + transition.provenance);
        event.action_ordinal = action.action_ordinal;
        event.slot = action.actor_slot;
        event.target_slot = action.target_slot;
        runtime.visual.pending_events.push_back(std::move(event));
        return false;
    }

    const auto selected_mode = static_cast<std::int16_t>(
        *transition.instruction_mode);
    CombatantStdActionRowSelectionResult selected_row;
    const auto* resource = visual_resource_for(runtime, action.actor_slot);
    if (resource != nullptr) {
        selected_row = select_combatant_std_action_row(
            resource->action_rows,
            CombatantStdActionRowSelectionRequest{
                .action_id = selected_mode,
                .secondary_key = combatant->visual_instruction_subtype_0x8 >= 0
                    ? std::optional<std::int16_t>{
                        combatant->visual_instruction_subtype_0x8}
                    : std::nullopt,
                .allow_transition_fallback = true,
            });
    } else if (combatant->selected_action_row_known) {
        selected_row.status = CombatantStdActionRowSelectionStatus::Matched;
        selected_row.requested_action_id = selected_mode;
        selected_row.selected_action_id = selected_mode;
        selected_row.row = CombatantStdActionRow{
            .index = combatant->selected_action_row_index,
            .action_id = selected_mode,
            .callback_index = combatant->selected_action_row_callback_index,
            .callback_ordinal = combatant->selected_action_row_callback_ordinal,
            .flags = combatant->selected_action_row_flags,
            .transition_gate_divisor_bits =
                combatant->selected_action_row_duration_bits,
        };
        selected_row.provenance =
            "queued STD action row was explicitly supplied by a low-level runtime fixture";
    } else {
        return false;
    }

    if (!selected_row.row.has_value()) {
        return false;
    }
    const auto* target_instruction_thread = active_instruction_thread_for_slot(
        runtime.thread_list, action.actor_slot);
    const auto& callback_before_commit =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(action.actor_slot)];
    if (target_instruction_thread == nullptr
        || !callback_before_commit.installed
        || callback_before_commit.thread_state_0x19 != 1
        || selected_row.row->callback_index < 0) {
        return false;
    }

    apply_selected_action_row(
        *combatant,
        *selected_row.row,
        selected_row.selected_action_id,
        resource != nullptr);

    CombatantVisualInstructionSnapshot instruction;
    instruction.slot = action.actor_slot;
    instruction.target_slot = action.target_slot;
    instruction.runtime_instruction_mode = selected_mode;
    instruction.selected_std_action_key = selected_row.selected_action_id;
    instruction.subtype = combatant->visual_instruction_subtype_0x8 >= 0
        ? std::optional<std::int16_t>{combatant->visual_instruction_subtype_0x8}
        : std::nullopt;
    instruction.instruction_flags = combatant->instruction_flags_0xec;
    instruction.knowledge =
        transition.status == QueuedInstructionParamStatus::Validated
            && selected_row.status == CombatantStdActionRowSelectionStatus::Matched
        ? CombatantVisualInstructionKnowledge::Known
        : CombatantVisualInstructionKnowledge::Provisional;
    instruction.provenance =
        "FUN_800221FC resolved the queued controller transition after attack result "
        "on the actor combatant-instruction visit and before the same visit's STD-row producer; queued_state="
        + (transition.queued_state.has_value()
            ? std::to_string(static_cast<int>(*transition.queued_state))
            : std::string("missing"))
        + "; transition_status="
        + std::string(queued_instruction_param_status_name(transition.status))
        + "; transition_provenance=" + transition.provenance
        + "; selected_action_row_status="
        + combatant_std_action_row_selection_status_name(selected_row.status)
        + "; selected_action_row_provenance=" + selected_row.provenance;
    const bool staged = stage_battle_frame_visual_instruction_state(
        runtime,
        action_ordinal,
        std::move(instruction));
    if (!staged) {
        return false;
    }
    if (!publish_battle_frame_persistent_instruction_callback(
            runtime,
            action.actor_slot,
            BattleFrameInstructionCallbackPublicationSource::
                State1QueuedTransition,
            "FUN_800221FC accepted the queued transition and executed the exact param4=0 publisher")) {
        return false;
    }
    if (!apply_queued_instruction_control_reset(
            runtime,
            action.action_ordinal,
            action.actor_slot,
            "ResolveQueuedStdActionTransition_800221FC committed IW+0x12=0 at 0x800227AC after callback publication")) {
        return false;
    }
    action.queued_state_transition_pending = false;
    action.queued_state_transition_published = true;
    return true;
}

bool publish_battle_frame_persistent_instruction_callback(
    BattleFrameRuntime& runtime,
    int slot,
    BattleFrameInstructionCallbackPublicationSource source,
    std::string provenance) {
    if (!runtime.initialized
        || slot < 0
        || slot >= static_cast<int>(
            runtime.visual.persistent_instruction_callbacks.size())) {
        runtime.warnings.push_back(
            "persistent instruction callback publication has an invalid slot");
        return false;
    }

    auto* combatant = find_frame_combatant(runtime.state, slot);
    const auto* resource = visual_resource_for(runtime, slot);
    auto& callback_runtime =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(slot)];
    const bool state0 = source
        == BattleFrameInstructionCallbackPublicationSource::State0Initialization;
    if (combatant == nullptr || (state0 && resource == nullptr)) {
        runtime.warnings.push_back(
            "persistent instruction callback publication has no combatant or required state-0 STD resource");
        return false;
    }
    if (state0) {
        if (callback_runtime.thread_state_0x19 != 0) {
            return false;
        }
        const auto selected = select_combatant_std_action_row(
            resource->action_rows,
            CombatantStdActionRowSelectionRequest{
                .action_id = 1,
                .allow_transition_fallback = false,
            });
        if (!selected.row.has_value()) {
            auto event = make_visual_event(
                runtime,
                nullptr,
                BattleFrameWorkerStepKind::VisualInstructionStatePublish,
                "FUN_80022850_state0_callback_publication",
                selected.status
                        == CombatantStdActionRowSelectionStatus::MissingInput
                    ? BattleFrameEventStatus::MissingInput
                    : BattleFrameEventStatus::Unsupported,
                "source=State0Initialization; selected_action_id=1; callback_store=not_executed; provenance="
                    + selected.provenance);
            event.slot = slot;
            runtime.visual.pending_events.push_back(std::move(event));
            return false;
        }
        apply_selected_action_row(
            *combatant,
            *selected.row,
            selected.selected_action_id,
            true);
        CombatantVisualInstructionSnapshot initial_instruction;
        initial_instruction.slot = slot;
        initial_instruction.runtime_instruction_mode = 1;
        initial_instruction.selected_std_action_key = selected.selected_action_id;
        initial_instruction.subtype = 0;
        initial_instruction.target_slot = combatant->instruction_target_slot_0x4 >= 0
            ? std::optional<int>{combatant->instruction_target_slot_0x4}
            : std::nullopt;
        initial_instruction.instruction_flags = combatant->instruction_flags_0xec;
        initial_instruction.knowledge =
            selected.status == CombatantStdActionRowSelectionStatus::Matched
            ? CombatantVisualInstructionKnowledge::Known
            : CombatantVisualInstructionKnowledge::Provisional;
        initial_instruction.provenance =
            "FUN_80022850 state 0 selected mode 1/subtype 0 before the exact param4=0 callback publisher; "
            + selected.provenance;
        if (!stage_battle_frame_visual_instruction_state(
                runtime,
                -1,
                std::move(initial_instruction))) {
            return false;
        }
    } else if (callback_runtime.thread_state_0x19 != 1) {
        return false;
    }

    const auto selected_row = selected_action_row_for(resource, *combatant);
    if (!selected_row.has_value() || selected_row->callback_index < 0) {
        auto event = make_visual_event(
            runtime,
            nullptr,
            BattleFrameWorkerStepKind::VisualInstructionStatePublish,
            "FindWorksheetActionRowAndInstallCallback_80020094",
            BattleFrameEventStatus::MissingInput,
            "source="
                + std::string(callback_publication_source_name(source))
                + "; callback_store=not_executed; selected action row or callback index is missing; provenance="
                + provenance);
        event.action_ordinal = combatant->visual_instruction_action_ordinal;
        event.slot = slot;
        event.target_slot = combatant->instruction_target_slot_0x4;
        runtime.visual.pending_events.push_back(std::move(event));
        return false;
    }

    const bool was_installed = callback_runtime.installed;
    const int action_ordinal_before = callback_runtime.action_ordinal;
    const auto instruction_state_revision_before =
        callback_runtime.instruction_state_revision;
    const auto callback_before = callback_runtime.callback_index;
    const bool callback_changed = !was_installed
        || callback_before != selected_row->callback_index;
    const bool same_value = was_installed && !callback_changed;
    const int callback_state_before = callback_runtime.callback_state;

    callback_runtime.previous_instruction_row =
        callback_runtime.current_instruction_row;
    callback_runtime.current_instruction_row = selected_row;
    callback_runtime.installed = true;
    callback_runtime.slot = slot;
    callback_runtime.action_ordinal = state0
        ? -1
        : combatant->visual_instruction_action_ordinal;
    callback_runtime.instruction_state_revision =
        combatant->visual_instruction_revision;
    callback_runtime.callback_index = selected_row->callback_index;
    callback_runtime.callback_family = action_motion_callback_family_for_index(
        selected_row->callback_index);
    const bool new_auxiliary_context = !was_installed
        || callback_changed
        || action_ordinal_before != callback_runtime.action_ordinal
        || instruction_state_revision_before
            != callback_runtime.instruction_state_revision;
    if (new_auxiliary_context
        || callback_runtime.callback_family
            != ActionMotionPersistentCallbackFamily::
                ActionMotionBasic_8001B1B0) {
        callback_runtime.auxiliary_publication_pending = false;
    }
    ++callback_runtime.publication_revision;
    ++callback_runtime.publications;
    if (same_value) {
        ++callback_runtime.same_value_publications;
    } else if (was_installed) {
        ++callback_runtime.callback_changes;
    }
    if (!was_installed) {
        callback_runtime.callback_state = 0;
    }
    callback_runtime.status =
        callback_runtime.callback_family
                == ActionMotionPersistentCallbackFamily::Unknown
            ? ActionMotionInvocationStatus::Unsupported
            : ActionMotionInvocationStatus::Matched;
    callback_runtime.provenance = std::move(provenance);
    if (state0) {
        callback_runtime.thread_state_0x19 = 1;
        runtime.visual.std_row_producers[static_cast<std::size_t>(slot)]
            .thread_state_0x19 = 1;
    }

    const char* publisher_callsite = state0
        ? "FUN_80022850_state0_800229F8"
        : source
                == BattleFrameInstructionCallbackPublicationSource::ExplicitModeledTransition
            ? "FUN_800214FC_to_800202B4"
            : "FUN_800221FC_state1_800224E0";
    std::ostringstream detail;
    detail << "source=" << callback_publication_source_name(source)
           << "; publisher=" << publisher_callsite
           << "; helper=FUN_80020094(param4=0)"
           << "; callback_store=executed"
           << "; callback_index=" << callback_before
           << "->" << callback_runtime.callback_index
           << "; callback_family="
           << action_motion_callback_family_name(
                callback_runtime.callback_family)
           << "; publication_revision="
           << callback_runtime.publication_revision
           << "; same_value=" << (same_value ? 1 : 0)
           << "; callback_changed=" << (callback_changed ? 1 : 0)
           << "; callback_state_preserved=" << callback_state_before
           << "->" << callback_runtime.callback_state
           << "; instruction_state_revision="
           << callback_runtime.instruction_state_revision
           << "; selected_action_row=" << selected_row->index
           << "; draws=0; provenance=" << callback_runtime.provenance;
    auto event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::VisualInstructionStatePublish,
        publisher_callsite,
        callback_runtime.status == ActionMotionInvocationStatus::Unsupported
            ? BattleFrameEventStatus::Unsupported
            : state0
                ? BattleFrameEventStatus::Matched
                : BattleFrameEventStatus::Provisional,
        detail.str());
    event.action_ordinal = callback_runtime.action_ordinal;
    event.slot = slot;
    event.target_slot = combatant->instruction_target_slot_0x4;
    event.action_motion_callback_family = callback_runtime.callback_family;
    event.action_motion_callback_state_before = callback_state_before;
    event.action_motion_callback_state_after = callback_runtime.callback_state;
    event.persistent_callback_publication_revision =
        callback_runtime.publication_revision;
    event.persistent_callback_index = callback_runtime.callback_index;
    event.persistent_callback_changed = callback_changed;
    event.persistent_callback_same_value = same_value;
    runtime.visual.pending_events.push_back(std::move(event));
    return true;
}

bool battle_frame_action_visual_publication_pending(
    const BattleFrameRuntime& runtime,
    int action_ordinal) {
    if (!runtime.initialized || action_ordinal < 0) {
        return false;
    }
    if (runtime.active_action.has_value()
        && runtime.active_action->action_ordinal == action_ordinal
        && runtime.active_action->queued_state_transition_pending) {
        return true;
    }
    if (std::any_of(
            runtime.visual.pending_instruction_control_resets.begin(),
            runtime.visual.pending_instruction_control_resets.end(),
            [action_ordinal](
                const BattleFramePendingInstructionControlReset& reset) {
                return reset.action_ordinal == action_ordinal;
            })) {
        return true;
    }
    if (std::any_of(
            runtime.visual.action_motion_playbacks.begin(),
            runtime.visual.action_motion_playbacks.end(),
            [action_ordinal](const ActionMotionPlaybackRuntime& playback) {
                return playback.action_ordinal == action_ordinal
                    && action_motion_playback_blocks_publication(playback);
            })) {
        return true;
    }
    if (std::any_of(
            runtime.visual.persistent_instruction_callbacks.begin(),
            runtime.visual.persistent_instruction_callbacks.end(),
            [action_ordinal](
                const BattleFramePersistentInstructionCallbackRuntime&
                    callback) {
                return callback.action_ordinal == action_ordinal
                    && callback.auxiliary_publication_pending;
            })) {
        return true;
    }
    for (const auto& combatant : runtime.state.combatants) {
        if (!combatant.present
            || combatant.visual_instruction_action_ordinal != action_ordinal
            || combatant.visual_instruction_revision == 0
            || combatant.slot < 0
            || combatant.slot
                >= static_cast<int>(runtime.visual.std_row_producers.size())) {
            continue;
        }
        const auto& producer = runtime.visual.std_row_producers[
            static_cast<std::size_t>(combatant.slot)];
        if (producer.last_instruction_state_revision
            != combatant.visual_instruction_revision) {
            return true;
        }
    }
    return std::any_of(
        runtime.visual.child_tasks.begin(),
        runtime.visual.child_tasks.end(),
        [action_ordinal](const BattleFrameVisualChildTask& task) {
            return !task.complete
                && task.action_ordinal == action_ordinal
                && task.kind == BattleFrameVisualChildKind::ActionViewRecord
                && task.effective_mode == 1
                && !task.mode1_pathing_consumed;
        });
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
    worker.destination_source = MovementCommitDestinationSource::SelectedPathNode;
    worker.use_supplied_movement_path_once = true;

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
    runtime.active_action.reset();
    runtime.passive_participants = {};
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }
    (void)schedule_first_turn_actor_action(runtime, input);
}

BattleFrameActionScheduleResult schedule_first_turn_actor_action(
    BattleFrameRuntime& runtime,
    const BattleFrameScheduleActionInput& input) {
    BattleFrameActionScheduleResult result;
    runtime.last_step_events.clear();
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        result.detail = "frame runtime is not initialized";
        return result;
    }
    if (runtime.active_action.has_value()
        && runtime.active_action->phase != BattleFrameActionPhase::Complete) {
        result.action_ordinal = runtime.active_action->action_ordinal;
        result.status = BattleMovementInvocationStatus::Ambiguous;
        result.detail = "prior action-local passive completion mask has not drained";
        runtime.warnings.push_back(result.detail);
        return result;
    }

    const int action_ordinal = input.action_ordinal >= 0
        ? input.action_ordinal
        : runtime.next_action_ordinal;
    runtime.next_action_ordinal = std::max(
        runtime.next_action_ordinal,
        action_ordinal + 1);
    const auto decision = model_active_movement_invocation(
        make_active_invocation_input(runtime, input, action_ordinal));
    if (!decision.should_invoke) {
        append_noninvoked_decision_history(runtime, decision);
        result.action_ordinal = action_ordinal;
        result.status = decision.status;
        result.detail = decision.provenance;
        return result;
    }

    const auto initial_parameter = input.initial_instruction_parameter.has_value()
        ? input.initial_instruction_parameter
        : std::optional<std::int16_t>{
            static_cast<std::int16_t>(input.combatant_command_parameter)};
    const auto final_parameter = input.final_instruction_parameter.has_value()
        ? input.final_instruction_parameter
        : std::optional<std::int16_t>{
            static_cast<std::int16_t>(input.combatant_command_parameter)};
    const auto execution_route = input.execution_route != BasicAttackExecutionRoute::Unknown
        ? input.execution_route
        : basic_attack_route_from_final_parameter(final_parameter);
    if (input.action_kind == BattleMovementActionKind::BasicAttack
        && !movement_worker_matches_basic_attack_route(
            input.selected_worker,
            execution_route)) {
        result.action_ordinal = action_ordinal;
        result.status = BattleMovementInvocationStatus::Ambiguous;
        result.detail =
            "final queued parameter, typed execution route, and selected movement worker are inconsistent";
        runtime.warnings.push_back(result.detail);
        return result;
    }

    const int active_worker_index = static_cast<int>(runtime.workers.size());
    runtime.active_action = BattleFrameActionRuntime{
        .active = true,
        .action_ordinal = action_ordinal,
        .actor_slot = input.actor_slot,
        .target_slot = input.target_slot,
        .action_kind = input.action_kind,
        .relation_scope = input.relation_scope,
        .turn_type = input.turn_type,
        .initial_instruction_parameter = initial_parameter,
        .final_instruction_parameter = final_parameter,
        .execution_route = execution_route,
        .phase = BattleFrameActionPhase::Scheduled,
        .active_worker_index = active_worker_index,
        .setup_publication_events_pending = true,
        .status = decision.status,
        .provenance = decision.provenance,
    };
    runtime.passive_participants = {};
    auto& action = *runtime.active_action;
    const auto dispatch = model_passive_movement_dispatch(
        make_passive_dispatch_input(runtime, action));
    action.status = dispatch.status;
    action.completion_turn_phase = 4;
    for (const auto& passive_decision : dispatch.decisions) {
        if (!passive_decision.should_invoke
            || passive_decision.slot < 0
            || passive_decision.slot
                >= static_cast<int>(runtime.passive_participants.size())) {
            append_noninvoked_decision_history(runtime, passive_decision);
            continue;
        }
        auto& participant = runtime.passive_participants[
            static_cast<std::size_t>(passive_decision.slot)];
        participant.active = true;
        participant.action_ordinal = action_ordinal;
        participant.slot = passive_decision.slot;
        participant.phase = BattleFramePassiveParticipantPhase::DispatchRelayPending;
        participant.actual_callback_pc = 0x800804B8u;
        participant.deferred_callback_pc = 0x8008DEECu;
        participant.thread_state_0x19 = 0;
        participant.relation_route = passive_decision.relation_route;
        participant.controller_family = passive_decision.controller_family;
        participant.semantic_target_slot = passive_decision.semantic_target_slot;
        participant.completion_bit_set = true;
        participant.status = passive_decision.status;
        participant.confidence = passive_decision.confidence;
        participant.provenance =
            "FUN_8008E2B0 initial relay completed before active selection; FUN_8008E338 then staged deferred 0x8008DEEC and the action-local completion bit";
        action.passive_completion_mask = static_cast<std::uint16_t>(
            action.passive_completion_mask | participant_bit(passive_decision.slot));
        runtime.movement_controller_states[
            static_cast<std::size_t>(passive_decision.slot)] =
            BattleMovementControllerState::Relay804B8;
    }
    enqueue_pending_movement_invocation(runtime, decision);
    runtime.active_action->phase = BattleFrameActionPhase::PassiveDispatched;
    result.scheduled = true;
    result.action_ordinal = action_ordinal;
    result.status = decision.status;
    result.detail = decision.provenance;
    return result;
}

bool notify_first_turn_action_resolution(
    BattleFrameRuntime& runtime,
    const BattleFrameActionResolution& resolution) {
    if (!runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != resolution.action_ordinal
        || runtime.active_action->phase == BattleFrameActionPhase::Complete) {
        return false;
    }
    auto& action = *runtime.active_action;
    action.action_resolution_available = true;
    action.attack_result = resolution.attack_result;
    action.attack_landed = resolution.attack_landed;
    action.target_dead = resolution.target_dead;
    action.queued_state_transition = model_basic_attack_queued_state({
        .route = action.execution_route,
        .attack_result = resolution.attack_result,
        .counter_follow_up = false,
    });
    action.queued_state_transition_pending = true;
    action.queued_state_transition_published = false;
    if (action.phase == BattleFrameActionPhase::PassiveDispatched) {
        action.phase = BattleFrameActionPhase::Resolving;
    }
    return true;
}

bool publish_first_turn_target_reaction(
    BattleFrameRuntime& runtime,
    const BattleFrameTargetReactionPublication& publication) {
    const auto& reaction = publication.reaction;
    if (!runtime.initialized
        || publication.action_ordinal < 0
        || reaction.target_slot < 0
        || reaction.target_slot >= static_cast<int>(runtime.target_reactions.size())
        || (reaction.status != BattleTargetReactionStatus::Matched
            && reaction.status != BattleTargetReactionStatus::Provisional)
        || !runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != publication.action_ordinal) {
        return false;
    }
    auto* target = find_frame_combatant(runtime.state, reaction.target_slot);
    if (target == nullptr || !target->present) {
        return false;
    }

    auto& staged = runtime.target_reactions[
        static_cast<std::size_t>(reaction.target_slot)];
    staged.available = true;
    staged.action_ordinal = publication.action_ordinal;
    ++staged.revision;
    staged.reaction = reaction;

    target->instruction_flags_0xf0 =
        (target->instruction_flags_0xf0 & ~0x04000000U)
        | reaction.selector_flags_0xf0;
    target->instruction_flags_0xf4 =
        (target->instruction_flags_0xf4 & ~(0x01000000U | 0x04000000U))
        | reaction.selector_flags_0xf4;

    auto event = make_visual_event(
        runtime,
        nullptr,
        BattleFrameWorkerStepKind::TargetReactionPublish,
        "performAttack_80081B94/FUN_8002ECA4",
        frame_event_status(reaction.status),
        "queued_result=" + std::to_string(reaction.queued_result)
            + "; queued_counter_byte="
            + std::to_string(reaction.queued_counter_byte)
            + "; pending_damage=" + std::to_string(reaction.pending_damage)
            + "; hp_before_flush="
            + std::to_string(reaction.target_hp_before_flush)
            + "; reaction_flags_0x50="
            + std::to_string(reaction.reaction_flags_0x50)
            + "; selector_f0=" + std::to_string(reaction.selector_flags_0xf0)
            + "; selector_f4=" + std::to_string(reaction.selector_flags_0xf4)
            + "; revision=" + std::to_string(staged.revision)
            + "; provenance=" + reaction.provenance);
    event.action_ordinal = publication.action_ordinal;
    event.slot = reaction.origin_slot;
    event.target_slot = reaction.target_slot;
    runtime.visual.pending_events.push_back(std::move(event));
    return true;
}

bool set_first_turn_action_completion_override(
    BattleFrameRuntime& runtime,
    int action_ordinal,
    bool enabled) {
    if (!runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != action_ordinal
        || runtime.active_action->phase == BattleFrameActionPhase::Complete) {
        return false;
    }
    runtime.active_action->completion_override = enabled;
    return true;
}

bool open_first_turn_action_completion(
    BattleFrameRuntime& runtime,
    int action_ordinal) {
    if (!runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != action_ordinal
        || runtime.active_action->phase == BattleFrameActionPhase::Complete) {
        return false;
    }
    runtime.active_action->completion_gate_open = true;
    runtime.active_action->phase = BattleFrameActionPhase::Draining;
    return true;
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
    worker.view_placement_publisher_source_id =
        ViewPlacementCacheSemanticSource::DirectViewPublication;
    worker.view_placement_provenance =
        "provisional FUN_80014474 invocation before frame-backed action-view camera RNG";
    worker.program_steps.clear();
    build_static_worker_program(worker);
    enqueue_worker(runtime, std::move(worker));
}

void schedule_first_turn_end_view_placement(
    BattleFrameRuntime& runtime,
    int actor_slot) {
    if (!runtime.initialized) {
        runtime.warnings.push_back("frame runtime is not initialized");
        return;
    }

    BattleFrameWorker worker = make_worker(
        runtime.state,
        actor_slot,
        actor_slot,
        BattleFrameWorkerKind::ViewPlacement,
        MovementSelectedWorker::None);
    worker.rng_label = "view_placement_end_turn";
    worker.detail =
        "provisional non-victory end-turn placement request";
    worker.event_status = BattleFrameEventStatus::Provisional;
    worker.view_placement_publisher_source_id =
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication;
    worker.view_placement_provenance =
        "provisional FUN_800121D8 invocation in frame-backed end-turn tail";
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
    if (!has_active_worker(runtime)
        && !has_active_combatant_instruction(runtime)
        && !action_lifecycle_has_runnable_work(runtime)) {
        return result;
    }

    ++runtime.state.frame_index;
    ++result.frames_executed;
    begin_battle_frame_thread_traversal(
        runtime.thread_list, runtime.state.frame_index);
    resolve_unvisitable_instruction_control_resets(runtime, result);
    if (runtime.active_action.has_value()
        && runtime.active_action->setup_publication_events_pending) {
        const auto mask = runtime.active_action->passive_completion_mask;
        append_recorded_event(
            runtime,
            result,
            make_action_lifecycle_event(
                runtime,
                nullptr,
                BattleFrameWorkerStepKind::PassiveRelayPublish,
                "FUN_8008E2B0_setup_relay",
                0x8008E2B0u,
                0,
                0,
                "initial 0x800804B8 relay completed before active worker selection"));
        append_recorded_event(
            runtime,
            result,
            make_action_lifecycle_event(
                runtime,
                nullptr,
                BattleFrameWorkerStepKind::PassiveDispatchPublish,
                "FUN_8008E338_dispatch_relay",
                0x8008E338u,
                0,
                mask,
                "deferred 0x8008DEEC callbacks and action-local completion mask were published before the active worker's first visit"));
        runtime.active_action->setup_publication_events_pending = false;
    }
    runtime.visual.current_visit_cursor = 0;
    flush_pending_visual_events(runtime, result);
    visit_persistent_action_view_controller(runtime, result, rng_state);
    advance_visual_children_at_cursor(runtime, result, rng_state, 0);
    append_frame_start_position_sync_events(runtime, result);
    maybe_publish_completion_override(runtime, result);
    clear_unvisitable_passive_participants(runtime, result);
    skip_unvisitable_pending_invocations(runtime, result);
    int packed_visit_cursor = 1;
    for (std::size_t thread_index = 0;
         thread_index < runtime.thread_list.nodes.size();
         ++thread_index) {
        const auto thread = runtime.thread_list.nodes[thread_index];
        if (!thread.active) {
            continue;
        }
        (void)set_battle_frame_thread_cursor(
            runtime.thread_list,
            thread.node_id,
            "battle.thread_runner.visit",
            "case-5 traversal published the currently visited thread",
            runtime.state.frame_index);
        runtime.visual.current_visit_cursor = packed_visit_cursor++;
        if (thread.kind
            == BattleFrameThreadNodeKind::AuxiliaryVisualChild) {
            (void)advance_visual_child_thread(
                runtime,
                result,
                rng_state,
                thread.node_id);
            continue;
        }
        if (thread.kind == BattleFrameThreadNodeKind::CombatantInstruction) {
            if (thread.owner_slot < 0
                || thread.owner_slot >= static_cast<int>(
                    runtime.visual.persistent_instruction_callbacks.size())) {
                advance_visual_children_at_cursor(
                    runtime,
                    result,
                    rng_state,
                    runtime.visual.current_visit_cursor);
                continue;
            }
            if (runtime.visual.persistent_instruction_callbacks[
                    static_cast<std::size_t>(thread.owner_slot)]
                        .thread_state_0x19 == 0) {
                (void)publish_battle_frame_persistent_instruction_callback(
                    runtime,
                    thread.owner_slot,
                    BattleFrameInstructionCallbackPublicationSource::
                        State0Initialization,
                    "state-0 instruction-thread visit installed the initial callback; no callback invocation occurs on this visit");
                flush_pending_visual_events(runtime, result);
                advance_visual_children_at_cursor(
                    runtime,
                    result,
                    rng_state,
                    runtime.visual.current_visit_cursor);
                continue;
            }
            const bool direct_reset_consumed =
                consume_instruction_control_resets_for_thread(
                    runtime, thread, result);
            bool queued_transition_committed = false;
            if (runtime.active_action.has_value()
                && runtime.active_action->actor_slot == thread.owner_slot
                && runtime.active_action->queued_state_transition_pending) {
                queued_transition_committed =
                    stage_battle_frame_queued_std_action_transition(
                    runtime,
                    runtime.active_action->action_ordinal);
            }
            if (!direct_reset_consumed && !queued_transition_committed) {
                (void)publish_battle_frame_persistent_instruction_callback(
                    runtime,
                    thread.owner_slot,
                    BattleFrameInstructionCallbackPublicationSource::
                        State1CurrentInstruction,
                    "state-1 instruction-thread visit executed the exact param4=0 callback publisher before invoking IW+0xE0");
            }
            flush_pending_visual_events(runtime, result);
            const bool playback_was_blocking = thread.owner_slot >= 0
                && thread.owner_slot < static_cast<int>(
                    runtime.visual.action_motion_playbacks.size())
                && action_motion_playback_blocks_publication(
                    runtime.visual.action_motion_playbacks[
                        static_cast<std::size_t>(thread.owner_slot)]);
            bool publication_allowed = visit_action_motion_playback_for_slot(
                runtime, thread.owner_slot, result);
            if (publication_allowed && !playback_was_blocking) {
                publication_allowed = visit_persistent_instruction_callback_for_slot(
                    runtime, thread.owner_slot, result);
            }
            visit_std_row_producer_for_slot(
                runtime, thread.owner_slot, result);
            if (auto event = advance_combatant_instruction(
                    runtime, thread.owner_slot)) {
                append_recorded_event(runtime, result, std::move(*event));
            }
            (void)refresh_collision_occupancy_for_slot(
                runtime, thread.owner_slot, &result);
            advance_visual_children_at_cursor(
                runtime, result, rng_state, runtime.visual.current_visit_cursor);
            continue;
        }
        if (thread.kind != BattleFrameThreadNodeKind::MovementController) {
            advance_visual_children_at_cursor(
                runtime, result, rng_state, runtime.visual.current_visit_cursor);
            continue;
        }
        bool participant_consumed_visit = false;
        if (thread.owner_slot >= 0
            && thread.owner_slot < static_cast<int>(runtime.passive_participants.size())) {
            participant_consumed_visit = advance_passive_participant(
                runtime,
                runtime.passive_participants[static_cast<std::size_t>(thread.owner_slot)],
                result);
        }
        if (participant_consumed_visit) {
            advance_visual_children_at_cursor(
                runtime, result, rng_state, runtime.visual.current_visit_cursor);
            continue;
        }
        const int worker_index = find_queued_worker_index(runtime, thread.owner_slot);
        if (worker_index < 0) {
            advance_visual_children_at_cursor(
                runtime, result, rng_state, runtime.visual.current_visit_cursor);
            continue;
        }
        auto& worker = runtime.workers[static_cast<std::size_t>(worker_index)];
        if (worker.activation_pending) {
            activate_pending_invocation(runtime, worker_index, result);
        }
        if (worker.complete) {
            advance_visual_children_at_cursor(
                runtime, result, rng_state, runtime.visual.current_visit_cursor);
            continue;
        }
        int same_visit_steps = 0;
        bool continue_same_visit = false;
        do {
            const auto executed_step_kind = worker.program_index < worker.program_steps.size()
                ? worker.program_steps[worker.program_index].kind
                : BattleFrameWorkerStepKind::Unsupported;
            const bool completion_override_before = runtime.active_action.has_value()
                && runtime.active_action->completion_override;
            auto event = execute_worker_frame(runtime, worker, &rng_state);
            append_recorded_event(runtime, result, std::move(event));
            flush_pending_visual_events(runtime, result);

            if (!completion_override_before
                && runtime.active_action.has_value()
                && runtime.active_action->completion_override) {
                append_recorded_event(
                    runtime,
                    result,
                    make_action_lifecycle_event(
                        runtime,
                        nullptr,
                        BattleFrameWorkerStepKind::ActionPhaseTransition,
                        "movement_completion_override_publish",
                        active_completion_override_publisher_pc(runtime),
                        runtime.active_action->passive_completion_mask,
                        runtime.active_action->passive_completion_mask,
                        "active callback reached its evidence-backed post-resolution override publication"));
            }

            continue_same_visit = continue_worker_in_same_thread_visit(
                worker, executed_step_kind);
            ++same_visit_steps;
        } while (continue_same_visit && same_visit_steps < 16);

        if (continue_same_visit) {
            result.ok = false;
            result.ambiguous = true;
            result.warnings.push_back(
                "movement callback control steps exceeded the same-thread-visit cap");
        }

        if (worker.complete) {
            if (runtime.active_action.has_value()
                && runtime.active_action->active_worker_index == worker_index) {
                runtime.movement_controller_states[
                    static_cast<std::size_t>(worker.slot)] =
                    BattleMovementControllerState::Idle;
            } else if (worker.kind == BattleFrameWorkerKind::PassiveController) {
                mark_passive_worker_complete(runtime, worker, result);
            } else if (worker.slot >= 0
                && worker.slot < static_cast<int>(runtime.movement_controller_states.size())
                && is_movement_worker(worker.kind)) {
                runtime.movement_controller_states[static_cast<std::size_t>(worker.slot)] =
                    BattleMovementControllerState::Idle;
            }
        }
        advance_visual_children_at_cursor(
            runtime, result, rng_state, runtime.visual.current_visit_cursor);
    }

    end_battle_frame_thread_traversal(runtime.thread_list);
    runtime.visual.current_visit_cursor = packed_visit_cursor;
    maybe_publish_completion_override(runtime, result);
    complete_action_if_drained(runtime, result);

    return result;
}

BattleFrameRunResult run_first_turn_until_idle(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state,
    int max_frames) {
    BattleFrameRunResult combined;
    for (int frame = 0; frame < max_frames; ++frame) {
        if (!has_active_worker(runtime)
            && !has_active_combatant_instruction(runtime)
            && !action_lifecycle_has_runnable_work(runtime)) {
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

    if (has_active_worker(runtime)
        || has_active_combatant_instruction(runtime)
        || action_lifecycle_has_runnable_work(runtime)) {
        combined.ok = false;
        combined.ambiguous = true;
        combined.warnings.push_back("first-turn frame workers did not finish before max frame budget");
    }
    return combined;
}

BattleFrameRunResult run_first_turn_action_until_complete(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng_state,
    int action_ordinal,
    int max_frames) {
    BattleFrameRunResult combined;
    if (!runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != action_ordinal) {
        combined.ok = false;
        combined.ambiguous = true;
        combined.warnings.push_back("requested action ordinal is not active");
        return combined;
    }

    complete_action_if_drained(runtime, combined);
    for (int frame = 0; frame < max_frames
        && runtime.active_action->phase != BattleFrameActionPhase::Complete;
        ++frame) {
        if (!has_active_worker(runtime)
            && !has_active_combatant_instruction(runtime)
            && !action_lifecycle_has_runnable_work(runtime)) {
            break;
        }
        auto step = run_first_turn_frame(runtime, rng_state);
        combined.frames_executed += step.frames_executed;
        combined.events.insert(
            combined.events.end(), step.events.begin(), step.events.end());
        combined.warnings.insert(
            combined.warnings.end(), step.warnings.begin(), step.warnings.end());
        if (!step.ok) {
            combined.ok = false;
            combined.ambiguous = combined.ambiguous || step.ambiguous;
            break;
        }
    }

    if (!runtime.active_action.has_value()
        || runtime.active_action->action_ordinal != action_ordinal
        || runtime.active_action->phase != BattleFrameActionPhase::Complete) {
        combined.ok = false;
        combined.ambiguous = true;
        combined.warnings.push_back(
            "action-local passive completion mask did not drain before frame cap");
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
        if (!has_active_worker(runtime)
            && !has_active_combatant_instruction(runtime)) {
            break;
        }

        ++runtime.state.frame_index;
        ++result.frames_executed;
        append_frame_start_position_sync_events(runtime, result);
        skip_unvisitable_pending_invocations(runtime, result);
        for (const auto& thread : runtime.thread_list.nodes) {
            if (!thread.active) {
                continue;
            }
            (void)set_battle_frame_thread_cursor(
                runtime.thread_list,
                thread.node_id,
                "battle.thread_runner.visit",
                "scheduled frame traversal published the currently visited thread",
                runtime.state.frame_index);
            if (thread.kind == BattleFrameThreadNodeKind::CombatantInstruction) {
                if (auto event = advance_combatant_instruction(
                        runtime, thread.owner_slot)) {
                    append_recorded_event(runtime, result, std::move(*event));
                }
                continue;
            }
            if (thread.kind != BattleFrameThreadNodeKind::MovementController) {
                continue;
            }
            const int worker_index = find_queued_worker_index(
                runtime, thread.owner_slot);
            if (worker_index < 0) {
                continue;
            }
            auto& worker = runtime.workers[static_cast<std::size_t>(worker_index)];
            if (worker.activation_pending) {
                activate_pending_invocation(runtime, worker_index, result);
            }
            if (worker.complete) {
                continue;
            }
            auto event = execute_worker_frame(runtime, worker, nullptr);
            append_recorded_event(runtime, result, std::move(event));
            if (worker.complete
                && worker.slot >= 0
                && worker.slot < static_cast<int>(runtime.movement_controller_states.size())
                && is_movement_worker(worker.kind)) {
                runtime.movement_controller_states[static_cast<std::size_t>(worker.slot)] =
                    BattleMovementControllerState::Idle;
            }
        }
    }

    if (has_active_worker(runtime)
        || has_active_combatant_instruction(runtime)) {
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
    case BattleFrameWorkerKind::ViewPlacement:
        return "ViewPlacement";
    case BattleFrameWorkerKind::MechanicalAttack:
        return "MechanicalAttack";
    case BattleFrameWorkerKind::EffectChunk:
        return "EffectChunk";
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
    case BattleFrameWorkerKind::PassiveController:
        return "PassiveController";
    case BattleFrameWorkerKind::VisualController:
        return "VisualController";
    case BattleFrameWorkerKind::VisualActionService:
        return "VisualActionService";
    case BattleFrameWorkerKind::VisualCollisionBox:
        return "VisualCollisionBox";
    case BattleFrameWorkerKind::VisualActionViewRecord:
        return "VisualActionViewRecord";
    case BattleFrameWorkerKind::VisualUnsupportedCommand:
        return "VisualUnsupportedCommand";
    case BattleFrameWorkerKind::CombatantInstruction:
        return "CombatantInstruction";
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
    case BattleFrameEventStatus::Skipped:
        return "Skipped";
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
        return "approach_motion";
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
    case BattleFrameWorkerStepKind::MovementInvocationActivate:
        return "MovementInvocationActivate";
    case BattleFrameWorkerStepKind::MovementControllerHandoff:
        return "MovementControllerHandoff";
    case BattleFrameWorkerStepKind::MovementInvocationSkipped:
        return "MovementInvocationSkipped";
    case BattleFrameWorkerStepKind::ActionPhaseTransition:
        return "ActionPhaseTransition";
    case BattleFrameWorkerStepKind::PassiveRelayPublish:
        return "PassiveRelayPublish";
    case BattleFrameWorkerStepKind::PassiveRelayAdvance:
        return "PassiveRelayAdvance";
    case BattleFrameWorkerStepKind::PassiveDispatchPublish:
        return "PassiveDispatchPublish";
    case BattleFrameWorkerStepKind::PassiveFamilySelect:
        return "PassiveFamilySelect";
    case BattleFrameWorkerStepKind::PassiveCompletionDeferred:
        return "PassiveCompletionDeferred";
    case BattleFrameWorkerStepKind::PassiveCompletionClear:
        return "PassiveCompletionClear";
    case BattleFrameWorkerStepKind::PassiveDeathClear:
        return "PassiveDeathClear";
    case BattleFrameWorkerStepKind::ActionComplete:
        return "ActionComplete";
    case BattleFrameWorkerStepKind::VisualControllerVisit:
        return "VisualControllerVisit";
    case BattleFrameWorkerStepKind::VisualInstructionDecision:
        return "VisualInstructionDecision";
    case BattleFrameWorkerStepKind::VisualInstructionStatePublish:
        return "VisualInstructionStatePublish";
    case BattleFrameWorkerStepKind::TargetReactionPublish:
        return "TargetReactionPublish";
    case BattleFrameWorkerStepKind::DirectTransitionSelect:
        return "DirectTransitionSelect";
    case BattleFrameWorkerStepKind::CollisionOccupancyRefresh:
        return "CollisionOccupancyRefresh";
    case BattleFrameWorkerStepKind::CollisionProbe:
        return "CollisionProbe";
    case BattleFrameWorkerStepKind::InstructionCallbackControlReset:
        return "InstructionCallbackControlReset";
    case BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume:
        return "InstructionCallbackControlResetConsume";
    case BattleFrameWorkerStepKind::ActionMotionInvocationDecision:
        return "ActionMotionInvocationDecision";
    case BattleFrameWorkerStepKind::ActionMotionPlaybackInstall:
        return "ActionMotionPlaybackInstall";
    case BattleFrameWorkerStepKind::ActionMotionRendererAdvance:
        return "ActionMotionRendererAdvance";
    case BattleFrameWorkerStepKind::ActionMotionState6Poll:
        return "ActionMotionState6Poll";
    case BattleFrameWorkerStepKind::ActionMotionPostState6Delay:
        return "ActionMotionPostState6Delay";
    case BattleFrameWorkerStepKind::ActionMotionPublicationRelease:
        return "ActionMotionPublicationRelease";
    case BattleFrameWorkerStepKind::VisualStdRowProducerVisit:
        return "VisualStdRowProducerVisit";
    case BattleFrameWorkerStepKind::VisualInstructionInstall:
        return "VisualInstructionInstall";
    case BattleFrameWorkerStepKind::VisualAuxiliaryPublication:
        return "VisualAuxiliaryPublication";
    case BattleFrameWorkerStepKind::VisualCommandPublish:
        return "VisualCommandPublish";
    case BattleFrameWorkerStepKind::VisualChildState0:
        return "VisualChildState0";
    case BattleFrameWorkerStepKind::VisualChildDelay:
        return "VisualChildDelay";
    case BattleFrameWorkerStepKind::VisualChildNested:
        return "VisualChildNested";
    case BattleFrameWorkerStepKind::VisualChildCleanup:
        return "VisualChildCleanup";
    case BattleFrameWorkerStepKind::VisualMode0Rewrite:
        return "VisualMode0Rewrite";
    case BattleFrameWorkerStepKind::VisualMode0eCamera:
        return "VisualMode0eCamera";
    case BattleFrameWorkerStepKind::VisualMode1Pathing:
        return "VisualMode1Pathing";
    case BattleFrameWorkerStepKind::VisualUnsupportedWait:
        return "VisualUnsupportedWait";
    case BattleFrameWorkerStepKind::CallbackEntry:
        return "CallbackEntry";
    case BattleFrameWorkerStepKind::PathBuild:
        return "PathBuild";
    case BattleFrameWorkerStepKind::PathNodeSelection:
        return "PathNodeSelection";
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
    case BattleFrameWorkerStepKind::CombatantInstructionPublish:
        return "CombatantInstructionPublish";
    case BattleFrameWorkerStepKind::CombatantInstructionWait:
        return "CombatantInstructionWait";
    case BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc:
        return "ActionMotionSetup_8001fabc";
    case BattleFrameWorkerStepKind::ActionMotionRotateStep_8001b630_80061114:
        return "ActionMotionRotateStep_8001b630_80061114";
    case BattleFrameWorkerStepKind::ActionMotionMoveStep_8001e910:
        return "ActionMotionMoveStep_8001e910";
    case BattleFrameWorkerStepKind::MoveIncrementApply_80061340:
        return "MoveIncrementApply_80061340";
    case BattleFrameWorkerStepKind::MotionStopResult_8001eb54:
        return "MotionStopResult_8001eb54";
    case BattleFrameWorkerStepKind::NextLegOrRebuildDecision:
        return "NextLegOrRebuildDecision";
    case BattleFrameWorkerStepKind::Rng:
        return "Rng";
    case BattleFrameWorkerStepKind::ViewPlacementResolve:
        return "ViewPlacementResolve";
    case BattleFrameWorkerStepKind::Marker:
        return "Marker";
    case BattleFrameWorkerStepKind::NoCommit:
        return "NoCommit";
    case BattleFrameWorkerStepKind::FallbackSetupWait:
        return "FallbackSetupWait";
    case BattleFrameWorkerStepKind::FallbackMode7Publish:
        return "FallbackMode7Publish";
    case BattleFrameWorkerStepKind::FallbackAttackResolutionWait:
        return "FallbackAttackResolutionWait";
    case BattleFrameWorkerStepKind::FallbackVisualCompletionWait:
        return "FallbackVisualCompletionWait";
    case BattleFrameWorkerStepKind::FallbackTerminalHandoff:
        return "FallbackTerminalHandoff";
    case BattleFrameWorkerStepKind::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

const char* battle_frame_action_phase_name(BattleFrameActionPhase phase) {
    switch (phase) {
    case BattleFrameActionPhase::None: return "None";
    case BattleFrameActionPhase::Scheduled: return "Scheduled";
    case BattleFrameActionPhase::Active: return "Active";
    case BattleFrameActionPhase::HandoffPending: return "HandoffPending";
    case BattleFrameActionPhase::PassiveDispatched: return "PassiveDispatched";
    case BattleFrameActionPhase::Resolving: return "Resolving";
    case BattleFrameActionPhase::Draining: return "Draining";
    case BattleFrameActionPhase::Complete: return "Complete";
    }
    return "None";
}

const char* battle_frame_passive_participant_phase_name(
    BattleFramePassiveParticipantPhase phase) {
    switch (phase) {
    case BattleFramePassiveParticipantPhase::Inactive: return "Inactive";
    case BattleFramePassiveParticipantPhase::InitialRelayPending:
        return "InitialRelayPending";
    case BattleFramePassiveParticipantPhase::WaitingForDispatch:
        return "WaitingForDispatch";
    case BattleFramePassiveParticipantPhase::DispatchRelayPending:
        return "DispatchRelayPending";
    case BattleFramePassiveParticipantPhase::Dispatching: return "Dispatching";
    case BattleFramePassiveParticipantPhase::FamilyPending: return "FamilyPending";
    case BattleFramePassiveParticipantPhase::FamilyActive: return "FamilyActive";
    case BattleFramePassiveParticipantPhase::CompletionDeferred:
        return "CompletionDeferred";
    case BattleFramePassiveParticipantPhase::Cleared: return "Cleared";
    case BattleFramePassiveParticipantPhase::Removed: return "Removed";
    }
    return "Inactive";
}

const char* battle_frame_combatant_instruction_phase_name(
    BattleFrameCombatantInstructionPhase phase) {
    switch (phase) {
    case BattleFrameCombatantInstructionPhase::Idle: return "Idle";
    case BattleFrameCombatantInstructionPhase::SetupPending: return "SetupPending";
    case BattleFrameCombatantInstructionPhase::Rotating: return "Rotating";
    case BattleFrameCombatantInstructionPhase::Moving: return "Moving";
    case BattleFrameCombatantInstructionPhase::StopPending: return "StopPending";
    case BattleFrameCombatantInstructionPhase::Complete: return "Complete";
    case BattleFrameCombatantInstructionPhase::Removed: return "Removed";
    case BattleFrameCombatantInstructionPhase::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* battle_frame_visual_child_kind_name(BattleFrameVisualChildKind kind) {
    switch (kind) {
    case BattleFrameVisualChildKind::ActionService: return "ActionService";
    case BattleFrameVisualChildKind::CollisionBox: return "CollisionBox";
    case BattleFrameVisualChildKind::ActionViewRecord: return "ActionViewRecord";
    case BattleFrameVisualChildKind::UnsupportedCommand:
        return "UnsupportedCommand";
    }
    return "ActionViewRecord";
}

const char* battle_frame_visual_child_phase_name(BattleFrameVisualChildPhase phase) {
    switch (phase) {
    case BattleFrameVisualChildPhase::Published: return "Published";
    case BattleFrameVisualChildPhase::State0: return "State0";
    case BattleFrameVisualChildPhase::Delay: return "Delay";
    case BattleFrameVisualChildPhase::Active: return "Active";
    case BattleFrameVisualChildPhase::CompletionWait: return "CompletionWait";
    case BattleFrameVisualChildPhase::Complete: return "Complete";
    }
    return "Complete";
}

const char* battle_frame_instruction_control_reset_source_name(
    BattleFrameInstructionControlResetSource source) {
    switch (source) {
    case BattleFrameInstructionControlResetSource::QueuedTransition:
        return "QueuedTransition";
    case BattleFrameInstructionControlResetSource::DirectTransition:
        return "DirectTransition";
    }
    return "QueuedTransition";
}

const char* battle_frame_instruction_control_reset_lifecycle_name(
    BattleFrameInstructionControlResetLifecycle lifecycle) {
    switch (lifecycle) {
    case BattleFrameInstructionControlResetLifecycle::Applied:
        return "Applied";
    case BattleFrameInstructionControlResetLifecycle::Consumed:
        return "Consumed";
    case BattleFrameInstructionControlResetLifecycle::TargetRemoved:
        return "TargetRemoved";
    case BattleFrameInstructionControlResetLifecycle::TargetReplaced:
        return "TargetReplaced";
    case BattleFrameInstructionControlResetLifecycle::Superseded:
        return "Superseded";
    case BattleFrameInstructionControlResetLifecycle::MissingInput:
        return "MissingInput";
    }
    return "MissingInput";
}

const char* battle_frame_instruction_control_reset_timing_name(
    BattleFrameInstructionControlResetTiming timing) {
    switch (timing) {
    case BattleFrameInstructionControlResetTiming::SameInstructionVisit:
        return "SameInstructionVisit";
    case BattleFrameInstructionControlResetTiming::SameFrameLaterVisit:
        return "SameFrameLaterVisit";
    case BattleFrameInstructionControlResetTiming::NextFrameVisit:
        return "NextFrameVisit";
    case BattleFrameInstructionControlResetTiming::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace savor::predict
