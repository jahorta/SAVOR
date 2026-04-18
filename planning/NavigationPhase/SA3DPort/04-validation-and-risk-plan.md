# SA3D NJ Port Validation + Risk Plan

Date: 2026-04-18

---

## 1) Validation strategy

## A) Parity levels

1. **Structural parity**
   - Node counts
   - Attach counts
   - Poly chunk counts by type
   - Motion/node/frame counts

2. **Semantic parity**
   - Triangle totals per attach
   - Material/texture run boundaries
   - Strip-derived winding behavior and degenerates

3. **Binary parity (optional)**
   - Re-serialization byte equality where deterministic
   - Header/block layout equivalence for NJ output

## B) Golden outputs per fixture

For each NJ model/animation fixture, persist:
- block map (`offset -> header`)
- object tree summary
- chunk histogram
- triangle/material stats
- diagnostics

Store under:
- `planning/NavigationPhase/SA3DPort/goldens/` (proposed)

## C) Acceptance gates

- Slice 3 gate: Node graph parity >= 100% structural metrics.
- Slice 5 gate: poly chunk histogram + triangle totals match reference.
- Slice 8 gate: animation frame/node summaries match reference.
- Slice 9 gate: weighted/corner stream checks match expected ranges.

---

## 2) Risk register

| Risk | Why it matters | Mitigation |
|---|---|---|
| Endian/imageBase mismatch | corrupt pointers and wrong subtree roots | build early pointer/endian microtests and assert all address arithmetic |
| LUT memoization divergence | duplicate nodes/attaches or broken references | emulate LUT get-add semantics exactly before parser recursion |
| Strip decoding drift | visible geometry mismatch | per-chunk golden tests; track winding/degenerate counts |
| Material state drift | wrong segmentation/material assignment | persist run-length stats and compare per attach |
| Hidden SA branches accidentally used | unexpected behavior creep | compile-time scope flags for NJ-only feature set |
| Over-porting nonessential files | schedule risk | strict “needed by entry path” checklist and slice gates |

---

## 3) Instrumentation checklist

- [ ] Add parser trace mode for block scan and image base changes.
- [ ] Add node read trace with address/label/attach pointer.
- [ ] Add poly chunk trace with type, size, and semantic impact.
- [ ] Add animation trace with node count, shortRot, frame counts.
- [ ] Add summary JSON emitter for fixture comparison.

---

## 4) Recommended first fixture set

- 3 NJ model files: one simple, one medium, one problematic/replay-heavy.
- 2 NJ animation files: one shortRot false, one shortRot true.
- 1 “failure expected” file to validate diagnostics.

---

## 5) Completion definition (NJ-first milestone)

NJ-first milestone is complete when:
1. `ModelFile.ReadNJ` equivalent parses fixture set with stable summaries.
2. `AnimationFile.ReadNJ` equivalent parses fixture set with stable motion summaries.
3. Known mismatch cases have explicit diagnostic categories and are reproducible.
4. Deferred SA/Level paths remain intentionally out-of-scope and documented.
