#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class BattleFrameThreadNodeKind {
    MovementController,
    CombatantInstruction,
    ResourceWorker,
    Unknown,
};

enum class BattleFrameThreadCallbackIdentity {
    MovementController,
    CombatantInstruction,
    ResourceQueue,
    Unknown,
};

enum class BattleFrameThreadInsertionKind {
    Append,
    AfterNode,
    LastChild,
    AfterCurrentCursor,
};

enum class BattleFrameThreadMutationKind {
    Create,
    ReplaceCallback,
    SetActive,
    SetCursor,
    Remove,
};

enum class BattleFrameThreadMutationStatus {
    Applied,
    MissingInput,
    Duplicate,
    NotFound,
};

struct BattleFrameThreadNode {
    int node_id = -1;
    std::uint64_t creation_sequence = 0;
    BattleFrameThreadNodeKind kind = BattleFrameThreadNodeKind::Unknown;
    int owner_slot = -1;
    BattleFrameThreadCallbackIdentity callback =
        BattleFrameThreadCallbackIdentity::Unknown;
    bool active = false;
    std::optional<int> parent_node_id;
    std::string semantic_source_id;
    std::string provenance;
};

struct BattleFrameThreadMutationEvent {
    std::uint64_t sequence = 0;
    int frame_index = -1;
    std::optional<std::uint64_t> worker_sequence;
    BattleFrameThreadMutationKind kind = BattleFrameThreadMutationKind::Create;
    BattleFrameThreadMutationStatus status =
        BattleFrameThreadMutationStatus::MissingInput;
    int node_id = -1;
    int owner_slot = -1;
    BattleFrameThreadCallbackIdentity callback_before =
        BattleFrameThreadCallbackIdentity::Unknown;
    BattleFrameThreadCallbackIdentity callback_after =
        BattleFrameThreadCallbackIdentity::Unknown;
    std::string semantic_source_id;
    std::string provenance;
};

struct BattleFrameThreadCreateRequest {
    BattleFrameThreadNodeKind kind = BattleFrameThreadNodeKind::Unknown;
    int owner_slot = -1;
    BattleFrameThreadCallbackIdentity callback =
        BattleFrameThreadCallbackIdentity::Unknown;
    bool active = true;
    BattleFrameThreadInsertionKind insertion =
        BattleFrameThreadInsertionKind::Append;
    std::optional<int> relative_node_id;
    std::optional<int> parent_node_id;
    std::string semantic_source_id;
    std::string provenance;
    int frame_index = -1;
    std::optional<std::uint64_t> worker_sequence;
};

struct BattleFrameThreadMutationResult {
    BattleFrameThreadMutationStatus status =
        BattleFrameThreadMutationStatus::MissingInput;
    int node_id = -1;
    std::string detail;
};

struct BattleFrameThreadListRuntime {
    std::vector<BattleFrameThreadNode> nodes;
    std::vector<BattleFrameThreadMutationEvent> history;
    std::optional<int> current_node_id;
    int next_node_id = 0;
    std::uint64_t next_creation_sequence = 0;
    std::uint64_t next_event_sequence = 0;
};

BattleFrameThreadMutationResult create_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    BattleFrameThreadCreateRequest request);

BattleFrameThreadMutationResult replace_battle_frame_thread_callback(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    BattleFrameThreadCallbackIdentity callback,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index = -1,
    std::optional<std::uint64_t> worker_sequence = std::nullopt);

BattleFrameThreadMutationResult set_battle_frame_thread_active(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    bool active,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index = -1,
    std::optional<std::uint64_t> worker_sequence = std::nullopt);

BattleFrameThreadMutationResult set_battle_frame_thread_cursor(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index = -1,
    std::optional<std::uint64_t> worker_sequence = std::nullopt);

BattleFrameThreadMutationResult remove_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    int node_id,
    std::string semantic_source_id,
    std::string provenance,
    int frame_index = -1,
    std::optional<std::uint64_t> worker_sequence = std::nullopt);

const BattleFrameThreadNode* find_battle_frame_thread(
    const BattleFrameThreadListRuntime& runtime,
    int node_id);

BattleFrameThreadNode* find_battle_frame_thread(
    BattleFrameThreadListRuntime& runtime,
    int node_id);

std::vector<const BattleFrameThreadNode*> active_battle_frame_threads(
    const BattleFrameThreadListRuntime& runtime,
    BattleFrameThreadNodeKind kind);

const char* battle_frame_thread_node_kind_name(BattleFrameThreadNodeKind kind);
const char* battle_frame_thread_callback_identity_name(
    BattleFrameThreadCallbackIdentity callback);
const char* battle_frame_thread_mutation_status_name(
    BattleFrameThreadMutationStatus status);

} // namespace savor::predict
