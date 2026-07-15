#include "BattleFrameThreadListModel.h"

#include <algorithm>

namespace savor::predict {
namespace {

void append_event(
    BattleFrameThreadListRuntime& runtime,
    const BattleFrameThreadCreateRequest& request,
    BattleFrameThreadMutationKind kind,
    BattleFrameThreadMutationStatus status,
    int node_id,
    BattleFrameThreadCallbackIdentity before,
    BattleFrameThreadCallbackIdentity after) {
    runtime.history.push_back(BattleFrameThreadMutationEvent{
        .sequence = runtime.next_event_sequence++,
        .frame_index = request.frame_index,
        .worker_sequence = request.worker_sequence,
        .kind = kind,
        .status = status,
        .node_id = node_id,
        .owner_slot = request.owner_slot,
        .callback_before = before,
        .callback_after = after,
        .semantic_source_id = request.semantic_source_id,
        .provenance = request.provenance,
    });
}

BattleFrameThreadMutationResult mutate_existing(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    BattleFrameThreadMutationKind kind,
    std::optional<BattleFrameThreadCallbackIdentity> callback,
    std::optional<bool> active,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index,
    std::optional<std::uint64_t> worker_sequence) {
    auto* node = find_battle_frame_thread(runtime, node_id);
    BattleFrameThreadCreateRequest event_request{
        .owner_slot = node != nullptr ? node->owner_slot : -1,
        .semantic_source_id = std::move(semantic_source_id),
        .provenance = std::move(provenance),
        .frame_index = frame_index,
        .worker_sequence = worker_sequence,
    };
    if (node == nullptr) {
        append_event(
            runtime,
            event_request,
            kind,
            BattleFrameThreadMutationStatus::NotFound,
            node_id,
            BattleFrameThreadCallbackIdentity::Unknown,
            callback.value_or(BattleFrameThreadCallbackIdentity::Unknown));
        return {
            .status = BattleFrameThreadMutationStatus::NotFound,
            .node_id = node_id,
            .detail = "thread node was not found",
        };
    }

    const auto before = node->callback;
    if (callback.has_value()) {
        node->callback = *callback;
    }
    if (active.has_value()) {
        node->active = *active;
    }
    node->semantic_source_id = event_request.semantic_source_id;
    node->provenance = event_request.provenance;
    append_event(
        runtime,
        event_request,
        kind,
        BattleFrameThreadMutationStatus::Applied,
        node_id,
        before,
        node->callback);
    return {
        .status = BattleFrameThreadMutationStatus::Applied,
        .node_id = node_id,
        .detail = "thread mutation applied",
    };
}

} // namespace

BattleFrameThreadMutationResult create_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    BattleFrameThreadCreateRequest request) {
    if (request.semantic_source_id.empty()
        || request.kind == BattleFrameThreadNodeKind::Unknown
        || request.callback == BattleFrameThreadCallbackIdentity::Unknown) {
        append_event(
            runtime,
            request,
            BattleFrameThreadMutationKind::Create,
            BattleFrameThreadMutationStatus::MissingInput,
            -1,
            BattleFrameThreadCallbackIdentity::Unknown,
            request.callback);
        return {
            .status = BattleFrameThreadMutationStatus::MissingInput,
            .detail = "thread creation requires a semantic source, kind, and callback",
        };
    }

    const auto duplicate = std::find_if(
        runtime.nodes.begin(),
        runtime.nodes.end(),
        [&](const BattleFrameThreadNode& node) {
            return node.active && node.kind == request.kind
                && node.owner_slot == request.owner_slot;
        });
    if (duplicate != runtime.nodes.end()) {
        append_event(
            runtime,
            request,
            BattleFrameThreadMutationKind::Create,
            BattleFrameThreadMutationStatus::Duplicate,
            duplicate->node_id,
            duplicate->callback,
            request.callback);
        return {
            .status = BattleFrameThreadMutationStatus::Duplicate,
            .node_id = duplicate->node_id,
            .detail = "an active thread of this kind already exists for the owner",
        };
    }

    std::size_t insertion_index = runtime.nodes.size();
    if (request.insertion == BattleFrameThreadInsertionKind::AfterCurrentCursor) {
        if (!runtime.current_node_id.has_value()) {
            if (!runtime.nodes.empty()) {
                append_event(
                    runtime,
                    request,
                    BattleFrameThreadMutationKind::Create,
                    BattleFrameThreadMutationStatus::MissingInput,
                    -1,
                    BattleFrameThreadCallbackIdentity::Unknown,
                    request.callback);
                return {
                    .status = BattleFrameThreadMutationStatus::MissingInput,
                    .detail = "current-cursor insertion requires the live thread cursor",
                };
            }
            insertion_index = 0;
        } else {
            const auto cursor = std::find_if(
                runtime.nodes.begin(),
                runtime.nodes.end(),
                [&](const BattleFrameThreadNode& node) {
                    return node.node_id == *runtime.current_node_id;
                });
            if (cursor == runtime.nodes.end()) {
                append_event(
                    runtime,
                    request,
                    BattleFrameThreadMutationKind::Create,
                    BattleFrameThreadMutationStatus::NotFound,
                    *runtime.current_node_id,
                    BattleFrameThreadCallbackIdentity::Unknown,
                    request.callback);
                return {
                    .status = BattleFrameThreadMutationStatus::NotFound,
                    .node_id = *runtime.current_node_id,
                    .detail = "current thread cursor does not identify a live list node",
                };
            }
            insertion_index = static_cast<std::size_t>(
                std::distance(runtime.nodes.begin(), cursor) + 1);
        }
    } else if (request.insertion == BattleFrameThreadInsertionKind::AfterNode) {
        if (!request.relative_node_id.has_value()) {
            append_event(
                runtime,
                request,
                BattleFrameThreadMutationKind::Create,
                BattleFrameThreadMutationStatus::MissingInput,
                -1,
                BattleFrameThreadCallbackIdentity::Unknown,
                request.callback);
            return {
                .status = BattleFrameThreadMutationStatus::MissingInput,
                .detail = "after-node insertion requires a relative node",
            };
        }
        const auto relative = std::find_if(
            runtime.nodes.begin(),
            runtime.nodes.end(),
            [&](const BattleFrameThreadNode& node) {
                return node.node_id == *request.relative_node_id;
            });
        if (relative == runtime.nodes.end()) {
            return {
                .status = BattleFrameThreadMutationStatus::NotFound,
                .detail = "relative thread node was not found",
            };
        }
        insertion_index = static_cast<std::size_t>(
            std::distance(runtime.nodes.begin(), relative) + 1);
    } else if (request.insertion == BattleFrameThreadInsertionKind::LastChild) {
        if (!request.parent_node_id.has_value()) {
            append_event(
                runtime,
                request,
                BattleFrameThreadMutationKind::Create,
                BattleFrameThreadMutationStatus::MissingInput,
                -1,
                BattleFrameThreadCallbackIdentity::Unknown,
                request.callback);
            return {
                .status = BattleFrameThreadMutationStatus::MissingInput,
                .detail = "child insertion requires a parent node",
            };
        }
        const auto parent = std::find_if(
            runtime.nodes.begin(),
            runtime.nodes.end(),
            [&](const BattleFrameThreadNode& node) {
                return node.node_id == *request.parent_node_id;
            });
        if (parent == runtime.nodes.end()) {
            return {
                .status = BattleFrameThreadMutationStatus::NotFound,
                .detail = "parent thread node was not found",
            };
        }
        insertion_index = static_cast<std::size_t>(
            std::distance(runtime.nodes.begin(), parent) + 1);
        while (insertion_index < runtime.nodes.size()
            && runtime.nodes[insertion_index].parent_node_id == request.parent_node_id) {
            ++insertion_index;
        }
    }

    BattleFrameThreadNode node{
        .node_id = runtime.next_node_id++,
        .creation_sequence = runtime.next_creation_sequence++,
        .kind = request.kind,
        .owner_slot = request.owner_slot,
        .callback = request.callback,
        .active = request.active,
        .parent_node_id = request.parent_node_id,
        .semantic_source_id = request.semantic_source_id,
        .provenance = request.provenance,
    };
    const int node_id = node.node_id;
    runtime.nodes.insert(
        runtime.nodes.begin() + static_cast<std::ptrdiff_t>(insertion_index),
        std::move(node));
    if (request.insertion == BattleFrameThreadInsertionKind::AfterCurrentCursor) {
        runtime.current_node_id = node_id;
    }
    append_event(
        runtime,
        request,
        BattleFrameThreadMutationKind::Create,
        BattleFrameThreadMutationStatus::Applied,
        node_id,
        BattleFrameThreadCallbackIdentity::Unknown,
        request.callback);
    return {
        .status = BattleFrameThreadMutationStatus::Applied,
        .node_id = node_id,
        .detail = "thread node created",
    };
}

BattleFrameThreadMutationResult replace_battle_frame_thread_callback(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    BattleFrameThreadCallbackIdentity callback,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index,
    std::optional<std::uint64_t> worker_sequence) {
    return mutate_existing(
        runtime,
        node_id,
        BattleFrameThreadMutationKind::ReplaceCallback,
        callback,
        std::nullopt,
        std::move(semantic_source_id),
        std::move(provenance),
        frame_index,
        worker_sequence);
}

BattleFrameThreadMutationResult set_battle_frame_thread_active(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    bool active,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index,
    std::optional<std::uint64_t> worker_sequence) {
    return mutate_existing(
        runtime,
        node_id,
        BattleFrameThreadMutationKind::SetActive,
        std::nullopt,
        active,
        std::move(semantic_source_id),
        std::move(provenance),
        frame_index,
        worker_sequence);
}

BattleFrameThreadMutationResult set_battle_frame_thread_cursor(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index,
    std::optional<std::uint64_t> worker_sequence) {
    auto* node = find_battle_frame_thread(runtime, node_id);
    BattleFrameThreadCreateRequest event_request{
        .owner_slot = node != nullptr ? node->owner_slot : -1,
        .semantic_source_id = std::move(semantic_source_id),
        .provenance = std::move(provenance),
        .frame_index = frame_index,
        .worker_sequence = worker_sequence,
    };
    if (event_request.semantic_source_id.empty()) {
        append_event(
            runtime,
            event_request,
            BattleFrameThreadMutationKind::SetCursor,
            BattleFrameThreadMutationStatus::MissingInput,
            node_id,
            BattleFrameThreadCallbackIdentity::Unknown,
            BattleFrameThreadCallbackIdentity::Unknown);
        return {
            .status = BattleFrameThreadMutationStatus::MissingInput,
            .node_id = node_id,
            .detail = "thread cursor publication requires a semantic source",
        };
    }
    if (node == nullptr) {
        append_event(
            runtime,
            event_request,
            BattleFrameThreadMutationKind::SetCursor,
            BattleFrameThreadMutationStatus::NotFound,
            node_id,
            BattleFrameThreadCallbackIdentity::Unknown,
            BattleFrameThreadCallbackIdentity::Unknown);
        return {
            .status = BattleFrameThreadMutationStatus::NotFound,
            .node_id = node_id,
            .detail = "thread cursor target was not found",
        };
    }

    runtime.current_node_id = node_id;
    append_event(
        runtime,
        event_request,
        BattleFrameThreadMutationKind::SetCursor,
        BattleFrameThreadMutationStatus::Applied,
        node_id,
        node->callback,
        node->callback);
    return {
        .status = BattleFrameThreadMutationStatus::Applied,
        .node_id = node_id,
        .detail = "thread cursor published",
    };
}

BattleFrameThreadMutationResult remove_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index,
    std::optional<std::uint64_t> worker_sequence) {
    const auto result = mutate_existing(
        runtime,
        node_id,
        BattleFrameThreadMutationKind::Remove,
        std::nullopt,
        false,
        std::move(semantic_source_id),
        std::move(provenance),
        frame_index,
        worker_sequence);
    if (result.status == BattleFrameThreadMutationStatus::Applied
        && runtime.current_node_id == node_id) {
        runtime.current_node_id.reset();
    }
    return result;
}

const BattleFrameThreadNode* find_battle_frame_thread(
    const BattleFrameThreadListRuntime& runtime,
    int node_id) {
    const auto it = std::find_if(
        runtime.nodes.begin(),
        runtime.nodes.end(),
        [node_id](const BattleFrameThreadNode& node) {
            return node.node_id == node_id;
        });
    return it == runtime.nodes.end() ? nullptr : &*it;
}

BattleFrameThreadNode* find_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    int node_id) {
    const auto it = std::find_if(
        runtime.nodes.begin(),
        runtime.nodes.end(),
        [node_id](const BattleFrameThreadNode& node) {
            return node.node_id == node_id;
        });
    return it == runtime.nodes.end() ? nullptr : &*it;
}

std::vector<const BattleFrameThreadNode*> active_battle_frame_threads(
    const BattleFrameThreadListRuntime& runtime,
    BattleFrameThreadNodeKind kind) {
    std::vector<const BattleFrameThreadNode*> result;
    for (const auto& node : runtime.nodes) {
        if (node.active && node.kind == kind) {
            result.push_back(&node);
        }
    }
    return result;
}

const char* battle_frame_thread_node_kind_name(BattleFrameThreadNodeKind kind) {
    switch (kind) {
    case BattleFrameThreadNodeKind::MovementController:
        return "movement_controller";
    case BattleFrameThreadNodeKind::CombatantInstruction:
        return "combatant_instruction";
    case BattleFrameThreadNodeKind::ResourceWorker:
        return "resource_worker";
    case BattleFrameThreadNodeKind::Unknown:
    default:
        return "unknown";
    }
}

const char* battle_frame_thread_callback_identity_name(
    BattleFrameThreadCallbackIdentity callback) {
    switch (callback) {
    case BattleFrameThreadCallbackIdentity::MovementController:
        return "battle.movement_controller";
    case BattleFrameThreadCallbackIdentity::CombatantInstruction:
        return "battle.combatant_instruction";
    case BattleFrameThreadCallbackIdentity::ResourceQueue:
        return "battle.resource_queue";
    case BattleFrameThreadCallbackIdentity::Unknown:
    default:
        return "unknown";
    }
}

const char* battle_frame_thread_mutation_status_name(
    BattleFrameThreadMutationStatus status) {
    switch (status) {
    case BattleFrameThreadMutationStatus::Applied:
        return "applied";
    case BattleFrameThreadMutationStatus::MissingInput:
        return "missing_input";
    case BattleFrameThreadMutationStatus::Duplicate:
        return "duplicate";
    case BattleFrameThreadMutationStatus::NotFound:
        return "not_found";
    default:
        return "unknown";
    }
}

} // namespace savor::predict
