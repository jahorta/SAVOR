# State

State bounded-context persistence and outbox relay support.

Implemented transactional write paths:
- Artifact store -> `State.ArtifactStored.v1`
- Savestate create -> `State.SavestateCreated.v1`
- Savestate derivation link -> `State.SavestateDerived.v1`
- TAS variant create -> `State.TasVariantCreated.v1`
