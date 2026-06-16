# Visual Debugger Architecture Plan

## Status

Current architecture document.

This document describes the implemented SAVOR visual debug replay model. It replaces the older DB-backed
`debug_sessions` / `StartDebug` / `DebugSlotBusy` design. The current implementation is intentionally
in-memory and coordinator-owned.

## Current Product Behavior

- Visual debug replay is opt-in and does not change normal worker throughput behavior.
- The user starts replay from Jobs or Explorer results for an existing job.
- Replay is not a new canonical job, not a normal queue entry, and not part of generic worker dispatch.
- Starting a new replay stops any currently active replay session first.
- The visual replay dialog is reused. Each replay passes the same native render widget handle to the
  replay worker, so new sessions should paint into the existing render surface instead of opening a
  separate stale render window.

## Current Components

### GUI

- `JobsPage` and `ExplorerRunsPage` emit visual replay requests for selected jobs.
- `MainWindow` routes requests to a hidden/reused `CoordinatorPane` host.
- `CoordinatorPane` owns a reusable `VisualReplayDialog`.
- `VisualReplayDialog` owns:
  - a native Qt render widget used as the Dolphin render target,
  - a live log model/view,
  - pause, resume, and VM-step controls,
  - a `VisualReplayCoordinator` for log polling and host-event pipe listening.
- Before every replay, `CoordinatorPane` resets the dialog state, shows the render surface, obtains
  `renderWidget()->winId()`, and passes that handle to the coordinator.

### DBWorkflowWorkerCoordinator

- Normal workers live in `workers_` as `WorkerSlot` entries.
- Visual debug replay uses a separate `VisualDebugSession`, not a normal `WorkerSlot`.
- `VisualDebugSession` owns its own `ProcessWorker`, private result queue, user directory, and synthetic
  worker id of `1000000 + session_id`.
- `StartVisualDebugReplay(job_id, render_widget_handle, host_events_pipe_name)`:
  1. validates the request,
  2. calls `StopVisualDebugReplay()` to enforce one active in-memory replay session,
  3. creates a new `VisualDebugSession`,
  4. starts `VisualDebugReplayThread(session_id)`.
- `VisualDebugReplayThread` materializes the selected job payload through `JobMaterializationService`,
  starts a private visual-debug worker, configures the program/runtime, sends the job, waits for one result,
  then stops the worker and marks the replay finished.

### SavorWorker / Runtime

- The replay worker starts with `visual=true`, `visual_debug=true`, and `vm_control=true`.
- Visual debug mode enables VM pause/resume/step control through the worker control channel.
- The render surface is provided by the GUI widget handle passed through process start parameters.
- Host events are sent through the visual host-events pipe and shown in the replay dialog log.

## Deliberate Non-Goals For Current Architecture

- No persisted `debug_sessions` table.
- No DB transaction guard for a debug slot.
- No durable debugger recovery after coordinator restart.
- No separate debug queue or canonical debug job row.
- No deterministic `DebugSlotBusy` rejection. A new replay request replaces the current in-memory replay
  by stopping it first.
- No shared-memory video transport. The current path relies on Dolphin's render surface integration.

## Normal Worker Path

Normal worker dispatch remains separate:

- `workers_` is sized from the desired worker count.
- Worker lifecycle, claim/materialize/dispatch, progress, and result draining operate on normal
  `WorkerSlot` instances.
- Optional visual worker dashboard mode can make normal workers render-enabled through
  `visual_workers`, but that is separate from visual debug replay.
- `SnapshotWorkers()` reports the normal worker pool through `WorkerStatusRegistry`.
- `SnapshotVisualDebugReplay()` reports the separate in-memory replay session.

## Current Acceptance Criteria

- Starting visual replay for a valid job creates one private replay worker outside the normal worker pool.
- Normal worker dispatch and queued job throughput are unaffected by replay.
- A new replay request stops the previous replay session before starting the next one.
- The same visual replay dialog and render widget are reused across replay requests.
- Pause, resume, and VM-step controls target the active replay worker.
- Closing the replay dialog stops the active replay and clears the stored render widget handle/host-events pipe.
- Replay completion hides the render surface and shows completion state without leaving a separate stale window.

## Future Work

- Add clearer UI language that this is "visual replay" of an existing job rather than a persistent debugger session.
- Add telemetry for replay startup time, worker launch failures, wait-ready failures, replay completion status,
  and control-command failures.
- Decide whether a durable debugger session model is ever needed. Do not reintroduce DB-backed
  `debug_sessions` unless restart recovery, multi-client ownership, or audit requirements justify it.
