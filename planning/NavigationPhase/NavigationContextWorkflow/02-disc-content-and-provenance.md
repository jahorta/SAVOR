# 02 - Disc Content and Provenance

## Status

Future plan. The current prototype loads loose files. This document specifies the later disc-backed,
content-addressed input path without making a machine-specific dump or path part of the runtime contract.

## Disc Image Identity

`DiscImageIdentity` identifies the immutable source image and contains:

- SHA-256 of the exact ISO/GCM bytes;
- file size in bytes;
- game ID;
- region and revision, when recoverable;
- identity schema version; and
- optional human-readable label.

The local filesystem path is a locator only. It is not part of equality, cache keys, prediction
compatibility, or persisted semantic identity. Moving the same image must not invalidate derived content.

Disc hashing must be streaming. The current general hash helper loads a complete file into memory, so it
is not the implementation contract for future multi-gigabyte image identity. The disc service must read
bounded chunks, report progress/cancellation, and reject an image if the size or content changes during
hashing.

## Disc Content Manifest

`DiscContentManifest` is an immutable index of extracted or discoverable internal files for one
`DiscImageIdentity`. Each entry records:

- normalized internal disc path and original path spelling;
- byte length and SHA-256 of the exact stored file bytes;
- compression/container facts when known;
- logical content role, such as MLD, SCT, ECT, ALX data, or unknown;
- normalized area key when derivable;
- extraction status and diagnostics;
- extraction tool/version; and
- object-store reference when the bytes are materialized durably.

The manifest distinguishes "not present" from "not inspected," "extraction failed," and "present but
parse failed." Companion-profile rules operate on manifest presence, not parser success.

## Extraction and Parsing Ownership

The intended path is:

```text
selected ISO/GCM
  -> Dolphin DiscIO reads internal files
  -> content-addressed extracted artifacts and manifest
  -> SPICE parses MLD/SCT/ECT and owns AKLZ handling
  -> ALX parser supplies other required game data
  -> SavorNavigation normalizes SAVOR-owned content models
```

Ownership boundaries are strict:

- Dolphin DiscIO owns filesystem/container access to the selected image.
- SPICE owns supported Skies file formats, decompression, typed parse results, and parser diagnostics.
- ALX tooling owns the game-data formats assigned to it.
- `SavorNavigation` owns area association, profile semantics, navigation normalization, and compatibility.
- `SavorDb` and the object store own durable metadata, byte artifacts, and derivation lineage.

Parser types do not cross into Qt, workflow payloads, or persisted public navigation schemas.

## Navigation Content Bundle

`NavigationContentBundle` is the immutable, normalized input set for one navigable area. It records:

- `DiscImageIdentity` reference;
- normalized area key and `NavigationAreaProfile`;
- selected internal MLD/SCT/ECT paths and their byte hashes;
- any ALX/game-data inputs and hashes;
- parser names, versions, revisions, and parse statuses;
- navigation normalization/schema version;
- coordinate-policy version;
- SAVOR-owned normalized content artifact references;
- completeness capabilities and diagnostics; and
- the `DiscContentManifest` revision or digest used for association.

The bundle ID is a content-derived digest over all semantic inputs. At minimum, its key includes:

```text
ISO SHA-256
+ normalized internal paths and exact file SHA-256 values
+ parser/model/schema versions
+ coordinate-policy version
+ normalization options
```

An SCT manually selected outside the disc may still support the current prototype, but it cannot be
presented as disc-derived. It receives external-source provenance and changes the bundle identity. The
future production workflow should prefer exact extraction from the selected image.

## Area Companion Association

The existing Navigation area-profile precedence remains authoritative:

- any `099*` key is Overworld regardless of companions;
- a non-099 area with a same-key ECT is Dungeon even if ECT parsing fails;
- a non-099 area with a matched SCT and no matched ECT is Safe or a Safe candidate; and
- content that cannot be associated safely remains Unknown/View-only.

Disc-backed association uses the complete manifest rather than assuming a loose-file directory is
complete. Case-insensitive comparison determines a match; original internal paths and names are retained
for provenance.

## Cache and Invalidation Contract

Derived content is reusable only when every key component matches. A cache entry is invalidated by:

- any ISO or internal-file hash change;
- a parser revision or semantic parser option change;
- a SAVOR normalization or public schema change;
- a coordinate-policy change;
- a change to companion association or profile-classification rules; or
- a corrected artifact whose bytes produce a different digest.

Human-readable filenames, machine paths, modification times, and "latest" labels are never sufficient
cache keys. Older bundles remain immutable historical evidence; they may be marked stale but are not
rewritten in place.

## Failure Semantics

- An unreadable or changing image produces no `DiscImageIdentity` and no derived bundle.
- An extraction failure remains distinct from an absent internal file.
- A present but unreadable/undecompressible/malformed ECT still classifies a non-099 area as Dungeon while
  marking encounter content incomplete.
- Missing/failed SCT may leave geometry usable but marks script, authored starts, and transition analysis
  incomplete.
- Unsupported parser or schema versions produce an explicit compatibility failure.
- A partial content bundle declares exact missing capabilities; consumers cannot infer defaults.

## Development Fixtures

Loose files and local disc dumps remain useful test fixtures. No path such as the current US disc dump,
ISO path, or extraction directory may become a checked-in default or part of semantic identity.

## Acceptance Rules

- Two byte-identical images at different paths have one disc identity.
- Two images with the same game ID but different bytes have different identities.
- Every normalized world or predictor input traces to exact internal-file hashes and parser/model versions.
- ECT presence and ECT parse status remain separate.
- No SPICE/ALX parser type crosses the `SavorNavigation` boundary.
