# Asynchronous SavorQt Coordinator Startup

Status: Implemented

## Problem

SavorQt currently starts the coordinator directly from the Qt UI thread. The
start action synchronously hashes the configured ISO and calls
`CoordinatorRuntime::Start`. Runtime startup then performs execution-DB
recovery and synchronously prepares, launches, negotiates with, and opens a
session on the first worker before returning. The remaining workers are started
asynchronously, but the initial path blocks the Qt event loop and makes the
application appear unresponsive.

The `start paused` option does not avoid this work. It is applied after the
first worker has completed startup.

## Recommended design

Introduce a one-shot `CoordinatorStartupOperation` dedicated to coordinator
startup. Do not use the database refresh pipeline for this stateful operation.

The UI-thread portion should:

- reject duplicate startup requests;
- capture immutable startup inputs and service handles;
- transition the controller to an explicit `Starting` state;
- disable conflicting startup, settings, pause, and worker-count actions;
- launch the background operation and return immediately to the event loop.

The background operation should:

1. Hash the ISO.
2. Build the immutable worker and coordinator configuration.
3. Construct and exclusively own a new `CoordinatorRuntime`.
4. Call `CoordinatorRuntime::Start`.
5. Return either a fully started runtime and initial snapshots or a detailed
   startup error.

Completion should be delivered back to the controller through a queued Qt
callback. Only a fully started runtime should be installed in
`coordinator_runtime_`. Success transitions to `Paused` or `Running`; failure
returns to `Stopped`, exposes the diagnostic, and re-enables controls.

## Cancellation and lifetime

Startup needs an explicit generation and cancellation state. Stop, application
shutdown, or a database-root switch during startup must invalidate the active
generation without accessing a partially initialized runtime from the UI
thread.

The operation should check cancellation between startup phases. If runtime
startup completes after cancellation, the background operation should stop the
runtime before reporting cancellation. The controller must ignore completion
from stale generations.

This cancellation/lifetime behavior is required in addition to moving startup
off the UI thread; otherwise shutdown and root-switch races remain.

Database move, reset, root switch, snapshot save, and snapshot load are all
disabled unless the coordinator is fully stopped. Closing the application
hides the UI first, drains startup and cleanup work, and only then stops the
database runtime.

## Correctness constraints

- Preserve the existing coordinator startup ordering and final cancellation
  admission gate.
- Do not publish a partially initialized runtime to UI refresh paths.
- Do not permit settings or desired-worker changes to mutate captured startup
  configuration.
- Keep all widget changes and final controller ownership transfer on the Qt UI
  thread.
- Keep worker launch, WRMS negotiation, Dolphin session opening, DB recovery,
  and ISO hashing off the Qt UI thread.
