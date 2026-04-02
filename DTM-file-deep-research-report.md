# Technical Brief: Building a Lossless DTM Editor and Annotation Sidecar

## Executive Summary

A Dolphin `.dtm` movie file is a binary container consisting of a fixed **256‑byte packed header** followed by a variable‑length **input byte stream**. The header layout, field semantics, and the controller record structures are defined directly in Dolphin’s current source (notably `Movie.h` and `Movie.cpp`). citeturn36view0turn20view4

During playback, Dolphin does **not** treat the input stream as “one record per rendered frame.” Instead, it consumes input records **when the emulated software polls controllers**, and playback correctness depends on the **poll order** matching what happened during recording. This is explicitly called out in Dolphin’s playback code for GameCube controller input. citeturn33view0

For editing, this implies a critical architectural separation:

- a **raw/lossless layer** that can round‑trip bytes exactly when unchanged (including reserved/unknown fields), and  
- a **semantic/edit layer** that exposes “timeline” operations (insert/remove/change) while preserving invariants tied to polling order, record boundaries, and movie end conditions. citeturn36view0turn33view0turn32view3

For annotations, the safest design is a **sidecar file bound to the exact DTM bytes** via a cryptographic hash, optionally also binding the associated `.sav` (when the movie starts from a savestate) and key determinism‑relevant header fields (game ID, ISO MD5, recording start time, config flags). (Inference, grounded in how Dolphin uses these fields and adjacent `.sav` files.) citeturn36view0turn32view3turn20view4

## Primary Sources and Authority Model

The most authoritative and current specification of DTM is Dolphin’s own implementation code (the effective “binary spec”), hosted on entity["company","GitHub","code hosting platform"]:

- `Source/Core/Core/Movie.h` defines the packed `DTMHeader` (256 bytes) and `ControllerState` (8 bytes) record, including reserved fields and explicit comments about stability expectations. citeturn36view0  
- `Source/Core/Core/Movie.cpp` implements file I/O (read/write), playback consumption (`PlayController`, `PlayWiimote`), recording (`RecordInput`, `RecordWiimote`), movie end logic (`CheckInputEnd`, `EndPlayInput`), and header population (`SaveRecording`). citeturn20view4turn33view0turn32view3  
- `Source/Core/Core/State.cpp` shows how Dolphin writes/loads `.dtm` alongside savestates and uses `SaveRecording`/`LoadInput` for movie‑savestate interactions. citeturn27search2  

Secondary sources can help corroborate field meanings or historical behavior, but should be treated as lower priority than source code:

- Dolphin Wiki movie/TAS documentation (useful for practical workflow notes; not the parsing authority). citeturn0search2  
- entity["organization","TASVideos","tool-assisted speedrun site"] community references describing DTM layout (helpful cross‑checks; lower priority if conflicting with current code). citeturn0search0  

Source link inventory (URLs in code per output constraint):

```text
Source code (primary)
- https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Movie.h
- https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Movie.cpp
- https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/State.cpp

Raw source (useful for searching specific functions)
- https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/Movie.cpp

Secondary references
- https://wiki.dolphin-emu.org/  (movie/TAS-related pages)
- https://tasvideos.org/ (DTM-related community documentation)
```

## DTM Binary Specification

### File layout

At playback start, Dolphin reads the first 256 bytes into `DTMHeader`, validates the magic, then reads the remainder of the file into an in-memory byte buffer (`m_temp_input`) interpreted as the movie’s input stream. citeturn20view4turn36view0

- **Offset 0x0000–0x00FF (256 bytes):** packed `DTMHeader` citeturn36view0  
- **Offset 0x0100–EOF:** input byte stream (`m_temp_input`) citeturn20view4turn33view0  

The header is explicitly asserted to be exactly 256 bytes in the current source, and Dolphin writes/reads it as raw bytes. citeturn36view0turn32view3  
(Inference: this makes the on-disk integer endianness effectively the host’s endianness; on supported desktop platforms this is practically little-endian. The authoritative point for an editor is that Dolphin uses raw struct I/O, so you must reproduce that layout for compatibility.) citeturn20view4turn32view3turn36view0

### Header fields, sizes, and meanings

Below is the packed field order as defined in `DTMHeader`. Byte offsets are derived directly from the packed struct layout and the stated total size (256). (Inference: offsets assume `bool` occupies one byte as implied by the compiled layout used by Dolphin; the struct’s 256-byte `static_assert` is Dolphin’s enforcement that its build produces that layout.) citeturn36view0turn32view3

| Offset | Size | Field | Meaning / notes |
|---:|---:|---|---|
| 0 | 4 | `filetype` | Magic identifier: `"DTM"` + `0x1A` citeturn36view0turn32view3 |
| 4 | 6 | `gameID` | Game ID string (6 bytes) citeturn36view0turn33view0 |
| 10 | 1 | `bWii` | True for Wii game citeturn36view0turn32view3 |
| 11 | 1 | `controllers` | Bitmask: GC pads 1–4 in bits 0–3, Wiimotes 1–4 in bits 4–7 citeturn36view0turn32view3 |
| 12 | 1 | `bFromSaveState` | Movie starts from boot vs savestate citeturn36view0turn20view4 |
| 13 | 8 | `frameCount` | “Number of frames” (VI frame count used for stats/UI) citeturn36view0turn32view3 |
| 21 | 8 | `inputCount` | “Number of input frames” in the recording citeturn36view0turn20view4 |
| 29 | 8 | `lagCount` | Number of lag frames citeturn36view0turn20view4 |
| 37 | 8 | `uniqueID` | Marked “not implemented” in source citeturn36view0turn32view3 |
| 45 | 4 | `numRerecords` | Rerecord count / “cuts” citeturn36view0turn33view0 |
| 49 | 32 | `author` | UTF‑8 author name citeturn36view0turn32view3 |
| 81 | 16 | `videoBackend` | UTF‑8 backend string citeturn36view0turn32view3 |
| 97 | 16 | `audioEmulator` | UTF‑8 audio emulator string citeturn36view0turn32view3 |
| 113 | 16 | `md5` | MD5 of the game ISO/ROM citeturn36view0turn32view3 |
| 129 | 8 | `recordingStartTime` | Seconds since 1970; used for RTC citeturn36view0turn32view3 |
| 137 | 1 | `bSaveConfig` | If true: load stored config options on startup citeturn36view0turn20view4 |
| 138–151 | 14 | various flags | Skip idle, dual core, progressive, DSP HLE, fast disc, CPU core, EFB/XFB flags, etc. citeturn36view0turn32view3 |
| 152 | 1 | `memcards` | Bitmask for slots A/B citeturn36view0turn20view4 |
| 153 | 1 | `bClearSave` | Whether to create new memory card / clear save citeturn36view0turn32view3 |
| 154 | 1 | `bongos` | Bitmask for bongo controllers ports 1–4 citeturn36view0turn20view4 |
| 155–159 | 5 | GPU/Net/region flags | `bSyncGPU`, `bNetPlay`, `bPAL60`, etc. citeturn36view0turn32view3 |
| 160 | 1 | `language` | Language setting citeturn36view0turn20view4 |
| 161 | 1 | `reserved3` | Reserved byte citeturn36view0 |
| 162–163 | 2 | `bFollowBranch`, `bUseFMA` | CPU/JIT behavior flags (recorded config) citeturn36view0turn20view4 |
| 164 | 1 | `GBAControllers` | Bitmask: GBA controllers plugged in ports 1–4 citeturn36view0turn20view4 |
| 165 | 1 | `bWidescreen` | SYSCONF aspect 16:9 vs 4:3 citeturn36view0turn32view3 |
| 166 | 1 | `countryCode` | SYSCONF country code citeturn36view0turn32view3 |
| 167 | 5 | `reserved` | Reserved padding for future config options citeturn36view0 |
| 172 | 40 | `discChange` | ISO filename to switch to for multi-disc games citeturn36view0turn32view3 |
| 212 | 20 | `revision` | Git hash stored as 20 bytes citeturn36view0turn32view3 |
| 232 | 4 | `DSPiromHash` | DSP ROM hash value citeturn36view0turn32view3 |
| 236 | 4 | `DSPcoefHash` | DSP coef hash value citeturn36view0turn32view3 |
| 240 | 8 | `tickCount` | Number of “ticks” in the recording citeturn36view0turn32view3 |
| 248 | 11 | `reserved2` | Reserved; comment notes padding to 256 bytes citeturn36view0 |

#### Required vs reserved/unknown

- **Required for Dolphin to accept as DTM:** `filetype` must match `"DTM"` + `0x1A` and the header must parse as the expected 256-byte structure. citeturn36view0turn20view4turn32view3  
- **Required for correct configuration replay/determinism:** fields gated by `bSaveConfig` (e.g., CPU/GPU flags, region/language) can influence determinism because Dolphin loads these settings as a configuration layer when `bSaveConfig` is true. citeturn36view0turn20view4turn32view3  
- **Explicitly reserved/unknown:** `reserved`, `reserved2`, `reserved3` are reserved bytes; `uniqueID` is labeled “not implemented.” Editors should preserve them byte-for-byte unless intentionally redefining semantics. citeturn36view0turn32view3  

### Input data layout and record structures

The payload after the 256-byte header is a single byte stream consumed by playback functions. Dolphin maintains an internal cursor `m_current_byte` into this stream and advances it as records are consumed. citeturn33view0turn32view2

#### GameCube controller record: `ControllerState` (8 bytes)

For GameCube input, Dolphin records and plays back `ControllerState`, asserted to be exactly 8 bytes. This struct includes:

- bitfields for buttons and special flags (`Start`, `A`, `B`, `X`, `Y`, `Z`, D-pad, `L`, `R`, `disc`, `reset`, `is_connected`, `get_origin`) and  
- 6 bytes of analog values (`TriggerL`, `TriggerR`, `AnalogStickX/Y`, `CStickX/Y`). citeturn36view0turn33view0

Important nuance for editors: `ControllerState` uses C++ bitfields. The semantic fields are clear, but the *exact bit ordering inside the 8 bytes* is compiler-dependent in standard C++ (inference). Practically, Dolphin reads/writes this raw layout via `memcpy`, so the correct on-disk mapping is “whatever Dolphin wrote,” and editors must match Dolphin’s effective layout for compatibility. citeturn36view0turn33view0turn32view3

#### Wiimote record: length-prefixed serialized desired state

Wiimote input is stored as:

- `u8 length`  
- `length` bytes of serialized state (copied into `SerializedWiimoteState.data`)  

Playback reads the length, validates it, copies that many bytes, then calls `DeserializeDesiredState` to apply it. citeturn33view0turn32view3

Recording mirrors this: Dolphin writes a size byte then copies the serialized payload bytes. citeturn20view4

#### Disc change and reset events

Dolphin uses two layers for disc changes:

- a header string `discChange` (40 bytes) that stores the disc image filename to switch to, and citeturn36view0turn32view3  
- a per-controller-record flag `ControllerState.disc` which marks the poll where the disc change should occur. During playback, if the `disc` flag is set, Dolphin attempts an automatic disc change and otherwise stops/breaks execution and prompts the user. citeturn36view0turn33view0

For reset, `ControllerState.reset` triggers a processor-interface “reset button tap” during playback. citeturn36view0turn33view0

## Playback and Timing Model

### Two counters: VI frames vs polled input consumption

Dolphin tracks at least three distinct notions of progress during movie operation:

- **VI frame index** (`m_current_frame`), advanced in `FrameUpdate()`. citeturn17search1  
- **Lag frame count** (`m_current_lag_count`), incremented when no input device is polled during a frame (`m_polled` is false). `SetPolledDevice()` marks a frame as having been polled. citeturn17search1turn20view4  
- **Input stream cursor** (`m_current_byte`), advanced when playback consumes controller records from the input byte stream. citeturn33view0turn32view2  

This is the core nuance for human editing: **DTM playback is fundamentally per-poll, not per-frame**. A single frame can contain multiple polls (consuming multiple records), and a lag frame can contain none (consuming zero records). (Inference supported by the separate frame-update and poll markers plus poll-ordered consumption.) citeturn17search1turn33view0

### How GameCube controller records are consumed

`PlayController()` reads the next 8 bytes as `ControllerState`, advances `m_current_byte`, then maps each semantic field to a `GCPadStatus` output structure (buttons, analog trigger values, sticks). citeturn33view0turn32view2

Crucially, the code warns that correctness depends on polling order matching the recording’s polling order. This implies the stream contains **no explicit “controller ID” tags per record**—the *meaning* of the next 8 bytes depends on which controller is being polled at that moment. citeturn33view0

Concrete example: representing and consuming an `A` press

- Representation: set `ControllerState.A = 1` in the controller record. citeturn36view0turn33view0  
- Consumption: playback checks the `A` flag and sets the GameCube pad button bit; it also sets the analog-A value to max (`0xFF`) when `A` is pressed. citeturn32view2turn33view0  

### How Wiimote serialized records are consumed

`PlayWiimote()` reads `length`, validates it doesn’t exceed buffer capacity, copies `length` bytes into a serialized structure, then calls `DeserializeDesiredState`. It increments the input counter and advances the stream cursor accordingly. citeturn33view0turn32view3

Two important validation behaviors fall out of this:

- If the `length` byte is invalid (too large for the destination buffer), playback aborts. citeturn33view0turn32view3  
- If deserialization fails, playback aborts; the error message suggests one potential desync cause is having GameCube controllers enabled when they shouldn’t be. citeturn32view3  

### Movie end conditions

Dolphin checks for movie end using both:

- whether the input cursor has reached the end of the input buffer (`m_current_byte >= m_temp_input.size()`), and  
- a timing bound using `CoreTiming` ticks compared to `m_total_tick_count`, but only when the movie **did not** start from a savestate (`!IsRecordingInputFromSaveState()`). citeturn33view0turn32view3

This makes the `tickCount` header field operationally relevant: it can end playback even if additional input bytes remain, and conversely it can prevent the movie from “hanging” if no further polls occur but the movie should still end. (Inference grounded in the check logic.) citeturn33view0turn36view0

## Editing Invariants and Roundtrip Requirements

### Practical invariants for safe editing

The following invariants are directly implied by Dolphin’s parsing and playback behavior:

- **Header must remain exactly 256 bytes** and preserve magic/type correctness. citeturn36view0turn32view3turn20view4  
- **Poll-order dependence:** you must not change the semantic mapping between “next record in stream” and “which device is polled now,” or playback will consume the wrong record for the polled device and desync. citeturn33view0  
- **Boundary safety:** edits must preserve record boundaries so that `m_current_byte + sizeof(ControllerState)` and `m_current_byte + 1 + length` checks never fail prematurely. citeturn33view0turn32view3  
- **Wiimote length must be correct** and consistent with the payload you write; otherwise playback aborts before applying your intended input. citeturn33view0turn32view3  
- **Disc change semantics are two-part:** if you set `ControllerState.disc` at some record, ensure the header’s `discChange` string is correct and within 40 bytes; Dolphin will attempt a change at that moment. citeturn36view0turn33view0turn32view3  
- **Determinism-relevant header fields** (especially `recordingStartTime` and config layer fields gated by `bSaveConfig`) should be preserved unless the user intentionally changes them, because Dolphin loads them as a movie config layer and uses the start time for RTC. citeturn36view0turn20view4turn32view3  

### Insert/remove operations: why “frames” are not a safe primitive

Because playback consumes records on polls (not frames), “insert one frame” is not a well-defined byte operation unless you also know:

- how many polls occur during that frame, and  
- in what order devices are polled in that frame. citeturn33view0turn17search1  

Therefore, the robust primitive for editing is:

- **Insert/remove poll records** (8 bytes for a GC controller poll record; `1 + length` bytes for a Wiimote poll record), *at the correct position in the poll-ordered stream*. citeturn36view0turn33view0turn32view3  

A frame-oriented UI is still possible, but it needs a mapping from frames → poll events. (Inference: this mapping must come from emulation tracing or a deterministic replay pass, because the stream does not self-describe device IDs.) citeturn33view0turn17search1

### Preserving determinism after edits

Determinism risks explicitly surfaced in current Dolphin code include:

- **Savestate movie mismatch detection:** when loading a `.dtm` that corresponds to a savestate, Dolphin compares the prefix of recorded input up to the current byte position and warns in detail on mismatch (and indicates desync risk). citeturn33view0turn32view1  
- **Tick-bound end logic:** for non-savestate-start movies, `CheckInputEnd()` can terminate playback based on tick count as well as stream exhaustion. If you extend or shorten the effective duration of the movie’s poll sequence without updating tick-related totals coherently, end-of-movie timing can change. citeturn33view0turn36view0turn20view4  
- **Wiimote deserialization failures:** malformed or incompatible serialized Wiimote payloads abort playback. citeturn32view3turn33view0  

Because some header totals (`frameCount`, `lagCount`, `inputCount`, `tickCount`) are *derived statistics* produced by recording/playback bookkeeping, a pure offline editor cannot always recompute them exactly without running an emulation pass. (Inference.) citeturn36view0turn33view0turn32view3

### Byte-accurate parse/serialize roundtrip

To guarantee a byte-identical output when “unchanged,” implement a strict lossless mode:

- Store the **raw 256 header bytes** plus the **raw input payload bytes** and re-emit them verbatim unless an edit actually changes content. citeturn36view0turn20view4  
- Treat reserved fields as opaque and preserve them exactly (`reserved`, `reserved2`, `reserved3`, `uniqueID`). citeturn36view0  

When edits do occur, prefer “surgical” modifications that update only the intended bytes (e.g., a single controller record or a single Wiimote record), leaving unrelated padding and metadata untouched unless intentionally changed. (Inference rooted in the presence of reserved fields and Dolphin’s stability expectations.) citeturn36view0turn32view3  

## Annotation Sidecar Design

### Binding strategy and “one sidecar ↔ one deterministic DTM”

A robust binding should treat the DTM as immutable content and bind annotations to it with a cryptographic hash. (Inference; recommended best practice.)

Grounding details from Dolphin behavior:

- The DTM’s determinism depends not only on input bytes but also on header fields that can affect game state (RTC start time, config options gated by `bSaveConfig`, and the ISO MD5 used for verification). citeturn36view0turn32view3turn20view4  
- When `bFromSaveState` is set, Dolphin expects/uses an adjacent `.sav` file (`movie_path + ".sav"`) and will copy a savestate when saving a movie that started from a savestate. citeturn20view4turn32view3  

**Recommendation (inference):** Bind your annotation file to:

1. `sha256(dtm_bytes)` plus file size.  
2. If `bFromSaveState` is true, also bind `sha256(sav_bytes)` of the associated savestate (or at minimum require the `.sav` to exist).  
3. Cache a small subset of header fields (game ID, ISO MD5, recordingStartTime, tickCount) for human-readable sanity checks and “accidental mismatch” detection.

### Rebinding policy when the DTM changes

**Strict mode (recommended for determinism):** If `sha256(dtm_bytes)` changes, refuse to load annotations unless the user explicitly rebonds.

**Two-tier policy (optional, inference):**

- Define `content_hash = sha256(input_payload_bytes)` (i.e., file bytes from offset 256 onward).  
- Define `full_hash = sha256(full_file_bytes)`.  

Then allow automatic rebind if `content_hash` matches and only header metadata changed (e.g., rerecord count updated by Dolphin). This matches Dolphin’s behavior of updating rerecord counters in some flows while keeping input bytes consistent. citeturn33view0turn32view3turn36view0  

### Minimal JSON schema for bookmarks and metadata

Below is a minimal schema that supports both strict binding and practical timeline addressing. (Inference; designed to align with Dolphin’s “byte cursor” consumption model.) citeturn33view0turn20view4

```json
{
  "schema_version": 1,
  "created_utc": "2026-04-02T00:00:00Z",
  "dtm_binding": {
    "hash_algorithm": "sha256",
    "dtm_sha256": "hex...",
    "dtm_byte_length": 123456,
    "header_snapshot": {
      "game_id": "GM8E01",
      "b_from_savestate": false,
      "iso_md5": "hex16bytes-or-hex32chars",
      "recording_start_time_unix": 0,
      "tick_count": 0
    },
    "sav_binding": {
      "required": false,
      "sav_sha256": null,
      "sav_byte_length": null
    }
  },
  "bookmarks": [
    {
      "id": "uuid-or-stable-string",
      "label": "Human label",
      "note": "Freeform text",
      "anchor": {
        "type": "input_byte_offset",
        "value": 1024
      },
      "secondary_anchors": [
        { "type": "vi_frame", "value": 12345 },
        { "type": "core_tick", "value": 678901234 }
      ],
      "tags": ["categoryA", "categoryB"]
    }
  ]
}
```

Rationale (inference):

- `input_byte_offset` is the most precise anchor because Dolphin’s playback cursor `m_current_byte` is literally a byte offset into the input payload; you can store offsets relative to the payload start (0 at file offset 256) or absolute file offsets. citeturn33view0turn20view4  
- Frame-based secondary anchors are useful for UI but are not self-sufficient for deterministic rebinding because the frame↔poll mapping is game-dependent. citeturn33view0turn17search1  

## Validation Checklist for Deterministic Editing

This checklist is written as directly implementable validation gates. Each check is justified by a Dolphin behavior that would otherwise error, abort playback, or risk desync. citeturn20view4turn33view0turn32view3turn36view0

**Structural / file-level**

- Verify file length ≥ 256 bytes. citeturn20view4turn36view0  
- Verify header magic matches `"DTM"` + `0x1A`. citeturn36view0turn32view3turn20view4  
- Verify you can parse a 256-byte `DTMHeader` without truncation. citeturn36view0turn20view4  

**Header-level (sanity and determinism relevance)**

- Preserve `recordingStartTime` unless explicitly changing RTC. citeturn36view0turn32view3  
- Preserve all config fields when `bSaveConfig` is true, unless you intentionally re-spec settings; Dolphin loads these during playback via a config layer. citeturn36view0turn20view4turn32view3  
- If you set/keep `bFromSaveState = true`, verify the paired `.sav` exists; Dolphin warns when missing and copies savestate data when saving such movies. citeturn20view4turn32view3  
- Preserve reserved bytes (`reserved`, `reserved2`, `reserved3`) and `uniqueID` (not implemented) to maintain compatibility. citeturn36view0  
- If disc change is used, ensure `discChange` fits 40 bytes and matches the intended disc filename. citeturn36view0turn33view0turn32view3  

**Input stream-level (record integrity)**

- For every GC controller record you edit/insert: ensure exactly 8 bytes are available at that offset; Dolphin aborts on overrun. citeturn33view0turn36view0  
- For every Wiimote record you edit/insert: ensure `length` ≤ serialized buffer size and that `m_current_byte + 1 + length` does not exceed payload size; Dolphin aborts otherwise. citeturn33view0turn32view3  
- Ensure any edited Wiimote payload still deserializes via `DeserializeDesiredState` (your own validation pass should replicate this if possible); Dolphin aborts on deserialization errors. citeturn32view3  

**Movie end behavior checks**

- If the movie is **not** from savestate (`bFromSaveState = false`), treat `tickCount` as an operational constraint because `CheckInputEnd()` can end playback when ticks exceed the stored total. citeturn33view0turn36view0  
- If the movie **is** from savestate, end-of-movie is enforced primarily by input stream exhaustion (`m_current_byte >= payload_size`) in `CheckInputEnd()` because the tickCount condition is gated out. citeturn33view0turn36view0  

**Savestate+DTM coherence (if you support movie+state workflows)**

- If users load savestates with `.dtm` sidecars, warn or block when the edited movie’s prefix no longer matches the savestate’s stored movie prefix; Dolphin emits detailed mismatch warnings and flags likely desync. citeturn33view0turn32view1turn27search2  

## Open Questions and Ambiguities

These are areas where the available primary sources are either explicitly “not implemented,” rely on external call sites not analyzed here, or depend on C++ layout rules that are not fully specified by the language standard.

- **Exact meaning of `inputCount` for GameCube-only movies:** the header documents it as “Number of input frames,” but `PlayController()` does not increment `m_current_input_count` itself, while `PlayWiimote()` does. This suggests `InputUpdate()` may be invoked by other subsystems for GC polling, but that call site is outside the snippets analyzed here. (Ambiguity; inference.) citeturn36view0turn33view0turn32view3  
- **Bit-level packing of `ControllerState` in the 8 bytes:** Dolphin uses C++ bitfields and `memcpy` to/from file buffers. The semantic fields are clear, but the exact bit ordering is compiler-dependent in standard C++. An editor should validate against real DTM samples and/or replicate Dolphin’s effective packing rather than assume a universal bit order. (Ambiguity; inference.) citeturn36view0turn33view0turn32view3  
- **GBA controller payload layout:** `DTMHeader` includes `GBAControllers`, but the corresponding input record encoding/decoding paths are not covered in the extracted playback snippets (no `PlayGBA` detail here). Further source review is needed to specify GBA record structure rigorously. (Ambiguity.) citeturn36view0  
- **Deriving a complete frame↔poll mapping offline:** `PlayController()` makes poll-order dependence explicit, and the stream does not self-identify device IDs per record. A true frame-accurate editor likely needs a replay/trace pass to annotate when each record is consumed. (Ambiguity; inference.) citeturn33view0turn17search1  
- **`uniqueID` field semantics:** marked “not implemented,” so third-party tools should treat it as reserved/opaque. citeturn36view0turn32view3