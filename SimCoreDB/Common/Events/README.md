# Common/Events

Shared event contracts for payload dispatch and typed payload resolver interfaces.

- `EventCatalog.h`: canonical event names.
- `EventEnvelope.h`: outbox envelope shape.
- `EventPayloadViews.h`: per-family typed payload view structs.
- `EventPayloadDispatch.h`: dispatch keys (`event_type`, `event_version`) -> payload resolver contracts.
- `EventPayloadValidation.h`: required-field validation helpers for v1 payload families.
