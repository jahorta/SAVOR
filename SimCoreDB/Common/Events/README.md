# Common/Events

Shared event contracts for payload dispatch and typed payload resolver interfaces.

- `EventCatalog.h`: canonical event names.
- `EventEnvelope.h`: outbox envelope shape.
- `EventPayloadViews.h`: event-specific typed payload view structs (with backward-compatible family aliases).
- `EventPayloadDispatch.h`: dispatch keys (`event_type`, `event_version`) -> payload resolver contracts.
- `EventPayloadResolvers.h`: resolver interfaces for loading typed rows via `(payload_ref_kind, payload_ref_id)`; context-owned SQLite implementations live under each bounded context (for example `Analysis/SqliteAnalysisPayloadResolvers.*`).
- `EventPayloadValidation.h`: required-field validation helpers for v1 concrete events and families.
