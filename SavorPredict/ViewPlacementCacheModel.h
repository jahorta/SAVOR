#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savor::predict {

namespace ViewPlacementCacheSemanticSource {
inline constexpr std::string_view PlacementLookup = "view_placement.lookup";
inline constexpr std::string_view PlacementFunctionPublication =
    "view_placement.publish.placement_function";
inline constexpr std::string_view RunnerPublication =
    "view_placement.publish.runner";
inline constexpr std::string_view DirectViewPublication =
    "view_placement.publish.direct_view";
inline constexpr std::string_view ActiveRecordReset =
    "view_placement.reset.active_record";
inline constexpr std::string_view WorkspaceSnapshotCopy =
    "view_placement.copy.workspace_snapshot";
} // namespace ViewPlacementCacheSemanticSource

struct ViewPlacementCacheKey {
    std::uint32_t distance_bits = 0;
    std::uint32_t center_x_bits = 0;
    std::uint32_t center_y_bits = 0;
    std::uint32_t center_z_bits = 0;
};

bool view_placement_cache_keys_equal(
    const ViewPlacementCacheKey& lhs,
    const ViewPlacementCacheKey& rhs);

std::uint32_t view_placement_angle_bits_from_rand15(std::uint16_t rand_value);

struct ViewPlacementCacheSnapshot {
    std::uint16_t control = 0;
    ViewPlacementCacheKey key{};
    std::uint32_t angle_bits = 0;
};

enum class ViewPlacementCacheKnowledge {
    Uninitialized,
    Coherent,
    Indeterminate,
};

struct ViewPlacementCacheState {
    std::uint16_t control = 0;
    ViewPlacementCacheKey key{};
    std::uint32_t angle_bits = 0;
    ViewPlacementCacheKnowledge knowledge = ViewPlacementCacheKnowledge::Uninitialized;
    std::uint64_t revision = 0;
    std::string last_source;
    std::string provenance;
};

struct ViewPlacementCacheEventContext {
    std::optional<std::uint64_t> frame_index;
    std::optional<int> worker_sequence;
};

enum class ViewPlacementCacheReadStatus {
    Hit,
    Miss,
    Unknown,
    MissingInput,
    Unsupported,
};

struct ViewPlacementCacheReadRequest {
    std::optional<ViewPlacementCacheKey> key;
    ViewPlacementCacheEventContext context{};
    std::string provenance;
};

struct ViewPlacementCacheReadDecision {
    ViewPlacementCacheReadStatus status = ViewPlacementCacheReadStatus::Unsupported;
    std::optional<std::uint32_t> angle_bits;
    std::string provenance;
};

enum class ViewPlacementCacheMutationKind {
    PublishSnapshot,
    Invalidate,
    MarkIndeterminate,
    NoChange,
};

struct ViewPlacementCacheMutationRequest {
    std::optional<ViewPlacementCacheSnapshot> snapshot;
    ViewPlacementCacheEventContext context{};
    std::string provenance;
};

struct ViewPlacementCacheMutationOperation {
    ViewPlacementCacheMutationKind kind = ViewPlacementCacheMutationKind::NoChange;
    std::optional<ViewPlacementCacheSnapshot> snapshot;
    std::uint16_t invalid_control = 0;
    std::string provenance;

    static ViewPlacementCacheMutationOperation publish_snapshot(
        ViewPlacementCacheSnapshot snapshot,
        std::string provenance = {});
    static ViewPlacementCacheMutationOperation invalidate(
        std::uint16_t invalid_control,
        std::string provenance = {});
    static ViewPlacementCacheMutationOperation mark_indeterminate(
        std::string provenance = {});
    static ViewPlacementCacheMutationOperation no_change(
        std::string provenance = {});
};

class ViewPlacementCacheHookRegistry {
public:
    using ReadHook = std::function<ViewPlacementCacheReadDecision(
        const ViewPlacementCacheState&,
        const ViewPlacementCacheReadRequest&)>;
    using MutationHook = std::function<ViewPlacementCacheMutationOperation(
        const ViewPlacementCacheState&,
        const ViewPlacementCacheMutationRequest&)>;

    bool register_read_hook(std::string source_id, ReadHook hook);
    bool register_mutation_hook(std::string source_id, MutationHook hook);

    bool has_read_hook(std::string_view source_id) const;
    bool has_mutation_hook(std::string_view source_id) const;
    const ReadHook* find_read_hook(std::string_view source_id) const;
    const MutationHook* find_mutation_hook(std::string_view source_id) const;

private:
    bool contains_source_id(std::string_view source_id) const;

    std::unordered_map<std::string, ReadHook> read_hooks_;
    std::unordered_map<std::string, MutationHook> mutation_hooks_;
};

enum class ViewPlacementCacheEventKind {
    Read,
    Mutation,
};

enum class ViewPlacementCacheMutationStatus {
    Applied,
    MarkedIndeterminate,
    NoChange,
    InvalidOperation,
};

struct ViewPlacementCacheEvent {
    ViewPlacementCacheEventKind kind = ViewPlacementCacheEventKind::Read;
    std::string source_id;
    std::optional<ViewPlacementCacheReadStatus> read_status;
    std::optional<ViewPlacementCacheMutationKind> mutation_kind;
    std::optional<ViewPlacementCacheMutationStatus> mutation_status;
    ViewPlacementCacheState state_before{};
    ViewPlacementCacheState state_after{};
    ViewPlacementCacheEventContext context{};
    std::string provenance;
};

struct ViewPlacementCacheRuntime {
    ViewPlacementCacheState state{};
    ViewPlacementCacheHookRegistry hooks{};
    std::vector<ViewPlacementCacheEvent> history;
};

struct ViewPlacementCacheReadResult {
    ViewPlacementCacheReadStatus status = ViewPlacementCacheReadStatus::Unsupported;
    std::optional<std::uint32_t> angle_bits;
    std::string provenance;
    ViewPlacementCacheEvent event{};
};

struct ViewPlacementCacheMutationResult {
    ViewPlacementCacheMutationStatus status = ViewPlacementCacheMutationStatus::InvalidOperation;
    std::uint64_t revision_before = 0;
    std::uint64_t revision_after = 0;
    ViewPlacementCacheEvent event{};
};

ViewPlacementCacheRuntime make_default_view_placement_cache_runtime();

ViewPlacementCacheReadResult read_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    std::string_view source_id,
    const ViewPlacementCacheReadRequest& request);

ViewPlacementCacheMutationResult mutate_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    std::string_view source_id,
    const ViewPlacementCacheMutationRequest& request);

ViewPlacementCacheMutationResult reset_active_record_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    ViewPlacementCacheEventContext context = {},
    std::string provenance = {});

ViewPlacementCacheMutationResult copy_view_placement_workspace_snapshot(
    ViewPlacementCacheRuntime& runtime,
    std::optional<ViewPlacementCacheSnapshot> snapshot,
    ViewPlacementCacheEventContext context = {},
    std::string provenance = {});

struct ViewPlacementRequest {
    std::string read_source_id{ViewPlacementCacheSemanticSource::PlacementLookup};
    std::string publisher_source_id{
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication};
    std::optional<ViewPlacementCacheKey> key;
    ViewPlacementCacheEventContext context{};
    std::string provenance;
};

struct ViewPlacementResolutionResult {
    ViewPlacementCacheReadStatus status = ViewPlacementCacheReadStatus::Unsupported;
    std::uint32_t seed_before = 0;
    std::uint32_t seed_after = 0;
    int draws_consumed = 0;
    std::optional<std::uint16_t> rand_value;
    std::optional<std::uint32_t> angle_bits;
    std::uint64_t revision_before = 0;
    std::uint64_t revision_after = 0;
    std::string provenance;
    std::vector<ViewPlacementCacheEvent> events;
};

ViewPlacementResolutionResult resolve_view_placement_request(
    ViewPlacementCacheRuntime& runtime,
    std::uint32_t& rng_state,
    const ViewPlacementRequest& request);

} // namespace savor::predict
