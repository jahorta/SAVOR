# Visual Debugger Architecture Plan

## Goals

- Add an **opt-in** visual debugger workflow without changing normal worker throughput behavior.
- Trigger debugging from the Jobs pane by opening an existing job's details (double-click), not by creating a separate debug job entry.
- Stream Dolphin video to GUI with low latency.
- Support interactive controls:
  - frame step,
  - run-to-breakpoint,
  - VM instruction step,
  - pause/stop,
  - mode switching in either direction at any time.
- Display current script location and human-readable input state.

## Updated Product Behavior

### Job triggering model

- Debug execution is **not** a separate queue entry and is **not** part of normal claim/ready job selection.
- User flow:
  1. User opens Jobs pane.
  2. User double-clicks a job to open details.
  3. User clicks `Start Visual Debug` in the details view.
  4. GUI sends `StartDebug(job_id)` to coordinator.
- Coordinator spins up/allocates a visual debug worker session for that `job_id`.
- Visual worker loads/decodes job definition from DB by `job_id` and executes it in debug runtime mode.

### Job state and locking policy (resolved)

- Debug runs are **read-only** with respect to canonical job outputs.
- Only jobs already in a **terminal state** are debug-eligible.
- While debug is active, the job remains locked in terminal state via a DB-backed debug-session lock.

## High-Level Architecture

Use a **dual-plane design**:

1. **Coordinator control/lifecycle plane**
   - Handles explicit async `StartDebug(job_id)` requests from GUI.
   - Allocates visual debug worker slot/process.
   - Tracks lifecycle, health, and completion status for the debug session.

2. **Debugger side-channel plane**
   - Dedicated debugger communication bypasses worker's normal command path.
   - GUI communicates directly with:
     - VM debug endpoint (script execution control/introspection),
     - Dolphin debug endpoint (video + frame/run controls).
   - Worker only brokers endpoint creation/discovery and remains lifecycle owner.

## Data Model and Transaction Guard (resolved)

### `debug_sessions` table

Add a dedicated table to represent active/recent debug sessions and enforce locking:

- `id` (PK)
- `job_id` (FK to jobs)
- `state` (`starting`, `attach_ready`, `active`, `stopping`, `stopped`, `failed`)
- `created_at`, `updated_at`
- `started_by`
- `worker_id` / `slot_id`
- `session_token` (ephemeral capability reference)
- `lock_acquired_at`, `lock_released_at`
- `failure_code`, `failure_detail`

### Transaction guard

`StartDebug(job_id)` admission path executes in a DB transaction:
1. Verify job exists.
2. Verify job is terminal.
3. Verify no active `debug_sessions` row for same `job_id`.
4. Verify global debug slot is free.
5. Insert `debug_sessions` row in `starting` and commit.

This transaction is the enforcement mechanism for read-only debug admission and lock correctness.
No extra enforcement layer is required beyond this transaction guard policy.

## Components and Responsibilities

### 1) GUI

- Jobs pane behavior:
  - double-click a job row opens details,
  - details view exposes `Start Visual Debug` and `Stop Debugging`.
- Sends `StartDebug(job_id)` request to coordinator.
- Polls/subscribes for async session state until attach-ready.
- Opens Debugger pane for active session.
- Attaches to debug endpoints and renders:
  - live video viewport,
  - script location,
  - current input text,
  - runtime state/telemetry,
  - audit/log output sourced from worker log files.
- Sends interactive commands:
  - `Step VM Instruction`,
  - `Step Frame`,
  - `Run To BP`,
  - `Pause`,
  - `Stop Debugging`.

### 2) DBWorkerCoordinator

- Accept explicit debug start requests by job ID.
- Keep visual debug capacity separate from normal workers.
  - fixed `debug_slot_count = 1`.
- If debug slot is occupied, reject new `StartDebug` requests with deterministic `DebugSlotBusy` response.
- For each accepted request:
  - validate and lock via transaction guard,
  - allocate/spawn visual debug worker session,
  - pass `job_id` and debug launch params.
- Track session metadata:
  - session ID,
  - mapped job ID,
  - endpoint URIs/pipe names,
  - attach/controller state,
  - run mode summary,
  - lock ownership metadata.
- On `StopDebug` / worker exit / failure:
  - tear down debug worker,
  - mark debug session stopped/failed,
  - release lock,
  - free debug slot.

### 3) Async `StartDebug` and simple cancellation semantics

- `StartDebug(job_id)` returns immediately with:
  - `request_id`,
  - initial status (`QueuedStartup` or `Rejected*`).
- GUI observes request via status stream or polling:
  - `QueuedStartup` -> `LaunchingWorker` -> `AttachReady` or `Failed`.
- Simple cancellation:
  - allow `CancelStartDebug(request_id)` only while in `QueuedStartup` / `LaunchingWorker`.
  - best-effort semantics: if worker already reached attach-ready, cancellation is rejected as `TooLate`; user must use `StopDebug`.

### 4) Visual Worker / Debug Session Host

- Launch Dolphin in render-enabled (non-headless) mode for debug sessions.
- Resolve job payload from DB using provided `job_id`.
- Host two direct-debug endpoints:
  - **VM endpoint**
    - instruction stepping,
    - script location snapshots (`script`, `pc`, source span),
    - VM run state/break reason.
  - **Dolphin endpoint**
    - frame stepping,
    - run-to-bp,
    - pause/interrupt,
    - frame/video publication,
    - current-frame input summary.
- Publish endpoint metadata back to coordinator for GUI attach.

### 5) DolphinWrapper changes (required)

- Extend `DolphinWrapper` to support runtime-selectable launch mode:
  - default remains headless,
  - debug mode enables video output/render path.
- Ensure this mode is opt-in per session and does not alter normal worker defaults.
- Surface pixel format/color space metadata needed by GUI renderer.

### 6) Normal Worker Path (unchanged)

- Existing queue/claim execution remains unchanged.
- No dependency on debug endpoints, video transport, or debug session control.

## Transport Design

### A) Side-channel control transport

Use local IPC (named pipes or UDS per platform abstraction), request/response + async events.

- Envelope fields:
  - protocol version,
  - session ID,
  - endpoint type (`VM`, `DOLPHIN`),
  - command/event type,
  - correlation ID.
- Must support reconnect/resubscribe by GUI.

### B) Video data transport

Use shared-memory ring buffer for high-throughput frames.

- Producer: Dolphin endpoint.
- Consumer: GUI renderer.
- Metadata per frame:
  - frame ID,
  - timestamp,
  - width/height,
  - format,
  - color space,
  - stride,
  - flags.
- Notification event via control channel (`FRAME_READY`).
- GUI pulls frames at ~60 FPS from shared buffer using render-loop delta pacing.
- Under load, skipped/intermediate frames are ignored (display latest available frame).
- Scaling behavior:
  - visualization pane auto-scales rendered texture when pane resizes.
- Performance policy:
  - no explicit max resolution/FPS caps in design;
  - rely on producer/consumer throughput and ring-buffer backpressure counters.

### DolphinQt format alignment notes (current recommendation)

Based on DolphinQt/Dolphin rendering conventions, prefer:
- primary upload format: 8-bit RGBA/BGRA path compatible with ImGui/DX texture upload,
- color space: SDR sRGB default,
- conversion point: in producer path only when Dolphin output differs from GUI texture target.

(Exact enum names and final mapping should be confirmed against the Dolphin source used in integration.)

## Unified Runtime State Model

Track VM and emulator as separate but coordinated state machines.

1. **VM state**
   - `VM_PAUSED`
   - `VM_STEPPING_INSTR`
   - `VM_RUNNING`

2. **Emulator state**
   - `EMU_PAUSED`
   - `EMU_FRAME_STEP`
   - `EMU_RUN_TO_BP`

3. **Session UX mode**
   - `FRAME_STEP_DEFAULT`
   - `RUN_TO_BP_ACTIVE`
   - `VM_INSTR_DEBUG`

### Critical requirement: switch from run-to-bp back to frame-step

When emulator is in `EMU_RUN_TO_BP`, GUI must always be able to switch to frame-step.

Implementation pattern:
- `SetMode(EMU_FRAME_STEP)` posts interrupt token.
- Run-to-bp loop polls token at safe points and transitions to paused/step-ready boundary.
- Endpoint emits transition event + ACK only when boundary reached.

VM runtime must support similar async pause/interrupt semantics so instruction stepping can resume deterministically.

## Breakpoints and Script-Step UI

### VM breakpoint UX

In the script-step list pane:
- Use a 2-column table.
  - Column 1: skinny toggle marker column.
  - Column 2: script step row text/details.
- Breakpoint toggle behavior:
  - click marker cell to toggle breakpoint for that step,
  - disabled: marker cell is empty,
  - enabled: marker cell shows a red circle.

### Breakpoint state policy (resolved)

- Every new debug start begins with a naive breakpoint state (no preloaded breakpoints).
- Do not auto-resume execution on reconnect.
- Preserve in-session breakpoints across GUI disconnect/reconnect while that session remains active.

## Script Location + Input Display

On every pause/step/break, emit consolidated snapshot:

- VM:
  - script ID/name,
  - instruction pointer/source location,
  - optional top stack frame.
- Emulator/frame:
  - frame index,
  - human-readable input string for current frame.
- Timing/sequence:
  - monotonic timestamp,
  - sequence number.

GUI shows these in linked `Script Position` and `Current Input` panels.

## UI/UX Additions (resolved)

- If user attempts to start debug when slot is occupied, show a `GUIToastBus` toast:
  - title/message: `Debug Slot Busy`.
- Left nav pane indicates active debugger hook by showing a bullet next to the Debugger pane label.
- User can click Debugger pane at any time to inspect active session.

## Reliability / Ownership

- Coordinator is owner of debug session lifecycle; worker is owner of runtime process tree.
- GUI is controller client, not lifecycle owner.
- If GUI disconnects:
  - preserve breakpoints,
  - remain paused (no auto-resume).
- If worker crashes:
  - session marked failed and cleaned up; coordinator reports failure state tied to `job_id`.
- Single-controller write lock:
  - one GUI can issue control commands; optional read-only observers.
- Explicit user stop:
  - `Stop Debugging` tears down worker/session and frees the single debug slot.
- Coordinator restart behavior:
  - active/pending debug startup is not resumed;
  - user must submit a new `StartDebug` request.

## Determinism and Auditability

- Assume debug runs are deterministic with headless runs.
- Reuse existing SCLOGx logging emitted by worker to file for debug auditability/log pane population.

### Log tailing suggestion (initial)

- Run non-blocking file tail reader on background thread.
- Poll file growth at short interval (e.g., 100–250 ms), append incremental lines to bounded in-memory ring.
- On rotation/truncation, detect inode/size reset and reopen from start of new file.

## Failure Taxonomy and UX Suggestions

### Failure categories

Track and surface distinct categories:
- startup/validation (`JobNotFound`, `JobNotTerminal`, `DebugSlotBusy`, lock acquisition failures),
- launch/setup (`WorkerLaunchFailed`, endpoint init failures),
- runtime VM (`VmRuntimeError`, breakpoint eval errors),
- runtime Dolphin/stream (`DolphinTransportError`, video buffer failures),
- user actions (`StoppedByUser`, `CancelStartAccepted`, `CancelStartTooLate`).

### Suggested default wording, severity, and recovery

- `DebugSlotBusy`
  - Severity: Info
  - Message: `Debugger is currently in use.`
  - Recovery: `Open Debugger pane to view active session, or try again after stopping it.`

- `JobNotTerminal`
  - Severity: Warning
  - Message: `Only terminal jobs can be debugged.`
  - Recovery: `Wait for the job to complete, then restart debug.`

- `WorkerLaunchFailed`
  - Severity: Error
  - Message: `Failed to start debug worker.`
  - Recovery: `Retry Start Visual Debug. If it persists, inspect worker logs.`

- `DolphinTransportError`
  - Severity: Error
  - Message: `Lost connection to video/debug stream.`
  - Recovery: `Stop debugging and start a new session.`

- `VmRuntimeError`
  - Severity: Error
  - Message: `Runtime error while executing debug session.`
  - Recovery: `Review log pane and restart debug session.`

## Security / Safety

- Local-only endpoint binding.
- Attach requires session token/capability from coordinator.
- Token expiry on completion/detach timeout.

## Weak Spots / Incomplete Information / Open Questions

1. **Stale session cleanup policy**
   - Exact cleanup trigger for orphaned `debug_sessions` rows after hard crash.
   - Whether cleanup occurs on startup only or continuously.

2. **Dolphin pixel format finalization**
   - Confirm exact source format enum and conversion path against integrated Dolphin build.

3. **Resource pressure behavior without hard caps**
   - Memory growth and GPU upload pressure when very high resolutions are produced.
   - Need observable metrics/alerts to diagnose overload.

4. **Toast throttling**
   - Prevent repeated `Debug Slot Busy` toasts from spamming user in rapid retries.

5. **Log privacy/redaction**
   - Whether sensitive values can appear in SCLOGx output and need masking in GUI log pane.

6. **Error-code to telemetry mapping**
   - Standardize counters/dashboard keys for each failure category.

## Rollout Plan

1. **DB + coordinator admission guard**
   - Add `debug_sessions` table and transactional `StartDebug(job_id)` guard.
2. **Coordinator trigger + slot policy**
   - Implement async start, single-slot reject behavior, `StopDebug`, and restart behavior.
3. **Worker/bootstrap + DolphinWrapper mode split**
   - Add render-enabled launch path while preserving default headless mode.
4. **Side-channel control MVP**
   - pause/frame-step/run-to-bp + interruption semantics + cancellation.
5. **Video MVP**
   - shared-memory ring + GUI viewport scaling + render-loop ~60 FPS pull.
6. **VM instruction debugging + script-step breakpoint table UX**
   - instruction stepping + 2-column breakpoint marker table.
7. **UX polish + hardening**
   - left-nav bullet indicator, `GUIToastBus` busy toast, log tailing, failure taxonomy wiring, stale session cleanup.

## Acceptance Criteria

- No separate debug queue/job entry is required for visual debug.
- From Jobs pane details, user can start debug for a selected terminal `job_id`.
- `StartDebug` is async and supports simple pre-ready cancellation.
- If debug slot is occupied, coordinator rejects new debug requests and GUI shows `Debug Slot Busy` toast.
- UI provides `Stop Debugging` that stops debug worker and frees the slot.
- Debug runs are read-only and use `debug_sessions` transaction guard while active.
- GUI can attach, view live video, and control step/run/pause.
- GUI can switch from `RunToBP` to `FrameStep` without restart.
- GUI can step VM instruction and observe script location updates.
- Script-step pane shows breakpoint toggle marker column (red circle when enabled).
- New debug sessions start without preloaded breakpoints; reconnect does not auto-resume.
- Left nav shows debugger-hook bullet indicator when active.
- Normal queue workers and claim logic remain unaffected.
