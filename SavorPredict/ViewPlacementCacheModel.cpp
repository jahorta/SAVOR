#include "ViewPlacementCacheModel.h"

#include "RngCore.h"

#include <bit>
#include <cmath>
#include <utility>

namespace savor::predict {
namespace {

bool soa_float_bits_equal(std::uint32_t lhs_bits, std::uint32_t rhs_bits) {
    const float lhs = std::bit_cast<float>(lhs_bits);
    const float rhs = std::bit_cast<float>(rhs_bits);
    return !std::isnan(lhs) && !std::isnan(rhs) && lhs == rhs;
}

ViewPlacementCacheSnapshot snapshot_from_state(const ViewPlacementCacheState& state) {
    return {
        .control = state.control,
        .key = state.key,
        .angle_bits = state.angle_bits,
    };
}

std::string combine_provenance(std::string_view first, std::string_view second) {
    if (first.empty()) {
        return std::string(second);
    }
    if (second.empty() || first == second) {
        return std::string(first);
    }
    return std::string(first) + "; " + std::string(second);
}

ViewPlacementCacheMutationResult apply_mutation_operation(
    ViewPlacementCacheRuntime& runtime,
    std::string_view source_id,
    const ViewPlacementCacheMutationRequest& request,
    const ViewPlacementCacheMutationOperation& operation) {
    ViewPlacementCacheMutationResult result;
    result.revision_before = runtime.state.revision;
    result.event.kind = ViewPlacementCacheEventKind::Mutation;
    result.event.source_id = std::string(source_id);
    result.event.mutation_kind = operation.kind;
    result.event.state_before = runtime.state;
    result.event.context = request.context;
    result.event.provenance = combine_provenance(request.provenance, operation.provenance);

    const auto commit_metadata = [&]() {
        ++runtime.state.revision;
        runtime.state.last_source = std::string(source_id);
        runtime.state.provenance = result.event.provenance;
    };

    switch (operation.kind) {
    case ViewPlacementCacheMutationKind::PublishSnapshot:
        if (!operation.snapshot.has_value()) {
            result.status = ViewPlacementCacheMutationStatus::InvalidOperation;
            break;
        }
        runtime.state.control = operation.snapshot->control;
        runtime.state.key = operation.snapshot->key;
        runtime.state.angle_bits = operation.snapshot->angle_bits;
        runtime.state.knowledge = ViewPlacementCacheKnowledge::Coherent;
        commit_metadata();
        result.status = ViewPlacementCacheMutationStatus::Applied;
        break;

    case ViewPlacementCacheMutationKind::Invalidate:
        if (operation.invalid_control == 10 || operation.invalid_control == 11) {
            result.status = ViewPlacementCacheMutationStatus::InvalidOperation;
            break;
        }
        runtime.state.control = operation.invalid_control;
        runtime.state.knowledge = ViewPlacementCacheKnowledge::Coherent;
        commit_metadata();
        result.status = ViewPlacementCacheMutationStatus::Applied;
        break;

    case ViewPlacementCacheMutationKind::MarkIndeterminate:
        runtime.state.knowledge = ViewPlacementCacheKnowledge::Indeterminate;
        commit_metadata();
        result.status = ViewPlacementCacheMutationStatus::MarkedIndeterminate;
        break;

    case ViewPlacementCacheMutationKind::NoChange:
        result.status = ViewPlacementCacheMutationStatus::NoChange;
        break;
    }

    result.revision_after = runtime.state.revision;
    result.event.mutation_status = result.status;
    result.event.state_after = runtime.state;
    runtime.history.push_back(result.event);
    return result;
}

ViewPlacementCacheReadDecision default_lookup_hook(
    const ViewPlacementCacheState& state,
    const ViewPlacementCacheReadRequest& request) {
    if (!request.key.has_value()) {
        return {
            .status = ViewPlacementCacheReadStatus::MissingInput,
            .provenance = "view-placement request key is incomplete",
        };
    }
    if (state.knowledge != ViewPlacementCacheKnowledge::Coherent) {
        return {
            .status = ViewPlacementCacheReadStatus::Unknown,
            .provenance = "view-placement cache knowledge is not coherent",
        };
    }

    const bool reusable_control = state.control == 10 || state.control == 11;
    if (reusable_control && view_placement_cache_keys_equal(state.key, *request.key)) {
        return {
            .status = ViewPlacementCacheReadStatus::Hit,
            .angle_bits = state.angle_bits,
            .provenance = "coherent cache control and key match",
        };
    }

    return {
        .status = ViewPlacementCacheReadStatus::Miss,
        .provenance = reusable_control
            ? "coherent cache key does not match"
            : "coherent cache control is not reusable",
    };
}

ViewPlacementCacheMutationOperation default_publication_hook(
    const ViewPlacementCacheState&,
    const ViewPlacementCacheMutationRequest& request) {
    if (!request.snapshot.has_value()) {
        return ViewPlacementCacheMutationOperation::no_change(
            "publication request has no snapshot");
    }
    return ViewPlacementCacheMutationOperation::publish_snapshot(
        *request.snapshot,
        request.provenance);
}

ViewPlacementCacheMutationOperation active_record_reset_hook(
    const ViewPlacementCacheState&,
    const ViewPlacementCacheMutationRequest&) {
    return ViewPlacementCacheMutationOperation::publish_snapshot(
        ViewPlacementCacheSnapshot{},
        "complete active-record workspace reset");
}

ViewPlacementCacheMutationOperation workspace_snapshot_copy_hook(
    const ViewPlacementCacheState&,
    const ViewPlacementCacheMutationRequest& request) {
    if (!request.snapshot.has_value()) {
        return ViewPlacementCacheMutationOperation::mark_indeterminate(
            "workspace copy did not provide every cache field");
    }
    return ViewPlacementCacheMutationOperation::publish_snapshot(
        *request.snapshot,
        "complete workspace snapshot copy");
}

} // namespace

bool view_placement_cache_keys_equal(
    const ViewPlacementCacheKey& lhs,
    const ViewPlacementCacheKey& rhs) {
    return soa_float_bits_equal(lhs.distance_bits, rhs.distance_bits)
        && soa_float_bits_equal(lhs.center_x_bits, rhs.center_x_bits)
        && soa_float_bits_equal(lhs.center_y_bits, rhs.center_y_bits)
        && soa_float_bits_equal(lhs.center_z_bits, rhs.center_z_bits);
}

std::uint32_t view_placement_angle_bits_from_rand15(std::uint16_t rand_value) {
    const int bucket = rand_value % 6;
    const float angle = bucket == 0
        ? 45.0f
        : static_cast<float>(bucket * 70);
    return std::bit_cast<std::uint32_t>(angle);
}

ViewPlacementCacheMutationOperation ViewPlacementCacheMutationOperation::publish_snapshot(
    ViewPlacementCacheSnapshot snapshot,
    std::string provenance) {
    return {
        .kind = ViewPlacementCacheMutationKind::PublishSnapshot,
        .snapshot = std::move(snapshot),
        .provenance = std::move(provenance),
    };
}

ViewPlacementCacheMutationOperation ViewPlacementCacheMutationOperation::invalidate(
    std::uint16_t invalid_control,
    std::string provenance) {
    return {
        .kind = ViewPlacementCacheMutationKind::Invalidate,
        .invalid_control = invalid_control,
        .provenance = std::move(provenance),
    };
}

ViewPlacementCacheMutationOperation ViewPlacementCacheMutationOperation::mark_indeterminate(
    std::string provenance) {
    return {
        .kind = ViewPlacementCacheMutationKind::MarkIndeterminate,
        .provenance = std::move(provenance),
    };
}

ViewPlacementCacheMutationOperation ViewPlacementCacheMutationOperation::no_change(
    std::string provenance) {
    return {
        .kind = ViewPlacementCacheMutationKind::NoChange,
        .provenance = std::move(provenance),
    };
}

bool ViewPlacementCacheHookRegistry::register_read_hook(
    std::string source_id,
    ReadHook hook) {
    if (source_id.empty() || !hook || contains_source_id(source_id)) {
        return false;
    }
    read_hooks_.emplace(std::move(source_id), std::move(hook));
    return true;
}

bool ViewPlacementCacheHookRegistry::register_mutation_hook(
    std::string source_id,
    MutationHook hook) {
    if (source_id.empty() || !hook || contains_source_id(source_id)) {
        return false;
    }
    mutation_hooks_.emplace(std::move(source_id), std::move(hook));
    return true;
}

bool ViewPlacementCacheHookRegistry::has_read_hook(std::string_view source_id) const {
    return find_read_hook(source_id) != nullptr;
}

bool ViewPlacementCacheHookRegistry::has_mutation_hook(std::string_view source_id) const {
    return find_mutation_hook(source_id) != nullptr;
}

const ViewPlacementCacheHookRegistry::ReadHook*
ViewPlacementCacheHookRegistry::find_read_hook(std::string_view source_id) const {
    const auto found = read_hooks_.find(std::string(source_id));
    return found == read_hooks_.end() ? nullptr : &found->second;
}

const ViewPlacementCacheHookRegistry::MutationHook*
ViewPlacementCacheHookRegistry::find_mutation_hook(std::string_view source_id) const {
    const auto found = mutation_hooks_.find(std::string(source_id));
    return found == mutation_hooks_.end() ? nullptr : &found->second;
}

bool ViewPlacementCacheHookRegistry::contains_source_id(std::string_view source_id) const {
    return find_read_hook(source_id) != nullptr || find_mutation_hook(source_id) != nullptr;
}

ViewPlacementCacheRuntime make_default_view_placement_cache_runtime() {
    ViewPlacementCacheRuntime runtime;
    runtime.hooks.register_read_hook(
        std::string(ViewPlacementCacheSemanticSource::PlacementLookup),
        default_lookup_hook);
    runtime.hooks.register_mutation_hook(
        std::string(ViewPlacementCacheSemanticSource::PlacementFunctionPublication),
        default_publication_hook);
    runtime.hooks.register_mutation_hook(
        std::string(ViewPlacementCacheSemanticSource::RunnerPublication),
        default_publication_hook);
    runtime.hooks.register_mutation_hook(
        std::string(ViewPlacementCacheSemanticSource::DirectViewPublication),
        default_publication_hook);
    runtime.hooks.register_mutation_hook(
        std::string(ViewPlacementCacheSemanticSource::ActiveRecordReset),
        active_record_reset_hook);
    runtime.hooks.register_mutation_hook(
        std::string(ViewPlacementCacheSemanticSource::WorkspaceSnapshotCopy),
        workspace_snapshot_copy_hook);
    return runtime;
}

ViewPlacementCacheReadResult read_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    std::string_view source_id,
    const ViewPlacementCacheReadRequest& request) {
    ViewPlacementCacheReadDecision decision;
    const auto* hook = runtime.hooks.find_read_hook(source_id);
    if (hook == nullptr) {
        decision.status = ViewPlacementCacheReadStatus::Unsupported;
        decision.provenance = "unsupported cache reader '" + std::string(source_id) + "'";
    } else {
        decision = (*hook)(runtime.state, request);
        if (decision.status == ViewPlacementCacheReadStatus::Hit
            && !decision.angle_bits.has_value()) {
            decision.status = ViewPlacementCacheReadStatus::Unsupported;
            decision.provenance = combine_provenance(
                decision.provenance,
                "reader returned a hit without an angle");
        }
    }

    ViewPlacementCacheEvent event;
    event.kind = ViewPlacementCacheEventKind::Read;
    event.source_id = std::string(source_id);
    event.read_status = decision.status;
    event.state_before = runtime.state;
    event.state_after = runtime.state;
    event.context = request.context;
    event.provenance = combine_provenance(request.provenance, decision.provenance);
    runtime.history.push_back(event);

    return {
        .status = decision.status,
        .angle_bits = decision.angle_bits,
        .provenance = event.provenance,
        .event = std::move(event),
    };
}

ViewPlacementCacheMutationResult mutate_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    std::string_view source_id,
    const ViewPlacementCacheMutationRequest& request) {
    const auto* hook = runtime.hooks.find_mutation_hook(source_id);
    if (hook == nullptr) {
        const auto provenance = combine_provenance(
            "unknown mutation source '" + std::string(source_id) + "'",
            request.provenance);
        ViewPlacementCacheMutationRequest unknown_request = request;
        unknown_request.provenance.clear();
        return apply_mutation_operation(
            runtime,
            source_id,
            unknown_request,
            ViewPlacementCacheMutationOperation::mark_indeterminate(provenance));
    }

    const auto operation = (*hook)(runtime.state, request);
    return apply_mutation_operation(runtime, source_id, request, operation);
}

ViewPlacementCacheMutationResult reset_active_record_view_placement_cache(
    ViewPlacementCacheRuntime& runtime,
    ViewPlacementCacheEventContext context,
    std::string provenance) {
    return mutate_view_placement_cache(
        runtime,
        ViewPlacementCacheSemanticSource::ActiveRecordReset,
        ViewPlacementCacheMutationRequest{
            .context = std::move(context),
            .provenance = std::move(provenance),
        });
}

ViewPlacementCacheMutationResult copy_view_placement_workspace_snapshot(
    ViewPlacementCacheRuntime& runtime,
    std::optional<ViewPlacementCacheSnapshot> snapshot,
    ViewPlacementCacheEventContext context,
    std::string provenance) {
    return mutate_view_placement_cache(
        runtime,
        ViewPlacementCacheSemanticSource::WorkspaceSnapshotCopy,
        ViewPlacementCacheMutationRequest{
            .snapshot = std::move(snapshot),
            .context = std::move(context),
            .provenance = std::move(provenance),
        });
}

ViewPlacementResolutionResult resolve_view_placement_request(
    ViewPlacementCacheRuntime& runtime,
    std::uint32_t& rng_state,
    const ViewPlacementRequest& request) {
    ViewPlacementResolutionResult result;
    result.seed_before = rng_state;
    result.seed_after = rng_state;
    result.revision_before = runtime.state.revision;
    result.revision_after = runtime.state.revision;
    const std::size_t history_begin = runtime.history.size();

    const auto finish = [&]() {
        result.seed_after = rng_state;
        result.revision_after = runtime.state.revision;
        result.events.assign(
            runtime.history.begin() + history_begin,
            runtime.history.end());
    };

    if (!runtime.hooks.has_read_hook(request.read_source_id)) {
        result.status = ViewPlacementCacheReadStatus::Unsupported;
        result.provenance = "unsupported cache reader '" + request.read_source_id + "'";
        finish();
        return result;
    }
    const auto* publisher_hook = runtime.hooks.find_mutation_hook(request.publisher_source_id);
    if (publisher_hook == nullptr) {
        result.status = ViewPlacementCacheReadStatus::Unsupported;
        result.provenance = "unsupported cache publisher '" + request.publisher_source_id + "'";
        finish();
        return result;
    }

    const auto read = read_view_placement_cache(
        runtime,
        request.read_source_id,
        ViewPlacementCacheReadRequest{
            .key = request.key,
            .context = request.context,
            .provenance = request.provenance,
        });
    result.status = read.status;
    result.angle_bits = read.angle_bits;
    result.provenance = read.provenance;
    if (read.status != ViewPlacementCacheReadStatus::Hit
        && read.status != ViewPlacementCacheReadStatus::Miss) {
        finish();
        return result;
    }

    ViewPlacementCacheSnapshot desired_snapshot;
    std::optional<RngDraw> pending_draw;
    if (read.status == ViewPlacementCacheReadStatus::Hit) {
        desired_snapshot = snapshot_from_state(runtime.state);
    } else {
        pending_draw = draw_rand15(rng_state);
        desired_snapshot = {
            .control = 11,
            .key = *request.key,
            .angle_bits = view_placement_angle_bits_from_rand15(pending_draw->value),
        };
    }

    const ViewPlacementCacheMutationRequest publication_request{
        .snapshot = desired_snapshot,
        .context = request.context,
        .provenance = request.provenance,
    };
    const auto publication = (*publisher_hook)(runtime.state, publication_request);
    if (publication.kind != ViewPlacementCacheMutationKind::PublishSnapshot
        || !publication.snapshot.has_value()) {
        result.status = ViewPlacementCacheReadStatus::Unsupported;
        result.angle_bits.reset();
        result.provenance = combine_provenance(
            result.provenance,
            "selected cache publisher did not produce a snapshot");
        finish();
        return result;
    }

    const auto mutation = apply_mutation_operation(
        runtime,
        request.publisher_source_id,
        publication_request,
        publication);
    if (mutation.status != ViewPlacementCacheMutationStatus::Applied) {
        result.status = ViewPlacementCacheReadStatus::Unsupported;
        result.angle_bits.reset();
        result.provenance = combine_provenance(
            result.provenance,
            "selected cache publication was not applied");
        finish();
        return result;
    }

    result.angle_bits = publication.snapshot->angle_bits;
    result.provenance = combine_provenance(
        result.provenance,
        mutation.event.provenance);
    if (pending_draw.has_value()) {
        rng_state = pending_draw->next_state;
        result.draws_consumed = 1;
        result.rand_value = pending_draw->value;
    }

    finish();
    return result;
}

} // namespace savor::predict
