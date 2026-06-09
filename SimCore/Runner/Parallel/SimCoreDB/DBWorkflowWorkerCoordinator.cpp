#include "DBWorkflowWorkerCoordinator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "../../../../SimCoreDB/Execution/Jobs/JobEventOrchestration.h"
#include "../../../Utils/Hash.h"
#include "../../../Utils/ModulePath.h"

namespace simcore::runner::parallel::simcoredb {
namespace {

WorkflowSchedulerAdapter::ScheduleFn ResolveWorkflowScheduleFn(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn) {
    if (workflow_schedule_fn) {
        return workflow_schedule_fn;
    }
    auto adapter_chain_orchestrator = std::make_shared<simcore::db::execution::workflow::AdapterChainOrchestrator>(
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
        std::optional<simcore::db::execution::programdb::WorkflowStepScheduleResult> persisted;
        if (step.input_ref_id.has_value() && descriptor->job_persistence != nullptr && step.step_kind != "battle_chain") {
            persisted = adapter_chain_orchestrator->OnInputComplete(step.step_kind, *step.input_ref_id);
        } else if (descriptor->graph_job_persistence != nullptr && execution_db->WorkflowQueryService() != nullptr) {
            const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(step.workflow_instance_id);
            if (!graph.has_value()) {
                return {};
            }

            simcore::db::execution::programdb::WorkflowGraphStepScheduleContext context{};
            context.workflow_instance_id = step.workflow_instance_id;
            context.workflow_step_id = step.workflow_step_id;
            context.workflow_graph_revision_id = graph->instance.workflow_graph_revision_id;
            context.step_key = step.step_key;
            context.step_kind = step.step_kind;
            if (step.input_ref_id.has_value() && *step.input_ref_id > 0) {
                if (step.step_kind == "seed_probe_chain" && step.input_ref_kind == "state.savestate") {
                    context.input_bindings.push_back(
                        simcore::db::execution::programdb::WorkflowGraphInputBinding{
                            .node_key = step.step_key,
                            .input_key = "entry_savestate",
                            .data_kind = "state.savestate_id",
                            .ref_kind = *step.input_ref_kind,
                            .ref_id = *step.input_ref_id,
                            .source_kind = "upstream",
                        });
                } else if (step.step_kind == "battle_chain" && step.input_ref_kind == "sp_probe_run") {
                    context.input_bindings.push_back(
                        simcore::db::execution::programdb::WorkflowGraphInputBinding{
                            .node_key = step.step_key,
                            .input_key = "initial_input_frames",
                            .data_kind = "analysis.input_frame_set_id",
                            .ref_kind = *step.input_ref_kind,
                            .ref_id = *step.input_ref_id,
                            .source_kind = "upstream",
                        });
                }
            }
            for (const auto& binding : graph->input_bindings) {
                if (binding.node_key != step.step_key) {
                    continue;
                }
                context.input_bindings.push_back(
                    simcore::db::execution::programdb::WorkflowGraphInputBinding{
                        .node_key = binding.node_key,
                        .input_key = binding.input_key,
                        .data_kind = binding.data_kind,
                        .ref_kind = binding.ref_kind,
                        .ref_id = binding.ref_id,
                        .source_kind = binding.source_kind,
                    });
            }
            for (const auto& argument : graph->arguments) {
                if (!argument.node_key.empty() && argument.node_key != step.step_key) {
                    continue;
                }
                context.arguments.push_back(
                    simcore::db::execution::programdb::WorkflowGraphArgument{
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

std::string WorkerRuntimeFingerprint(
    const std::filesystem::path& source_worker_exe,
    const std::filesystem::path& dolphin_base_dir) {
    const auto canonical_exe = WeaklyCanonicalOrAbsolute(source_worker_exe);
    const auto canonical_base = WeaklyCanonicalOrAbsolute(dolphin_base_dir);
    const auto dsp_coef = canonical_base / "Sys" / "GC" / "dsp_coef.bin";

    std::ostringstream input;
    input << "worker_exe=" << canonical_exe.string() << "\n";
    input << "worker_exe_stamp=" << FileStamp(canonical_exe) << "\n";
    input << "dolphin_base=" << canonical_base.string() << "\n";
    input << "dsp_coef_stamp=" << FileStamp(dsp_coef) << "\n";

    const auto input_text = input.str();
    const auto digest = ::hash::sha256(input_text.data(), input_text.size());
    return digest.empty() ? "unknown-runtime" : digest.substr(0, 16);
}

std::string ExpectedWorkerRuntimeManifest(
    size_t worker_idx,
    const std::filesystem::path& source_worker_exe,
    const std::filesystem::path& dolphin_base_dir,
    const std::string& fingerprint,
    const std::string& exe_materialization) {
    std::ostringstream manifest;
    manifest << "simcore_worker_runtime_manifest_version=2\n";
    manifest << "slot_id=" << worker_idx << "\n";
    manifest << "fingerprint=" << fingerprint << "\n";
    manifest << "source_worker_exe=" << WeaklyCanonicalOrAbsolute(source_worker_exe).string() << "\n";
    manifest << "source_worker_exe_stamp=" << FileStamp(source_worker_exe) << "\n";
    manifest << "dolphin_base_dir=" << WeaklyCanonicalOrAbsolute(dolphin_base_dir).string() << "\n";
    manifest << "dsp_coef_stamp=" << FileStamp(dolphin_base_dir / "Sys" / "GC" / "dsp_coef.bin") << "\n";
    manifest << "exe_materialization=" << exe_materialization << "\n";
    return manifest.str();
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
    if (!out) {
        if (error_out) *error_out = "write runtime manifest failed: " + path.string();
        return false;
    }
    return true;
}

bool MaterializeWorkerExe(
    const std::filesystem::path& source_worker_exe,
    const std::filesystem::path& runtime_worker_exe,
    std::string* exe_materialization,
    std::string* error_out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_hard_link(source_worker_exe, runtime_worker_exe, ec);
    if (!ec) {
        if (exe_materialization) *exe_materialization = "hardlink";
        return true;
    }

    const auto hardlink_error = ec.message();
    ec.clear();
    fs::copy_file(source_worker_exe, runtime_worker_exe, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        if (exe_materialization) *exe_materialization = "copy_after_hardlink_failed:" + hardlink_error;
        return true;
    }

    if (error_out) {
        *error_out = "materialize worker exe failed; hardlink=" + hardlink_error + "; copy=" + ec.message();
    }
    return false;
}

void PruneStaleWorkerRuntimeFingerprints(
    const std::filesystem::path& runtime_cache_root,
    const std::string& active_fingerprint) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(runtime_cache_root, ec) || ec) {
        return;
    }

    for (fs::directory_iterator it(runtime_cache_root, ec), end; !ec && it != end; ++it) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto fingerprint_dir = it->path();
        if (fingerprint_dir.filename().string() == active_fingerprint) {
            continue;
        }

        bool owns_dir = false;
        std::error_code child_ec;
        for (fs::directory_iterator child(fingerprint_dir, child_ec), child_end; !child_ec && child != child_end; ++child) {
            if (fs::exists(child->path() / "worker-runtime.manifest", child_ec) && !child_ec) {
                owns_dir = true;
                break;
            }
        }
        if (owns_dir) {
            fs::remove_all(fingerprint_dir, ec);
            ec.clear();
        }
    }
}

bool EnsureWorkflowWorkerRuntimeSlot(
    size_t worker_idx,
    const DBWorkflowWorkerCoordinatorConfig& worker_cfg,
    std::filesystem::path* runtime_worker_exe_out,
    std::string* error_out) {
    namespace fs = std::filesystem;
    const fs::path source_worker_exe = worker_cfg.worker_exe_path;
    const fs::path dolphin_base_dir = worker_cfg.dolphin_base_dir;
    const fs::path source_sys = dolphin_base_dir / "Sys";
    const fs::path source_portable = dolphin_base_dir / "portable.txt";
    const fs::path source_dsp_coef = source_sys / "GC" / "dsp_coef.bin";

    std::error_code ec;
    if (!fs::is_regular_file(source_worker_exe, ec) || ec) {
        if (error_out) *error_out = "source SimCoreWorker.exe missing: " + source_worker_exe.string();
        return false;
    }
    if (!fs::is_directory(source_sys, ec) || ec || !fs::is_regular_file(source_dsp_coef, ec) || ec) {
        if (error_out) *error_out = "Dolphin base Sys is missing or incomplete: " + source_sys.string();
        return false;
    }
    if (!fs::is_regular_file(source_portable, ec) || ec) {
        if (error_out) *error_out = "Dolphin base portable.txt is missing: " + dolphin_base_dir.string();
        return false;
    }

    const auto fingerprint = WorkerRuntimeFingerprint(source_worker_exe, dolphin_base_dir);
    const fs::path runtime_cache_root = utils::getExecutablePath() / ".worker-runtime";
    PruneStaleWorkerRuntimeFingerprints(runtime_cache_root, fingerprint);

    const fs::path slot_root = runtime_cache_root / fingerprint / ("slot-" + std::to_string(worker_idx));
    const fs::path runtime_worker_exe = slot_root / "SimCoreWorker.exe";
    const fs::path runtime_sys = slot_root / "Sys";
    const fs::path runtime_user = slot_root / "User";
    const fs::path runtime_portable = slot_root / "portable.txt";
    const fs::path manifest_path = slot_root / "worker-runtime.manifest";

    std::string existing_manifest;
    if (ReadFileToString(manifest_path, &existing_manifest)
        && fs::is_regular_file(runtime_worker_exe, ec) && !ec
        && fs::is_regular_file(runtime_sys / "GC" / "dsp_coef.bin", ec) && !ec
        && fs::is_directory(runtime_user, ec) && !ec
        && fs::is_regular_file(runtime_portable, ec) && !ec) {
        const auto expected_hardlink_manifest = ExpectedWorkerRuntimeManifest(
            worker_idx,
            source_worker_exe,
            dolphin_base_dir,
            fingerprint,
            "hardlink");
        if (existing_manifest == expected_hardlink_manifest
            || existing_manifest.find("simcore_worker_runtime_manifest_version=2\n") == 0) {
            if (runtime_worker_exe_out) *runtime_worker_exe_out = runtime_worker_exe;
            return true;
        }
    }

    fs::remove_all(slot_root, ec);
    if (ec) {
        if (error_out) *error_out = "clear stale worker runtime slot failed: " + ec.message();
        return false;
    }
    fs::create_directories(slot_root, ec);
    if (ec) {
        if (error_out) *error_out = "create worker runtime slot failed: " + ec.message();
        return false;
    }

    std::string exe_materialization;
    if (!MaterializeWorkerExe(source_worker_exe, runtime_worker_exe, &exe_materialization, error_out)) {
        return false;
    }
    if (!CopyTree(source_sys, runtime_sys, error_out)) {
        return false;
    }
    fs::create_directories(runtime_user, ec);
    if (ec) {
        if (error_out) *error_out = "create runtime User directory failed: " + ec.message();
        return false;
    }
    fs::copy_file(source_portable, runtime_portable, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error_out) *error_out = "copy runtime portable.txt failed: " + ec.message();
        return false;
    }

    const auto manifest = ExpectedWorkerRuntimeManifest(
        worker_idx,
        source_worker_exe,
        dolphin_base_dir,
        fingerprint,
        exe_materialization);
    if (!WriteStringToFile(manifest_path, manifest, error_out)) {
        return false;
    }

    if (!fs::is_regular_file(runtime_worker_exe, ec) || ec
        || !fs::is_regular_file(runtime_sys / "GC" / "dsp_coef.bin", ec) || ec
        || !fs::is_directory(runtime_user, ec) || ec
        || !fs::is_regular_file(runtime_portable, ec) || ec) {
        if (error_out) *error_out = "worker runtime validation failed: " + slot_root.string();
        return false;
    }

    if (runtime_worker_exe_out) *runtime_worker_exe_out = runtime_worker_exe;
    return true;
}

} // namespace

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    simcore::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    ReadyStepPersistFn persist_materialization_fn,
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    simcore::db::IStateDb* state_db)
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
    simcore::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
    ReadyStepPersistFn persist_materialization_fn,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    simcore::db::IStateDb* state_db)
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
    if (step_completion_gate != nullptr) {
        step_completion_gate_ = step_completion_gate;
    } else {
        owned_step_completion_gate_ = std::make_unique<simcore::db::execution::workflow::StepCompletionGateService>();
        step_completion_gate_ = owned_step_completion_gate_.get();
    }
    if (program_kind_registry_ != nullptr) {
        adapter_chain_orchestrator_ = std::make_unique<simcore::db::execution::workflow::AdapterChainOrchestrator>(
            program_kind_registry_,
            step_completion_gate_);
    }
}

DBWorkflowWorkerCoordinator::~DBWorkflowWorkerCoordinator() {
    Stop();
}

void DBWorkflowWorkerCoordinator::Start() {
    if (worker_job_thread_.joinable() || worker_lifecycle_thread_.joinable()) {
        return;
    }

    stop_.store(false);
    job_materialization_service_.ResetForStart();

    {
        const auto desired_workers = desired_worker_count_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(workers_mtx_);
        workers_.clear();
        workers_.reserve(desired_workers);
        for (size_t i = 0; i < desired_workers; ++i) {
            auto slot = std::make_unique<WorkerSlot>();
            slot->id = i;
            slot->worker = std::make_unique<simcore::ProcessWorker>();
            slot->worker->set_progress_queue(&progress_q_);
            const auto surface_it = worker_visual_surfaces_.find(i);
            if (surface_it != worker_visual_surfaces_.end()) {
                slot->visual_render_widget_handle = surface_it->second.render_widget_handle;
                slot->visual_host_events_pipe_name = surface_it->second.host_events_pipe_name;
            }
            RegisterWorkerSlotTelemetry(*slot);
            workers_.push_back(std::move(slot));
        }
        worker_slot_count_.store(workers_.size(), std::memory_order_relaxed);
    }

    progress_drainer_thread_ = std::thread([this]() { DrainProgressLoop(); });
    results_drainer_thread_ = std::thread([this]() { DrainResultsLoop(); });
    job_materializer_thread_ = std::thread([this]() { job_materialization_service_.MaterializeClaimedJobPayloadLoop(stop_); });
    worker_lifecycle_thread_ = std::thread([this]() { WorkerLifecycleCoordinatorLoop(); });
    worker_job_thread_ = std::thread([this]() { WorkerJobCoordinatorLoop(); });
}

void DBWorkflowWorkerCoordinator::Stop() {
    StopVisualDebugReplay();
    stop_.store(true);
    queue_cv_.notify_all();
    job_materialization_service_.StopMaterializationLoop();
    progress_q_.close();
    results_q_.close();

    if (worker_job_thread_.joinable()) {
        worker_job_thread_.join();
    }
    if (worker_lifecycle_thread_.joinable()) {
        worker_lifecycle_thread_.join();
    }
    if (job_materializer_thread_.joinable()) {
        job_materializer_thread_.join();
    }
    if (progress_drainer_thread_.joinable()) {
        progress_drainer_thread_.join();
    }
    if (results_drainer_thread_.joinable()) {
        results_drainer_thread_.join();
    }

    std::vector<std::thread> startup_threads;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (auto& slot : workers_) {
            if (slot->startup_thread.joinable()) {
                startup_threads.push_back(std::move(slot->startup_thread));
            }
        }
    }
    for (auto& thread : startup_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        for (auto& slot : workers_) {
            StopWorkerSlot(*slot);
        }
        workers_.clear();
        worker_slot_count_.store(0, std::memory_order_relaxed);
    }
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
    if (!integration_cfg_.workflow_enabled) {
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
    if (!integration_cfg_.workflow_enabled) {
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
    std::lock_guard<std::mutex> lock(queue_mtx_);
    const auto key = ReadyDedupKey(step.workflow_step_id);
    if (!seen_ready_step_ids_.emplace(key).second) {
        return;
    }
    ready_queue_.push_back(step);
    queue_cv_.notify_one();
}

std::optional<ScheduledJobSet> DBWorkflowWorkerCoordinator::MaterializeWorkflowStep(const WorkflowReadyStep& step) {
    if (!integration_cfg_.workflow_enabled) {
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
    if (!schedule_ready_step_fn_) {
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

bool DBWorkflowWorkerCoordinator::SendJobToWorker(size_t worker_idx, uint64_t job_id, const simcore::PSJob& job) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    if (!slot.ready.load() || !slot.worker->try_acquire_slot()) {
        return false;
    }

    if (!slot.worker->send_job(job_id, epoch_.load(), job)) {
        slot.worker->release_slot();
        MarkWorkerError(slot, "send_job failed");
        return false;
    }

    slot.in_flight_job_id = job_id;
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), static_cast<std::int64_t>(job_id), std::nullopt);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Running);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
    return true;
}

bool DBWorkflowWorkerCoordinator::DispatchClaimedJobToWorker(size_t worker_idx, const ClaimedJobRecord& claimed_job) {
    const auto job_id = claimed_job.job_id;
    if (adapter_chain_orchestrator_) {
        simcore::db::execution::workflow::AdapterChainTrace trace{};
        (void)adapter_chain_orchestrator_->OnJobClaimed(claimed_job.step.step_kind, job_id, &trace);
        ++adapter_job_claimed_invocations_;
        EmitAdapterTraceEvent(claimed_job.step, "OnJobClaimed", "invoked", job_id, claimed_job.job_set_id);
    }
    if (!claimed_job.payload.has_value()) {
        return false;
    }
    if (!EnsureWorkerProgramForJob(worker_idx, claimed_job)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> worker_lock(workers_mtx_);
        dispatched_job_context_by_id_[static_cast<std::uint64_t>(job_id)] = DispatchedJobContext{
            .step = claimed_job.step,
            .job_set_id = claimed_job.job_set_id,
        };
    }
    if (!SendJobToWorker(worker_idx, static_cast<std::uint64_t>(job_id), *claimed_job.payload)) {
        std::lock_guard<std::mutex> worker_lock(workers_mtx_);
        dispatched_job_context_by_id_.erase(static_cast<std::uint64_t>(job_id));
        return false;
    }
    if (execution_db_ != nullptr && execution_db_->JobCommandService() != nullptr) {
        std::string error;
        if (!execution_db_->JobCommandService()->AppendLifecycleEvent(
                {
                    .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobClaimed,
                    .job_id = job_id,
                    .requested_by = "workflow_dispatch_coordinator",
                },
                &error)) {
            std::ostringstream line;
            line << "[workflow-job-claim-persist-failed] job=" << job_id;
            if (!error.empty()) {
                line << " error=" << error;
            }
            EmitDurableEventLine(line.str());
        }
    }
    std::lock_guard<std::mutex> worker_lock(workers_mtx_);
    if (worker_idx < workers_.size()) {
        workers_[worker_idx]->loaded_program_kind = claimed_job.program_kind;
        workers_[worker_idx]->loaded_program_runtime_affinity_key = claimed_job.affinity.program_runtime_affinity_key;
        workers_[worker_idx]->loaded_savestate_affinity_key = claimed_job.affinity.savestate_affinity_key;
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::DispatchNextEligibleForWorker(
    size_t worker_idx,
    const MaterializedJobSelectionAffinity& worker_affinity,
    std::chrono::steady_clock::time_point now) {
    ClaimedJobRecord candidate{};
    if (!job_materialization_service_.TrySelectMaterializedJobForWorker(worker_affinity, &candidate)) {
        return false;
    }

    if (!candidate.payload.has_value()) {
        std::ostringstream line;
        line << "[seedprobe-dispatch-invalid] job=" << candidate.job_id
             << " worker=" << worker_idx
             << " step=" << candidate.step.step_key
             << " kind=" << candidate.step.step_kind
             << " workflow_step_id=" << candidate.step.workflow_step_id
             << " job_set=" << candidate.job_set_id
             << " reason=missing_payload";
        EmitDurableEventLine(line.str());
        return false;
    }
    const bool dispatched = DispatchClaimedJobToWorker(worker_idx, candidate);
    if (!dispatched) {
        const bool requeued = job_materialization_service_.RequeueMaterializedJob(candidate.job_id);
        std::ostringstream line;
        line << "[seedprobe-dispatch-requeue] job=" << candidate.job_id
             << " worker=" << worker_idx
             << " step=" << candidate.step.step_key
             << " kind=" << candidate.step.step_kind
             << " workflow_step_id=" << candidate.step.workflow_step_id
             << " job_set=" << candidate.job_set_id
             << " reason=send_failed"
             << " requeued=" << (requeued ? "true" : "false");
        EmitDurableEventLine(line.str());
        return false;
    }
    const bool marked_dispatched = job_materialization_service_.MarkDispatched(candidate.job_id, now);
    std::ostringstream line;
    line << "[seedprobe-dispatch] job=" << candidate.job_id
         << " worker=" << worker_idx
         << " step=" << candidate.step.step_key
         << " kind=" << candidate.step.step_kind
         << " workflow_step_id=" << candidate.step.workflow_step_id
         << " job_set=" << candidate.job_set_id
         << " program_kind=" << candidate.program_kind
         << " claim_sequence=" << candidate.claim_sequence
         << " mark_dispatched=" << (marked_dispatched ? "true" : "false");
    if (candidate.affinity.savestate_affinity_key.has_value()) {
        line << " savestate_affinity=" << *candidate.affinity.savestate_affinity_key;
    }
    if (candidate.affinity.program_runtime_affinity_key.has_value()) {
        line << " runtime_affinity=" << *candidate.affinity.program_runtime_affinity_key;
    }
    EmitDurableEventLine(line.str());
    return true;
}

void DBWorkflowWorkerCoordinator::HandlePayloadMaterializationFailures() {
    for (const auto& failed_payload : job_materialization_service_.ListByState(ClaimedJobLifecycleState::MaterializationFailed)) {
        ++payload_materialization_failure_count_;
        std::ostringstream message;
        message << "job_id=" << failed_payload.job_id << ";job_set_id=" << failed_payload.job_set_id
                << ";reason=build_payload returned empty";
        EmitWorkflowFailureEvents(failed_payload.step, "BuildClaimedPayload", message.str());
        MaybeTerminalFailStepInStrictSmokeMode(failed_payload.step, "workflow_payload_materialize_strict_smoke");
        (void)job_materialization_service_.AbandonClaim(failed_payload.job_id);
    }
}

bool DBWorkflowWorkerCoordinator::EnsureWorkerProgramForJob(size_t worker_idx, const ClaimedJobRecord& claimed_job) {
    if (claimed_job.program_kind <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    if (!slot.ready.load() || slot.worker == nullptr) {
        return false;
    }

    const auto runtime_key = claimed_job.affinity.program_runtime_affinity_key;
    const auto savestate_key = claimed_job.affinity.savestate_affinity_key;
    const bool needs_program = !slot.loaded_program_kind.has_value()
        || slot.loaded_program_kind.value() != claimed_job.program_kind;
    const bool needs_runtime = slot.loaded_program_runtime_affinity_key != runtime_key;
    const bool needs_savestate = slot.loaded_savestate_affinity_key != savestate_key;
    if (!needs_program && !needs_runtime && !needs_savestate) {
        return true;
    }

    simcore::PSInit init{};
    init.default_timeout_ms = claimed_job.runtime_init.default_timeout_ms > 0
        ? static_cast<uint32_t>(claimed_job.runtime_init.default_timeout_ms)
        : 10000;
    init.derived_buffer_type = claimed_job.runtime_init.derived_buffer_type;
    if (claimed_job.runtime_init.savestate_ref_id > 0) {
        const auto savestate_path = PrepareWorkerSavestatePathForJob(worker_idx, claimed_job, needs_savestate);
        if (!savestate_path.has_value()) {
            MarkWorkerError(slot, "savestate materialization failed");
            return false;
        }
        init.savestate_path = *savestate_path;
    }
    if (!slot.worker->ctl_set_program(
        static_cast<uint8_t>(claimed_job.program_kind),
        static_cast<uint8_t>(claimed_job.program_kind),
        init)) {
        MarkWorkerError(slot, "ctl_set_program failed");
        return false;
    }
    if (!slot.worker->ctl_run_init_once()) {
        MarkWorkerError(slot, "ctl_run_init_once failed");
        return false;
    }
    if (!slot.worker->ctl_activate_main()) {
        MarkWorkerError(slot, "ctl_activate_main failed");
        return false;
    }
    if (worker_cfg_.visual_debug_workers && worker_cfg_.auto_resume_visual_workers) {
        bool resumed = false;
        for (int attempt = 0; attempt < 20 && !resumed; ++attempt) {
            resumed = slot.worker->visual_resume_emulation();
            if (!resumed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!resumed) {
            MarkWorkerError(slot, "visual resume failed");
            return false;
        }
    }

    slot.loaded_program_kind = claimed_job.program_kind;
    slot.loaded_program_runtime_affinity_key = runtime_key;
    slot.loaded_savestate_affinity_key = savestate_key;
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, claimed_job.program_kind);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Idle);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
    return true;
}

std::optional<std::string> DBWorkflowWorkerCoordinator::PrepareWorkerSavestatePathForJob(
    size_t worker_idx,
    const ClaimedJobRecord& claimed_job,
    bool force_rematerialize) {
    if (claimed_job.runtime_init.savestate_ref_id <= 0) {
        return std::string{};
    }
    if (state_db_ == nullptr) {
        return std::nullopt;
    }

    const auto worker_root = std::filesystem::path(worker_cfg_.worker_dir_root)
        / ".worker"
        / ("worker-" + std::to_string(worker_idx));
    const auto savestate_dir = worker_root / "savestate";
    const auto savestate_path = savestate_dir / "current.sav";

    std::error_code ec;
    if (!force_rematerialize && std::filesystem::exists(savestate_path, ec) && !ec) {
        return savestate_path.string();
    }

    std::filesystem::remove_all(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    std::filesystem::create_directories(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }

    std::string error;
    const auto materialized = state_db_->MaterializeSavestateToPath(
        claimed_job.runtime_init.savestate_ref_id,
        savestate_path.string(),
        &error);
    if (!materialized.has_value() || materialized->empty()) {
        return std::nullopt;
    }
    return materialized;
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

void DBWorkflowWorkerCoordinator::EnqueueProgressForTest(const simcore::PRProgress& progress) {
    progress_q_.push(progress);
}

void DBWorkflowWorkerCoordinator::EnqueueResultForTest(const simcore::PRResult& result) {
    results_q_.push(result);
}

PRStatus DBWorkflowWorkerCoordinator::SnapshotStatus() const {
    PRStatus status{};
    status.epoch = epoch_.load();
    status.workers = ActiveWorkerCount();

    std::lock_guard<std::mutex> lock(queue_mtx_);
    status.queued_jobs = ready_queue_.size();
    status.running_workers = materialized_count_;
    status.pending_start_workers = terminal_published_count_;
    status.ready_workers = status.workers;
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
    return telemetry;
}

std::vector<WorkerSnapshot> DBWorkflowWorkerCoordinator::SnapshotWorkers() const {
    return worker_status_.GetClusterSnapshot();
}

bool DBWorkflowWorkerCoordinator::SetWorkerVisualSurface(
    size_t worker_idx,
    uint64_t render_widget_handle,
    std::string host_events_pipe_name) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    worker_visual_surfaces_[worker_idx] = WorkerVisualSurface{
        .render_widget_handle = render_widget_handle,
        .host_events_pipe_name = std::move(host_events_pipe_name),
    };
    if (worker_idx < workers_.size()) {
        workers_[worker_idx]->visual_render_widget_handle = render_widget_handle;
        workers_[worker_idx]->visual_host_events_pipe_name = worker_visual_surfaces_[worker_idx].host_events_pipe_name;
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::StartVisualDebugReplay(
    std::int64_t job_id,
    uint64_t render_widget_handle,
    std::string host_events_pipe_name,
    std::string* error_out) {
    if (job_id <= 0) {
        if (error_out) *error_out = "job id must be positive";
        return false;
    }
    if (render_widget_handle == 0) {
        if (error_out) *error_out = "render widget handle is required";
        return false;
    }
    if (worker_cfg_.worker_exe_path.empty() || worker_cfg_.iso_path.empty() || worker_cfg_.worker_dir_root.empty()) {
        if (error_out) *error_out = "worker configuration is incomplete";
        return false;
    }

    StopVisualDebugReplay();

    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    const auto session_id = next_visual_debug_session_id_++;
    auto session = std::make_unique<VisualDebugSession>();
    session->session_id = session_id;
    session->job_id = job_id;
    session->worker_id = static_cast<size_t>(1000000 + session_id);
    session->render_widget_handle = render_widget_handle;
    session->host_events_pipe_name = std::move(host_events_pipe_name);
    session->worker = std::make_unique<simcore::ProcessWorker>();
    SetVisualDebugState(*session, VisualReplayRuntimeState::QueuedStartup, "visual debug replay queued");
    visual_debug_results_q_.reset();
    visual_debug_session_ = std::move(session);
    visual_debug_session_->thread = std::thread([this, session_id]() {
        VisualDebugReplayThread(session_id);
    });

    if (error_out) error_out->clear();
    return true;
}

bool DBWorkflowWorkerCoordinator::StopVisualDebugReplay() {
    std::thread thread_to_join;
    bool had_session = false;
    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_) {
            visual_debug_results_q_.reset();
            return false;
        }
        had_session = true;
        visual_debug_session_->stop_requested = true;
        SetVisualDebugState(*visual_debug_session_, VisualReplayRuntimeState::Stopping, "visual debug replay stopped");
        if (visual_debug_session_->worker) {
            visual_debug_session_->worker->stop();
        }
        visual_debug_results_q_.close();
        if (visual_debug_session_->thread.joinable()
            && visual_debug_session_->thread.get_id() != std::this_thread::get_id()) {
            thread_to_join = std::move(visual_debug_session_->thread);
        }
    }

    if (thread_to_join.joinable()) {
        thread_to_join.join();
    }

    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (visual_debug_session_ && visual_debug_session_->thread.joinable()
            && visual_debug_session_->thread.get_id() == std::this_thread::get_id()) {
            visual_debug_session_->thread.detach();
        }
        visual_debug_session_.reset();
        visual_debug_results_q_.reset();
    }
    return had_session;
}

bool DBWorkflowWorkerCoordinator::PauseVisualDebugReplayEmulation() {
    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    if (!visual_debug_session_ || !visual_debug_session_->worker) {
        return false;
    }
    return visual_debug_session_->worker->visual_pause_emulation();
}

bool DBWorkflowWorkerCoordinator::ResumeVisualDebugReplayEmulation() {
    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    if (!visual_debug_session_ || !visual_debug_session_->worker) {
        return false;
    }
    return visual_debug_session_->worker->visual_resume_emulation();
}

bool DBWorkflowWorkerCoordinator::StepVisualDebugReplayVm() {
    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    if (!visual_debug_session_ || !visual_debug_session_->worker) {
        return false;
    }
    return visual_debug_session_->worker->visual_step_vm();
}

VisualDebugReplaySnapshot DBWorkflowWorkerCoordinator::SnapshotVisualDebugReplay() const {
    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    if (!visual_debug_session_) {
        return {};
    }
    return VisualDebugReplaySnapshot{
        .active = true,
        .session_id = visual_debug_session_->session_id,
        .job_id = visual_debug_session_->job_id,
        .worker_id = static_cast<std::int64_t>(visual_debug_session_->worker_id),
        .state = visual_debug_session_->state,
        .detail = visual_debug_session_->detail,
        .controls_enabled = visual_debug_session_->controls_enabled,
    };
}

std::vector<std::string> DBWorkflowWorkerCoordinator::TakeVisualDebugLogLines() {
    std::lock_guard<std::mutex> lock(visual_debug_mtx_);
    if (!visual_debug_session_) {
        return {};
    }
    std::vector<std::string> lines;
    lines.swap(visual_debug_session_->pending_log_lines);
    return lines;
}

void DBWorkflowWorkerCoordinator::WorkerJobCoordinatorLoop() {
    while (!stop_.load()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto worker_target = ActiveWorkerCount();
        const auto buffered = job_materialization_service_.CountBufferedJobs();
        std::size_t claimed_count = 0;
        if (buffered < worker_target) {
            const auto claim_budget = worker_target - buffered;
            claimed_count = job_materialization_service_.ClaimJobs(claim_budget, now);
            if (claimed_count > 0) {
                std::ostringstream line;
                line << "[seedprobe-claim-batch] claimed=" << claimed_count
                     << " budget=" << claim_budget
                     << " buffered_before=" << buffered
                     << " worker_target=" << worker_target
                     << " buffered_after=" << job_materialization_service_.CountBufferedJobs();
                EmitDurableEventLine(line.str());
            }
        }

        HandlePayloadMaterializationFailures();

        bool dispatched_any = false;
        const auto dispatchable_workers = CollectDispatchableWorkers();
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
                ++dispatch_miss_count_;
            }
        }

        if (claimed_count == 0 && !dispatched_any) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            const auto sleep_ms = worker_cfg_.controller_sleep_ms ? worker_cfg_.controller_sleep_ms : 5;
            queue_cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms));
        }
    }
}

void DBWorkflowWorkerCoordinator::WorkerLifecycleCoordinatorLoop() {
    while (!stop_.load()) {
        ReconcileWorkerPool();

        std::unique_lock<std::mutex> lock(queue_mtx_);
        const auto sleep_ms = worker_cfg_.controller_sleep_ms ? worker_cfg_.controller_sleep_ms : 5;
        queue_cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms));
    }
}

void DBWorkflowWorkerCoordinator::ProcessReadyWorkflowStep(const WorkflowReadyStep& step) {
    if (!integration_cfg_.workflow_enabled) {
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
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
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
    simcore::PRProgress progress;
    while (progress_q_.pop_wait(progress)) {
        std::vector<simcore::PRProgress> batch;
        batch.reserve(kMaxBatchSize);
        batch.push_back(progress);
        simcore::PRProgress next;
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
            worker_status_.RecordProgress(
                static_cast<std::int64_t>(item.worker_id),
                item.text,
                static_cast<std::int64_t>(item.job_id));
            if (execution_db_ != nullptr && execution_db_->JobCommandService() != nullptr && item.job_id > 0) {
                std::ostringstream message;
                message << "worker=" << item.worker_id << " progress=" << item.text;
                std::string error;
                if (!execution_db_->JobCommandService()->AppendLifecycleEvent(
                        {
                            .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
                            .job_id = static_cast<std::int64_t>(item.job_id),
                            .message = message.str(),
                            .requested_by = "workflow_progress_drainer",
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
            }
            if (progress_callback_) {
                progress_callback_(item);
            }
        }
    }
}

void DBWorkflowWorkerCoordinator::DrainResultsLoop() {
    simcore::PRResult result;
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
                worker_in_flight_job_id = workers_[result.worker_id]->in_flight_job_id;
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

        if (context.has_value() && adapter_chain_orchestrator_) {
            simcore::db::execution::workflow::AdapterChainTrace trace{};
            std::string adapter_error;
            const auto mapped = adapter_chain_orchestrator_->OnJobTerminal(
                context->step.step_kind,
                static_cast<std::int64_t>(result.job_id),
                result,
                &trace,
                &adapter_error);
            ++adapter_job_terminal_invocations_;
            EmitAdapterTraceEvent(
                context->step,
                "OnJobTerminal",
                mapped.has_value() ? "invoked" : "failed",
                static_cast<std::int64_t>(result.job_id),
                context->job_set_id,
                adapter_error.empty() ? std::nullopt : std::optional<std::string>(adapter_error));
            if (mapped.has_value() && result_map_event_callback_) {
                for (const auto& line : mapped->event_lines) {
                    result_map_event_callback_(line);
                }
            }
            const auto mapped_output_ref_kind = mapped.has_value() && !mapped->output_ref_kind.empty()
                ? mapped->output_ref_kind
                : (mapped.has_value() ? mapped->result_kind : std::string{});
            const auto mapped_output_ref_id = mapped.has_value() && mapped->output_ref_id > 0
                ? mapped->output_ref_id
                : (mapped.has_value() ? mapped->result_ref_id : 0);
            if (mapped.has_value()
                && !mapped->output_key.empty()
                && !mapped->output_data_kind.empty()
                && !mapped_output_ref_kind.empty()
                && mapped_output_ref_id > 0
                && execution_db_ != nullptr) {
                std::string output_error;
                if (!execution_db_->RecordJobOutput(
                        {
                            .job_id = static_cast<std::int64_t>(result.job_id),
                            .output_key = mapped->output_key,
                            .data_kind = mapped->output_data_kind,
                            .ref_kind = mapped_output_ref_kind,
                            .ref_id = mapped_output_ref_id,
                            .requested_by = "workflow_worker_result_mapper",
                        },
                        &output_error)) {
                    std::ostringstream line;
                    line << "[execution-job-output-record-failed] job=" << result.job_id
                         << " worker=" << result.worker_id
                         << " step=" << context->step.step_key
                         << " kind=" << context->step.step_kind
                         << " output_key=" << mapped->output_key
                         << " output_ref_kind=" << mapped_output_ref_kind
                         << " output_ref_id=" << mapped_output_ref_id;
                    if (!output_error.empty()) {
                        line << " error=" << output_error;
                    }
                    EmitDurableEventLine(line.str());
                }
            }
            if (!mapped.has_value() && !adapter_error.empty()) {
                std::ostringstream line;
                line << "[seedprobe-result-map-failed] job=" << result.job_id
                     << " worker=" << result.worker_id
                     << " step=" << context->step.step_key
                     << " kind=" << context->step.step_kind
                     << " workflow_step_id=" << context->step.workflow_step_id
                     << " job_set=" << context->job_set_id
                     << " error=" << adapter_error;
                EmitDurableEventLine(line.str());
                MarkDeterministicFailure(
                    context->step,
                    static_cast<std::int64_t>(result.job_id),
                    context->job_set_id,
                    "ADAPTER_RESULT_PERSIST_FAILED:" + adapter_error);
            }
        }

        ReleaseWorkerByResult(result);
        (void)job_materialization_service_.CleanupDispatchedOrExpired(static_cast<std::int64_t>(result.job_id));
        if (result_callback_) {
            result_callback_(result);
        }
    }
}

void DBWorkflowWorkerCoordinator::ReconcileWorkerPool() {
    std::vector<std::thread> completed_startup_threads;
    std::unique_lock<std::mutex> lock(workers_mtx_);
    const auto desired_workers = desired_worker_count_.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const auto max_start_attempts = std::max<std::uint32_t>(1u, worker_cfg_.max_worker_start_attempts);
    const auto max_concurrent_starts = std::max<std::uint32_t>(1u, worker_cfg_.max_concurrent_worker_starts);
    while (workers_.size() < desired_workers) {
        const auto worker_idx = workers_.size();
        auto slot = std::make_unique<WorkerSlot>();
        slot->id = worker_idx;
        slot->worker = std::make_unique<simcore::ProcessWorker>();
        slot->worker->set_progress_queue(&progress_q_);
        const auto surface_it = worker_visual_surfaces_.find(worker_idx);
        if (surface_it != worker_visual_surfaces_.end()) {
            slot->visual_render_widget_handle = surface_it->second.render_widget_handle;
            slot->visual_host_events_pipe_name = surface_it->second.host_events_pipe_name;
        }
        workers_.push_back(std::move(slot));
    }
    worker_slot_count_.store(workers_.size(), std::memory_order_relaxed);

    for (auto& slot : workers_) {
        if (!slot->startup_in_progress && slot->startup_thread.joinable()) {
            completed_startup_threads.push_back(std::move(slot->startup_thread));
        }
    }

    std::uint32_t active_startups = 0;
    for (const auto& slot : workers_) {
        if (slot->startup_in_progress) {
            ++active_startups;
        }
    }

    for (size_t worker_idx = 0; worker_idx < workers_.size(); ++worker_idx) {
        if (active_startups >= max_concurrent_starts) {
            break;
        }
        auto& slot = *workers_[worker_idx];
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
                    EmitDurableEventLine(line.str());
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
            EmitDurableEventLine(line.str());
            ResetWorkerSlotRuntime(slot);
        }
        if (slot.start_attempted) {
            continue;
        }
        if (StartWorkerSlot(worker_idx)) {
            ++active_startups;
        }
    }

    while (workers_.size() > desired_workers) {
        auto& slot = workers_.back();
        if (slot->startup_in_progress || slot->startup_thread.joinable() || slot->in_flight_job_id.has_value()) {
            break;
        }
        StopWorkerSlot(*slot);
        workers_.pop_back();
    }
    worker_slot_count_.store(workers_.size(), std::memory_order_relaxed);

    lock.unlock();
    for (auto& thread : completed_startup_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool DBWorkflowWorkerCoordinator::StartWorkerSlot(size_t worker_idx) {
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    if (slot.startup_in_progress || slot.startup_thread.joinable()) {
        return false;
    }
    slot.start_attempted = true;
    slot.startup_in_progress = true;
    slot.ready.store(false);
    slot.start_retry_exhausted_logged = false;
    const auto attempt = slot.start_attempts + 1;
    slot.start_attempts = attempt;
    const auto schedule_retry = [&](const std::string& error) {
        slot.ready.store(false);
        slot.startup_in_progress = false;
        slot.last_start_error = error;
        slot.next_start_after = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(worker_cfg_.worker_start_retry_backoff_ms);
        slot.loaded_program_kind.reset();
        slot.loaded_program_runtime_affinity_key.reset();
        slot.loaded_savestate_affinity_key.reset();
        if (slot.worker) {
            slot.worker->stop();
        }
        std::ostringstream line;
        line << "[workflow-worker-start-failed]"
             << " worker=" << worker_idx
             << " attempt=" << attempt
             << " max_attempts=" << std::max<std::uint32_t>(1u, worker_cfg_.max_worker_start_attempts)
             << " error=" << error;
        EmitDurableEventLine(line.str());
        MarkWorkerError(slot, error);
        return false;
    };

    std::filesystem::path runtime_worker_exe;
    std::string runtime_error;
    if (!EnsureWorkflowWorkerRuntimeSlot(worker_idx, worker_cfg_, &runtime_worker_exe, &runtime_error)) {
        RegisterWorkerSlotTelemetry(slot);
        return schedule_retry("worker runtime setup failed: " + runtime_error);
    }

    simcore::ProcStartParams ps{};
    ps.worker_id = worker_idx;
    ps.exe_path = runtime_worker_exe.string();
    ps.iso_path = worker_cfg_.iso_path;
    ps.dolphin_base_dir.clear();
    ps.visual = worker_cfg_.visual_workers || worker_cfg_.visual_debug_workers;
    ps.visual_debug = worker_cfg_.visual_debug_workers;
    if (ps.visual) {
        ps.render_widget_handle = slot.visual_render_widget_handle;
        ps.visual_host_events_pipe_name = slot.visual_host_events_pipe_name;
    }
    ps.visual_screenshot_dir = worker_cfg_.visual_screenshot_dir;

    std::ostringstream user_dir;
    user_dir << worker_cfg_.worker_dir_root << "\\workflow-worker-" << worker_idx << "\\User";
    ps.user_dir = user_dir.str();
    ps.vm_control = true;

    namespace fs = std::filesystem;
    std::error_code user_ec;
    fs::remove_all(ps.user_dir, user_ec);
    if (user_ec) {
        return schedule_retry("clear worker user dir failed: " + user_ec.message());
    }
    fs::create_directories(ps.user_dir, user_ec);
    if (user_ec) {
        return schedule_retry("create worker user dir failed: " + user_ec.message());
    }

    simcore::ProcessWorker* worker = slot.worker.get();
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Spawning);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
    slot.startup_thread = std::thread([this, worker_idx, attempt, worker, ps = std::move(ps)]() mutable {
        if (stop_.load()) {
            CompleteWorkerSlotStartup(worker_idx, attempt, false, "startup canceled");
            return;
        }

        if (worker == nullptr || !worker->start(ps, &results_q_)) {
            CompleteWorkerSlotStartup(worker_idx, attempt, false, "ProcessWorker.start failed");
            return;
        }

        const auto timeout = std::chrono::milliseconds(worker_cfg_.worker_start_timeout_ms
            ? worker_cfg_.worker_start_timeout_ms
            : 10000);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        bool ready = false;
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            if (worker->wait_ready(50)) {
                ready = true;
                break;
            }
            if (worker->is_failed()) {
                break;
            }
        }

        if (ready) {
            CompleteWorkerSlotStartup(worker_idx, attempt, true, {});
            return;
        }

        std::ostringstream error;
        if (stop_.load()) {
            error << "startup canceled";
        } else {
            error << "wait_ready failed err=" << worker->ready_error();
        }
        worker->stop();
        CompleteWorkerSlotStartup(worker_idx, attempt, false, error.str());
    });
    return true;
}

void DBWorkflowWorkerCoordinator::CompleteWorkerSlotStartup(
    size_t worker_idx,
    uint32_t attempt,
    bool ready,
    const std::string& error) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return;
    }

    auto& slot = *workers_[worker_idx];
    if (slot.start_attempts != attempt) {
        return;
    }

    slot.startup_in_progress = false;
    slot.ready.store(ready);
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
    slot.next_start_after = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(worker_cfg_.worker_start_retry_backoff_ms);
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();

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

void DBWorkflowWorkerCoordinator::ResetWorkerSlotRuntime(WorkerSlot& slot) {
    slot.ready.store(false);
    slot.start_attempted = false;
    slot.startup_in_progress = false;
    slot.in_flight_job_id.reset();
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();
    if (slot.worker) {
        slot.worker->stop();
    }
    slot.worker = std::make_unique<simcore::ProcessWorker>();
    slot.worker->set_progress_queue(&progress_q_);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Spawning);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
}

void DBWorkflowWorkerCoordinator::StopWorkerSlot(WorkerSlot& slot) {
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Stopping);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    slot.ready.store(false);
    slot.start_attempted = false;
    slot.startup_in_progress = false;
    slot.in_flight_job_id.reset();
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();
    if (slot.worker) {
        slot.worker->stop();
    }
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Dead);
    worker_status_.UnregisterWorker(static_cast<std::int64_t>(slot.id));
}

void DBWorkflowWorkerCoordinator::VisualDebugReplayThread(std::uint64_t session_id) {
    auto update_state = [this, session_id](VisualReplayRuntimeState state, const std::string& detail) {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (visual_debug_session_ && visual_debug_session_->session_id == session_id) {
            SetVisualDebugState(*visual_debug_session_, state, detail);
        }
    };
    auto fail = [&](const std::string& detail) {
        update_state(VisualReplayRuntimeState::Failed, detail);
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (visual_debug_session_ && visual_debug_session_->session_id == session_id) {
            visual_debug_session_->controls_enabled = false;
        }
    };

    std::int64_t job_id = 0;
    size_t worker_id = 0;
    uint64_t render_widget_handle = 0;
    std::string host_events_pipe_name;
    simcore::ProcessWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_ || visual_debug_session_->session_id != session_id) {
            return;
        }
        job_id = visual_debug_session_->job_id;
        worker_id = visual_debug_session_->worker_id;
        render_widget_handle = visual_debug_session_->render_widget_handle;
        host_events_pipe_name = visual_debug_session_->host_events_pipe_name;
        worker = visual_debug_session_->worker.get();
        SetVisualDebugState(*visual_debug_session_, VisualReplayRuntimeState::LaunchingWorker, "materializing visual debug job");
    }

    ClaimedJobRecord claimed_job{};
    std::string materialize_error;
    if (!job_materialization_service_.MaterializeJobForDebugReplay(job_id, &claimed_job, &materialize_error)) {
        fail("materialize visual debug job failed: " + materialize_error);
        return;
    }

    std::filesystem::path runtime_worker_exe;
    std::string runtime_error;
    if (!EnsureWorkflowWorkerRuntimeSlot(worker_id, worker_cfg_, &runtime_worker_exe, &runtime_error)) {
        fail("worker runtime setup failed: " + runtime_error);
        return;
    }

    simcore::ProcStartParams ps{};
    ps.worker_id = worker_id;
    ps.exe_path = runtime_worker_exe.string();
    ps.iso_path = worker_cfg_.iso_path;
    ps.dolphin_base_dir.clear();
    ps.visual = true;
    ps.visual_debug = true;
    ps.render_widget_handle = render_widget_handle;
    ps.visual_host_events_pipe_name = host_events_pipe_name;
    ps.visual_screenshot_dir = worker_cfg_.visual_screenshot_dir;
    ps.vm_control = true;
    ps.user_dir = (std::filesystem::path(worker_cfg_.worker_dir_root)
        / ("visual-debug-" + std::to_string(session_id))
        / "User").string();

    std::error_code user_ec;
    std::filesystem::remove_all(ps.user_dir, user_ec);
    if (user_ec) {
        fail("clear visual debug user dir failed: " + user_ec.message());
        return;
    }
    std::filesystem::create_directories(ps.user_dir, user_ec);
    if (user_ec) {
        fail("create visual debug user dir failed: " + user_ec.message());
        return;
    }

    update_state(VisualReplayRuntimeState::LaunchingWorker, "starting visual debug worker");
    if (worker == nullptr || !worker->start(ps, &visual_debug_results_q_)) {
        fail("ProcessWorker.start failed");
        return;
    }

    const auto timeout = std::chrono::milliseconds(worker_cfg_.worker_start_timeout_ms
        ? worker_cfg_.worker_start_timeout_ms
        : 10000);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(visual_debug_mtx_);
            if (!visual_debug_session_ || visual_debug_session_->session_id != session_id || visual_debug_session_->stop_requested) {
                return;
            }
        }
        if (worker->wait_ready(50)) {
            ready = true;
            break;
        }
        if (worker->is_failed()) {
            break;
        }
    }
    if (!ready) {
        std::ostringstream error;
        error << "wait_ready failed err=" << worker->ready_error();
        fail(error.str());
        worker->stop();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_ || visual_debug_session_->session_id != session_id) {
            return;
        }
        SetVisualDebugState(*visual_debug_session_, VisualReplayRuntimeState::AttachReady, "visual debug worker ready");
        visual_debug_session_->controls_enabled = true;
    }

    VisualDebugSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_ || visual_debug_session_->session_id != session_id) {
            return;
        }
        session = visual_debug_session_.get();
    }
    std::string configure_error;
    if (!ConfigureVisualDebugWorkerForJob(*session, claimed_job, &configure_error)) {
        fail("configure visual debug worker failed: " + configure_error);
        worker->stop();
        return;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (worker->visual_resume_emulation()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_ || visual_debug_session_->session_id != session_id) {
            return;
        }
        SetVisualDebugState(*visual_debug_session_, VisualReplayRuntimeState::Active, "visual debug replay active");
        visual_debug_session_->controls_enabled = true;
    }

    if (!worker->try_acquire_slot()) {
        fail("visual debug worker slot is busy");
        worker->stop();
        return;
    }
    if (!claimed_job.payload.has_value()
        || !worker->send_job(static_cast<std::uint64_t>(job_id), epoch_.load(), *claimed_job.payload)) {
        worker->release_slot();
        fail("send visual debug job failed");
        worker->stop();
        return;
    }

    simcore::PRResult result{};
    if (!visual_debug_results_q_.pop_wait(result)) {
        worker->release_slot();
        return;
    }
    worker->release_slot();
    worker->stop();

    {
        std::lock_guard<std::mutex> lock(visual_debug_mtx_);
        if (!visual_debug_session_ || visual_debug_session_->session_id != session_id) {
            return;
        }
        std::ostringstream detail;
        detail << "visual debug replay finished"
               << " job=" << result.job_id
               << " accepted=" << (result.accepted ? "true" : "false");
        SetVisualDebugState(*visual_debug_session_, VisualReplayRuntimeState::Finished, detail.str());
        visual_debug_session_->controls_enabled = false;
    }
}

bool DBWorkflowWorkerCoordinator::ConfigureVisualDebugWorkerForJob(
    VisualDebugSession& session,
    const ClaimedJobRecord& claimed_job,
    std::string* error_out) {
    if (session.worker == nullptr) {
        if (error_out) *error_out = "worker is unavailable";
        return false;
    }
    if (claimed_job.program_kind <= 0) {
        if (error_out) *error_out = "program kind is invalid";
        return false;
    }

    simcore::PSInit init{};
    init.default_timeout_ms = claimed_job.runtime_init.default_timeout_ms > 0
        ? static_cast<uint32_t>(claimed_job.runtime_init.default_timeout_ms)
        : 10000;
    init.derived_buffer_type = claimed_job.runtime_init.derived_buffer_type;
    if (claimed_job.runtime_init.savestate_ref_id > 0) {
        const auto savestate_path = PrepareVisualDebugSavestatePathForJob(session, claimed_job);
        if (!savestate_path.has_value()) {
            if (error_out) *error_out = "savestate materialization failed";
            return false;
        }
        init.savestate_path = *savestate_path;
    }
    if (!session.worker->ctl_set_program(
        static_cast<uint8_t>(claimed_job.program_kind),
        static_cast<uint8_t>(claimed_job.program_kind),
        init)) {
        if (error_out) *error_out = "ctl_set_program failed";
        return false;
    }
    if (!session.worker->ctl_run_init_once()) {
        if (error_out) *error_out = "ctl_run_init_once failed";
        return false;
    }
    if (!session.worker->ctl_activate_main()) {
        if (error_out) *error_out = "ctl_activate_main failed";
        return false;
    }
    if (error_out) error_out->clear();
    return true;
}

std::optional<std::string> DBWorkflowWorkerCoordinator::PrepareVisualDebugSavestatePathForJob(
    const VisualDebugSession& session,
    const ClaimedJobRecord& claimed_job) {
    if (claimed_job.runtime_init.savestate_ref_id <= 0) {
        return std::string{};
    }
    if (state_db_ == nullptr) {
        return std::nullopt;
    }

    const auto savestate_dir = std::filesystem::path(worker_cfg_.worker_dir_root)
        / ("visual-debug-" + std::to_string(session.session_id))
        / "savestate";
    const auto savestate_path = savestate_dir / "current.sav";

    std::error_code ec;
    std::filesystem::remove_all(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    std::filesystem::create_directories(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }

    std::string error;
    const auto materialized = state_db_->MaterializeSavestateToPath(
        claimed_job.runtime_init.savestate_ref_id,
        savestate_path.string(),
        &error);
    if (!materialized.has_value() || materialized->empty()) {
        return std::nullopt;
    }
    return materialized;
}

void DBWorkflowWorkerCoordinator::SetVisualDebugState(
    VisualDebugSession& session,
    VisualReplayRuntimeState state,
    std::string detail) {
    session.state = state;
    session.detail = std::move(detail);
    session.pending_log_lines.push_back(session.detail);
}

std::vector<DBWorkflowWorkerCoordinator::DispatchableWorkerInfo> DBWorkflowWorkerCoordinator::CollectDispatchableWorkers() {
    std::vector<DispatchableWorkerInfo> dispatchable;
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (workers_.empty()) {
        return dispatchable;
    }

    for (size_t i = 0; i < workers_.size(); ++i) {
        const size_t idx = (rr_worker_cursor_ + i) % workers_.size();
        auto& slot = *workers_[idx];
        if (!slot.ready.load()) {
            continue;
        }
        if (slot.in_flight_job_id.has_value()) {
            continue;
        }
        dispatchable.push_back(DispatchableWorkerInfo{
            .worker_idx = idx,
            .loaded_program_kind = slot.loaded_program_kind,
            .loaded_program_runtime_affinity_key = slot.loaded_program_runtime_affinity_key,
            .loaded_savestate_affinity_key = slot.loaded_savestate_affinity_key,
        });
    }

    if (!dispatchable.empty()) {
        rr_worker_cursor_ = (dispatchable.back().worker_idx + 1) % workers_.size();
    }
    return dispatchable;
}

void DBWorkflowWorkerCoordinator::ReleaseWorkerByResult(const simcore::PRResult& result) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (result.worker_id >= workers_.size()) {
        return;
    }

    auto& slot = *workers_[result.worker_id];
    slot.in_flight_job_id.reset();
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
                .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
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
        result_map_event_callback_(line);
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

void DBWorkflowWorkerCoordinator::PollReadyStepsFromDb() {
    const auto t0 = std::chrono::steady_clock::now();
    if (execution_db_ == nullptr || execution_db_->WorkflowQueryService() == nullptr) {
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

} // namespace simcore::runner::parallel::simcoredb
