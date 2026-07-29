#include "DBWorkflowWorkerCoordinator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Utils/Hash.h"
#include "Utils/ModulePath.h"

namespace savor::runner::parallel::savordb {
namespace {
constexpr std::size_t kMaxCoordinatorWarnings = 32;
std::atomic<std::uint64_t> g_worker_process_generation{0};

std::uint64_t NextWorkerProcessGeneration() noexcept {
    auto generation =
        g_worker_process_generation.fetch_add(
            1,
            std::memory_order_relaxed)
        + 1;
    if (generation == 0) {
        generation =
            g_worker_process_generation.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;
    }
    return generation;
}

std::int64_t NowMonoNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string WorksetItemMapKey(
    std::size_t worker_idx,
    std::uint64_t workset_id,
    std::uint64_t item_id) {
    return std::to_string(worker_idx) + ":"
        + std::to_string(workset_id) + ":" + std::to_string(item_id);
}

struct ProgressDedupState {
    bool has_value = false;
    std::uint64_t job_id = 0;
    std::string text;
    std::size_t duplicate_count = 0;
    bool warning_written = false;
};

struct CapabilityPreflightEvaluation {
    bool ready = false;
    bool non_retryable = false;
    bool workset_dispatch_missing = false;
    bool workset_catalog_missing = false;
    bool interactive_visual_debug_missing = false;
    std::string error;
};

bool HasUsableWorksetLimits(
    const savor::runtime::WorkerWorksetLimits& limits) {
    constexpr std::size_t kWrmsMaximumPayloadBytes =
        64ull * 1024ull * 1024ull;
    return limits.maximum_items_per_workset > 0
        && limits.maximum_encoded_workset_bytes > 0
        && limits.maximum_encoded_workset_bytes
            <= kWrmsMaximumPayloadBytes
        && limits.maximum_item_credits > 0
        && limits.maximum_active_and_staged_items > 0
        && limits.maximum_active_and_staged_items
            <= limits.maximum_item_credits
        && limits.maximum_state_cache_entries > 0
        && limits.maximum_state_cache_bytes > 0
        && limits.finalizer_threads > 0
        && limits.maximum_pending_finalizers > 0
        && limits.maximum_pending_finalizer_bytes > 0
        && limits.maximum_retained_terminals > 0
        && limits.maximum_retained_terminal_bytes > 0
        && limits.progressive_start_concurrency > 0;
}

bool HasExactProductionCatalogShape(
    const savor::runtime::WorkerRuntimeManifest& manifest,
    std::string* error_out) {
    static constexpr std::array<
        std::pair<std::string_view, std::string_view>,
        9>
        kProductionModules{{
            {"soa.seed_probe", "probe"},
            {"soa.navigation.context", "capture"},
            {"soa.tas_movie", "play_and_checkpoint"},
            {"soa.tas_frame_detector", "detect"},
            {"soa.battle.context", "capture"},
            {"soa.battle.macro_probe", "probe"},
            {"soa.battle.single_turn", "execute"},
            {"soa.battle.completion", "complete"},
            {"soa.battle.results_screen", "advance"},
        }};
    if (manifest.modules.size() != kProductionModules.size()) {
        if (error_out != nullptr) {
            *error_out = "CompleteExact catalog must contain exactly nine "
                "production modules";
        }
        return false;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(kProductionModules.size());
    for (const auto& module : manifest.modules) {
        if (module.development_only
            || module.module.canonical_id.empty()
            || !seen.emplace(module.module.canonical_id).second) {
            if (error_out != nullptr) {
                *error_out = "CompleteExact catalog contains a development, "
                    "unnamed, or duplicate module";
            }
            return false;
        }
        const auto expected = std::find_if(
            kProductionModules.begin(),
            kProductionModules.end(),
            [&](const auto& candidate) {
                return candidate.first == module.module.canonical_id;
            });
        if (expected == kProductionModules.end()
            || module.entrypoints.size() != 1
            || std::find(
                module.entrypoints.begin(),
                module.entrypoints.end(),
                std::string(expected->second))
                == module.entrypoints.end()) {
            if (error_out != nullptr) {
                *error_out = "CompleteExact catalog module or entrypoint "
                    "does not match the production nine-module catalog";
            }
            return false;
        }
    }
    return true;
}

CapabilityPreflightEvaluation EvaluateCapabilityPreflight(
    const CoordinatorWorkerCapabilityPreflightResult& preflight,
    bool require_interactive_visual_debug,
    std::string_view expected_catalog_sha256,
    std::string_view expected_runtime_profile_sha256,
    std::string_view expected_dependency_manifest_sha256) {
    CapabilityPreflightEvaluation evaluation;
    if (!preflight.process_ready) {
        evaluation.error = preflight.error.empty()
            ? "worker was not process-ready"
            : preflight.error;
        return evaluation;
    }

    if (!savor::runtime::HasCapability(
            preflight.capabilities,
            savor::runtime::WorkerCapability::WorksetDispatch)) {
        evaluation.non_retryable = true;
        evaluation.workset_dispatch_missing = true;
        evaluation.error = "worker does not advertise WorksetDispatch";
        if (!preflight.error.empty()) {
            evaluation.error += ": " + preflight.error;
        }
        return evaluation;
    }

    std::string catalog_shape_error;
    if (!preflight.runtime_manifest.has_value()
        || preflight.runtime_manifest->catalog_status
            != savor::runtime::RuntimeCatalogStatus::CompleteExact
        || !HasExactProductionCatalogShape(
            *preflight.runtime_manifest,
            &catalog_shape_error)
        || expected_catalog_sha256.empty()
        || preflight.runtime_manifest->catalog_sha256
            != expected_catalog_sha256) {
        evaluation.non_retryable = true;
        evaluation.workset_catalog_missing = true;
        std::ostringstream error;
        error << "worker does not advertise the required CompleteExact catalog";
        if (expected_catalog_sha256.empty()) {
            error << " (coordinator expected catalog hash is not configured)";
        } else if (!catalog_shape_error.empty()) {
            error << " (" << catalog_shape_error << ")";
        } else if (preflight.runtime_manifest.has_value()) {
            error << " expected=" << expected_catalog_sha256
                  << " actual="
                  << preflight.runtime_manifest->catalog_sha256;
        }
        evaluation.error = error.str();
        return evaluation;
    }

    if (expected_runtime_profile_sha256.empty()
        || preflight.runtime_manifest->runtime_profile_sha256
            != expected_runtime_profile_sha256
        || expected_dependency_manifest_sha256.empty()
        || preflight.runtime_manifest->dependency_manifest_sha256
            != expected_dependency_manifest_sha256) {
        evaluation.non_retryable = true;
        evaluation.workset_catalog_missing = true;
        evaluation.error =
            "worker runtime profile or dependency manifest does not match "
            "the coordinator requirement";
        return evaluation;
    }

    if (!HasUsableWorksetLimits(preflight.runtime_manifest->limits)) {
        evaluation.non_retryable = true;
        evaluation.workset_catalog_missing = true;
        evaluation.error =
            "worker advertises invalid or unusable workset limits";
        return evaluation;
    }

    if (require_interactive_visual_debug
        && !savor::runtime::HasCapability(
            preflight.capabilities,
            savor::runtime::WorkerCapability::InteractiveVisualDebug)) {
        evaluation.non_retryable = true;
        evaluation.interactive_visual_debug_missing = true;
        evaluation.error = preflight.error.empty()
            ? "interactive visual debugging is unavailable until Dependency Slice 3"
            : preflight.error;
        return evaluation;
    }

    if (!preflight.error.empty()) {
        evaluation.error = preflight.error;
        return evaluation;
    }

    evaluation.ready = true;
    return evaluation;
}

std::string FormatWorkerStopSnapshot(const savor::ProcessWorkerStopSnapshot& snapshot) {
    std::ostringstream detail;
    detail << " already_stopping=" << (snapshot.already_stopping ? "true" : "false")
           << " was_running=" << (snapshot.was_running ? "true" : "false")
           << " stdin_close=" << (snapshot.stdin_close_attempted ? (snapshot.stdin_close_succeeded ? "ok" : "failed") : "skipped");
    if (snapshot.stdin_close_error != 0) {
        detail << " stdin_error=" << snapshot.stdin_close_error;
    }
    detail << " terminate=" << (snapshot.termination_attempted ? (snapshot.termination_succeeded ? "ok" : "failed") : "skipped");
    if (!snapshot.termination_method.empty()) {
        detail << " terminate_method=" << snapshot.termination_method;
    }
    if (snapshot.termination_error != 0) {
        detail << " terminate_error=" << snapshot.termination_error;
    }
    detail << " cancel_pipe=" << (snapshot.cancel_pipe_attempted ? (snapshot.cancel_pipe_succeeded ? "ok" : "failed") : "skipped");
    if (snapshot.cancel_pipe_error != 0) {
        detail << " cancel_pipe_error=" << snapshot.cancel_pipe_error;
    }
    detail << " cancel_reader=" << (snapshot.cancel_reader_attempted ? (snapshot.cancel_reader_succeeded ? "ok" : "failed") : "skipped");
    if (snapshot.cancel_reader_error != 0) {
        detail << " cancel_reader_error=" << snapshot.cancel_reader_error;
    }
    detail << " reader_joined=" << (snapshot.reader_joined ? "true" : "false")
           << " process_wait=" << snapshot.process_wait_result;
    if (snapshot.process_wait_error != 0) {
        detail << " process_wait_error=" << snapshot.process_wait_error;
    }
    detail << " process_exit=" << snapshot.process_exit_code;
    return detail.str();
}

WorkflowSchedulerAdapter::ScheduleFn ResolveWorkflowScheduleFn(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn) {
    if (workflow_schedule_fn) {
        return workflow_schedule_fn;
    }
    auto adapter_chain_orchestrator = std::make_shared<savor::db::execution::workflow::AdapterChainOrchestrator>(
        program_kind_registry,
        nullptr);
    return [execution_db, program_kind_registry, adapter_chain_orchestrator](const WorkflowReadyStep& step) -> ScheduledJobSet {
        if (execution_db == nullptr || program_kind_registry == nullptr) {
            return {};
        }
        const auto* descriptor = program_kind_registry->FindForStepKind(step.step_kind);
        if (descriptor == nullptr || adapter_chain_orchestrator == nullptr) {
            return {};
        }
        std::optional<savor::db::execution::programdb::WorkflowStepScheduleResult> persisted;
        if (step.input_ref_id.has_value() && descriptor->job_persistence != nullptr && step.step_kind != "battle_chain") {
            persisted = adapter_chain_orchestrator->OnInputComplete(
                step.step_kind,
                savor::db::execution::programdb::WorkflowStepScheduleContext{
                    .workflow_instance_id = step.workflow_instance_id,
                    .workflow_step_id = step.workflow_step_id,
                    .step_key = step.step_key,
                    .step_kind = step.step_kind,
                    .domain_ref_id = *step.input_ref_id,
                    .step_priority = step.priority,
                });
        } else if (descriptor->graph_job_persistence != nullptr && execution_db->WorkflowQueryService() != nullptr) {
            const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(step.workflow_instance_id);
            if (!graph.has_value()) {
                return {};
            }

            savor::db::execution::programdb::WorkflowGraphStepScheduleContext context{};
            context.workflow_instance_id = step.workflow_instance_id;
            context.workflow_step_id = step.workflow_step_id;
            context.workflow_graph_revision_id = graph->instance.workflow_graph_revision_id;
            context.step_key = step.step_key;
            context.step_kind = step.step_kind;
            context.step_priority = step.priority;
            const auto graph_step = std::find_if(
                graph->steps.begin(),
                graph->steps.end(),
                [&](const auto& candidate) {
                    return candidate.workflow_step_id
                        == step.workflow_step_id;
                });
            if (graph_step != graph->steps.end()
                && graph_step->workflow_unit_activation_id.has_value()) {
                context.workflow_unit_activation_id =
                    graph_step->workflow_unit_activation_id;
            }

            const savor::db::execution::workflow::
                WorkflowUnitActivationRecord* owning_activation = nullptr;
            if (context.workflow_unit_activation_id.has_value()) {
                const auto activation = std::find_if(
                    graph->unit_activations.begin(),
                    graph->unit_activations.end(),
                    [&](const auto& candidate) {
                        return candidate.workflow_unit_activation_id
                            == *context.workflow_unit_activation_id;
                    });
                if (activation != graph->unit_activations.end()) {
                    owning_activation = &*activation;
                }
            }
            if (owning_activation == nullptr) {
                const auto activation = std::find_if(
                    graph->unit_activations.begin(),
                    graph->unit_activations.end(),
                    [&](const auto& candidate) {
                        return candidate.graph_node_key == step.step_key
                            || candidate.activation_key == step.step_key;
                    });
                if (activation != graph->unit_activations.end()) {
                    owning_activation = &*activation;
                    context.workflow_unit_activation_id =
                        activation->workflow_unit_activation_id;
                }
            }
            if (owning_activation != nullptr) {
                context.activation_key = owning_activation->activation_key;
                context.activation_graph_node_key =
                    owning_activation->graph_node_key;
                context.unit_kind = owning_activation->unit_kind;
                context.activation_params_json =
                    owning_activation->activation_params_json;
                const auto units =
                    savor::db::execution::workflow::
                        BuildDefaultWorkflowUnitRegistry();
                const auto* unit = units.Find(owning_activation->unit_kind);
                if (unit != nullptr) {
                    context.unit_variant = unit->unit_variant;
                    context.breakpoint_profile_key =
                        unit->breakpoint_profile_key;
                }
            }
            const auto expected_node_key =
                context.activation_graph_node_key.empty()
                ? step.step_key
                : context.activation_graph_node_key;
            if (step.input_ref_id.has_value() && *step.input_ref_id > 0) {
                if (step.step_kind == "seed_probe_chain" && step.input_ref_kind == "state.savestate") {
                    context.input_bindings.push_back(
                        savor::db::execution::programdb::WorkflowGraphInputBinding{
                            .node_key = expected_node_key,
                            .input_key = "entry_savestate",
                            .data_kind = "state.savestate_id",
                            .ref_kind = *step.input_ref_kind,
                            .ref_id = *step.input_ref_id,
                            .source_kind = "upstream",
                        });
                } else if (step.step_kind == "battle_chain" && step.input_ref_kind == "sp_probe_run") {
                    context.input_bindings.push_back(
                        savor::db::execution::programdb::WorkflowGraphInputBinding{
                            .node_key = expected_node_key,
                            .input_key = "initial_input_frames",
                            .data_kind = "analysis.input_frame_set_id",
                            .ref_kind = *step.input_ref_kind,
                            .ref_id = *step.input_ref_id,
                            .source_kind = "upstream",
                        });
                }
            }
            for (const auto& binding : graph->input_bindings) {
                if (binding.node_key != expected_node_key) {
                    continue;
                }
                context.input_bindings.push_back(
                    savor::db::execution::programdb::WorkflowGraphInputBinding{
                        .node_key = binding.node_key,
                        .input_key = binding.input_key,
                        .data_kind = binding.data_kind,
                        .ref_kind = binding.ref_kind,
                        .ref_id = binding.ref_id,
                        .source_kind = binding.source_kind,
                    });
            }
            for (const auto& argument : graph->arguments) {
                if (!argument.node_key.empty()
                    && argument.node_key != expected_node_key) {
                    continue;
                }
                context.arguments.push_back(
                    savor::db::execution::programdb::WorkflowGraphArgument{
                        .node_key = argument.node_key,
                        .argument_key = argument.argument_key,
                        .value_type = argument.value_type,
                        .integer_value = argument.integer_value,
                        .text_value = argument.text_value,
                        .source_kind = argument.source_kind,
                    });
            }
            persisted = adapter_chain_orchestrator->OnGraphInputComplete(step.step_kind, context);
        }
        if (!persisted.has_value() || persisted->root_job_set_id <= 0) {
            return {};
        }
        return ScheduledJobSet{
            .job_set_id = persisted->root_job_set_id,
            .workflow_step_id = step.workflow_step_id,
            .program_ref_kind = persisted->persistence.program_ref_kind,
            .program_ref_id = persisted->persistence.program_ref_id,
            .event_lines = persisted->event_lines,
        };
    };
}

bool IsNoWorkWorkflowStep(const WorkflowReadyStep& step) {
    return step.step_kind == "seedprobe.done";
}

std::filesystem::path WeaklyCanonicalOrAbsolute(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    auto absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute;
}

std::string FileStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return "missing";
    }
    const auto write_time = std::filesystem::last_write_time(path, ec);
    const auto ticks = ec ? 0 : write_time.time_since_epoch().count();
    return std::to_string(size) + ":" + std::to_string(ticks);
}

bool CopyTree(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        if (error_out) *error_out = "create runtime directory failed: " + ec.message();
        return false;
    }

    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; ++it) {
        const auto& from = it->path();
        const auto rel = fs::relative(from, src, ec);
        if (ec) {
            break;
        }
        const auto to = dst / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(to, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(to.parent_path(), ec);
            if (!ec) {
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            }
        }
        if (ec) {
            break;
        }
    }

    if (ec) {
        if (error_out) *error_out = "copy runtime tree failed: " + ec.message();
        return false;
    }
    return true;
}

bool ReadFileToString(const std::filesystem::path& path, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *out = buffer.str();
    return true;
}

bool WriteStringToFile(const std::filesystem::path& path, const std::string& text, std::string* error_out) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "open runtime manifest failed: " + path.string();
        return false;
    }
    out << text;
    out.flush();
    if (!out) {
        if (error_out) *error_out = "write runtime manifest failed: " + path.string();
        return false;
    }
    out.close();
    if (!out) {
        if (error_out) *error_out = "close runtime manifest failed: " + path.string();
        return false;
    }
    return true;
}

std::optional<std::string> DirectoryTreeStamp(
    const std::filesystem::path& root,
    std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> entries;
    fs::recursive_directory_iterator it(root, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code item_ec;
        if (it->is_directory(item_ec)) {
            continue;
        }
        if (item_ec || !it->is_regular_file(item_ec) || item_ec) {
            if (error_out) {
                *error_out = "unsupported or unreadable runtime source entry: " + it->path().string();
            }
            return std::nullopt;
        }

        const auto size = it->file_size(item_ec);
        if (item_ec) {
            if (error_out) *error_out = "read runtime source file size failed: " + item_ec.message();
            return std::nullopt;
        }
        const auto write_time = it->last_write_time(item_ec);
        if (item_ec) {
            if (error_out) *error_out = "read runtime source timestamp failed: " + item_ec.message();
            return std::nullopt;
        }

        std::ostringstream entry;
        entry << it->path().lexically_relative(root).generic_string()
              << "|" << size
              << "|" << write_time.time_since_epoch().count();
        entries.push_back(entry.str());
    }
    if (ec) {
        if (error_out) *error_out = "enumerate runtime source tree failed: " + ec.message();
        return std::nullopt;
    }

    std::sort(entries.begin(), entries.end());
    std::ostringstream input;
    input << "file_count=" << entries.size() << "\n";
    for (const auto& entry : entries) {
        input << entry << "\n";
    }
    const auto text = input.str();
    const auto digest = ::hash::sha256(text.data(), text.size());
    if (digest.empty()) {
        if (error_out) *error_out = "hash runtime source tree failed";
        return std::nullopt;
    }
    return digest;
}

struct WorkerRuntimeSourceSnapshot {
    std::filesystem::path worker_exe;
    std::filesystem::path dolphin_base;
    std::filesystem::path sys;
    std::filesystem::path portable;
    std::string worker_exe_stamp;
    std::string sys_tree_stamp;
    std::string portable_stamp;
    std::string fingerprint;
};

std::optional<WorkerRuntimeSourceSnapshot> InspectWorkerRuntimeSource(
    const DBWorkflowWorkerCoordinatorConfig& worker_cfg,
    std::string* error_out) {
    namespace fs = std::filesystem;
    WorkerRuntimeSourceSnapshot source{};
    source.worker_exe = WeaklyCanonicalOrAbsolute(worker_cfg.worker_exe_path);
    source.dolphin_base = WeaklyCanonicalOrAbsolute(worker_cfg.dolphin_base_dir);
    source.sys = source.dolphin_base / "Sys";
    source.portable = source.dolphin_base / "portable.txt";
    const auto dsp_coef = source.sys / "GC" / "dsp_coef.bin";

    std::error_code ec;
    if (!fs::is_regular_file(source.worker_exe, ec) || ec) {
        if (error_out) *error_out = "source SavorWorker.exe missing: " + source.worker_exe.string();
        return std::nullopt;
    }
    ec.clear();
    if (!fs::is_directory(source.sys, ec) || ec) {
        if (error_out) *error_out = "Dolphin base Sys is missing: " + source.sys.string();
        return std::nullopt;
    }
    ec.clear();
    if (!fs::is_regular_file(dsp_coef, ec) || ec) {
        if (error_out) *error_out = "Dolphin base Sys is incomplete: " + source.sys.string();
        return std::nullopt;
    }
    ec.clear();
    if (!fs::is_regular_file(source.portable, ec) || ec) {
        if (error_out) *error_out = "Dolphin base portable.txt is missing: " + source.dolphin_base.string();
        return std::nullopt;
    }

    source.worker_exe_stamp = FileStamp(source.worker_exe);
    source.portable_stamp = FileStamp(source.portable);
    auto sys_tree_stamp = DirectoryTreeStamp(source.sys, error_out);
    if (!sys_tree_stamp.has_value()) {
        return std::nullopt;
    }
    source.sys_tree_stamp = std::move(*sys_tree_stamp);

    std::ostringstream input;
    input << "savor_worker_runtime_layout_version=3\n";
    input << "worker_exe=" << source.worker_exe.string() << "\n";
    input << "worker_exe_stamp=" << source.worker_exe_stamp << "\n";
    input << "dolphin_base=" << source.dolphin_base.string() << "\n";
    input << "sys_tree_stamp=" << source.sys_tree_stamp << "\n";
    input << "portable_stamp=" << source.portable_stamp << "\n";
    const auto text = input.str();
    const auto digest = ::hash::sha256(text.data(), text.size());
    if (digest.empty()) {
        if (error_out) *error_out = "hash worker runtime fingerprint failed";
        return std::nullopt;
    }
    source.fingerprint = digest;
    return source;
}

std::string ExpectedWorkerRuntimeManifest(const WorkerRuntimeSourceSnapshot& source) {
    std::ostringstream manifest;
    manifest << "savor_worker_runtime_manifest_version=3\n";
    manifest << "layout=shared_immutable\n";
    manifest << "fingerprint=" << source.fingerprint << "\n";
    manifest << "source_worker_exe=" << source.worker_exe.string() << "\n";
    manifest << "source_worker_exe_stamp=" << source.worker_exe_stamp << "\n";
    manifest << "dolphin_base_dir=" << source.dolphin_base.string() << "\n";
    manifest << "sys_tree_stamp=" << source.sys_tree_stamp << "\n";
    manifest << "portable_stamp=" << source.portable_stamp << "\n";
    manifest << "exe_materialization=copy\n";
    manifest << "user_template=empty\n";
    return manifest.str();
}

bool ValidateWorkerRuntimeImage(
    const std::filesystem::path& runtime_root,
    const WorkerRuntimeSourceSnapshot& source,
    std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto worker_exe = runtime_root / "SavorWorker.exe";
    const auto runtime_sys = runtime_root / "Sys";
    const auto runtime_user = runtime_root / "User";
    const auto runtime_portable = runtime_root / "portable.txt";
    const auto manifest_path = runtime_root / "worker-runtime.manifest";

    std::string manifest;
    if (!ReadFileToString(manifest_path, &manifest)
        || manifest != ExpectedWorkerRuntimeManifest(source)) {
        if (error_out) *error_out = "runtime manifest is missing or does not match its source";
        return false;
    }
    if (!fs::is_regular_file(worker_exe, ec) || ec) {
        if (error_out) *error_out = "runtime worker executable is missing";
        return false;
    }
    ec.clear();
    if (!fs::is_regular_file(runtime_sys / "GC" / "dsp_coef.bin", ec) || ec) {
        if (error_out) *error_out = "runtime Sys is missing or incomplete";
        return false;
    }
    ec.clear();
    if (!fs::is_regular_file(runtime_portable, ec) || ec) {
        if (error_out) *error_out = "runtime portable.txt is missing";
        return false;
    }
    ec.clear();
    if (!fs::is_directory(runtime_user, ec) || ec) {
        if (error_out) *error_out = "runtime User template is missing";
        return false;
    }
    ec.clear();
    if (!fs::is_empty(runtime_user, ec) || ec) {
        if (error_out) *error_out = "runtime User template is not empty";
        return false;
    }
    return true;
}

class RuntimeMaterializationLock {
public:
    ~RuntimeMaterializationLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    RuntimeMaterializationLock(const RuntimeMaterializationLock&) = delete;
    RuntimeMaterializationLock& operator=(const RuntimeMaterializationLock&) = delete;
    RuntimeMaterializationLock() = default;

    bool Acquire(const std::filesystem::path& path, std::string* error_out) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while (std::chrono::steady_clock::now() < deadline) {
            handle_ = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }

            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
                if (error_out) {
                    *error_out = "acquire runtime materialization lock failed: "
                        + std::to_string(error);
                }
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (error_out) *error_out = "timed out waiting for runtime materialization lock";
        return false;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool EnsureWorkflowWorkerRuntimeSlot(
    size_t worker_idx,
    const DBWorkflowWorkerCoordinatorConfig& worker_cfg,
    std::filesystem::path* runtime_worker_exe_out,
    std::string* error_out) {
    namespace fs = std::filesystem;
    (void)worker_idx;
    if (error_out) {
        error_out->clear();
    }
    auto source = InspectWorkerRuntimeSource(worker_cfg, error_out);
    if (!source.has_value()) {
        return false;
    }

    const fs::path configured_cache_root = worker_cfg.worker_binary_runtime_root.empty()
        ? utils::getExecutablePath() / ".worker-runtime"
        : fs::path(worker_cfg.worker_binary_runtime_root);
    const fs::path runtime_cache_root = WeaklyCanonicalOrAbsolute(configured_cache_root);
    const fs::path fingerprint_root = runtime_cache_root / source->fingerprint;
    const fs::path runtime_root = fingerprint_root / "runtime";
    const fs::path runtime_worker_exe = runtime_root / "SavorWorker.exe";
    const fs::path staging_root = fingerprint_root / "runtime.pending";
    const fs::path lock_path = runtime_cache_root / (source->fingerprint + ".lock");

    std::string validation_error;
    std::error_code ec;
    if (fs::exists(runtime_root, ec) && !ec
        && ValidateWorkerRuntimeImage(runtime_root, *source, &validation_error)) {
        if (runtime_worker_exe_out) *runtime_worker_exe_out = runtime_worker_exe;
        return true;
    }
    if (ec) {
        if (error_out) *error_out = "inspect shared worker runtime failed: " + ec.message();
        return false;
    }

    fs::create_directories(runtime_cache_root, ec);
    if (ec) {
        if (error_out) *error_out = "create worker runtime cache failed: " + ec.message();
        return false;
    }

    RuntimeMaterializationLock lock;
    if (!lock.Acquire(lock_path, error_out)) {
        return false;
    }

    validation_error.clear();
    ec.clear();
    if (fs::exists(runtime_root, ec) && !ec) {
        if (ValidateWorkerRuntimeImage(runtime_root, *source, &validation_error)) {
            if (runtime_worker_exe_out) *runtime_worker_exe_out = runtime_worker_exe;
            return true;
        }
        if (error_out) {
            *error_out = "existing shared worker runtime is invalid and was left untouched: "
                + validation_error;
        }
        return false;
    }
    if (ec) {
        if (error_out) *error_out = "inspect shared worker runtime failed: " + ec.message();
        return false;
    }

    fs::create_directories(fingerprint_root, ec);
    if (ec) {
        if (error_out) *error_out = "create worker runtime fingerprint directory failed: " + ec.message();
        return false;
    }

    fs::remove_all(staging_root, ec);
    if (ec) {
        if (error_out) *error_out = "clear stale pending worker runtime failed: " + ec.message();
        return false;
    }
    fs::create_directories(staging_root, ec);
    if (ec) {
        if (error_out) *error_out = "create pending worker runtime failed: " + ec.message();
        return false;
    }

    const auto fail_pending = [&](const std::string& message) {
        std::error_code cleanup_ec;
        fs::remove_all(staging_root, cleanup_ec);
        if (error_out && error_out->empty()) {
            *error_out = message;
        }
        return false;
    };

    fs::copy_file(
        source->worker_exe,
        staging_root / "SavorWorker.exe",
        fs::copy_options::none,
        ec);
    if (ec) {
        return fail_pending("copy worker executable into shared runtime failed: " + ec.message());
    }
    if (!CopyTree(source->sys, staging_root / "Sys", error_out)) {
        return fail_pending("copy Sys into shared worker runtime failed");
    }
    fs::create_directories(staging_root / "User", ec);
    if (ec) {
        return fail_pending("create empty runtime User template failed: " + ec.message());
    }
    fs::copy_file(
        source->portable,
        staging_root / "portable.txt",
        fs::copy_options::none,
        ec);
    if (ec) {
        return fail_pending("copy runtime portable.txt failed: " + ec.message());
    }

    auto source_after_copy = InspectWorkerRuntimeSource(worker_cfg, error_out);
    if (!source_after_copy.has_value()
        || source_after_copy->fingerprint != source->fingerprint) {
        return fail_pending("worker runtime source changed while the shared image was being copied");
    }

    if (!WriteStringToFile(
            staging_root / "worker-runtime.manifest",
            ExpectedWorkerRuntimeManifest(*source),
            error_out)) {
        return fail_pending("write shared worker runtime manifest failed");
    }
    validation_error.clear();
    if (!ValidateWorkerRuntimeImage(staging_root, *source, &validation_error)) {
        return fail_pending("pending shared worker runtime validation failed: " + validation_error);
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        ec.clear();
        fs::rename(staging_root, runtime_root, ec);
        if (!ec
            || (ec.value() != ERROR_ACCESS_DENIED
                && ec.value() != ERROR_SHARING_VIOLATION
                && ec.value() != ERROR_LOCK_VIOLATION)
            || attempt == 19) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (ec) {
        return fail_pending("publish shared worker runtime failed: " + ec.message());
    }

    validation_error.clear();
    if (!ValidateWorkerRuntimeImage(runtime_root, *source, &validation_error)) {
        if (error_out) {
            *error_out = "published shared worker runtime validation failed: " + validation_error;
        }
        return false;
    }

    if (runtime_worker_exe_out) *runtime_worker_exe_out = runtime_worker_exe;
    return true;
}

} // namespace

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    savor::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    ReadyStepPersistFn persist_materialization_fn,
    savor::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    savor::db::IStateDb* state_db)
    : DBWorkflowWorkerCoordinator(
        execution_db,
        std::move(worker_cfg),
        integration_cfg,
        {},
        std::move(persist_materialization_fn),
        program_kind_registry,
        step_completion_gate,
        state_db) {
}

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    savor::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
    ReadyStepPersistFn persist_materialization_fn,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    savor::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    savor::db::IStateDb* state_db)
    : execution_db_(execution_db)
    , state_db_(state_db)
    , worker_cfg_(std::move(worker_cfg))
    , desired_worker_count_(std::max<size_t>(1, worker_cfg_.desired_workers))
    , integration_cfg_(integration_cfg)
    , schedule_ready_step_fn_(ResolveWorkflowScheduleFn(
        execution_db,
        program_kind_registry,
        std::move(workflow_schedule_fn)))
    , input_aggregation_service_(
        StepInputAggregationConfig{},
        [this](
            const WorkflowReadyStep& step,
            const std::string& event_kind,
            const std::optional<std::string>& source_key,
            const std::optional<std::string>& request_id,
            const std::optional<std::string>& message) {
            (void)this;
            (void)step;
            (void)event_kind;
            (void)source_key;
            (void)request_id;
            (void)message;
        })
    , persist_materialization_fn_(std::move(persist_materialization_fn))
    , job_materialization_service_(execution_db, program_kind_registry)
    , program_kind_registry_(program_kind_registry) {
    if (!worker_cfg_.item_credit_source) {
        worker_cfg_.item_credit_source = std::make_shared<
            savor::db::execution::workflow::
                CoordinatorItemCreditSource>();
    }
    if (step_completion_gate != nullptr) {
        step_completion_gate_ = step_completion_gate;
    } else {
        owned_step_completion_gate_ = std::make_unique<savor::db::execution::workflow::StepCompletionGateService>();
        step_completion_gate_ = owned_step_completion_gate_.get();
    }
    if (program_kind_registry_ != nullptr) {
        adapter_chain_orchestrator_ = std::make_unique<savor::db::execution::workflow::AdapterChainOrchestrator>(
            program_kind_registry_,
            step_completion_gate_);
    }
}

DBWorkflowWorkerCoordinator::~DBWorkflowWorkerCoordinator() {
    Stop();
}

void DBWorkflowWorkerCoordinator::ConfigureWorkerCallbacks(
    std::size_t worker_idx,
    std::uint64_t worker_generation,
    const std::shared_ptr<savor::ProcessWorker>& worker) {
    if (!worker) {
        return;
    }
    worker->set_workset_state_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::WorksetStatePayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::State,
                        .state = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
    worker->set_workset_item_started_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::WorksetItemStartedPayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::ItemStarted,
                        .started = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
    worker->set_invocation_progress_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::InvocationProgressPayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::ItemProgress,
                        .progress = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
    worker->set_workset_item_terminal_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::WorksetItemTerminalPayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::ItemTerminal,
                        .terminal = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
    worker->set_workset_credits_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::WorksetCreditsPayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::Credits,
                        .credits = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
    worker->set_workset_summary_callback(
        [this, worker_idx, worker_generation](
            const savor::wrms::WorksetSummaryPayload& payload) {
            if (workset_event_q_.push(
                    WorksetEventIngress{
                        .worker_idx = worker_idx,
                        .worker_generation = worker_generation,
                        .kind = WorksetEventKind::Summary,
                        .summary = payload,
                    })) {
                queue_cv_.notify_all();
            }
        });
}

DBWorkflowWorkerCoordinator::WorkerSlotPtr DBWorkflowWorkerCoordinator::MakeWorkerSlot(size_t worker_idx) {
    auto slot = std::make_shared<WorkerSlot>();
    slot->id = worker_idx;
    slot->process_generation = NextWorkerProcessGeneration();
    slot->worker = std::make_shared<savor::ProcessWorker>();
    ConfigureWorkerCallbacks(
        worker_idx,
        slot->process_generation,
        slot->worker);
    const auto surface_it = worker_visual_surfaces_.find(worker_idx);
    if (surface_it != worker_visual_surfaces_.end()) {
        slot->visual_render_widget_handle = surface_it->second.render_widget_handle;
        slot->visual_host_events_pipe_name = surface_it->second.host_events_pipe_name;
    }
    return slot;
}

DBWorkflowWorkerCoordinator::WorkerSlotPtr DBWorkflowWorkerCoordinator::GetWorkerSlot(size_t worker_idx) const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return {};
    }
    return workers_[worker_idx];
}

std::vector<DBWorkflowWorkerCoordinator::WorkerSlotPtr> DBWorkflowWorkerCoordinator::CopyWorkerSlots() const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    return workers_;
}

bool DBWorkflowWorkerCoordinator::TryRecordWorkerContactFromProgress(
    std::size_t worker_id,
    std::uint64_t job_id,
    std::chrono::steady_clock::time_point observed_at) {
    const auto slot = GetWorkerSlot(worker_id);
    if (!slot) {
        return false;
    }
    std::lock_guard<std::mutex> slot_lock(slot->mtx);
    if (slot->in_flight_job_id.has_value() && *slot->in_flight_job_id == job_id) {
        RecordWorkerContactLocked(*slot, observed_at);
        return true;
    }
    return false;
}

bool DBWorkflowWorkerCoordinator::PrepareRuntimeSlotForWorker(
    size_t worker_idx,
    std::filesystem::path* runtime_worker_exe_out,
    std::string* error_out) {
    if (worker_cfg_.runtime_slot_preparer) {
        return worker_cfg_.runtime_slot_preparer(worker_idx, worker_cfg_, runtime_worker_exe_out, error_out);
    }

    std::lock_guard<std::mutex> lock(runtime_preparation_mtx_);
    std::error_code ec;
    if (prepared_runtime_worker_exe_.has_value()
        && std::filesystem::is_regular_file(*prepared_runtime_worker_exe_, ec)
        && !ec) {
        if (runtime_worker_exe_out) {
            *runtime_worker_exe_out = *prepared_runtime_worker_exe_;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }

    prepared_runtime_worker_exe_.reset();
    std::filesystem::path prepared;
    if (!EnsureWorkflowWorkerRuntimeSlot(worker_idx, worker_cfg_, &prepared, error_out)) {
        return false;
    }
    prepared_runtime_worker_exe_ = prepared;
    if (runtime_worker_exe_out) {
        *runtime_worker_exe_out = std::move(prepared);
    }
    return true;
}

CoordinatorStartResult DBWorkflowWorkerCoordinator::Start() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);
    std::lock_guard<std::mutex> start_lock(start_mtx_);
    if (start_attempted_) {
        return last_start_result_;
    }
    start_attempted_ = true;

    if (worker_job_thread_.joinable() || worker_lifecycle_thread_.joinable()) {
        last_start_result_ = CoordinatorStartResult{
            .status = CoordinatorStartStatus::Started,
            .ready_workset_capable_workers = ReadyWorksetCapableWorkerCount(),
            .non_retryable = false,
        };
        return last_start_result_;
    }

    data_plane_enabled_.store(false, std::memory_order_release);
    if (worker_cfg_.item_credit_source) {
        worker_cfg_.item_credit_source->Close();
    }
    {
        std::lock_guard<std::mutex> lock(runtime_preparation_mtx_);
        prepared_runtime_worker_exe_.reset();
    }

    stop_started_.store(false);
    stop_.store(false);
    job_materialization_service_.ResetForStart();
    progress_q_.reset();
    results_q_.reset();
    workset_event_q_.reset();
    terminal_commit_sequence_.store(0, std::memory_order_relaxed);
    last_claim_lease_maintenance_ = {};
    no_jobs_available_.store(false);
    claim_attempt_count_.store(0);
    claimed_job_count_.store(0);
    clean_zero_claim_count_.store(0);
    claim_error_count_.store(0);
    partial_claim_count_.store(0);
    dispatch_attempt_count_.store(0);
    dispatch_success_count_.store(0);
    dispatch_miss_count_.store(0);
    {
        std::lock_guard<std::mutex> warning_lock(coordinator_warning_mtx_);
        coordinator_warnings_.clear();
    }

    std::vector<WorkerSlotPtr> initial_slots;
    {
        const auto desired_workers = desired_worker_count_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(workers_mtx_);
        workers_.clear();
        workers_.reserve(desired_workers);
        initial_slots.reserve(desired_workers);
        for (size_t i = 0; i < desired_workers; ++i) {
            auto slot = MakeWorkerSlot(i);
            initial_slots.push_back(slot);
            workers_.push_back(std::move(slot));
        }
        worker_slot_count_.store(workers_.size(), std::memory_order_relaxed);
    }
    for (const auto& slot : initial_slots) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        RegisterWorkerSlotTelemetry(*slot);
    }

    size_t ready_workset_capable_workers = 0;
    bool any_process_ready = false;
    bool saw_workset_dispatch_capability_mismatch = false;
    bool saw_workset_catalog_mismatch = false;
    bool saw_interactive_visual_debug_capability_mismatch = false;
    std::vector<std::string> preflight_errors;
    std::vector<std::shared_ptr<savor::ProcessWorker>>
        rejected_preflight_workers;
    preflight_errors.reserve(initial_slots.size());
    rejected_preflight_workers.reserve(initial_slots.size());
    const auto max_start_attempts =
        std::max<std::uint32_t>(1u, worker_cfg_.max_worker_start_attempts);
    // Production starts one compatible worker synchronously, opens the data
    // plane, then lets the lifecycle loop add the rest with bounded
    // concurrency. This avoids making useful capacity wait on the desired
    // pool size without allowing any DB work before the complete-catalog gate.
    const auto synchronous_preflight_count = initial_slots.size();
    for (std::size_t initial_index = 0;
         initial_index < synchronous_preflight_count
            && ready_workset_capable_workers == 0;
         ++initial_index) {
        const auto& slot = initial_slots[initial_index];
        const auto preflight = RunWorkerCapabilityPreflightForSlot(slot);
        const auto evaluation = EvaluateCapabilityPreflight(
            preflight,
            worker_cfg_.visual_debug_workers,
            worker_cfg_.expected_catalog_sha256,
            worker_cfg_.expected_runtime_profile_sha256,
            worker_cfg_.expected_dependency_manifest_sha256);

        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            slot->capabilities = preflight.capabilities;
            slot->runtime_manifest = preflight.runtime_manifest;
            slot->ready.store(evaluation.ready, std::memory_order_release);
            if (evaluation.ready) {
                slot->last_start_error.clear();
            } else {
                slot->last_start_error = evaluation.error;
                slot->start_attempted = true;
                slot->start_attempts = evaluation.non_retryable
                    ? max_start_attempts
                    : 1u;
                slot->next_start_after = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(
                        worker_cfg_.worker_start_retry_backoff_ms);
                if (slot->worker) {
                    rejected_preflight_workers.push_back(
                        std::move(slot->worker));
                }
                slot->process_generation =
                    NextWorkerProcessGeneration();
                slot->worker = std::make_shared<savor::ProcessWorker>();
                ConfigureWorkerCallbacks(
                    slot->id,
                    slot->process_generation,
                    slot->worker);
                slot->runtime_manifest.reset();
            }
            RegisterWorkerSlotTelemetry(*slot);
        }

        if (preflight.process_ready) {
            any_process_ready = true;
        }
        saw_workset_dispatch_capability_mismatch |=
            evaluation.workset_dispatch_missing;
        saw_workset_catalog_mismatch |=
            evaluation.workset_catalog_missing;
        saw_interactive_visual_debug_capability_mismatch |=
            evaluation.interactive_visual_debug_missing;
        if (evaluation.ready) {
            ++ready_workset_capable_workers;
        } else {
            std::ostringstream error;
            error << "worker " << slot->id << ": " << evaluation.error;
            preflight_errors.push_back(error.str());
        }
    }
    for (const auto& worker : rejected_preflight_workers) {
        if (worker) {
            worker->stop();
        }
    }

    if (ready_workset_capable_workers == 0) {
        std::ostringstream error;
        if (saw_workset_catalog_mismatch
            && !saw_workset_dispatch_capability_mismatch) {
            error
                << "no ready worker advertises the required CompleteExact workset catalog";
        } else if (saw_interactive_visual_debug_capability_mismatch
            && !saw_workset_dispatch_capability_mismatch) {
            error
                << "no ready worker advertises InteractiveVisualDebug";
        } else {
            error << "no ready worker advertises WorksetDispatch";
        }
        for (const auto& preflight_error : preflight_errors) {
            error << "; " << preflight_error;
        }
        const auto status = saw_workset_dispatch_capability_mismatch
            ? CoordinatorStartStatus::WorksetDispatchUnavailable
            : saw_workset_catalog_mismatch
                ? CoordinatorStartStatus::WorksetCatalogUnavailable
            : saw_interactive_visual_debug_capability_mismatch
                ? CoordinatorStartStatus::InteractiveVisualDebugUnavailable
                : CoordinatorStartStatus::CapabilityPreflightFailed;
        return FailStart(
            any_process_ready
                ? status
                : CoordinatorStartStatus::CapabilityPreflightFailed,
            error.str(),
            std::move(initial_slots));
    }
    if (!worker_cfg_.workset_definition_builder
        || !worker_cfg_.workset_terminal_decoder) {
        std::ostringstream error;
        error << "CompleteExact activation requires both the canonical "
                 "workset-definition builder and workset-terminal decoder";
        if (!worker_cfg_.workset_definition_builder) {
            error << "; builder is unavailable";
        }
        if (!worker_cfg_.workset_terminal_decoder) {
            error << "; terminal decoder is unavailable";
        }
        return FailStart(
            CoordinatorStartStatus::WorksetCatalogUnavailable,
            error.str(),
            std::move(initial_slots));
    }

    data_plane_enabled_.store(true, std::memory_order_release);
    if (worker_cfg_.item_credit_source) {
        worker_cfg_.item_credit_source->Open(
            ReadyItemCreditCapacity());
        RefreshSharedItemCredits();
    }
    progress_drainer_thread_ = std::thread([this]() { DrainProgressLoop(); });
    results_drainer_thread_ = std::thread([this]() { DrainResultsLoop(); });
    workset_event_drainer_thread_ =
        std::thread([this]() { DrainWorksetEventsLoop(); });
    job_materializer_thread_ = std::thread([this]() { job_materialization_service_.MaterializeClaimedJobPayloadLoop(stop_); });
    worker_lifecycle_thread_ = std::thread([this]() { WorkerLifecycleCoordinatorLoop(); });
    worker_job_thread_ = std::thread([this]() { WorkerJobCoordinatorLoop(); });

    last_start_result_ = CoordinatorStartResult{
        .status = CoordinatorStartStatus::Started,
        .ready_workset_capable_workers = ready_workset_capable_workers,
        .non_retryable = false,
    };
    return last_start_result_;
}

CoordinatorWorkerCapabilityPreflightResult
DBWorkflowWorkerCoordinator::RunWorkerCapabilityPreflightForSlot(
    const WorkerSlotPtr& slot) {
    CoordinatorWorkerCapabilityPreflightResult preflight{};
    std::size_t worker_id = 0;
    std::shared_ptr<savor::ProcessWorker> worker;
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        worker_id = slot->id;
        worker = slot->worker;
    }
    try {
        preflight = worker_cfg_.worker_capability_preflight
            ? worker_cfg_.worker_capability_preflight(
                worker_id,
                worker_cfg_,
                worker)
            : PreflightWorkerSlot(slot);
    } catch (const std::exception& ex) {
        preflight.error = std::string("preflight threw: ") + ex.what();
    } catch (...) {
        preflight.error = "preflight threw an unknown exception";
    }
    return preflight;
}

CoordinatorWorkerCapabilityPreflightResult
DBWorkflowWorkerCoordinator::PreflightWorkerSlot(const WorkerSlotPtr& slot) {
    std::size_t worker_id = 0;
    std::shared_ptr<savor::ProcessWorker> worker;
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        worker_id = slot->id;
        worker = slot->worker;
    }
    if (!slot || !worker) {
        return {
            .process_ready = false,
            .error = "worker slot is unavailable for capability preflight",
        };
    }

    std::filesystem::path runtime_worker_exe;
    std::string error;
    if (!PrepareRuntimeSlotForWorker(
            worker_id,
            &runtime_worker_exe,
            &error)) {
        return {
            .process_ready = false,
            .error = "worker runtime setup failed: " + error,
        };
    }

    const auto worker_root =
        std::filesystem::path(worker_cfg_.worker_dir_root)
        / ("workflow-worker-" + std::to_string(worker_id));
    if (!worker->launch_and_negotiate(
            savor::ProcessLaunchOptions{
                .worker_id = worker_id,
                .exe_path = runtime_worker_exe.string(),
                .log_directory = worker_root.string(),
                .hello_timeout_ms = worker_cfg_.worker_start_timeout_ms,
            },
            &error)) {
        return {
            .process_ready = false,
            .error = error.empty()
                ? "worker launch or WRMS negotiation failed"
                : error,
        };
    }

    const auto capabilities = worker->process_capabilities();
    const auto runtime_manifest = worker->runtime_manifest();
    if (worker_cfg_.visual_debug_workers
        && !savor::runtime::HasCapability(
            capabilities,
            savor::runtime::WorkerCapability::InteractiveVisualDebug)) {
        return {
            .process_ready = true,
            .capabilities = capabilities,
            .runtime_manifest = runtime_manifest,
            .error =
                "interactive visual debugging is deferred to Dependency Slice 3",
        };
    }
    const auto process_evaluation = EvaluateCapabilityPreflight(
        CoordinatorWorkerCapabilityPreflightResult{
            .process_ready = true,
            .capabilities = capabilities,
            .runtime_manifest = runtime_manifest,
        },
        worker_cfg_.visual_debug_workers,
        worker_cfg_.expected_catalog_sha256,
        worker_cfg_.expected_runtime_profile_sha256,
        worker_cfg_.expected_dependency_manifest_sha256);
    if (!process_evaluation.ready) {
        return {
            .process_ready = true,
            .capabilities = capabilities,
            .runtime_manifest = runtime_manifest,
            .error = process_evaluation.error,
        };
    }

    const auto user_directory = worker_root / "User";
    std::error_code ec;
    std::filesystem::create_directories(user_directory, ec);
    if (ec) {
        return {
            .process_ready = false,
            .capabilities = capabilities,
            .runtime_manifest = runtime_manifest,
            .error = "create worker user directory failed: " + ec.message(),
        };
    }

    std::uint64_t render_widget_handle = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        render_widget_handle = slot->visual_render_widget_handle;
    }
    savor::wrms::OpenSessionResultPayload open_result;
    if (!worker->open_session(
            savor::ProcessOpenSessionOptions{
                .runtime_root = runtime_worker_exe.parent_path().string(),
                .user_directory = user_directory.string(),
                .iso_path = worker_cfg_.iso_path,
                .visual = worker_cfg_.visual_workers,
                .render_widget_handle = render_widget_handle,
                .screenshot_directory =
                    worker_cfg_.visual_screenshot_dir,
                .screenshot_timeout_ms = 5000,
                .screenshot_on_terminal =
                    !worker_cfg_.visual_screenshot_dir.empty(),
            },
            &open_result,
            &error,
            worker_cfg_.worker_start_timeout_ms)) {
        return {
            .process_ready = false,
            .capabilities = capabilities,
            .runtime_manifest = runtime_manifest,
            .error = error.empty()
                ? "worker OpenSession failed"
                : error,
        };
    }

    return {
        .process_ready = true,
        .capabilities = open_result.capability_mask,
        .runtime_manifest = runtime_manifest,
    };
}

void DBWorkflowWorkerCoordinator::Stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mtx_);
    if (stop_started_.exchange(true)) {
        return;
    }
    data_plane_enabled_.store(false, std::memory_order_release);
    if (worker_cfg_.item_credit_source) {
        worker_cfg_.item_credit_source->Close();
    }
    EmitShutdownPhase("stop_requested");
    stop_.store(true);
    queue_cv_.notify_all();
    job_materialization_service_.StopMaterializationLoop();

    StopVisualDebugReplay();
    EmitShutdownPhase("visual_debug_stopped");

    std::vector<WorkerSlotPtr> slots_to_stop;
    std::vector<std::thread> startup_threads;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        slots_to_stop = workers_;
    }
    {
        std::ostringstream detail;
        detail << "workers=" << slots_to_stop.size();
        EmitShutdownPhase("workers_snapshot", detail.str());
    }
    for (const auto& slot : slots_to_stop) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (slot->startup_thread.joinable()) {
            startup_threads.push_back(std::move(slot->startup_thread));
        }
    }
    for (const auto& slot : slots_to_stop) {
        if (slot) {
            StopWorkerSlot(slot, true);
        }
    }
    EmitShutdownPhase("workers_stopped");

    queue_cv_.notify_all();

    EmitShutdownPhase("join_begin", "thread=worker_job");
    if (worker_job_thread_.joinable()) {
        worker_job_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=worker_job");
    EmitShutdownPhase("join_begin", "thread=worker_lifecycle");
    if (worker_lifecycle_thread_.joinable()) {
        worker_lifecycle_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=worker_lifecycle");
    EmitShutdownPhase("join_begin", "thread=job_materializer");
    if (job_materializer_thread_.joinable()) {
        job_materializer_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=job_materializer");
    for (auto& thread : startup_threads) {
        EmitShutdownPhase("join_begin", "thread=startup");
        if (thread.joinable()) {
            thread.join();
        }
        EmitShutdownPhase("join_end", "thread=startup");
    }

    EmitShutdownPhase("queues_close");
    progress_q_.close();
    results_q_.close();
    workset_event_q_.close();

    EmitShutdownPhase("join_begin", "thread=progress_drainer");
    if (progress_drainer_thread_.joinable()) {
        progress_drainer_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=progress_drainer");
    EmitShutdownPhase("join_begin", "thread=results_drainer");
    if (results_drainer_thread_.joinable()) {
        results_drainer_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=results_drainer");
    EmitShutdownPhase("join_begin", "thread=workset_event_drainer");
    if (workset_event_drainer_thread_.joinable()) {
        workset_event_drainer_thread_.join();
    }
    EmitShutdownPhase("join_end", "thread=workset_event_drainer");
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        dispatched_job_context_by_id_.clear();
        dispatched_workset_items_.clear();
        for (const auto& slot : workers_) {
            if (!slot) {
                continue;
            }
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            slot->in_flight_job_id.reset();
            slot->active_workset_id.reset();
            slot->staged_workset_id.reset();
            slot->retained_workset_ids.clear();
            slot->retained_workset_item_counts.clear();
            slot->failed_closed_workset_ids.clear();
            slot->last_workset_outbound_sequence = 0;
            slot->last_workset_terminal_order = 0;
            slot->workset_admission_blocked = false;
            slot->in_flight_started_at = {};
            slot->last_worker_contact_at = {};
            slot->dead_in_flight_observed_at = {};
            slot->loaded_program_kind.reset();
            slot->loaded_program_runtime_affinity_key.reset();
            slot->loaded_savestate_affinity_key.reset();
            slot->loaded_workset_execution_key.reset();
            slot->worker.reset();
            worker_status_.UpdateState(
                static_cast<std::int64_t>(slot->id),
                WorkerStateKind::Dead);
            worker_status_.UnregisterWorker(
                static_cast<std::int64_t>(slot->id));
        }
        workers_.clear();
        worker_slot_count_.store(0, std::memory_order_relaxed);
    }
    EmitShutdownPhase("stop_complete");
    {
        std::lock_guard<std::mutex> start_lock(start_mtx_);
        start_attempted_ = false;
    }
}

CoordinatorStartResult DBWorkflowWorkerCoordinator::SnapshotStartResult() const {
    std::lock_guard<std::mutex> start_lock(start_mtx_);
    return last_start_result_;
}

bool DBWorkflowWorkerCoordinator::IsDataPlaneEnabled() const noexcept {
    return data_plane_enabled_.load(std::memory_order_acquire);
}

size_t DBWorkflowWorkerCoordinator::ReadyWorksetCapableWorkerCount() const {
    size_t count = 0;
    const auto slots = CopyWorkerSlots();
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (slot->ready.load(std::memory_order_acquire)
            && savor::runtime::HasCapability(
                slot->capabilities,
                savor::runtime::WorkerCapability::WorksetDispatch)) {
            ++count;
        }
    }
    return count;
}

size_t DBWorkflowWorkerCoordinator::ReadyItemCreditCapacity() const {
    size_t credits = 0;
    const auto slots = CopyWorkerSlots();
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (!slot->ready.load(std::memory_order_acquire)
            || !slot->runtime_manifest.has_value()
            || !savor::runtime::HasCapability(
                slot->capabilities,
                savor::runtime::WorkerCapability::WorksetDispatch)) {
            continue;
        }
        const auto slot_credits = static_cast<size_t>(
            slot->runtime_manifest->limits.maximum_item_credits);
        const auto available = std::numeric_limits<size_t>::max() - credits;
        credits += std::min(available, slot_credits);
    }
    return credits;
}

std::shared_ptr<
    savor::db::execution::workflow::CoordinatorItemCreditSource>
DBWorkflowWorkerCoordinator::ItemCreditSource() const {
    return worker_cfg_.item_credit_source;
}

CoordinatorStartResult DBWorkflowWorkerCoordinator::FailStart(
    CoordinatorStartStatus status,
    std::string error,
    std::vector<WorkerSlotPtr> slots_to_stop) {
    data_plane_enabled_.store(false, std::memory_order_release);
    if (worker_cfg_.item_credit_source) {
        worker_cfg_.item_credit_source->Close();
    }
    stop_.store(true, std::memory_order_release);
    for (const auto& slot : slots_to_stop) {
        if (!slot) {
            continue;
        }
        std::shared_ptr<savor::ProcessWorker> worker;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            slot->ready.store(false, std::memory_order_release);
            slot->capabilities = 0;
            worker = std::move(slot->worker);
            worker_status_.UpdateState(static_cast<std::int64_t>(slot->id), WorkerStateKind::Dead);
            worker_status_.UnregisterWorker(static_cast<std::int64_t>(slot->id));
        }
        if (worker) {
            worker->stop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        workers_.clear();
        worker_slot_count_.store(0, std::memory_order_relaxed);
    }
    last_start_result_ = CoordinatorStartResult{
        .status = status,
        .ready_workset_capable_workers = 0,
        .non_retryable = status == CoordinatorStartStatus::CapabilityPreflightUnavailable
            || status == CoordinatorStartStatus::WorksetDispatchUnavailable
            || status == CoordinatorStartStatus::WorksetCatalogUnavailable
            || status == CoordinatorStartStatus::InteractiveVisualDebugUnavailable,
        .error = std::move(error),
    };
    return last_start_result_;
}

void DBWorkflowWorkerCoordinator::SetPaused(bool paused) {
    paused_.store(paused);
    if (!paused) {
        queue_cv_.notify_all();
    }
}

bool DBWorkflowWorkerCoordinator::IsPaused() const {
    return paused_.load();
}

void DBWorkflowWorkerCoordinator::SetDesiredWorkerCount(size_t desired_workers) {
    const size_t clamped_workers = std::max<size_t>(1, desired_workers);
    const size_t previous = desired_worker_count_.exchange(clamped_workers, std::memory_order_relaxed);
    if (previous == clamped_workers) {
        return;
    }
    queue_cv_.notify_all();
}

void DBWorkflowWorkerCoordinator::SetWorkflowMaterializationCallback(WorkflowCoordinatorBridge::MaterializationCallback callback) {
    workflow_bridge_.SetMaterializationCallback(std::move(callback));
}

void DBWorkflowWorkerCoordinator::SetWorkflowTerminalCallback(WorkflowCoordinatorBridge::TerminalCallback callback) {
    workflow_bridge_.SetTerminalCallback(std::move(callback));
}

void DBWorkflowWorkerCoordinator::SetWorkflowCreatedCallback(WorkflowCoordinatorBridge::WorkflowCreatedCallback callback) {
    workflow_bridge_.SetWorkflowCreatedCallback(std::move(callback));
}

bool DBWorkflowWorkerCoordinator::PublishTerminalJobSet(const TerminalJobSetSignal& signal) {
    if (!IsDataPlaneEnabled() || !integration_cfg_.workflow_enabled) {
        return false;
    }

    const bool published = workflow_bridge_.NotifyTerminal(signal);
    if (published) {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        ++terminal_published_count_;
    }
    return published;
}

bool DBWorkflowWorkerCoordinator::PublishWorkflowCreated(const WorkflowCreatedSignal& signal) {
    if (!IsDataPlaneEnabled() || !integration_cfg_.workflow_enabled) {
        return false;
    }

    const bool published = workflow_bridge_.NotifyWorkflowCreated(signal);
    if (published) {
        ++workflow_created_signal_count_;
        queue_cv_.notify_one();
    }
    return published;
}

void DBWorkflowWorkerCoordinator::EnqueueReadyStep(const WorkflowReadyStep& step) {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(queue_mtx_);
    const auto key = ReadyDedupKey(step.workflow_step_id);
    if (!seen_ready_step_ids_.emplace(key).second) {
        return;
    }
    ready_queue_.push_back(step);
    queue_cv_.notify_one();
}

std::optional<ScheduledJobSet> DBWorkflowWorkerCoordinator::MaterializeWorkflowStep(const WorkflowReadyStep& step) {
    if (!IsDataPlaneEnabled() || !integration_cfg_.workflow_enabled) {
        return std::nullopt;
    }

    const auto scheduled = MaterializeWorkflowStepInternal(step);
    if (!scheduled.has_value()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(queue_mtx_);
    ++materialized_count_;
    ++epoch_;
    return scheduled;
}

std::optional<ScheduledJobSet> DBWorkflowWorkerCoordinator::MaterializeWorkflowStepInternal(const WorkflowReadyStep& step) {
    if (!IsDataPlaneEnabled() || !schedule_ready_step_fn_) {
        return std::nullopt;
    }

    const auto scheduled = schedule_ready_step_fn_(step);
    if (scheduled.job_set_id <= 0) {
        return std::nullopt;
    }

    if (execution_db_ && execution_db_->WorkflowCommandService()) {
        std::string error;
        const bool marked = execution_db_->WorkflowCommandService()->MarkStepMaterialized(
            {
                .workflow_step_id = step.workflow_step_id,
                .job_set_id = scheduled.job_set_id,
                .input_ref_kind = scheduled.program_ref_kind.empty()
                    ? std::nullopt
                    : std::optional<std::string>(scheduled.program_ref_kind),
                .input_ref_id = scheduled.program_ref_id > 0
                    ? std::optional<std::int64_t>(scheduled.program_ref_id)
                    : std::nullopt,
                .requested_by = "workflow_materialize",
            },
            &error);
        if (!marked) {
            ++materialization_failure_count_;
            EmitWorkflowFailureEvents(
                step,
                "MarkStepMaterialized",
                error.empty() ? "unknown error" : error);
            MaybeTerminalFailStepInStrictSmokeMode(step, "workflow_materialize_strict_smoke");
            return std::nullopt;
        }
    }

    if (persist_materialization_fn_) {
        persist_materialization_fn_(step, scheduled);
    }
    for (const auto& line : scheduled.event_lines) {
        EmitDurableEventLine(line);
    }
    if (execution_db_ != nullptr) {
        const auto details = execution_db_->GetJobSetProgress(scheduled.job_set_id);
        if (details.has_value()) {
            std::ostringstream line;
            line << "[seedprobe-materialization-counts]"
                 << " step=" << step.step_key
                 << " kind=" << step.step_kind
                 << " workflow_step_id=" << step.workflow_step_id
                 << " job_set=" << scheduled.job_set_id
                 << " total=" << details->total_jobs
                 << " done=" << details->completed_jobs;
            if (details->expected_total.has_value()) {
                line << " expected_total=" << *details->expected_total;
            }
            const auto child_rows = execution_db_->GetChildJobSetProgress(scheduled.job_set_id);
            if (!child_rows.empty()) {
                std::int64_t child_total = 0;
                std::int64_t child_expected = 0;
                for (const auto& child : child_rows) {
                    child_total += child.total_jobs;
                    child_expected += child.expected_total.value_or(0);
                }
                line << " child_job_sets=" << child_rows.size()
                     << " child_total=" << child_total
                     << " child_expected_total=" << child_expected;
            }
            EmitDurableEventLine(line.str());
        }
    }
    workflow_bridge_.NotifyMaterialized(step.workflow_step_id, scheduled.job_set_id);
    return scheduled;
}

bool DBWorkflowWorkerCoordinator::SendJobToWorker(
    size_t worker_idx,
    uint64_t job_id,
    const savor::PSJob& job) {
    (void)worker_idx;
    (void)job_id;
    (void)job;
    // Hard cutover: production dispatch is SubmitWorkset for both singleton
    // and multi-item execution. This retained source-compatibility facade
    // fails locally and never invokes ProcessWorker::send_job.
    return false;
}

bool DBWorkflowWorkerCoordinator::ValidateClaimedWorksetAuthority(
    const std::vector<ClaimedJobRecord>& claimed_jobs) {
    if (execution_db_ == nullptr || claimed_jobs.empty()) {
        return false;
    }
    std::vector<savor::db::ExecutionJobLeaseRequest> requests;
    requests.reserve(claimed_jobs.size());
    for (const auto& job : claimed_jobs) {
        requests.push_back(savor::db::ExecutionJobLeaseRequest{
            .job_id = job.job_id,
            .claimed_by_token = job.claimed_by_token,
        });
    }

    savor::db::ExecutionJobStartAuthoritySetReceipt receipt{};
    std::string error;
    const bool validated =
        execution_db_->ValidateExecutionJobStartAuthoritySet(
            requests,
            &receipt,
            &error);
    if (validated && receipt.all_valid
        && receipt.items.size() == claimed_jobs.size()) {
        return true;
    }

    std::ostringstream line;
    line << "[workflow-workset-authority-rejected]"
         << " items=" << claimed_jobs.size()
         << " receipts=" << receipt.items.size();
    if (!error.empty()) {
        line << " error=" << error;
    }
    EmitDurableEventLine(line.str());

    for (std::size_t index = 0; index < claimed_jobs.size(); ++index) {
        auto disposition =
            savor::db::ExecutionJobLeaseRenewalDisposition::BackendError;
        if (index < receipt.items.size()) {
            switch (receipt.items[index].disposition) {
            case savor::db::ExecutionJobStartAuthorityDisposition::Missing:
                disposition =
                    savor::db::ExecutionJobLeaseRenewalDisposition::Missing;
                break;
            case savor::db::ExecutionJobStartAuthorityDisposition::WrongState:
                disposition = savor::db::
                    ExecutionJobLeaseRenewalDisposition::WrongState;
                break;
            case savor::db::ExecutionJobStartAuthorityDisposition::TokenMismatch:
                disposition = savor::db::
                    ExecutionJobLeaseRenewalDisposition::TokenMismatch;
                break;
            case savor::db::ExecutionJobStartAuthorityDisposition::Expired:
                disposition =
                    savor::db::ExecutionJobLeaseRenewalDisposition::Expired;
                break;
            case savor::db::ExecutionJobStartAuthorityDisposition::Valid:
                continue;
            default:
                break;
            }
        }
        // These items have not crossed the worker boundary. Route the exact
        // loss through the guarded internal path so invalid local materialized
        // records are removed instead of being selected again in a tight loop.
        HandleItemAuthorityLost(
            ClaimLeaseMaintenanceResult::LostAuthority{
                .job_id = claimed_jobs[index].job_id,
                .claimed_by_token =
                    claimed_jobs[index].claimed_by_token,
                .disposition = disposition,
            });
    }
    return false;
}

void DBWorkflowWorkerCoordinator::DrainWorksetEventsLoop() {
    WorksetEventIngress event{};
    while (workset_event_q_.pop_wait(event)) {
        const auto slot = GetWorkerSlot(event.worker_idx);
        if (!slot) {
            continue;
        }
        std::uint64_t current_generation = 0;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            current_generation = slot->process_generation;
        }
        if (event.worker_generation != current_generation) {
            std::ostringstream line;
            line << "[workflow-workset-stale-process-event]"
                 << " worker=" << event.worker_idx
                 << " event_generation="
                 << event.worker_generation
                 << " current_generation="
                 << current_generation;
            EmitDurableEventLine(line.str());
            continue;
        }
        // SubmitWorkset can publish events immediately. Serialize handling
        // behind provisional context registration and the local transition to
        // Dispatched so ItemStarted is always the first durable mutation.
        std::unique_lock<std::mutex> submission_lock(
            slot->workset_submission_mtx);
        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            if (event.worker_generation != slot->process_generation) {
                EmitDurableEventLine(
                    "[workflow-workset-stale-process-event] worker="
                    + std::to_string(event.worker_idx)
                    + " event_generation="
                    + std::to_string(event.worker_generation)
                    + " current_generation="
                    + std::to_string(slot->process_generation));
                continue;
            }
        }
        switch (event.kind) {
        case WorksetEventKind::State:
            HandleWorksetState(event);
            break;
        case WorksetEventKind::ItemStarted:
            HandleWorksetItemStarted(event);
            break;
        case WorksetEventKind::ItemProgress:
            HandleWorksetItemProgress(event);
            break;
        case WorksetEventKind::ItemTerminal:
            HandleWorksetItemTerminal(event);
            break;
        case WorksetEventKind::Credits:
            HandleWorksetCredits(event);
            break;
        case WorksetEventKind::Summary:
            HandleWorksetSummary(event);
            break;
        }
    }
}

void DBWorkflowWorkerCoordinator::HandleWorksetState(
    const WorksetEventIngress& event) {
    if (!AcceptWorksetEventSequence(
            event.worker_idx,
            event.state.workset_id,
            event.state.outbound_sequence,
            "state")) {
        FailClosedWorker(
            event.worker_idx,
            "invalid or out-of-order workset-state event");
    }
}

void DBWorkflowWorkerCoordinator::HandleWorksetItemStarted(
    const WorksetEventIngress& event) {
    if (!AcceptWorksetEventSequence(
            event.worker_idx,
            event.started.workset_id,
            event.started.outbound_sequence,
            "item-start")) {
        FailClosedWorker(
            event.worker_idx,
            "workset item-start sequence integrity failed");
        return;
    }
    DispatchedWorksetItemContext context{};
    bool context_found = false;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        const auto it = dispatched_workset_items_.find(
            WorksetItemMapKey(
                event.worker_idx,
                event.started.workset_id,
                event.started.item_id));
        if (it != dispatched_workset_items_.end()
            && it->second.worker_idx == event.worker_idx) {
            context = it->second;
            context_found = true;
        }
    }
    if (!context_found) {
        std::ostringstream line;
        line << "[workflow-workset-item-start-unmatched]"
             << " worker=" << event.worker_idx
             << " workset=" << event.started.workset_id
             << " item=" << event.started.item_id;
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            event.started.workset_id,
            "item-start has no exact retained claim");
        return;
    }
    if (context.item_ordinal != event.started.item_ordinal
        || context.invocation_id != event.started.invocation_id
        || context.attempt_id != event.started.attempt_id) {
        std::ostringstream line;
        line << "[workflow-workset-item-start-correlation-mismatch]"
             << " worker=" << event.worker_idx
             << " workset=" << event.started.workset_id
             << " item=" << event.started.item_id;
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            event.started.workset_id,
            "item-start correlation mismatch");
        return;
    }
    if (context.start_persisted) {
        EmitDurableEventLine(
            "[workflow-workset-item-start-duplicate] worker="
            + std::to_string(event.worker_idx)
            + " workset=" + std::to_string(event.started.workset_id)
            + " item=" + std::to_string(event.started.item_id));
        FailClosedWorkset(
            event.worker_idx,
            event.started.workset_id,
            "duplicate item-start event");
        return;
    }
    {
        const auto slot = GetWorkerSlot(event.worker_idx);
        if (!slot) {
            FailClosedWorkset(
                event.worker_idx,
                event.started.workset_id,
                "worker slot disappeared before item start");
            return;
        }
        bool valid_execution_owner = false;
        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            if (slot->staged_workset_id.has_value()
                && *slot->staged_workset_id
                    == event.started.workset_id) {
                // The worker has promoted its host-only staged package. The
                // predecessor remains retained only for completion-ledger
                // terminals and its eventual bookkeeping summary.
                slot->active_workset_id =
                    slot->staged_workset_id;
                slot->staged_workset_id.reset();
                valid_execution_owner = true;
            } else {
                valid_execution_owner =
                    slot->active_workset_id.has_value()
                    && *slot->active_workset_id
                        == event.started.workset_id;
            }
        }
        if (!valid_execution_owner) {
            FailClosedWorkset(
                event.worker_idx,
                event.started.workset_id,
                "draining workset attempted to start another item");
            return;
        }
    }

    savor::db::ExecutionJobStartReceipt receipt{};
    std::string error;
    const bool persisted = execution_db_ != nullptr
        && execution_db_->MarkExecutionJobStarted(
            context.claimed.job_id,
            context.claimed.claimed_by_token,
            "worker_workset_item_started",
            &receipt,
            &error);
    if (!persisted
        || receipt.disposition
            != savor::db::ExecutionJobStartDisposition::Started
        || !receipt.durable_attempt_id.has_value()
        || *receipt.durable_attempt_id != context.attempt_id) {
        std::ostringstream line;
        line << "[workflow-workset-item-start-persist-failed]"
             << " worker=" << event.worker_idx
             << " workset=" << event.started.workset_id
             << " item=" << event.started.item_id
             << " job=" << context.claimed.job_id
             << " disposition="
             << static_cast<int>(receipt.disposition);
        if (!error.empty()) {
            line << " error=" << error;
        }
        EmitDurableEventLine(line.str());

        auto disposition =
            savor::db::ExecutionJobLeaseRenewalDisposition::BackendError;
        switch (receipt.disposition) {
        case savor::db::ExecutionJobStartDisposition::Missing:
            disposition =
                savor::db::ExecutionJobLeaseRenewalDisposition::Missing;
            break;
        case savor::db::ExecutionJobStartDisposition::WrongState:
            disposition =
                savor::db::ExecutionJobLeaseRenewalDisposition::WrongState;
            break;
        case savor::db::ExecutionJobStartDisposition::TokenMismatch:
            disposition = savor::db::
                ExecutionJobLeaseRenewalDisposition::TokenMismatch;
            break;
        case savor::db::ExecutionJobStartDisposition::Expired:
            disposition =
                savor::db::ExecutionJobLeaseRenewalDisposition::Expired;
            break;
        default:
            break;
        }
        HandleItemAuthorityLost(
            ClaimLeaseMaintenanceResult::LostAuthority{
                .job_id = context.claimed.job_id,
                .claimed_by_token =
                    context.claimed.claimed_by_token,
                .disposition = disposition,
            });
        FailClosedWorkset(
            event.worker_idx,
            event.started.workset_id,
            "durable item-start authority was lost");
        return;
    }

    bool retained_start_updated = false;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        const auto it = dispatched_workset_items_.find(
            WorksetItemMapKey(
                event.worker_idx,
                event.started.workset_id,
                event.started.item_id));
        if (it != dispatched_workset_items_.end()
            && it->second.worker_idx == event.worker_idx) {
            it->second.start_persisted = true;
            retained_start_updated = true;
        }
    }
    if (!retained_start_updated) {
        FailClosedWorkset(
            event.worker_idx,
            event.started.workset_id,
            "item-start claim disappeared after durable start");
        return;
    }

    const auto slot = GetWorkerSlot(event.worker_idx);
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        slot->in_flight_job_id =
            static_cast<std::uint64_t>(context.claimed.job_id);
        slot->loaded_program_kind =
            context.claimed.program_kind;
        slot->loaded_program_runtime_affinity_key =
            context.claimed.affinity.program_runtime_affinity_key;
        slot->loaded_savestate_affinity_key =
            context.claimed.affinity.savestate_affinity_key;
        slot->loaded_workset_execution_key =
            context.claimed.workset_execution_key;
        worker_status_.SetCurrentJob(
            static_cast<std::int64_t>(slot->id),
            context.claimed.job_id,
            std::nullopt);
        worker_status_.UpdateState(
            static_cast<std::int64_t>(slot->id),
            WorkerStateKind::Running);
        worker_status_.RecordHeartbeat(
            static_cast<std::int64_t>(slot->id));
    }
}

void DBWorkflowWorkerCoordinator::HandleWorksetItemProgress(
    const WorksetEventIngress& event) {
    const auto& progress = event.progress;
    DispatchedWorksetItemContext context{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        const auto it = dispatched_workset_items_.find(
            WorksetItemMapKey(
                event.worker_idx,
                progress.workset_id,
                progress.item_id));
        if (it != dispatched_workset_items_.end()
            && it->second.worker_idx == event.worker_idx) {
            context = it->second;
            found = true;
        }
    }
    if (!found
        || context.item_ordinal != progress.item_ordinal
        || context.invocation_id != progress.invocation_id
        || context.attempt_id != progress.attempt_id
        || !context.start_persisted) {
        // Progress is passive and may be coalesced, but it must never be
        // attributed to the wrong durable job. Drop an unmatched projection
        // without changing workset/session state; authoritative lifecycle
        // events remain fail-closed.
        std::ostringstream line;
        line << "[workflow-workset-progress-unmatched]"
             << " worker=" << event.worker_idx
             << " workset=" << progress.workset_id
             << " item=" << progress.item_id
             << " invocation=" << progress.invocation_id;
        EmitDurableEventLine(line.str());
        return;
    }

    savor::PRProgress projected{
        .worker_id = event.worker_idx,
        .job_id =
            static_cast<std::uint64_t>(context.claimed.job_id),
        .record_progress = true,
    };
    if (!progress.progress.empty()) {
        projected.text.assign(
            reinterpret_cast<const char*>(progress.progress.data()),
            progress.progress.size());
    }
    (void)progress_q_.push(std::move(projected));
}

bool DBWorkflowWorkerCoordinator::AcceptWorksetEventSequence(
    std::size_t worker_idx,
    std::uint64_t workset_id,
    std::uint64_t outbound_sequence,
    std::string_view event_kind) {
    const auto slot = GetWorkerSlot(worker_idx);
    if (!slot) {
        return false;
    }
    bool accepted = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (workset_id != 0
            && !slot->retained_workset_ids.contains(workset_id)) {
            reason = "event does not match a retained workset";
        } else if (workset_id != 0
            && slot->failed_closed_workset_ids.contains(
                       workset_id)) {
            reason = "workset is already fail-closed";
        } else if (outbound_sequence == 0
            || slot->last_workset_outbound_sequence
                == std::numeric_limits<std::uint64_t>::max()
            || outbound_sequence
                != slot->last_workset_outbound_sequence + 1) {
            reason =
                "outbound sequence is not the exact next worker sequence";
        } else {
            slot->last_workset_outbound_sequence =
                outbound_sequence;
            RecordWorkerContactLocked(
                *slot,
                std::chrono::steady_clock::now());
            accepted = true;
        }
    }
    if (!accepted) {
        std::ostringstream line;
        line << "[workflow-workset-event-rejected]"
             << " worker=" << worker_idx
             << " workset=" << workset_id
             << " sequence=" << outbound_sequence
             << " kind=" << event_kind
             << " reason=" << reason;
        EmitDurableEventLine(line.str());
    }
    return accepted;
}

void DBWorkflowWorkerCoordinator::FailClosedWorker(
    std::size_t worker_idx,
    std::string_view reason) {
    const auto slot = GetWorkerSlot(worker_idx);
    if (!slot) {
        return;
    }
    std::shared_ptr<savor::ProcessWorker> worker;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        slot->workset_admission_blocked = true;
        slot->ready.store(false, std::memory_order_release);
        for (const auto workset_id : slot->retained_workset_ids) {
            slot->failed_closed_workset_ids.insert(workset_id);
        }
        worker = slot->worker;
        worker_status_.UpdateState(
            static_cast<std::int64_t>(slot->id),
            WorkerStateKind::Draining);
        worker_status_.RecordError(
            static_cast<std::int64_t>(slot->id),
            std::string(reason));
    }
    EmitDurableEventLine(
        "[workflow-worker-sequence-fail-closed] worker="
        + std::to_string(worker_idx)
        + " reason=" + std::string(reason));
    if (worker) {
        worker->stop();
    }
}

void DBWorkflowWorkerCoordinator::FailClosedWorkset(
    std::size_t worker_idx,
    std::uint64_t workset_id,
    std::string_view reason) {
    const auto slot = GetWorkerSlot(worker_idx);
    if (!slot) {
        return;
    }
    std::shared_ptr<savor::ProcessWorker> worker;
    bool should_cancel = false;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (!slot->retained_workset_ids.contains(workset_id)
            || slot->failed_closed_workset_ids.contains(
                workset_id)) {
            return;
        }
        slot->failed_closed_workset_ids.insert(workset_id);
        slot->workset_admission_blocked = true;
        should_cancel =
            (slot->active_workset_id.has_value()
                && *slot->active_workset_id == workset_id)
            || (slot->staged_workset_id.has_value()
                && *slot->staged_workset_id == workset_id);
        worker = slot->worker;
        worker_status_.UpdateState(
            static_cast<std::int64_t>(slot->id),
            WorkerStateKind::Draining);
        worker_status_.RecordError(
            static_cast<std::int64_t>(slot->id),
            std::string(reason));
    }

    std::ostringstream line;
    line << "[workflow-workset-fail-closed]"
         << " worker=" << worker_idx
         << " workset=" << workset_id
         << " reason=" << reason;
    EmitDurableEventLine(line.str());

    if (should_cancel && worker && !worker->cancel_workset(
            savor::runtime::WorkerWorksetId{workset_id},
            std::string(reason))) {
        const auto worker_error = worker->last_error();
        std::ostringstream cancel_line;
        cancel_line << "[workflow-workset-fail-closed-cancel-failed]"
                    << " worker=" << worker_idx
                    << " workset=" << workset_id;
        if (!worker_error.empty()) {
            cancel_line << " error=" << worker_error;
        }
        EmitDurableEventLine(cancel_line.str());
        worker->stop();
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        slot->ready.store(false, std::memory_order_release);
    }
}

void DBWorkflowWorkerCoordinator::HandleWorksetItemTerminal(
    const WorksetEventIngress& event) {
    const auto& terminal = event.terminal;
    if (!AcceptWorksetEventSequence(
            event.worker_idx,
            terminal.workset_id,
            terminal.outbound_sequence,
            "item-terminal")) {
        FailClosedWorker(
            event.worker_idx,
            "workset item-terminal sequence integrity failed");
        return;
    }
    {
        const auto slot = GetWorkerSlot(event.worker_idx);
        bool valid_terminal_order = false;
        if (slot) {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            valid_terminal_order =
                terminal.terminal_order != 0
                && slot->last_workset_terminal_order
                    != std::numeric_limits<std::uint64_t>::max()
                && terminal.terminal_order
                    == slot->last_workset_terminal_order + 1;
            if (valid_terminal_order) {
                slot->last_workset_terminal_order =
                    terminal.terminal_order;
            }
        }
        if (!valid_terminal_order) {
            EmitDurableEventLine(
                "[workflow-workset-terminal-order-rejected] worker="
                + std::to_string(event.worker_idx)
                + " workset="
                + std::to_string(terminal.workset_id)
                + " terminal_order="
                + std::to_string(terminal.terminal_order));
            FailClosedWorker(
                event.worker_idx,
                "workset terminal order is not the exact next order");
            return;
        }
    }

    DispatchedWorksetItemContext item_context{};
    DispatchedJobContext durable_context{};
    bool item_context_found = false;
    bool durable_context_found = false;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        const auto item_it = dispatched_workset_items_.find(
            WorksetItemMapKey(
                event.worker_idx,
                terminal.workset_id,
                terminal.item_id));
        if (item_it != dispatched_workset_items_.end()
            && item_it->second.worker_idx == event.worker_idx) {
            item_context = item_it->second;
            item_context_found = true;
        }
        if (item_context_found) {
            const auto durable_it =
                dispatched_job_context_by_id_.find(
                    static_cast<std::uint64_t>(
                        item_context.claimed.job_id));
            if (durable_it
                != dispatched_job_context_by_id_.end()) {
                durable_context = durable_it->second;
                durable_context_found = true;
            }
        }
    }
    if (!item_context_found) {
        std::ostringstream line;
        line << "[workflow-workset-item-terminal-unmatched]"
             << " worker=" << event.worker_idx
             << " workset=" << terminal.workset_id
             << " item=" << terminal.item_id;
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            terminal.workset_id,
            "item-terminal has no exact retained claim");
        return;
    }
    if (!durable_context_found) {
        std::ostringstream line;
        line << "[workflow-workset-item-terminal-context-missing]"
             << " worker=" << event.worker_idx
             << " workset=" << terminal.workset_id
             << " item=" << terminal.item_id
             << " job=" << item_context.claimed.job_id;
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            terminal.workset_id,
            "durable projection context is missing");
        return;
    }

    if (item_context.item_ordinal != terminal.item_ordinal
        || item_context.invocation_id != terminal.invocation_id
        || item_context.attempt_id != terminal.attempt_id
        || terminal.terminal_id == 0
        || terminal.terminal_order == 0) {
        std::ostringstream line;
        line << "[workflow-workset-item-terminal-correlation-mismatch]"
             << " worker=" << event.worker_idx
             << " workset=" << terminal.workset_id
             << " item=" << terminal.item_id
             << " job=" << item_context.claimed.job_id;
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            terminal.workset_id,
            "item-terminal correlation mismatch");
        return;
    }
    if (!terminal.unstarted && !item_context.start_persisted) {
        EmitDurableEventLine(
            "[workflow-workset-item-terminal-before-start] worker="
            + std::to_string(event.worker_idx)
            + " workset=" + std::to_string(terminal.workset_id)
            + " item=" + std::to_string(terminal.item_id)
            + " job="
            + std::to_string(item_context.claimed.job_id));
        FailClosedWorkset(
            event.worker_idx,
            terminal.workset_id,
            "item terminal arrived before durable JobStarted");
        return;
    }

    std::optional<savor::PRResult> decoded;
    savor::db::execution::workflow::TerminalWorkflowStepNotification
        terminal_notification{};
    std::vector<std::string> post_ack_event_lines;
    bool requires_recovery =
        terminal.unstarted || item_context.authority_lost;
    if (!requires_recovery) {
        std::string decode_error;
        try {
            decoded = worker_cfg_.workset_terminal_decoder
                ? worker_cfg_.workset_terminal_decoder(
                    item_context.claimed,
                    terminal,
                    &decode_error)
                : std::nullopt;
        } catch (const std::exception& ex) {
            decode_error =
                std::string("terminal decoder threw: ") + ex.what();
        } catch (...) {
            decode_error = "terminal decoder threw an unknown exception";
        }
        if (!decoded.has_value()) {
            std::ostringstream line;
            line << "[workflow-workset-item-terminal-decode-failed]"
                 << " worker=" << event.worker_idx
                 << " workset=" << terminal.workset_id
                 << " item=" << terminal.item_id
                 << " job=" << item_context.claimed.job_id;
            if (!decode_error.empty()) {
                line << " error=" << decode_error;
            }
            EmitDurableEventLine(line.str());
            FailClosedWorkset(
                event.worker_idx,
                terminal.workset_id,
                decode_error.empty()
                    ? "workset terminal decoder rejected the result"
                    : decode_error);
            return;
        }

        decoded->job_id =
            static_cast<std::uint64_t>(item_context.claimed.job_id);
        decoded->worker_id = event.worker_idx;
        decoded->epoch = terminal.state_epoch;

        savor::db::ExecutionJobTerminalAuthorityReceipt
            authority_receipt{};
        std::string authority_error;
        bool projection_committed = false;
        {
            // Lease loss and terminal projection share this serialization
            // boundary. The exact DB operation also renews the lease, so
            // ordinary expiry recovery cannot interleave after confirmation
            // and before the existing adapter transaction(s).
            std::lock_guard<std::mutex> projection_lock(
                result_projection_mtx_);
            const bool confirmed = execution_db_ != nullptr
                && execution_db_->ConfirmExecutionJobTerminalAuthority(
                    item_context.claimed.job_id,
                    item_context.claimed.claimed_by_token,
                    item_context.attempt_id,
                    30000,
                    &authority_receipt,
                    &authority_error);
            if (confirmed
                && authority_receipt.disposition
                    == savor::db::
                        ExecutionJobTerminalAuthorityDisposition::Valid) {
                ++results_received_count_;
                projection_committed =
                    ProcessDurableResultProjectionLocked(
                        *decoded,
                        durable_context,
                        &terminal_notification,
                        &post_ack_event_lines);
            } else {
                requires_recovery = true;
            }
        }
        if (requires_recovery) {
            item_context.authority_lost = true;
            {
                std::lock_guard<std::mutex> lock(workers_mtx_);
                const auto it = dispatched_workset_items_.find(
                    WorksetItemMapKey(
                        event.worker_idx,
                        terminal.workset_id,
                        terminal.item_id));
                if (it != dispatched_workset_items_.end()) {
                    it->second.authority_lost = true;
                }
            }
            std::ostringstream line;
            line << "[workflow-workset-terminal-authority-lost]"
                 << " worker=" << event.worker_idx
                 << " workset=" << terminal.workset_id
                 << " item=" << terminal.item_id
                 << " job=" << item_context.claimed.job_id
                 << " disposition="
                 << static_cast<int>(authority_receipt.disposition);
            if (!authority_error.empty()) {
                line << " error=" << authority_error;
            }
            post_ack_event_lines.push_back(line.str());
            decoded.reset();
        } else if (!projection_committed) {
            FailClosedWorkset(
                event.worker_idx,
                terminal.workset_id,
                "durable result projection failed");
            return;
        }
    }

    if (requires_recovery) {
        bool recovery_is_durable = false;
        std::string recovery_error;
        if (execution_db_ != nullptr) {
            savor::db::ExecutionJobWorkerLossRecoveryReceipt
                recovery_receipt{};
            const bool recovered =
                execution_db_->RecoverExecutionJobAfterWorkerLoss(
                    item_context.claimed.job_id,
                    item_context.claimed.claimed_by_token,
                    item_context.attempt_id,
                    terminal.message.empty()
                        ? "WORKSET_ITEM_UNSTARTED"
                        : terminal.message,
                    &recovery_receipt,
                    &recovery_error);
            if (recovered) {
                switch (recovery_receipt.disposition) {
                case savor::db::
                    ExecutionJobWorkerLossRecoveryDisposition::Requeued:
                case savor::db::
                    ExecutionJobWorkerLossRecoveryDisposition::
                        AttemptsExhaustedFailed:
                case savor::db::
                    ExecutionJobWorkerLossRecoveryDisposition::
                        AlreadyDurable:
                case savor::db::
                    ExecutionJobWorkerLossRecoveryDisposition::
                        TokenMismatch:
                case savor::db::
                    ExecutionJobWorkerLossRecoveryDisposition::
                        AttemptMismatch:
                    recovery_is_durable = true;
                    break;
                default:
                    break;
                }
            }
        }
        if (!recovery_is_durable) {
            std::ostringstream line;
            line << "[workflow-workset-unstarted-recovery-failed]"
                 << " worker=" << event.worker_idx
                 << " workset=" << terminal.workset_id
                 << " item=" << terminal.item_id
                 << " job=" << item_context.claimed.job_id;
            if (!recovery_error.empty()) {
                line << " error=" << recovery_error;
            }
            EmitDurableEventLine(line.str());
            FailClosedWorkset(
                event.worker_idx,
                terminal.workset_id,
                "unstarted item could not enter per-job recovery");
            return;
        }
        post_ack_event_lines.push_back(
            "[workflow-workset-unstarted-recovery] worker="
            + std::to_string(event.worker_idx)
            + " workset=" + std::to_string(terminal.workset_id)
            + " item=" + std::to_string(terminal.item_id)
            + " job=" + std::to_string(item_context.claimed.job_id)
            + (item_context.authority_lost
                ? " disposition=authority_lost_recovered"
                : " disposition=requeued"));
    }

    const auto slot = GetWorkerSlot(event.worker_idx);
    std::shared_ptr<savor::ProcessWorker> worker;
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        worker = slot->worker;
    }
    const savor::runtime::WorkerItemTerminalCorrelation correlation{
        .workset_id =
            savor::runtime::WorkerWorksetId{terminal.workset_id},
        .item_id =
            savor::runtime::WorkerWorksetItemId{terminal.item_id},
        .item_ordinal = terminal.item_ordinal,
        .invocation_id =
            savor::runtime::InvocationId{terminal.invocation_id},
        .attempt_id =
            savor::runtime::AttemptId{terminal.attempt_id},
        .terminal_id =
            savor::runtime::WorkerTerminalId{terminal.terminal_id},
        .terminal_order =
            savor::runtime::WorkerTerminalOrder{
                terminal.terminal_order},
    };
    if (!worker || !worker->acknowledge_terminal(correlation)) {
        std::ostringstream line;
        line << "[workflow-workset-item-terminal-ack-failed]"
             << " worker=" << event.worker_idx
             << " workset=" << terminal.workset_id
             << " item=" << terminal.item_id
             << " job=" << item_context.claimed.job_id;
        if (worker) {
            const auto worker_error = worker->last_error();
            if (!worker_error.empty()) {
                line << " error=" << worker_error;
            }
        }
        EmitDurableEventLine(line.str());
        FailClosedWorkset(
            event.worker_idx,
            terminal.workset_id,
            "terminal acknowledgement failed after durable projection");
        return;
    }
    // The terminal's existing durable projection/recovery is authoritative at
    // this point. Observational callbacks and targeted workflow advancement
    // run only after the exact acknowledgement and cannot hold its bounded
    // worker ledger slot.
    for (const auto& line : post_ack_event_lines) {
        EmitDurableEventLine(line);
    }
    PublishTerminalCommitIsolated(terminal_notification);

    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        dispatched_workset_items_.erase(
            WorksetItemMapKey(
                event.worker_idx,
                terminal.workset_id,
                terminal.item_id));
        dispatched_job_context_by_id_.erase(
            static_cast<std::uint64_t>(
                item_context.claimed.job_id));
    }
    (void)job_materialization_service_.CleanupDispatchedOrExpired(
        item_context.claimed.job_id);
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (slot->active_workset_id.has_value()
            && *slot->active_workset_id == terminal.workset_id
            && slot->in_flight_job_id.has_value()
            && *slot->in_flight_job_id
                == static_cast<std::uint64_t>(
                    item_context.claimed.job_id)) {
            worker_status_.SetCurrentJob(
                static_cast<std::int64_t>(slot->id),
                std::nullopt,
                std::nullopt);
            worker_status_.UpdateState(
                static_cast<std::int64_t>(slot->id),
                WorkerStateKind::Leasing);
        }
        worker_status_.RecordHeartbeat(
            static_cast<std::int64_t>(slot->id));
    }
    if (decoded.has_value() && result_callback_) {
        try {
            result_callback_(*decoded);
        } catch (const std::exception& ex) {
            EmitDurableEventLine(
                "[workflow-result-callback-failed] job="
                + std::to_string(item_context.claimed.job_id)
                + " error=" + ex.what());
        } catch (...) {
            EmitDurableEventLine(
                "[workflow-result-callback-failed] job="
                + std::to_string(item_context.claimed.job_id)
                + " error=unknown_exception");
        }
    }
}

void DBWorkflowWorkerCoordinator::HandleWorksetCredits(
    const WorksetEventIngress& event) {
    if (!AcceptWorksetEventSequence(
            event.worker_idx,
            0,
            event.credits.outbound_sequence,
            "credits")) {
        FailClosedWorker(
            event.worker_idx,
            "invalid or out-of-order workset-credits event");
        return;
    }
    RefreshSharedItemCredits();
    queue_cv_.notify_all();
}

void DBWorkflowWorkerCoordinator::HandleWorksetSummary(
    const WorksetEventIngress& event) {
    const auto& summary = event.summary;
    if (!AcceptWorksetEventSequence(
            event.worker_idx,
            summary.workset_id,
            summary.outbound_sequence,
            "summary")) {
        FailClosedWorker(
            event.worker_idx,
            "workset summary sequence integrity failed");
        return;
    }

    {
        const auto slot = GetWorkerSlot(event.worker_idx);
        std::optional<std::size_t> expected_item_count;
        if (slot) {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            const auto count_it =
                slot->retained_workset_item_counts.find(
                    summary.workset_id);
            if (count_it
                != slot->retained_workset_item_counts.end()) {
                expected_item_count = count_it->second;
            }
        }
        const auto classified_count =
            static_cast<std::uint64_t>(summary.completed_count)
            + static_cast<std::uint64_t>(
                summary.unstarted_count);
        if (!expected_item_count.has_value()
            || summary.item_count != *expected_item_count
            || classified_count != summary.item_count) {
            EmitDurableEventLine(
                "[workflow-workset-summary-correlation-mismatch] "
                "worker="
                + std::to_string(event.worker_idx)
                + " workset="
                + std::to_string(summary.workset_id));
            FailClosedWorkset(
                event.worker_idx,
                summary.workset_id,
                "workset summary counts do not match admission");
            return;
        }
    }

    bool retained_item = false;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        retained_item = std::any_of(
            dispatched_workset_items_.begin(),
            dispatched_workset_items_.end(),
            [&](const auto& pair) {
                return pair.second.worker_idx == event.worker_idx
                    && pair.second.workset_id
                        == summary.workset_id;
            });
    }
    if (retained_item) {
        EmitDurableEventLine(
            "[workflow-workset-summary-before-terminal-ack] worker="
            + std::to_string(event.worker_idx)
            + " workset=" + std::to_string(summary.workset_id));
        FailClosedWorkset(
            event.worker_idx,
            summary.workset_id,
            "workset summary arrived before all terminals were acknowledged");
        return;
    }

    const auto slot = GetWorkerSlot(event.worker_idx);
    if (!slot) {
        return;
    }
    std::shared_ptr<savor::ProcessWorker> worker_to_stop;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        slot->retained_workset_ids.erase(summary.workset_id);
        slot->retained_workset_item_counts.erase(
            summary.workset_id);
        slot->failed_closed_workset_ids.erase(
            summary.workset_id);
        if (slot->active_workset_id.has_value()
            && *slot->active_workset_id
                == summary.workset_id) {
            slot->active_workset_id.reset();
        }
        if (slot->staged_workset_id.has_value()
            && *slot->staged_workset_id
                == summary.workset_id) {
            slot->staged_workset_id.reset();
        }
        if (slot->failed_closed_workset_ids.empty()) {
            slot->workset_admission_blocked = false;
        }
        const bool has_resident_workset =
            slot->active_workset_id.has_value()
            || slot->staged_workset_id.has_value();
        if (!has_resident_workset) {
            slot->in_flight_job_id.reset();
            slot->in_flight_started_at = {};
            slot->dead_in_flight_observed_at = {};
            worker_status_.SetCurrentJob(
                static_cast<std::int64_t>(slot->id),
                std::nullopt,
                std::nullopt);
        }
        if (!has_resident_workset
            && slot->workset_admission_blocked) {
            // A prior completion-ledger terminal is deliberately retained
            // without ACK. Once the successor drains, stop this process so
            // individual lease/attempt recovery becomes authoritative.
            slot->last_worker_contact_at =
                std::chrono::steady_clock::now();
            slot->ready.store(false, std::memory_order_release);
            worker_to_stop = slot->worker;
            MarkWorkerError(
                *slot,
                "worker drained with an unacknowledged fail-closed "
                "workset terminal");
        } else if (slot->worker && slot->worker->is_ready()
            && !has_resident_workset) {
            slot->last_worker_contact_at = {};
            worker_status_.UpdateState(
                static_cast<std::int64_t>(slot->id),
                WorkerStateKind::Idle);
        } else if (slot->worker && slot->worker->is_ready()
            && !slot->in_flight_job_id.has_value()) {
            worker_status_.UpdateState(
                static_cast<std::int64_t>(slot->id),
                WorkerStateKind::Leasing);
        } else if (!worker_to_stop) {
            MarkWorkerError(
                *slot,
                "worker became unavailable after workset summary");
        }
    }
    if (worker_to_stop) {
        worker_to_stop->stop();
    }
    queue_cv_.notify_all();
}

bool DBWorkflowWorkerCoordinator::ValidateBuiltWorkset(
    const savor::runtime::WorkerWorksetDefinition& workset,
    const std::vector<ClaimedJobRecord>& claimed_jobs,
    const savor::runtime::WorkerRuntimeManifest& manifest,
    std::string* error_out) const {
    const auto validation =
        savor::runtime::ValidateWorkerWorksetDefinition(
            workset,
            manifest.limits);
    if (!validation.ok) {
        if (error_out != nullptr) {
            *error_out = validation.error.message;
        }
        return false;
    }
    if (workset.items.size() != claimed_jobs.size()
        || workset.items.size()
            > manifest.limits.maximum_active_and_staged_items) {
        if (error_out != nullptr) {
            *error_out = "built workset membership does not match the "
                "selected claimed items or negotiated resident limit";
        }
        return false;
    }
    const auto module = std::find_if(
        manifest.modules.begin(),
        manifest.modules.end(),
        [&](const auto& entry) {
            return entry.module == workset.execution_key.module;
        });
    if (module == manifest.modules.end()
        || std::find(
            module->entrypoints.begin(),
            module->entrypoints.end(),
            std::string(workset.execution_key.entrypoint))
            == module->entrypoints.end()) {
        if (error_out != nullptr) {
            *error_out =
                "built workset module/entrypoint is absent from the "
                "negotiated worker catalog";
        }
        return false;
    }
    for (std::size_t index = 0; index < claimed_jobs.size(); ++index) {
        const auto& claimed = claimed_jobs[index];
        const auto& item = workset.items[index];
        if (claimed.workset_execution_key
                != workset.execution_key.canonical_sha256
            || item.correlation.durable_job_id
                != std::to_string(claimed.job_id)
            || item.correlation.claim_token
                != claimed.claimed_by_token
            || claimed.durable_attempt_id == 0
            || item.invocation.attempt_id.value()
                != claimed.durable_attempt_id) {
            if (error_out != nullptr) {
                *error_out =
                    "built workset order, execution key, durable job, or "
                    "claim-token/attempt correlation does not match "
                    "selection";
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::DispatchClaimedWorksetToWorker(
    size_t worker_idx,
    const std::vector<ClaimedJobRecord>& claimed_jobs) {
    if (!IsDataPlaneEnabled() || claimed_jobs.empty()
        || !worker_cfg_.workset_definition_builder) {
        return false;
    }
    const auto slot = GetWorkerSlot(worker_idx);
    if (!slot) {
        return false;
    }

    std::shared_ptr<savor::ProcessWorker> worker;
    savor::runtime::WorkerRuntimeManifest manifest{};
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (!slot->ready.load(std::memory_order_acquire)
            || slot->workset_admission_blocked
            || slot->staged_workset_id.has_value()
            || !slot->runtime_manifest.has_value()
            || !slot->worker) {
            return false;
        }
        worker = slot->worker;
        manifest = *slot->runtime_manifest;
    }

    std::string error;
    auto workset = worker_cfg_.workset_definition_builder(
        worker_idx,
        claimed_jobs,
        manifest,
        &error);
    if (!workset.has_value()
        || !ValidateBuiltWorkset(
            *workset,
            claimed_jobs,
            manifest,
            &error)) {
        std::ostringstream line;
        line << "[workflow-workset-build-rejected]"
             << " worker=" << worker_idx
             << " items=" << claimed_jobs.size();
        if (!error.empty()) {
            line << " error=" << error;
        }
        EmitDurableEventLine(line.str());
        return false;
    }
    if (!ValidateClaimedWorksetAuthority(claimed_jobs)) {
        return false;
    }

    for (const auto& claimed : claimed_jobs) {
        if (adapter_chain_orchestrator_) {
            savor::db::execution::workflow::AdapterChainTrace trace{};
            (void)adapter_chain_orchestrator_->OnJobClaimed(
                claimed.step.step_kind,
                claimed.job_id,
                &trace);
            ++adapter_job_claimed_invocations_;
            EmitAdapterTraceEvent(
                claimed.step,
                "OnJobClaimed",
                "invoked",
                claimed.job_id,
                claimed.job_set_id);
        }
    }

    std::unique_lock<std::mutex> submission_lock(
        slot->workset_submission_mtx);
    const auto workset_id = workset->workset_id.value();
    bool staged_submission = false;
    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (!slot->ready.load(std::memory_order_acquire)
            || slot->workset_admission_blocked
            || slot->staged_workset_id.has_value()
            || slot->retained_workset_ids.contains(workset_id)
            || slot->worker != worker) {
            return false;
        }
        staged_submission = slot->active_workset_id.has_value();
        if (staged_submission) {
            slot->staged_workset_id = workset_id;
        } else {
            slot->active_workset_id = workset_id;
            slot->in_flight_job_id =
                static_cast<std::uint64_t>(
                    claimed_jobs.front().job_id);
            slot->in_flight_started_at =
                std::chrono::steady_clock::now();
            slot->last_worker_contact_at =
                slot->in_flight_started_at;
            slot->dead_in_flight_observed_at = {};
        }
        slot->retained_workset_ids.insert(workset_id);
        slot->retained_workset_item_counts[workset_id] =
            claimed_jobs.size();
        worker_status_.UpdateState(
            static_cast<std::int64_t>(slot->id),
            WorkerStateKind::Leasing);
    }

    // Register exact provisional contexts before SubmitWorkset. A worker may
    // publish ItemStarted as soon as admission succeeds; the callback drainer
    // must already be able to resolve its durable claim and projection
    // context.
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (std::size_t index = 0; index < claimed_jobs.size(); ++index) {
            const auto& claimed = claimed_jobs[index];
            const auto& item = workset->items[index];
            dispatched_job_context_by_id_[
                static_cast<std::uint64_t>(claimed.job_id)] =
                DispatchedJobContext{
                    .step = claimed.step,
                    .job_set_id = claimed.job_set_id,
                };
            dispatched_workset_items_[WorksetItemMapKey(
                worker_idx,
                workset_id,
                item.item_id.value())] =
                DispatchedWorksetItemContext{
                    .worker_idx = worker_idx,
                    .workset_id = workset_id,
                    .item_id = item.item_id.value(),
                    .item_ordinal = item.ordinal,
                    .invocation_id =
                        item.invocation.invocation_id.value(),
                    .attempt_id = item.invocation.attempt_id.value(),
                    .claimed = claimed,
                };
        }
    }

    const auto submit_outcome =
        worker->submit_workset_with_outcome(*workset);
    if (!submit_outcome.accepted()) {
        std::ostringstream line;
        line << (submit_outcome.disposition
                    == savor::ProcessWorksetSubmitDisposition::
                        AmbiguousAfterWrite
                ? "[workflow-workset-submit-ambiguous]"
                : "[workflow-workset-submit-rejected]")
             << " worker=" << worker_idx
             << " workset=" << workset->workset_id.value()
             << " items=" << claimed_jobs.size()
             << " frame_written="
             << (submit_outcome.request_frame_written ? 1 : 0)
             << " correlated_result="
             << (submit_outcome.correlated_result_received ? 1 : 0);
        if (!submit_outcome.diagnostic.empty()) {
            line << " error=" << submit_outcome.diagnostic;
        }
        EmitDurableEventLine(line.str());

        if (submit_outcome.disposition
            == savor::ProcessWorksetSubmitDisposition::
                AmbiguousAfterWrite) {
            // The child may already own the workset. Preserve every exact
            // claim/correlation record, prevent any local requeue, and
            // quarantine the process. Normal dead-worker recovery will
            // classify each retained item from its durable state.
            bool local_dispatch_state_complete = true;
            for (const auto& claimed : claimed_jobs) {
                if (!job_materialization_service_.MarkDispatched(
                        claimed.job_id,
                        std::chrono::steady_clock::now())) {
                    local_dispatch_state_complete = false;
                    EmitDurableEventLine(
                        "[workflow-workset-ambiguous-local-state-missed] "
                        "worker=" + std::to_string(worker_idx)
                        + " workset=" + std::to_string(workset_id)
                        + " job=" + std::to_string(claimed.job_id));
                }
            }
            {
                std::lock_guard<std::mutex> slot_lock(slot->mtx);
                slot->workset_admission_blocked = true;
                slot->ready.store(false, std::memory_order_release);
                slot->last_worker_contact_at =
                    std::chrono::steady_clock::now();
                MarkWorkerError(
                    *slot,
                    "SubmitWorkset outcome was ambiguous after the "
                    "request frame was written");
            }
            EmitDurableEventLine(
                "[workflow-workset-submit-ambiguous-quarantine] worker="
                + std::to_string(worker_idx)
                + " workset=" + std::to_string(workset_id)
                + " local_dispatch_state_complete="
                + (local_dispatch_state_complete ? "1" : "0"));
            StopWorkerSlot(slot, true);
            // Returning true transfers the selected records into retained
            // recovery ownership. The caller must not put them back into
            // the coordinator materialized queue.
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(workers_mtx_);
            for (std::size_t index = 0;
                 index < claimed_jobs.size();
                 ++index) {
                dispatched_job_context_by_id_.erase(
                    static_cast<std::uint64_t>(
                        claimed_jobs[index].job_id));
                dispatched_workset_items_.erase(
                    WorksetItemMapKey(
                        worker_idx,
                        workset_id,
                        workset->items[index].item_id.value()));
            }
        }
        {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            slot->retained_workset_ids.erase(workset_id);
            slot->retained_workset_item_counts.erase(workset_id);
            if (slot->active_workset_id.has_value()
                && *slot->active_workset_id == workset_id) {
                slot->active_workset_id.reset();
            }
            if (slot->staged_workset_id.has_value()
                && *slot->staged_workset_id == workset_id) {
                slot->staged_workset_id.reset();
            }
            if (!slot->active_workset_id.has_value()
                && !slot->staged_workset_id.has_value()) {
                slot->in_flight_job_id.reset();
                slot->in_flight_started_at = {};
                slot->last_worker_contact_at = {};
                slot->dead_in_flight_observed_at = {};
                worker_status_.UpdateState(
                    static_cast<std::int64_t>(slot->id),
                    WorkerStateKind::Idle);
            }
        }
        return false;
    }

    bool local_dispatch_state_complete = true;
    for (const auto& claimed : claimed_jobs) {
        if (!job_materialization_service_.MarkDispatched(
                claimed.job_id,
                std::chrono::steady_clock::now())) {
            local_dispatch_state_complete = false;
            std::ostringstream line;
            line << "[workflow-workset-local-dispatch-state-missed]"
                 << " worker=" << worker_idx
                 << " job=" << claimed.job_id;
            EmitDurableEventLine(line.str());
        }
    }

    {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        const auto& anchor = claimed_jobs.front();
        ++slot->dispatch_success_count;
        if (!staged_submission) {
            slot->loaded_program_kind = anchor.program_kind;
            slot->loaded_program_runtime_affinity_key =
                anchor.affinity.program_runtime_affinity_key;
            slot->loaded_savestate_affinity_key =
                anchor.affinity.savestate_affinity_key;
            slot->loaded_workset_execution_key =
                anchor.workset_execution_key;
        }
        worker_status_.UpdateState(
            static_cast<std::int64_t>(slot->id),
            WorkerStateKind::Leasing);
        worker_status_.RecordHeartbeat(
            static_cast<std::int64_t>(slot->id));
    }
    if (!local_dispatch_state_complete) {
        FailClosedWorkset(
            worker_idx,
            workset_id,
            "accepted workset could not enter local dispatched state");
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::DispatchNextEligibleForWorker(
    size_t worker_idx,
    const MaterializedJobSelectionAffinity& worker_affinity,
    std::chrono::steady_clock::time_point now) {
    if (!IsDataPlaneEnabled()
        || !worker_cfg_.workset_definition_builder) {
        return false;
    }
    const auto slot = GetWorkerSlot(worker_idx);
    std::size_t maximum_items = 1;
    std::size_t maximum_encoded_bytes = 1;
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (slot->workset_admission_blocked
            || slot->staged_workset_id.has_value()) {
            return false;
        }
        if (slot->runtime_manifest.has_value()) {
            const auto resident_limit = static_cast<std::size_t>(
                slot->runtime_manifest->limits
                    .maximum_active_and_staged_items);
            std::size_t resident_items = 0;
            const auto add_resident =
                [&](const std::optional<std::uint64_t>& workset_id) {
                    if (!workset_id.has_value()) {
                        return;
                    }
                    const auto count =
                        slot->retained_workset_item_counts.find(
                            *workset_id);
                    if (count !=
                        slot->retained_workset_item_counts.end()) {
                        resident_items += count->second;
                    }
                };
            add_resident(slot->active_workset_id);
            add_resident(slot->staged_workset_id);
            if (resident_items >= resident_limit) {
                return false;
            }
            maximum_items = std::max<std::size_t>(
                1,
                std::min<std::size_t>(
                    slot->runtime_manifest->limits
                        .maximum_items_per_workset,
                    resident_limit - resident_items));
            maximum_encoded_bytes = std::max<std::size_t>(
                1,
                slot->runtime_manifest->limits
                    .maximum_encoded_workset_bytes);
        }
        if (!slot->worker) {
            return false;
        }
        const auto available_credits =
            slot->worker->latest_snapshot()
                .available_item_credits;
        if (available_credits == 0) {
            return false;
        }
        maximum_items = std::min<std::size_t>(
            maximum_items,
            available_credits);
    }
    std::vector<ClaimedJobRecord> candidates;
    if (!job_materialization_service_.
            TrySelectMaterializedWorksetForWorker(
                worker_affinity,
                MaterializedWorksetSelectionLimits{
                    .max_items = maximum_items,
                    .lookahead_items = std::max<std::size_t>(
                        1,
                        worker_cfg_.workset_lookahead_items),
                    .max_selected_bytes = maximum_encoded_bytes,
                    .lookahead_bytes = std::max<std::size_t>(
                        1,
                        worker_cfg_.workset_lookahead_bytes),
                },
                &candidates)
        || candidates.empty()) {
        return false;
    }
    const auto& anchor = candidates.front();
    const bool dispatched =
        DispatchClaimedWorksetToWorker(worker_idx, candidates);
    if (!dispatched) {
        std::size_t requeued = 0;
        for (const auto& candidate : candidates) {
            if (job_materialization_service_.RequeueMaterializedJob(
                    candidate.job_id)) {
                ++requeued;
            }
        }
        std::ostringstream line;
        line << "[workflow-workset-dispatch-requeue]"
             << " anchor_job=" << anchor.job_id
             << " worker=" << worker_idx
             << " items=" << candidates.size()
             << " reason=submit_failed"
             << " requeued=" << requeued;
        EmitDurableEventLine(line.str());
        return false;
    }
    (void)now;
    std::ostringstream line;
    line << "[workflow-workset-dispatch]"
         << " anchor_job=" << anchor.job_id
         << " worker=" << worker_idx
         << " items=" << candidates.size()
         << " step=" << anchor.step.step_key
         << " kind=" << anchor.step.step_kind
         << " workflow_step_id=" << anchor.step.workflow_step_id
         << " job_set=" << anchor.job_set_id
         << " program_kind=" << anchor.program_kind
         << " claim_sequence=" << anchor.claim_sequence;
    if (anchor.affinity.savestate_affinity_key.has_value()) {
        line << " savestate_affinity="
             << *anchor.affinity.savestate_affinity_key;
    }
    if (anchor.affinity.program_runtime_affinity_key.has_value()) {
        line << " runtime_affinity="
             << *anchor.affinity.program_runtime_affinity_key;
    }
    EmitDurableEventLine(line.str());
    return true;
}

void DBWorkflowWorkerCoordinator::HandlePayloadMaterializationFailures() {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    for (const auto& failed_payload : job_materialization_service_.ListByState(ClaimedJobLifecycleState::MaterializationFailed)) {
        ++payload_materialization_failure_count_;
        std::ostringstream message;
        message << "job_id=" << failed_payload.job_id << ";job_set_id=" << failed_payload.job_set_id
                << ";reason=build_payload returned empty";
        EmitWorkflowFailureEvents(failed_payload.step, "BuildClaimedPayload", message.str());
        MaybeTerminalFailStepInStrictSmokeMode(failed_payload.step, "workflow_payload_materialize_strict_smoke");
        if (execution_db_ != nullptr) {
            std::string error;
            if (!execution_db_->RequeueClaimedExecutionJob(
                    failed_payload.job_id,
                    failed_payload.claimed_by_token,
                    "PAYLOAD_MATERIALIZATION_FAILED",
                    &error)) {
                std::ostringstream line;
                line << "[seedprobe-payload-requeue-failed] job=" << failed_payload.job_id;
                if (!error.empty()) {
                    line << " error=" << error;
                }
                EmitDurableEventLine(line.str());
            }
        }
        (void)job_materialization_service_.AbandonClaim(failed_payload.job_id);
    }
}

void DBWorkflowWorkerCoordinator::HandleItemAuthorityLost(
    const ClaimLeaseMaintenanceResult::LostAuthority& lost) {
    std::optional<DispatchedWorksetItemContext> context;
    {
        // Serialize the local authority bit with exact terminal confirmation
        // and durable result projection. Once this block exits, a terminal
        // can only observe authority_lost or reconfirm exact DB authority.
        std::lock_guard<std::mutex> projection_lock(
            result_projection_mtx_);
        {
            std::lock_guard<std::mutex> lock(workers_mtx_);
            for (auto& [_, candidate] : dispatched_workset_items_) {
                if (candidate.claimed.job_id == lost.job_id
                    && candidate.claimed.claimed_by_token
                        == lost.claimed_by_token) {
                    candidate.authority_lost = true;
                    context = candidate;
                    break;
                }
            }
        }
        // Buffered/materializing work has not crossed the worker boundary.
        // Removing it locally is sufficient; its durable owner/state is
        // already authoritative and normal lease recovery can make it
        // eligible again.
        (void)job_materialization_service_.AbandonClaim(lost.job_id);
    }

    if (context.has_value()) {
        const auto slot = GetWorkerSlot(context->worker_idx);
        std::shared_ptr<savor::ProcessWorker> worker;
        if (slot) {
            std::lock_guard<std::mutex> slot_lock(slot->mtx);
            worker = slot->worker;
        }
        const bool cancellation_delivered =
            worker
            && worker->cancel_workset_item(
                savor::runtime::WorkerWorksetId{context->workset_id},
                savor::runtime::WorkerWorksetItemId{context->item_id},
                "durable claim or lease authority was lost");
        if (!cancellation_delivered) {
            FailClosedWorkset(
                context->worker_idx,
                context->workset_id,
                "exact item cancellation failed after durable authority loss");
        }
    }

    if (worker_cfg_.item_authority_lost_callback) {
        try {
            worker_cfg_.item_authority_lost_callback(
                lost.job_id,
                lost.claimed_by_token,
                lost.disposition);
        } catch (const std::exception& ex) {
            EmitDurableEventLine(
                "[workflow-item-authority-callback-failed] job="
                + std::to_string(lost.job_id) + " error=" + ex.what());
        } catch (...) {
            EmitDurableEventLine(
                "[workflow-item-authority-callback-failed] job="
                + std::to_string(lost.job_id)
                + " error=unknown_exception");
        }
    }
}

void DBWorkflowWorkerCoordinator::MaintainMaterializerClaims(std::chrono::steady_clock::time_point now) {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    constexpr auto kLeaseMaintenanceCadence = std::chrono::seconds(10);
    constexpr auto kLeaseDuration = std::chrono::seconds(30);

    if (last_claim_lease_maintenance_ != std::chrono::steady_clock::time_point{}
        && now - last_claim_lease_maintenance_ < kLeaseMaintenanceCadence) {
        return;
    }
    last_claim_lease_maintenance_ = now;

    const auto renewed = job_materialization_service_.RenewActiveClaimLeases(
        std::chrono::duration_cast<std::chrono::milliseconds>(kLeaseDuration));
    if (renewed.attempted > 0 || renewed.failed > 0) {
        std::ostringstream line;
        line << "[workflow-claim-lease-renew] attempted=" << renewed.attempted
             << " renewed=" << renewed.renewed
             << " failed=" << renewed.failed;
        EmitDurableEventLine(line.str());
    }
    for (const auto& lost : renewed.lost_authority) {
        HandleItemAuthorityLost(lost);
    }

    if (execution_db_ == nullptr) {
        return;
    }

    int rows_requeued = 0;
    std::string error;
    if (!execution_db_->RequeueExpiredClaimedExecutionJobs(&rows_requeued, &error)) {
        std::ostringstream line;
        line << "[workflow-claim-recovery-failed]";
        if (!error.empty()) {
            line << " error=" << error;
        }
        EmitDurableEventLine(line.str());
        return;
    }
    if (rows_requeued > 0) {
        std::ostringstream line;
        line << "[workflow-claim-recovery] requeued=" << rows_requeued;
        EmitDurableEventLine(line.str());
    }
}

size_t DBWorkflowWorkerCoordinator::ActiveWorkerCount() const {
    return worker_slot_count_.load(std::memory_order_relaxed);
}

void DBWorkflowWorkerCoordinator::SetProgressCallback(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void DBWorkflowWorkerCoordinator::SetResultCallback(ResultCallback callback) {
    result_callback_ = std::move(callback);
}

void DBWorkflowWorkerCoordinator::SetResultMapEventCallback(ResultMapEventCallback callback) {
    result_map_event_callback_ = std::move(callback);
    job_materialization_service_.SetEventCallback([this](const std::string& line) {
        EmitDurableEventLine(line);
    });
}

void DBWorkflowWorkerCoordinator::EnqueueProgressForTest(const savor::PRProgress& progress) {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    progress_q_.push(progress);
}

void DBWorkflowWorkerCoordinator::EnqueueResultForTest(const savor::PRResult& result) {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    results_q_.push(result);
}

bool DBWorkflowWorkerCoordinator::MaterializeWorkerRuntimeForTest(
    size_t worker_idx,
    const DBWorkflowWorkerCoordinatorConfig& worker_cfg,
    std::filesystem::path* runtime_worker_exe_out,
    std::string* error_out) {
    return EnsureWorkflowWorkerRuntimeSlot(worker_idx, worker_cfg, runtime_worker_exe_out, error_out);
}

PRStatus DBWorkflowWorkerCoordinator::SnapshotStatus() const {
    PRStatus status{};
    status.epoch = epoch_.load();
    status.workers = ActiveWorkerCount();
    status.ready_workers = ReadyWorksetCapableWorkerCount();

    std::lock_guard<std::mutex> lock(queue_mtx_);
    status.queued_jobs = ready_queue_.size();
    status.running_workers = materialized_count_;
    status.pending_start_workers = terminal_published_count_;
    return status;
}

WorkflowCoordinatorTelemetry DBWorkflowWorkerCoordinator::SnapshotTelemetry() const {
    WorkflowCoordinatorTelemetry telemetry{};
    telemetry.ready_scan_count = ready_scan_count_.load();
    telemetry.ready_steps_enqueued = ready_steps_enqueued_.load();
    telemetry.last_ready_scan_latency_ms = last_ready_scan_latency_ms_.load();
    telemetry.max_ready_queue_depth = max_ready_queue_depth_.load();
    telemetry.input_complete_count = input_complete_count_.load();
    telemetry.input_timeout_count = input_timeout_count_.load();
    telemetry.terminal_input_failure_count = terminal_input_failure_count_.load();
    telemetry.last_input_latency_ms = last_input_latency_ms_.load();
    telemetry.materialization_count = materialization_count_.load();
    telemetry.last_materialization_latency_ms = last_materialization_latency_ms_.load();
    telemetry.max_materialization_latency_ms = max_materialization_latency_ms_.load();
    telemetry.stale_claim_count = stale_claim_count_.load();
    telemetry.dispatch_attempt_count = dispatch_attempt_count_.load();
    telemetry.dispatch_success_count = dispatch_success_count_.load();
    telemetry.dispatch_miss_count = dispatch_miss_count_.load();
    const auto attempts = telemetry.dispatch_attempt_count;
    telemetry.dispatch_miss_rate_basis_points = attempts <= 0
        ? 0
        : static_cast<std::int64_t>((telemetry.dispatch_miss_count * 10000) / attempts);
    telemetry.progress_batch_count = progress_batch_count_.load();
    telemetry.max_progress_batch_size = max_progress_batch_size_.load();
    telemetry.results_received_count = results_received_count_.load();
    telemetry.workflow_created_signal_count = workflow_created_signal_count_.load();
    telemetry.materialization_failure_count = materialization_failure_count_.load();
    telemetry.payload_materialization_failure_count = payload_materialization_failure_count_.load();
    telemetry.claim_attempt_count = claim_attempt_count_.load();
    telemetry.claimed_job_count = claimed_job_count_.load();
    telemetry.clean_zero_claim_count = clean_zero_claim_count_.load();
    telemetry.claim_error_count = claim_error_count_.load();
    telemetry.partial_claim_count = partial_claim_count_.load();
    telemetry.no_jobs_available = no_jobs_available_.load();
    const auto workers = CopyWorkerSlots();
    telemetry.workers.reserve(workers.size());
    for (const auto& worker : workers) {
        if (!worker) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(worker->mtx);
        telemetry.workers.push_back(WorkflowCoordinatorTelemetry::WorkerEfficiency{
            .worker_id = static_cast<std::int64_t>(worker->id),
            .dispatch_success_count = worker->dispatch_success_count,
            .program_kind_switch_count = worker->program_kind_switch_count,
        });
    }
    return telemetry;
}

std::vector<CoordinatorWarningSnapshot> DBWorkflowWorkerCoordinator::SnapshotWarnings() const {
    std::lock_guard<std::mutex> lock(coordinator_warning_mtx_);
    return std::vector<CoordinatorWarningSnapshot>(
        coordinator_warnings_.begin(),
        coordinator_warnings_.end());
}

std::vector<WorkerSnapshot> DBWorkflowWorkerCoordinator::SnapshotWorkers() const {
    return worker_status_.GetClusterSnapshot();
}

bool DBWorkflowWorkerCoordinator::SetWorkerVisualSurface(
    size_t worker_idx,
    uint64_t render_widget_handle,
    std::string host_events_pipe_name) {
    WorkerSlotPtr slot;
    std::string stored_pipe_name = std::move(host_events_pipe_name);
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        worker_visual_surfaces_[worker_idx] = WorkerVisualSurface{
            .render_widget_handle = render_widget_handle,
            .host_events_pipe_name = stored_pipe_name,
        };
        if (worker_idx < workers_.size()) {
            slot = workers_[worker_idx];
        }
    }
    if (slot) {
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        slot->visual_render_widget_handle = render_widget_handle;
        slot->visual_host_events_pipe_name = stored_pipe_name;
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::StartVisualDebugReplay(
    std::int64_t,
    uint64_t,
    std::string,
    std::string* error_out) {
    if (error_out) {
        *error_out =
            "interactive visual debugging is unavailable until "
            "Dependency Slice 3";
    }
    return false;
}

bool DBWorkflowWorkerCoordinator::StopVisualDebugReplay() {
    return false;
}

bool DBWorkflowWorkerCoordinator::PauseVisualDebugReplayEmulation() {
    return false;
}

bool DBWorkflowWorkerCoordinator::ResumeVisualDebugReplayEmulation() {
    return false;
}

bool DBWorkflowWorkerCoordinator::StepVisualDebugReplayVm() {
    return false;
}

VisualDebugReplaySnapshot DBWorkflowWorkerCoordinator::SnapshotVisualDebugReplay() const {
    return {};
}

std::vector<std::string> DBWorkflowWorkerCoordinator::TakeVisualDebugLogLines() {
    return {};
}

void DBWorkflowWorkerCoordinator::WorkerJobCoordinatorLoop() {
    while (!stop_.load() && IsDataPlaneEnabled()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        RecoverDeadInFlightWorkers();
        MaintainMaterializerClaims(now);
        RefreshSharedItemCredits();
        const auto worker_target = ReadyItemCreditCapacity();
        const auto coordinator_buffer_target =
            ReadyCoordinatorBufferCapacity();
        const auto buffered = job_materialization_service_.CountBufferedJobs();
        auto capacity = worker_cfg_.worker_item_usage_provider
            ? worker_cfg_.worker_item_usage_provider()
            : CoordinatorItemCapacitySnapshot{};
        capacity.total_credits = worker_target;
        capacity.coordinator_buffered = buffered;
        if (!worker_cfg_.worker_item_usage_provider) {
            capacity.active_invocations = CountActiveInFlightItems();
        }
        const auto available_claim_credits =
            capacity.AvailableCredits();
        const auto available_buffer_credits =
            coordinator_buffer_target > buffered
            ? coordinator_buffer_target - buffered
            : 0;
        const auto materialized_before_claim = job_materialization_service_.CountMaterializedJobs();
        if (buffered >= worker_target || materialized_before_claim > 0) {
            no_jobs_available_.store(false);
        }
        std::size_t claimed_count = 0;
        if (available_claim_credits > 0
            && available_buffer_credits > 0) {
            const auto claim_budget = std::min(
                available_claim_credits,
                available_buffer_credits);
            const auto claim_result = job_materialization_service_.ClaimJobsDetailed(claim_budget, now);
            if (claim_result.attempted) {
                ++claim_attempt_count_;
                claimed_job_count_.fetch_add(static_cast<std::int64_t>(claim_result.claimed));
                if (claim_result.error) {
                    ++claim_error_count_;
                    no_jobs_available_.store(false);
                    std::ostringstream line;
                    line << "[seedprobe-claim-error] claimed=" << claim_result.claimed
                         << " requested=" << claim_result.requested
                         << " budget=" << claim_budget
                         << " buffered_before=" << buffered
                     << " materialized_before=" << materialized_before_claim
                     << " worker_target=" << worker_target
                     << " coordinator_buffer_target="
                     << coordinator_buffer_target
                     << " consumed_credits=" << capacity.ConsumedCredits()
                         << " buffered_after=" << job_materialization_service_.CountBufferedJobs()
                         << " materialized_after=" << job_materialization_service_.CountMaterializedJobs()
                         << " error=" << (claim_result.error_message.empty() ? "unknown" : claim_result.error_message);
                    EmitDurableEventLine(line.str());
                } else if (claim_result.claimed == 0
                    && job_materialization_service_.CountMaterializedJobs() == 0) {
                    ++clean_zero_claim_count_;
                    no_jobs_available_.store(true);
                } else {
                    no_jobs_available_.store(false);
                }
                if (claim_result.claimed > 0 && claim_result.claimed < claim_result.requested) {
                    ++partial_claim_count_;
                }
            }
            claimed_count = claim_result.claimed;
            if (claimed_count > 0) {
                RefreshSharedItemCredits();
                std::ostringstream line;
                line << "[seedprobe-claim-batch] claimed=" << claimed_count
                     << " budget=" << claim_budget
                     << " buffered_before=" << buffered
                     << " worker_target=" << worker_target
                     << " consumed_credits=" << capacity.ConsumedCredits()
                     << " buffered_after=" << job_materialization_service_.CountBufferedJobs();
                EmitDurableEventLine(line.str());
            }
        }

        HandlePayloadMaterializationFailures();

        bool dispatched_any = false;
        auto dispatchable_workers = CollectDispatchableWorkers();
        ClaimedJobRecord anchor{};
        if (job_materialization_service_.PeekMaterializedAnchor(
                &anchor)) {
            const auto is_exact_warm =
                [&](const DispatchableWorkerInfo& worker) {
                    return worker.loaded_workset_execution_key.has_value()
                        && *worker.loaded_workset_execution_key
                            == anchor.workset_execution_key;
                };
            std::sort(
                dispatchable_workers.begin(),
                dispatchable_workers.end(),
                [&](const auto& lhs, const auto& rhs) {
                    const bool lhs_warm = is_exact_warm(lhs);
                    const bool rhs_warm = is_exact_warm(rhs);
                    if (lhs_warm != rhs_warm) {
                        return lhs_warm;
                    }
                    return lhs.worker_idx < rhs.worker_idx;
                });
        }
        for (const auto& worker : dispatchable_workers) {
            ++dispatch_attempt_count_;
            const bool dispatched = DispatchNextEligibleForWorker(
                worker.worker_idx,
                MaterializedJobSelectionAffinity{
                    .savestate_affinity_key = worker.loaded_savestate_affinity_key,
                    .program_kind = worker.loaded_program_kind,
                    .program_runtime_affinity_key = worker.loaded_program_runtime_affinity_key,
                },
                std::chrono::steady_clock::now());
            if (dispatched) {
                ++dispatch_success_count_;
                dispatched_any = true;
            } else {
                if (no_jobs_available_.load()) {
                    ++dispatch_miss_count_;
                }
            }
        }

        if (claimed_count == 0 && !dispatched_any) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            const auto sleep_ms = worker_cfg_.controller_sleep_ms ? worker_cfg_.controller_sleep_ms : 5;
            queue_cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms));
        }
    }
}

void DBWorkflowWorkerCoordinator::RecoverDeadInFlightWorkers() {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    struct LostJob {
        size_t worker_idx = 0;
        std::uint64_t job_id = 0;
        std::string claimed_by_token;
        std::uint64_t attempt_id = 0;
        std::string message;
    };
    struct LostWorker {
        size_t worker_idx = 0;
        std::string message;
    };

    constexpr auto kDeadWorkerResultGrace = std::chrono::seconds(2);
    const auto now = std::chrono::steady_clock::now();
    std::vector<LostJob> lost_jobs;
    std::vector<LostWorker> lost_workers;
    std::vector<std::string> event_lines;
    std::vector<std::shared_ptr<savor::ProcessWorker>> workers_to_stop;
    const auto slots = CopyWorkerSlots();
    for (auto& slot_ptr : slots) {
        if (!slot_ptr) {
            continue;
        }
        std::unique_lock<std::mutex> submission_lock(
            slot_ptr->workset_submission_mtx);
        std::lock_guard<std::mutex> slot_lock(slot_ptr->mtx);
        if (slot_ptr->retained_workset_ids.empty()
            || !slot_ptr->worker) {
            continue;
        }
        auto& slot = *slot_ptr;
        if (slot.worker->is_running()) {
            slot.dead_in_flight_observed_at = {};
            continue;
        }
        if (slot.dead_in_flight_observed_at == std::chrono::steady_clock::time_point{}) {
            slot.dead_in_flight_observed_at = now;
            continue;
        }
        if (now - slot.dead_in_flight_observed_at < kDeadWorkerResultGrace) {
            continue;
        }

        const auto job_id = slot.in_flight_job_id.value_or(0);
        std::ostringstream line;
        line << "[workflow-worker-dead-in-flight]"
             << " worker=" << slot.id
             << " job=" << job_id;
        event_lines.push_back(line.str());
        MarkWorkerError(slot, "worker exited while job was in flight");
        lost_workers.push_back(LostWorker{
            .worker_idx = slot.id,
            .message = "WORKER_EXITED_DURING_JOB",
        });
        if (auto worker_to_stop = ResetWorkerSlotRuntime(slot)) {
            workers_to_stop.push_back(std::move(worker_to_stop));
        }
    }
    for (auto& worker : workers_to_stop) {
        if (worker) {
            worker->stop();
        }
    }
    if (!lost_workers.empty()) {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (auto it = dispatched_workset_items_.begin();
             it != dispatched_workset_items_.end();) {
            const auto lost_worker = std::find_if(
                lost_workers.begin(),
                lost_workers.end(),
                [&](const LostWorker& candidate) {
                    return candidate.worker_idx
                        == it->second.worker_idx;
                });
            if (lost_worker == lost_workers.end()) {
                ++it;
                continue;
            }
            const auto job_id = static_cast<std::uint64_t>(
                it->second.claimed.job_id);
            // Preserve every exact token/attempt correlation. If an
            // impossible duplicate exists, the guarded DB operation makes
            // the second recovery an idempotent no-op instead of allowing an
            // unordered-map winner to select which owner is recovered.
            lost_jobs.push_back(LostJob{
                .worker_idx = it->second.worker_idx,
                .job_id = job_id,
                .claimed_by_token =
                    it->second.claimed.claimed_by_token,
                .attempt_id = it->second.attempt_id,
                .message = lost_worker->message,
            });
            dispatched_job_context_by_id_.erase(job_id);
            it = dispatched_workset_items_.erase(it);
        }
    }

    for (const auto& line : event_lines) {
        EmitDurableEventLine(line);
    }

    for (const auto& lost : lost_jobs) {
        (void)job_materialization_service_.
            CleanupDispatchedOrExpired(
                static_cast<std::int64_t>(lost.job_id));
        if (execution_db_ == nullptr) {
            continue;
        }
        savor::db::ExecutionJobWorkerLossRecoveryReceipt receipt{};
        std::string error;
        const bool recovered =
            execution_db_->RecoverExecutionJobAfterWorkerLoss(
                static_cast<std::int64_t>(lost.job_id),
                lost.claimed_by_token,
                lost.attempt_id,
                lost.message,
                &receipt,
                &error);
        const bool durable_disposition = recovered
            && (receipt.disposition
                    == savor::db::
                        ExecutionJobWorkerLossRecoveryDisposition::
                            Requeued
                || receipt.disposition
                    == savor::db::
                        ExecutionJobWorkerLossRecoveryDisposition::
                            AttemptsExhaustedFailed
                || receipt.disposition
                    == savor::db::
                        ExecutionJobWorkerLossRecoveryDisposition::
                            AlreadyDurable
                || receipt.disposition
                    == savor::db::
                        ExecutionJobWorkerLossRecoveryDisposition::
                            TokenMismatch
                || receipt.disposition
                    == savor::db::
                        ExecutionJobWorkerLossRecoveryDisposition::
                            AttemptMismatch);
        if (!durable_disposition) {
            std::ostringstream line;
            line << "[workflow-worker-dead-in-flight-recovery-failed]"
                 << " worker=" << lost.worker_idx
                 << " job=" << lost.job_id
                 << " attempt=" << lost.attempt_id
                 << " disposition="
                 << static_cast<int>(receipt.disposition);
            if (!error.empty()) {
                line << " error=" << error;
            }
            EmitDurableEventLine(line.str());
        } else {
            std::ostringstream line;
            line << "[workflow-worker-dead-in-flight-recovery]"
                 << " worker=" << lost.worker_idx
                 << " job=" << lost.job_id
                 << " attempt=" << lost.attempt_id
                 << " disposition="
                 << static_cast<int>(receipt.disposition);
            EmitDurableEventLine(line.str());
        }
    }

    if (!lost_jobs.empty()) {
        queue_cv_.notify_all();
    }
}

void DBWorkflowWorkerCoordinator::WorkerLifecycleCoordinatorLoop() {
    while (!stop_.load() && IsDataPlaneEnabled()) {
        ReconcileWorkerPool();
        RefreshSharedItemCredits();

        std::unique_lock<std::mutex> lock(queue_mtx_);
        const auto sleep_ms = worker_cfg_.controller_sleep_ms ? worker_cfg_.controller_sleep_ms : 5;
        queue_cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms));
    }
}

void DBWorkflowWorkerCoordinator::ProcessReadyWorkflowStep(const WorkflowReadyStep& step) {
    if (!IsDataPlaneEnabled() || !integration_cfg_.workflow_enabled) {
        return;
    }

    if (IsNoWorkWorkflowStep(step)) {
        if (CompleteNoWorkWorkflowStep(step)) {
            queue_cv_.notify_all();
        }
        return;
    }

    const auto aggregation = input_aggregation_service_.Evaluate(
        step,
        std::chrono::steady_clock::now(),
        true);
    if (!aggregation.input_complete) {
        if (aggregation.timed_out) {
            ++input_timeout_count_;
        }
        if (aggregation.terminal_failure_ready) {
            ++terminal_input_failure_count_;
            if (execution_db_ && execution_db_->WorkflowCommandService()) {
                std::string error;
                (void)execution_db_->WorkflowCommandService()->MarkStepTerminal(
                    {
                        .workflow_step_id = step.workflow_step_id,
                        .terminal_state = "FAILED",
                        .requested_by = "step_input_aggregation_timeout",
                    },
                    &error);
            }
            return;
        }
        EnqueueReadyStep(step);
        return;
    }

    ++input_complete_count_;
    if (aggregation.input_latency_ms.has_value()) {
        last_input_latency_ms_.store(*aggregation.input_latency_ms);
    }

    const auto materialize_started = std::chrono::steady_clock::now();
    (void)MaterializeWorkflowStepInternal(step);
    ++materialization_count_;
    const auto materialization_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - materialize_started).count();
    last_materialization_latency_ms_.store(static_cast<std::int64_t>(materialization_latency_ms));
    {
        const auto prev_max = max_materialization_latency_ms_.load();
        if (materialization_latency_ms > prev_max) {
            max_materialization_latency_ms_.store(static_cast<std::int64_t>(materialization_latency_ms));
        }
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        ++materialized_count_;
        ++epoch_;
    }
    queue_cv_.notify_one();
}

bool DBWorkflowWorkerCoordinator::CompleteNoWorkWorkflowStep(const WorkflowReadyStep& step) const {
    if (!IsDataPlaneEnabled()
        || execution_db_ == nullptr
        || execution_db_->WorkflowCommandService() == nullptr) {
        return false;
    }

    auto* commands = execution_db_->WorkflowCommandService();
    std::string error;
    if (!commands->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "COMPLETED",
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "mark terminal failed" : error);
        return false;
    }

    if (!commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("transition_evaluated"),
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "append transition evaluation failed" : error);
        return false;
    }

    if (!commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionAdvanced.v1",
            .message = std::optional<std::string>("transition_advanced"),
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "append transition advanced failed" : error);
        return false;
    }

    if (!commands->CompleteWorkflowInstance(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "complete workflow failed" : error);
        return false;
    }

    std::ostringstream line;
    line << "[seedprobe-terminal-advance]"
         << " step=" << step.step_key
         << " kind=" << step.step_kind
         << " workflow_step_id=" << step.workflow_step_id
         << " status=workflow_completed"
         << " step_terminal=true"
         << " gate=true"
         << " next_step=false"
         << " workflow_completed=true"
         << " no_work=true";
    EmitDurableEventLine(line.str());
    return true;
}

void DBWorkflowWorkerCoordinator::DrainProgressLoop() {
    constexpr std::size_t kMaxBatchSize = 64;
    std::unordered_map<std::size_t, ProgressDedupState> progress_dedup_by_worker;

    const auto persist_progress_event = [this](
        const savor::PRProgress& item,
        const std::string& message,
        const char* requested_by) {
        if (execution_db_ == nullptr || execution_db_->JobCommandService() == nullptr || item.job_id == 0) {
            return;
        }

        std::string error;
        if (!execution_db_->JobCommandService()->AppendLifecycleEvent(
                {
                    .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
                    .job_id = static_cast<std::int64_t>(item.job_id),
                    .message = message,
                    .requested_by = requested_by,
                },
                &error)) {
            std::ostringstream line;
            line << "[workflow-progress-persist-failed] job=" << item.job_id
                 << " worker=" << item.worker_id;
            if (!error.empty()) {
                line << " error=" << error;
            }
            EmitDurableEventLine(line.str());
        }
    };

    const auto flush_duplicate_summary = [&](std::size_t worker_id, ProgressDedupState& state) {
        if (!state.has_value || state.duplicate_count == 0) {
            return;
        }

        savor::PRProgress summary_progress{
            .worker_id = worker_id,
            .job_id = state.job_id,
            .text = state.text,
        };
        std::ostringstream message;
        message << "worker=" << worker_id
                << " progress_summary=suppressed " << state.duplicate_count
                << " duplicate consecutive progress line(s) after first: " << state.text;
        persist_progress_event(summary_progress, message.str(), "workflow_progress_duplicate_summary");
        state.duplicate_count = 0;
        state.warning_written = false;
    };

    savor::PRProgress progress;
    while (progress_q_.pop_wait(progress)) {
        std::vector<savor::PRProgress> batch;
        batch.reserve(kMaxBatchSize);
        batch.push_back(progress);
        savor::PRProgress next;
        while (batch.size() < kMaxBatchSize && progress_q_.try_pop(next)) {
            batch.push_back(std::move(next));
        }
        ++progress_batch_count_;
        const auto batch_size = static_cast<std::int64_t>(batch.size());
        const auto prev_max = max_progress_batch_size_.load();
        if (batch_size > prev_max) {
            max_progress_batch_size_.store(batch_size);
        }
        for (const auto& item : batch) {
            if (item.record_progress) {
                worker_status_.RecordProgress(
                    static_cast<std::int64_t>(item.worker_id),
                    item.text,
                    static_cast<std::int64_t>(item.job_id));
            }
            (void)TryRecordWorkerContactFromProgress(
                item.worker_id,
                item.job_id,
                std::chrono::steady_clock::now());

            bool duplicate_progress = false;
            if (item.record_progress) {
                auto& dedup_state = progress_dedup_by_worker[item.worker_id];
                duplicate_progress = dedup_state.has_value
                    && dedup_state.job_id == item.job_id
                    && dedup_state.text == item.text;

                if (duplicate_progress) {
                    ++dedup_state.duplicate_count;
                    if (!dedup_state.warning_written) {
                        std::ostringstream warning;
                        warning << "worker=" << item.worker_id
                                << " progress_warning=duplicate consecutive progress suppressed for job="
                                << item.job_id
                                << " text=" << item.text;
                        persist_progress_event(item, warning.str(), "workflow_progress_duplicate_warning");
                        RecordCoordinatorWarning(
                            static_cast<std::int64_t>(item.worker_id),
                            static_cast<std::int64_t>(item.job_id),
                            "Duplicate progress suppressed",
                            warning.str());
                        EmitDurableEventLine("[workflow-progress-duplicate-suppressed] " + warning.str());
                        dedup_state.warning_written = true;
                    }
                } else {
                    flush_duplicate_summary(item.worker_id, dedup_state);
                    dedup_state.has_value = true;
                    dedup_state.job_id = item.job_id;
                    dedup_state.text = item.text;
                    dedup_state.duplicate_count = 0;
                    dedup_state.warning_written = false;

                    std::ostringstream message;
                    message << "worker=" << item.worker_id << " progress=" << item.text;
                    persist_progress_event(item, message.str(), "workflow_progress_drainer");
                }
            }

            if (item.record_progress && !duplicate_progress && progress_callback_) {
                progress_callback_(item);
            }
        }
    }

    for (auto& [worker_id, state] : progress_dedup_by_worker) {
        flush_duplicate_summary(worker_id, state);
    }
}

bool DBWorkflowWorkerCoordinator::ProcessDurableResultProjection(
    const savor::PRResult& result,
    const DispatchedJobContext& context,
    savor::db::execution::workflow::
        TerminalWorkflowStepNotification* notification_out,
    std::vector<std::string>* post_ack_event_lines_out) {
    std::lock_guard<std::mutex> projection_lock(
        result_projection_mtx_);
    return ProcessDurableResultProjectionLocked(
        result,
        context,
        notification_out,
        post_ack_event_lines_out);
}

bool DBWorkflowWorkerCoordinator::ProcessDurableResultProjectionLocked(
    const savor::PRResult& result,
    const DispatchedJobContext& context,
    savor::db::execution::workflow::
        TerminalWorkflowStepNotification* notification_out,
    std::vector<std::string>* post_ack_event_lines_out) {
    if (notification_out != nullptr) {
        *notification_out = {};
    }
    if (!adapter_chain_orchestrator_) {
        EmitDurableEventLine(
            "[workflow-result-projection-unavailable] job="
            + std::to_string(result.job_id)
            + " worker=" + std::to_string(result.worker_id));
        return false;
    }

    savor::db::execution::workflow::AdapterChainTrace trace{};
    std::string adapter_error;
    const auto mapped = adapter_chain_orchestrator_->OnJobTerminal(
        context.step.step_kind,
        static_cast<std::int64_t>(result.job_id),
        result,
        &trace,
        &adapter_error);
    ++adapter_job_terminal_invocations_;
    EmitAdapterTraceEvent(
        context.step,
        "OnJobTerminal",
        mapped.has_value() ? "invoked" : "failed",
        static_cast<std::int64_t>(result.job_id),
        context.job_set_id,
        adapter_error.empty()
            ? std::nullopt
            : std::optional<std::string>(adapter_error));
    if (mapped.has_value() && result_map_event_callback_) {
        for (const auto& line : mapped->event_lines) {
            if (post_ack_event_lines_out != nullptr) {
                post_ack_event_lines_out->push_back(line);
            } else {
                EmitDurableEventLine(line);
            }
        }
    }

    const auto mapped_output_ref_kind =
        mapped.has_value() && !mapped->output_ref_kind.empty()
        ? mapped->output_ref_kind
        : (mapped.has_value()
            ? mapped->result_kind
            : std::string{});
    const auto mapped_output_ref_id =
        mapped.has_value() && mapped->output_ref_id > 0
        ? mapped->output_ref_id
        : (mapped.has_value() ? mapped->result_ref_id : 0);
    bool output_recorded = true;
    if (mapped.has_value()
        && !mapped->output_key.empty()
        && !mapped->output_data_kind.empty()
        && !mapped_output_ref_kind.empty()
        && mapped_output_ref_id > 0
        && execution_db_ != nullptr) {
        std::string output_error;
        if (!execution_db_->RecordJobOutput(
                {
                    .job_id = static_cast<std::int64_t>(
                        result.job_id),
                    .output_key = mapped->output_key,
                    .data_kind = mapped->output_data_kind,
                    .ref_kind = mapped_output_ref_kind,
                    .ref_id = mapped_output_ref_id,
                    .requested_by =
                        "workflow_worker_result_mapper",
                },
                &output_error)) {
            output_recorded = false;
            std::ostringstream line;
            line << "[execution-job-output-record-failed]"
                 << " job=" << result.job_id
                 << " worker=" << result.worker_id
                 << " step=" << context.step.step_key
                 << " kind=" << context.step.step_kind
                 << " output_key=" << mapped->output_key
                 << " output_ref_kind="
                 << mapped_output_ref_kind
                 << " output_ref_id=" << mapped_output_ref_id;
            if (!output_error.empty()) {
                line << " error=" << output_error;
            }
            EmitDurableEventLine(line.str());
        }
    }

    const bool durable_projection_committed =
        mapped.has_value() && output_recorded;
    if (!mapped.has_value()) {
        std::ostringstream line;
        line << "[workflow-result-map-failed]"
             << " job=" << result.job_id
             << " worker=" << result.worker_id
             << " step=" << context.step.step_key
             << " kind=" << context.step.step_kind
             << " workflow_step_id="
             << context.step.workflow_step_id
             << " job_set=" << context.job_set_id;
        if (!adapter_error.empty()) {
            line << " error=" << adapter_error;
        }
        EmitDurableEventLine(line.str());
        MarkDeterministicFailure(
            context.step,
            static_cast<std::int64_t>(result.job_id),
            context.job_set_id,
            "ADAPTER_RESULT_PERSIST_FAILED:"
                + (adapter_error.empty()
                    ? std::string("mapping returned no result")
                    : adapter_error));
    }

    if (durable_projection_committed && notification_out != nullptr) {
        const auto commit_sequence =
            terminal_commit_sequence_.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;
        *notification_out =
            savor::db::execution::workflow::
                TerminalWorkflowStepNotification{
                    .commit_sequence = commit_sequence,
                    .workflow_step_id =
                        context.step.workflow_step_id,
                    .job_id = static_cast<std::int64_t>(
                        result.job_id),
                };
    }
    return durable_projection_committed;
}

void DBWorkflowWorkerCoordinator::PublishTerminalCommitIsolated(
    const savor::db::execution::workflow::
        TerminalWorkflowStepNotification& notification) {
    if (!worker_cfg_.terminal_commit_callback
        || notification.commit_sequence == 0) {
        return;
    }
    try {
        worker_cfg_.terminal_commit_callback(notification);
    } catch (const std::exception& ex) {
        EmitDurableEventLine(
            "[workflow-terminal-notification-callback-failed] job="
            + std::to_string(notification.job_id)
            + " error=" + ex.what());
    } catch (...) {
        EmitDurableEventLine(
            "[workflow-terminal-notification-callback-failed] job="
            + std::to_string(notification.job_id)
            + " error=unknown_exception");
    }
}

void DBWorkflowWorkerCoordinator::DrainResultsLoop() {
    savor::PRResult result;
    while (results_q_.pop_wait(result)) {
        ++results_received_count_;
        std::optional<DispatchedJobContext> context;
        bool worker_known = false;
        std::optional<std::uint64_t> worker_in_flight_job_id;
        {
            std::lock_guard<std::mutex> lock(workers_mtx_);
            const auto it = dispatched_job_context_by_id_.find(result.job_id);
            if (it != dispatched_job_context_by_id_.end()) {
                context = it->second;
                dispatched_job_context_by_id_.erase(it);
            }
            if (result.worker_id < workers_.size()) {
                worker_known = true;
            }
        }
        if (worker_known) {
            const auto slot = GetWorkerSlot(result.worker_id);
            if (slot) {
                std::lock_guard<std::mutex> slot_lock(slot->mtx);
                worker_in_flight_job_id = slot->in_flight_job_id;
            }
        }

        if (!context.has_value()) {
            std::ostringstream line;
            line << "[seedprobe-result-no-context] job=" << result.job_id
                 << " worker=" << result.worker_id;
            if (worker_known) {
                line << " worker_in_flight=";
                if (worker_in_flight_job_id.has_value()) {
                    line << *worker_in_flight_job_id;
                } else {
                    line << "none";
                }
            } else {
                line << " worker_in_flight=unknown_worker";
            }
            EmitDurableEventLine(line.str());
        }
        if (worker_known && (!worker_in_flight_job_id.has_value() || *worker_in_flight_job_id != result.job_id)) {
            std::ostringstream line;
            line << "[seedprobe-result-worker-mismatch] job=" << result.job_id
                 << " worker=" << result.worker_id
                 << " worker_in_flight=";
            if (worker_in_flight_job_id.has_value()) {
                line << *worker_in_flight_job_id;
            } else {
                line << "none";
            }
            EmitDurableEventLine(line.str());
        }

        if (context.has_value()) {
            (void)ProcessDurableResultProjection(
                result,
                *context);
        }

        ReleaseWorkerByResult(result);
        (void)job_materialization_service_.CleanupDispatchedOrExpired(static_cast<std::int64_t>(result.job_id));
        if (result_callback_) {
            result_callback_(result);
        }
    }
}

void DBWorkflowWorkerCoordinator::ReconcileWorkerPool() {
    if (!IsDataPlaneEnabled()) {
        return;
    }
    std::vector<WorkerSlotPtr> slots;
    std::vector<WorkerSlotPtr> new_slots;
    std::vector<WorkerSlotPtr> slots_to_stop;
    std::vector<std::thread> completed_startup_threads;
    const auto desired_workers = desired_worker_count_.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const auto max_start_attempts = std::max<std::uint32_t>(1u, worker_cfg_.max_worker_start_attempts);
    auto max_concurrent_starts = std::max<std::uint32_t>(
        1u,
        worker_cfg_.max_concurrent_worker_starts);

    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        while (workers_.size() < desired_workers) {
            const auto worker_idx = workers_.size();
            auto slot = MakeWorkerSlot(worker_idx);
            new_slots.push_back(slot);
            workers_.push_back(std::move(slot));
        }

        while (workers_.size() > desired_workers) {
            auto slot = workers_.back();
            bool removable = false;
            if (slot) {
                std::lock_guard<std::mutex> slot_lock(slot->mtx);
                removable = !slot->startup_in_progress
                    && !slot->startup_thread.joinable()
                    && !slot->in_flight_job_id.has_value();
            }
            if (!removable) {
                break;
            }
            slots_to_stop.push_back(slot);
            workers_.pop_back();
        }

        worker_slot_count_.store(workers_.size(), std::memory_order_relaxed);
        slots = workers_;
    }

    for (const auto& slot : new_slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        RegisterWorkerSlotTelemetry(*slot);
    }

    std::uint32_t active_startups = 0;
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (slot->ready.load(std::memory_order_acquire)
            && slot->runtime_manifest.has_value()) {
            max_concurrent_starts = std::min(
                max_concurrent_starts,
                std::max<std::uint32_t>(
                    1u,
                    slot->runtime_manifest->limits
                        .progressive_start_concurrency));
        }
        if (!slot->startup_in_progress && slot->startup_thread.joinable()) {
            completed_startup_threads.push_back(std::move(slot->startup_thread));
        }
        if (slot->startup_in_progress) {
            ++active_startups;
        }
    }

    std::vector<std::string> event_lines;
    std::vector<std::shared_ptr<savor::ProcessWorker>> workers_to_stop;
    for (const auto& slot_handle : slots) {
        if (active_startups >= max_concurrent_starts) {
            break;
        }
        if (!slot_handle) {
            continue;
        }
        bool should_start = false;
        bool restart_deferred_until_old_worker_stops = false;
        {
            std::unique_lock<std::mutex> submission_lock(
                slot_handle->workset_submission_mtx);
            std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
            auto& slot = *slot_handle;
            const auto worker_idx = slot.id;
            if (slot.ready.load()) {
                continue;
            }
            if (slot.startup_in_progress) {
                continue;
            }
            if (slot.in_flight_job_id.has_value()) {
                continue;
            }
            if (slot.start_attempted) {
                if (slot.start_attempts >= max_start_attempts) {
                    if (!slot.start_retry_exhausted_logged) {
                        std::ostringstream line;
                        line << "[workflow-worker-restart-exhausted]"
                             << " worker=" << worker_idx
                             << " attempts=" << slot.start_attempts
                             << " max_attempts=" << max_start_attempts;
                        if (!slot.last_start_error.empty()) {
                            line << " last_error=" << slot.last_start_error;
                        }
                        event_lines.push_back(line.str());
                        slot.start_retry_exhausted_logged = true;
                    }
                    continue;
                }
                if (now < slot.next_start_after) {
                    continue;
                }
                std::ostringstream line;
                line << "[workflow-worker-restart]"
                     << " worker=" << worker_idx
                     << " next_attempt=" << (slot.start_attempts + 1)
                     << " max_attempts=" << max_start_attempts;
                if (!slot.last_start_error.empty()) {
                    line << " last_error=" << slot.last_start_error;
                }
                event_lines.push_back(line.str());
                if (auto worker_to_stop = ResetWorkerSlotRuntime(slot)) {
                    workers_to_stop.push_back(std::move(worker_to_stop));
                    restart_deferred_until_old_worker_stops = true;
                }
            }
            if (slot.start_attempted) {
                continue;
            }
            should_start = true;
        }
        if (restart_deferred_until_old_worker_stops) {
            continue;
        }
        if (should_start && StartWorkerSlot(slot_handle)) {
            ++active_startups;
        }
    }

    for (const auto& line : event_lines) {
        EmitDurableEventLine(line);
    }
    for (auto& worker : workers_to_stop) {
        if (worker) {
            worker->stop();
        }
    }
    for (auto& thread : completed_startup_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    for (const auto& slot : slots_to_stop) {
        if (slot) {
            StopWorkerSlot(slot);
        }
    }
}

bool DBWorkflowWorkerCoordinator::StartWorkerSlot(WorkerSlotPtr slot_handle) {
    if (!slot_handle) {
        return false;
    }

    size_t worker_idx = 0;
    uint32_t attempt = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
        auto& slot = *slot_handle;
        worker_idx = slot.id;
        if (slot.startup_in_progress || slot.startup_thread.joinable()) {
            return false;
        }
        slot.start_attempted = true;
        slot.startup_in_progress = true;
        slot.ready.store(false);
        slot.capabilities = 0;
        slot.runtime_manifest.reset();
        slot.start_retry_exhausted_logged = false;
        attempt = slot.start_attempts + 1;
        slot.start_attempts = attempt;
    }

    if (stop_.load()) {
        CompleteWorkerSlotStartup(
            worker_idx,
            attempt,
            false,
            0,
            std::nullopt,
            "startup canceled",
            true);
        return false;
    }

    std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
    auto& slot = *slot_handle;
    if (slot.start_attempts != attempt || !slot.startup_in_progress) {
        return false;
    }
    if (stop_.load(std::memory_order_acquire)) {
        slot.startup_in_progress = false;
        slot.last_start_error = "startup canceled";
        slot.capabilities = 0;
        slot.runtime_manifest.reset();
        slot.ready.store(false, std::memory_order_release);
        return false;
    }
    worker_status_.UpdateState(
        static_cast<std::int64_t>(slot.id),
        WorkerStateKind::Spawning);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
    slot.startup_thread = std::thread(
        [this, slot_handle, worker_idx, attempt]() {
            if (stop_.load()) {
                CompleteWorkerSlotStartup(
                    worker_idx,
                    attempt,
                    false,
                    0,
                    std::nullopt,
                    "startup canceled",
                    true);
                return;
            }

            const auto preflight =
                RunWorkerCapabilityPreflightForSlot(slot_handle);
            const auto evaluation = EvaluateCapabilityPreflight(
                preflight,
                worker_cfg_.visual_debug_workers,
                worker_cfg_.expected_catalog_sha256,
                worker_cfg_.expected_runtime_profile_sha256,
                worker_cfg_.expected_dependency_manifest_sha256);
            if (!evaluation.ready) {
                std::shared_ptr<savor::ProcessWorker> worker;
                {
                    std::lock_guard<std::mutex> failed_slot_lock(
                        slot_handle->mtx);
                    worker = slot_handle->worker;
                }
                if (worker) {
                    worker->stop();
                }
            }

            CompleteWorkerSlotStartup(
                worker_idx,
                attempt,
                evaluation.ready,
                preflight.capabilities,
                preflight.runtime_manifest,
                evaluation.error,
                !evaluation.non_retryable);
        });
    return true;
}

void DBWorkflowWorkerCoordinator::CompleteWorkerSlotStartup(
    size_t worker_idx,
    uint32_t attempt,
    bool ready,
    savor::runtime::WorkerCapabilityMask capabilities,
    std::optional<savor::runtime::WorkerRuntimeManifest> runtime_manifest,
    const std::string& error,
    bool retryable) {
    const auto slot_handle = GetWorkerSlot(worker_idx);
    if (!slot_handle) {
        return;
    }

    std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
    auto& slot = *slot_handle;
    if (slot.start_attempts != attempt) {
        return;
    }

    slot.startup_in_progress = false;
    slot.ready.store(ready);
    slot.capabilities = ready ? capabilities : 0;
    slot.runtime_manifest = ready
        ? std::move(runtime_manifest)
        : std::nullopt;
    RegisterWorkerSlotTelemetry(slot);
    if (ready) {
        slot.start_attempts = 0;
        slot.next_start_after = {};
        slot.last_start_error.clear();
        slot.start_retry_exhausted_logged = false;
        queue_cv_.notify_all();
        return;
    }

    slot.last_start_error = error;
    if (retryable) {
        slot.next_start_after = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(
                worker_cfg_.worker_start_retry_backoff_ms);
    } else {
        slot.start_attempts = std::max<std::uint32_t>(
            1u,
            worker_cfg_.max_worker_start_attempts);
        slot.next_start_after = {};
    }
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();
    slot.loaded_workset_execution_key.reset();
    slot.runtime_manifest.reset();

    std::ostringstream line;
    line << "[workflow-worker-start-failed]"
         << " worker=" << worker_idx
         << " attempt=" << attempt
         << " max_attempts=" << std::max<std::uint32_t>(1u, worker_cfg_.max_worker_start_attempts)
         << " error=" << error;
    EmitDurableEventLine(line.str());
    MarkWorkerError(slot, error);
    queue_cv_.notify_all();
}

std::shared_ptr<savor::ProcessWorker> DBWorkflowWorkerCoordinator::ResetWorkerSlotRuntime(WorkerSlot& slot) {
    auto worker_to_stop = std::move(slot.worker);
    slot.process_generation = NextWorkerProcessGeneration();
    slot.ready.store(false);
    slot.capabilities = 0;
    slot.runtime_manifest.reset();
    slot.start_attempted = false;
    slot.startup_in_progress = false;
    slot.in_flight_job_id.reset();
    slot.active_workset_id.reset();
    slot.staged_workset_id.reset();
    slot.retained_workset_ids.clear();
    slot.retained_workset_item_counts.clear();
    slot.failed_closed_workset_ids.clear();
    slot.last_workset_outbound_sequence = 0;
    slot.last_workset_terminal_order = 0;
    slot.workset_admission_blocked = false;
    slot.in_flight_started_at = {};
    slot.last_worker_contact_at = {};
    slot.dead_in_flight_observed_at = {};
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();
    slot.loaded_workset_execution_key.reset();
    slot.worker = std::make_shared<savor::ProcessWorker>();
    ConfigureWorkerCallbacks(
        slot.id,
        slot.process_generation,
        slot.worker);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Spawning);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    return worker_to_stop;
}

void DBWorkflowWorkerCoordinator::StopWorkerSlot(
    WorkerSlotPtr slot_handle,
    bool preserve_workset_event_context) {
    if (!slot_handle) {
        return;
    }
    std::shared_ptr<savor::ProcessWorker> worker_to_stop;
    std::int64_t worker_id = -1;
    std::int64_t pid = 0;
    {
        std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
        auto& slot = *slot_handle;
        worker_id = static_cast<std::int64_t>(slot.id);
        worker_status_.UpdateState(worker_id, WorkerStateKind::Stopping);
        worker_status_.SetCurrentJob(worker_id, std::nullopt, std::nullopt);
        slot.ready.store(false);
        slot.capabilities = 0;
        slot.runtime_manifest.reset();
        slot.start_attempted = false;
        slot.startup_in_progress = false;
        if (!preserve_workset_event_context) {
            slot.in_flight_job_id.reset();
            slot.active_workset_id.reset();
            slot.staged_workset_id.reset();
            slot.retained_workset_ids.clear();
            slot.retained_workset_item_counts.clear();
            slot.failed_closed_workset_ids.clear();
            slot.last_workset_outbound_sequence = 0;
            slot.last_workset_terminal_order = 0;
            slot.workset_admission_blocked = false;
            slot.in_flight_started_at = {};
            slot.last_worker_contact_at = {};
            slot.dead_in_flight_observed_at = {};
            slot.loaded_program_kind.reset();
            slot.loaded_program_runtime_affinity_key.reset();
            slot.loaded_savestate_affinity_key.reset();
            slot.loaded_workset_execution_key.reset();
        } else {
            slot.workset_admission_blocked = true;
        }
        worker_to_stop = slot.worker;
        if (worker_to_stop) {
            pid = worker_to_stop->GetPid();
        }
        if (!preserve_workset_event_context) {
            slot.worker.reset();
        }
    }

    {
        std::ostringstream detail;
        detail << "worker=" << worker_id << " pid=" << pid;
        EmitShutdownPhase("worker_stop_begin", detail.str());
    }
    if (worker_to_stop) {
        worker_to_stop->stop();
    }
    {
        const auto stop_snapshot = worker_to_stop
            ? worker_to_stop->last_stop_snapshot()
            : savor::ProcessWorkerStopSnapshot{};
        std::ostringstream detail;
        detail << "worker=" << worker_id << " pid=" << pid
               << FormatWorkerStopSnapshot(stop_snapshot);
        EmitShutdownPhase("worker_stop_end", detail.str());
    }

    if (!preserve_workset_event_context) {
        std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
        worker_status_.UpdateState(worker_id, WorkerStateKind::Dead);
        worker_status_.UnregisterWorker(worker_id);
    }
}

void DBWorkflowWorkerCoordinator::EmitShutdownPhase(const std::string& phase, const std::string& detail) const {
    std::ostringstream line;
    line << "[workflow-coordinator-shutdown] phase=" << phase;
    if (!detail.empty()) {
        line << " " << detail;
    }
    EmitDurableEventLine(line.str());
}

void DBWorkflowWorkerCoordinator::RecordWorkerContactLocked(
    WorkerSlot& slot,
    std::chrono::steady_clock::time_point observed_at) {
    slot.last_worker_contact_at = observed_at;
    slot.dead_in_flight_observed_at = {};
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
}

std::vector<DBWorkflowWorkerCoordinator::DispatchableWorkerInfo> DBWorkflowWorkerCoordinator::CollectDispatchableWorkers() {
    if (!IsDataPlaneEnabled()) {
        return {};
    }
    std::vector<DispatchableWorkerInfo> dispatchable;
    std::vector<WorkerSlotPtr> slots;
    size_t rr_cursor = 0;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        slots = workers_;
        rr_cursor = rr_worker_cursor_;
    }
    if (slots.empty()) {
        return dispatchable;
    }

    for (size_t i = 0; i < slots.size(); ++i) {
        const size_t idx = (rr_cursor + i) % slots.size();
        const auto& slot_handle = slots[idx];
        if (!slot_handle) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
        auto& slot = *slot_handle;
        if (!slot.ready.load()
            || !savor::runtime::HasCapability(
                slot.capabilities,
                savor::runtime::WorkerCapability::WorksetDispatch)) {
            continue;
        }
        if (slot.workset_admission_blocked
            || slot.staged_workset_id.has_value()) {
            continue;
        }
        if (!slot.worker
            || slot.worker->latest_snapshot()
                    .available_item_credits == 0) {
            continue;
        }
        dispatchable.push_back(DispatchableWorkerInfo{
            .worker_idx = idx,
            .loaded_program_kind = slot.loaded_program_kind,
            .loaded_program_runtime_affinity_key = slot.loaded_program_runtime_affinity_key,
            .loaded_savestate_affinity_key = slot.loaded_savestate_affinity_key,
            .loaded_workset_execution_key =
                slot.loaded_workset_execution_key,
        });
    }

    if (!dispatchable.empty()) {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        if (!workers_.empty()) {
            rr_worker_cursor_ = (dispatchable.back().worker_idx + 1) % workers_.size();
        }
    }
    return dispatchable;
}

std::size_t DBWorkflowWorkerCoordinator::CountActiveInFlightItems() const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    return dispatched_workset_items_.size();
}

std::size_t
DBWorkflowWorkerCoordinator::ReadyCoordinatorBufferCapacity() const {
    std::size_t capacity = 0;
    const auto slots = CopyWorkerSlots();
    for (const auto& slot : slots) {
        if (!slot) {
            continue;
        }
        std::lock_guard<std::mutex> slot_lock(slot->mtx);
        if (!slot->ready.load(std::memory_order_acquire)
            || !slot->runtime_manifest.has_value()
            || !savor::runtime::HasCapability(
                slot->capabilities,
                savor::runtime::WorkerCapability::WorksetDispatch)) {
            continue;
        }
        const auto per_worker = static_cast<std::size_t>(
            slot->runtime_manifest->limits.maximum_items_per_workset);
        const auto available =
            std::numeric_limits<std::size_t>::max() - capacity;
        capacity += std::min(available, per_worker);
    }
    return capacity;
}

void DBWorkflowWorkerCoordinator::RefreshSharedItemCredits() {
    const auto& source = worker_cfg_.item_credit_source;
    if (!source || !IsDataPlaneEnabled() || !source->IsOpen()) {
        return;
    }

    const auto total_credits = ReadyItemCreditCapacity();
    auto capacity = worker_cfg_.worker_item_usage_provider
        ? worker_cfg_.worker_item_usage_provider()
        : CoordinatorItemCapacitySnapshot{};
    capacity.total_credits = total_credits;
    capacity.coordinator_buffered =
        job_materialization_service_.CountBufferedJobs();
    if (!worker_cfg_.worker_item_usage_provider) {
        capacity.active_invocations = CountActiveInFlightItems();
    }
    source->SetCapacity(total_credits);
    source->SetExternalUsage(capacity.ConsumedCredits());
}

void DBWorkflowWorkerCoordinator::ReleaseWorkerByResult(const savor::PRResult& result) {
    const auto slot_handle = GetWorkerSlot(result.worker_id);
    if (!slot_handle) {
        return;
    }

    std::lock_guard<std::mutex> slot_lock(slot_handle->mtx);
    auto& slot = *slot_handle;
    RecordWorkerContactLocked(slot, std::chrono::steady_clock::now());
    slot.in_flight_job_id.reset();
    slot.in_flight_started_at = {};
    slot.last_worker_contact_at = {};
    slot.dead_in_flight_observed_at = {};
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    if (slot.worker) {
        slot.worker->release_slot();
        if (slot.worker->is_ready()) {
            worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Idle);
        } else {
            MarkWorkerError(slot, "worker became unavailable after result");
        }
    }
    queue_cv_.notify_all();
}


void DBWorkflowWorkerCoordinator::EmitAdapterTraceEvent(
    const WorkflowReadyStep& step,
    const std::string& stage,
    const std::string& status,
    std::optional<std::int64_t> job_id,
    std::optional<std::int64_t> job_set_id,
    const std::optional<std::string>& message) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::ostringstream detail;
    detail << "stage=" << stage << ";status=" << status;
    if (job_id.has_value()) detail << ";job_id=" << *job_id;
    if (job_set_id.has_value()) detail << ";job_set_id=" << *job_set_id;
    if (message.has_value()) detail << ";message=" << *message;

    std::string error;
    (void)error;
}

void DBWorkflowWorkerCoordinator::MarkDeterministicFailure(
    const WorkflowReadyStep& step,
    std::optional<std::int64_t> job_id,
    std::optional<std::int64_t> job_set_id,
    const std::string& reason) const {
    if (execution_db_ == nullptr) {
        return;
    }
    std::string error;
    if (execution_db_->JobCommandService() != nullptr && job_id.has_value() && job_set_id.has_value()) {
        (void)execution_db_->JobCommandService()->AppendLifecycleEvent(
            {
                .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
                .job_set_id = *job_set_id,
                .job_id = *job_id,
                .message = reason,
                .requested_by = "adapter_chain_orchestrator",
            },
            &error);
    }
}

void DBWorkflowWorkerCoordinator::EmitWorkflowFailureEvents(
    const WorkflowReadyStep& step,
    const std::string& stage,
    const std::string& reason) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::ostringstream detail;
    detail << "stage=" << stage << ";reason=" << reason;
    std::string error;
    (void)execution_db_->WorkflowCommandService()->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowStepCoordinatorFailure.v1",
            .message = detail.str(),
            .requested_by = "workflow_coordinator",
        },
        &error);
}

void DBWorkflowWorkerCoordinator::EmitDurableEventLine(const std::string& line) const {
    if (result_map_event_callback_) {
        try {
            result_map_event_callback_(line);
        } catch (...) {
            // Diagnostic/event-line consumers are observational. They must
            // never interrupt claim handling, durable projection, or the
            // worker's non-lossy terminal acknowledgement path.
        }
    }
}

void DBWorkflowWorkerCoordinator::MaybeTerminalFailStepInStrictSmokeMode(
    const WorkflowReadyStep& step,
    const std::string& requested_by) const {
    if (!integration_cfg_.strict_smoke_terminal_on_failure) {
        return;
    }
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::string error;
    (void)execution_db_->WorkflowCommandService()->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "FAILED",
            .requested_by = requested_by,
        },
        &error);
}

void DBWorkflowWorkerCoordinator::RegisterWorkerSlotTelemetry(const WorkerSlot& slot) {
    int pid = 0;
    WorkerStateKind state = WorkerStateKind::Dead;
    if (slot.worker) {
        pid = static_cast<int>(slot.worker->GetPid());
        state = slot.worker->is_ready() ? WorkerStateKind::Idle : WorkerStateKind::Dead;
    }
    worker_status_.RegisterWorker(static_cast<std::int64_t>(slot.id), "localhost", pid, "workflow");
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), state);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
}

void DBWorkflowWorkerCoordinator::MarkWorkerError(const WorkerSlot& slot, const std::string& error) {
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Dead);
    worker_status_.RecordError(static_cast<std::int64_t>(slot.id), error);
}

void DBWorkflowWorkerCoordinator::RecordCoordinatorWarning(
    std::int64_t worker_id,
    std::int64_t job_id,
    std::string message,
    std::string detail) {
    std::lock_guard<std::mutex> lock(coordinator_warning_mtx_);
    coordinator_warnings_.push_back(CoordinatorWarningSnapshot{
        .sequence = next_coordinator_warning_sequence_++,
        .worker_id = worker_id,
        .job_id = job_id,
        .observed_mono_ns = NowMonoNs(),
        .message = std::move(message),
        .detail = std::move(detail),
    });
    while (coordinator_warnings_.size() > kMaxCoordinatorWarnings) {
        coordinator_warnings_.pop_front();
    }
}

void DBWorkflowWorkerCoordinator::PollReadyStepsFromDb() {
    const auto t0 = std::chrono::steady_clock::now();
    if (!IsDataPlaneEnabled()
        || execution_db_ == nullptr
        || execution_db_->WorkflowQueryService() == nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
        ++ready_scan_count_;
        return;
    }

    if (!integration_cfg_.workflow_enabled) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
        ++ready_scan_count_;
        return;
    }

    constexpr std::size_t kReadyStepScanLimit = 2048;
    const auto ready_steps = execution_db_->WorkflowQueryService()->ListReadySteps(kReadyStepScanLimit);
    for (const auto& step : ready_steps) {
        bool announce_created = false;
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            announce_created = seen_workflow_instance_ids_.emplace(step.workflow_instance_id).second;
        }
        if (announce_created) {
            (void)PublishWorkflowCreated(WorkflowCreatedSignal{
                .workflow_instance_id = step.workflow_instance_id,
            });
        }
        EnqueueReadyStep(WorkflowReadyStep{
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .step_key = step.step_key,
            .step_kind = step.step_kind,
            .priority = step.priority,
            .input_ref_kind = step.input_ref_kind,
            .input_ref_id = step.input_ref_id,
        });
        ++ready_steps_enqueued_;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        const auto depth = static_cast<std::int64_t>(ready_queue_.size());
        const auto prev = max_ready_queue_depth_.load();
        if (depth > prev) {
            max_ready_queue_depth_.store(depth);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
    ++ready_scan_count_;
}

bool DBWorkflowWorkerCoordinator::TryDequeueReadyStep(WorkflowReadyStep* step_out) {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    if (ready_queue_.empty()) {
        return false;
    }

    *step_out = ready_queue_.front();
    ready_queue_.pop_front();
    seen_ready_step_ids_.erase(ReadyDedupKey(step_out->workflow_step_id));
    return true;
}

std::string DBWorkflowWorkerCoordinator::ReadyDedupKey(std::int64_t workflow_step_id) const {
    return std::to_string(workflow_step_id);
}

} // namespace savor::runner::parallel::savordb
