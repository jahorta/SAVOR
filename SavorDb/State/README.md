# State

State bounded-context persistence and outbox relay support.

Implemented transactional write paths:
- Artifact store -> `State.ArtifactStored.v1`
- Savestate create -> `State.SavestateCreated.v1`
- Savestate derivation link -> `State.SavestateDerived.v1`
- TAS variant create -> `State.TasVariantCreated.v1`

## Durable artifact locations

`state_artifact.object_relpath` is the only durable physical locator. It is a
validated relative path beneath `DbConfigPaths::object_store_root`; it is never
resolved against the process working directory. `state_artifact.filename` is
presentation metadata only.

`StoreArtifact` accepts a physical source path for ingestion, copies that file
into the canonical `state_artifacts/<sha-prefix>/<sha>.<ext>` object namespace,
and persists only the relative object locator. Backend reads resolve the
locator against the current database root. This lets a complete database root
and its `object_store` move together without rewriting artifact rows.

At startup, `DBService` reconciles pre-hard-cut rows. An available legacy
absolute object-store path is imported into the current canonical namespace;
the durable row is then rewritten to the portable locator. Missing physical
objects remain missing evidence at the canonical locator and are rejected only
when an operation requires their bytes, rather than preventing database
browsing.
