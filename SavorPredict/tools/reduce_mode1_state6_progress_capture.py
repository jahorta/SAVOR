#!/usr/bin/env python3
"""Reduce mode-1 state-6 motion progress and publication captures."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import struct
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
DURATION_ID = "motion_duration_read_8001EC38"
SET_ID = "motion_bit31_set_complete_8001EC8C"
RESET_IDS = {
    "motion_resolver_progress_write_complete_80075F00",
    "motion_setup_progress_reset_complete_80076270",
}
STATE6_ID = "callback_gate_state6_motion_return_8001B6DC"
QUEUED_STATE_ID = "queued_state_write_complete_800811C8"
MODE1_ID = "mode1_geometry_call_80051BB0"
RNG_ID = "memwatch.rng_seed_write_803469A8"
GLOBAL_EVENT_LIMIT = 131072

PROGRESS_WATCH_RE = re.compile(r"memwatch\.(?:slot|root)(\d+)_iw_motion_progress_write$")
FLAGS_WATCH_RE = re.compile(r"memwatch\.(?:slot|root)(\d+)_iw_flags_write$")
ROOT_SAMPLE_RE = re.compile(r"(?:slot|root)(\d+)_iw_slot_0x00$")

PUBLICATION_IDS = {
    "callback_aux_publication_call_8001B750",
    "serialized_action_view_publication_8003C738",
    "action_view_record_state0_helper_80051320",
    MODE1_ID,
    RNG_ID,
}

PREFIXES = (
    "callback_state",
    "motion_entry",
    "motion_iw",
    "renderer_iw",
    "gate_iw",
    "mode1_origin",
    "record_origin",
)

PROGRESS_WRITERS = {
    0x80075EFC: "resolver_progress_initialization",
    0x8007626C: "setup_progress_reset",
    0x80018F98: "normalized_progress_advance",
    0x80018FA8: "normalized_progress_clamp",
    0x800191DC: "action_row_frame_step_advance",
    0x8001921C: "action_row_progress_wrap",
    0x80019270: "action_row_terminal_progress",
    0x8001927C: "action_row_terminal_progress",
}

FLAGS_WRITERS = {
    0x8001EC88: "motion_install_set_bit31",
    0x80075D9C: "motion_gate_clear_bit31",
}


def integer(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def hex32(value: Any) -> str:
    parsed = integer(value)
    return "" if parsed is None else f"0x{parsed & 0xFFFFFFFF:08X}"


def bits_float(value: Any) -> float | None:
    parsed = integer(value)
    if parsed is None:
        return None
    return struct.unpack(">f", struct.pack(">I", parsed & 0xFFFFFFFF))[0]


def float_bits(value: float) -> int:
    return struct.unpack(">I", struct.pack(">f", value))[0]


def f32(value: float) -> float:
    return bits_float(float_bits(value)) or 0.0


def f32_add(left: float, right: float) -> float:
    return f32(f32(left) + f32(right))


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id", ""))


def load_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    value = json.loads(data.decode(encoding))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected an object")
    return value


def profile_limits(profile_path: Path) -> dict[str, int]:
    limits: dict[str, int] = {}
    current: str | None = None
    for raw_line in profile_path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if line.startswith("[checkpoint.") and line.endswith("]"):
            current = line[len("[checkpoint."):-1]
        elif line.startswith("["):
            current = None
        elif current and line.startswith("max_hits="):
            limits[current] = int(line.split("=", 1)[1], 0)
    return limits


def locate_captures(run_root: Path) -> tuple[dict[str, Any], dict[int, Path]]:
    manifest = load_json(run_root / "manifest.json")
    captures: dict[int, Path] = {}
    for row in manifest.get("jobs", []):
        if not isinstance(row, dict):
            continue
        job = integer(row.get("original_exec_job_id"))
        raw_path = row.get("stable_capture_path")
        if job is None or raw_path is None:
            continue
        path = Path(str(raw_path))
        captures[job] = path if path.is_absolute() else run_root / path
    return manifest, captures


def is_relevant(event_id: str) -> bool:
    return (
        event_id.startswith("motion_")
        or event_id.startswith("callback_gate_state")
        or event_id.startswith("basic_attack_callback_state")
        or event_id.startswith(("memwatch.slot", "memwatch.root"))
        or event_id in PUBLICATION_IDS
        or event_id == QUEUED_STATE_ID
    )


def project_event(event: dict[str, Any], frame_window: int) -> dict[str, Any]:
    direct = {
        "capture_sequence", "checkpoint_id", "pc", "vi_field_count", "frame_count",
        "rng_draw_index_before", "owns_rng_draw", "rng_seed_after", "result", "thread",
        "thread_saved", "instruction_worksheet", "origin_instruction", "record_thread",
        "record_worksheet", "action_row", "requested_action_row", "motion_id",
        "row_word", "row_argument", "memwatch_confirmed_current_instruction",
        "memwatch_unattributed_extra_hits", "memwatch_addr", "decoded_memory_value",
        "decoded_pc", "decoded_access", "caller_callsite_pc", "r4", "r5", "r28",
        "r29", "r30", "r31", "active_actor_slot_80347334",
    }
    projected = {key: value for key, value in event.items() if key in direct}
    for key, value in event.items():
        if (
            key.startswith(PREFIXES)
            or key.startswith("selected_action_")
            or key.startswith("record_payload_")
        ):
            projected[key] = value
        elif key.startswith("stack_frame_") and key.endswith("_callsite_pc"):
            projected[key] = value
    projected["_frame_window"] = frame_window
    return projected


def update_iw_slot_map(event: dict[str, Any], iw_slots: dict[int, int]) -> None:
    for key, value in event.items():
        match = ROOT_SAMPLE_RE.match(key)
        if not match:
            continue
        address = integer(event.get(f"{key}_address"))
        slot = integer(value)
        if address not in (None, 0) and slot is not None:
            iw_slots[address & 0xFFFFFFFF] = slot


def root_iw_snapshot(event: dict[str, Any]) -> dict[int, int] | None:
    roots: dict[int, int] = {}
    saw_sample = False
    for key in event:
        match = ROOT_SAMPLE_RE.match(key)
        if not match:
            continue
        saw_sample = True
        if event.get(f"{key}_read_ok") is not True:
            continue
        address = integer(event.get(f"{key}_address"))
        if address not in (None, 0):
            roots[int(match.group(1))] = address & 0xFFFFFFFF
    return roots if saw_sample else None


def frame_slot_rows(
    job: int,
    event: dict[str, Any],
    frame_end_ordinal: int,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for naming in ("root", "slot"):
        for root_index in range(12):
            prefix = f"{naming}{root_index}"
            slot_value = integer(event.get(f"{prefix}_iw_slot_0x00"))
            slot_address = integer(event.get(f"{prefix}_iw_slot_0x00_address"))
            if slot_value is None or slot_address is None:
                continue
            rows.append({
                "job": job,
                "frame_end_ordinal": frame_end_ordinal,
                "capture_sequence": sequence(event),
                "vi": integer(event.get("vi_field_count")),
                "root_index": root_index,
                "slot": slot_value,
                "instruction_worksheet": hex32(slot_address),
                "mode": integer(event.get(f"{prefix}_iw_mode_0x06")),
                "control": integer(event.get(f"{prefix}_iw_control_0x12")),
                "motion_id": integer(event.get(f"{prefix}_iw_motion_id_0x64")),
                "progress_bits": hex32(event.get(f"{prefix}_iw_motion_progress_0x68")),
                "progress": bits_float(event.get(f"{prefix}_iw_motion_progress_0x68")),
                "increment_bits": hex32(event.get(f"{prefix}_iw_motion_increment_0x6c")),
                "increment": bits_float(event.get(f"{prefix}_iw_motion_increment_0x6c")),
                "flags_0xec": hex32(event.get(f"{prefix}_iw_flags_0xec")),
            })
        if rows:
            break
    return rows


def read_capture(job: int, path: Path) -> dict[str, Any]:
    counts: Counter[str] = Counter()
    events: list[dict[str, Any]] = []
    frames: list[dict[str, Any]] = []
    event_count = 0
    frame_window = 0
    list_snapshots = 0
    invalid_lists = 0
    false_samples = 0
    first_vi: int | None = None
    last_vi: int | None = None
    iw_slots: dict[int, int] = {}
    current_root_iws: dict[int, int] = {}

    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: expected an object")
            event_count += 1
            event_id = checkpoint_id(value)
            counts[event_id] += 1
            update_iw_slot_map(value, iw_slots)
            observed_roots = root_iw_snapshot(value)
            if observed_roots is not None:
                current_root_iws = observed_roots
            vi = integer(value.get("vi_field_count"))
            if vi is not None:
                first_vi = vi if first_vi is None else min(first_vi, vi)
                last_vi = vi if last_vi is None else max(last_vi, vi)
            if isinstance(value.get("thread_list_nodes"), list):
                list_snapshots += 1
                if (
                    value.get("thread_list_read_ok") is not True
                    or value.get("thread_list_truncated") is not False
                    or value.get("thread_list_cycle_detected") is not False
                ):
                    invalid_lists += 1
            false_samples += sum(
                sample is False
                for key, sample in value.items()
                if key.endswith(("_eval_ok", "_read_ok"))
            )
            if event_id == FRAME_ID:
                frames.extend(frame_slot_rows(job, value, frame_window + 1))
            if is_relevant(event_id):
                projected = project_event(value, frame_window)
                projected_iw = event_iw(projected)
                if projected_iw in iw_slots:
                    projected["_resolved_slot"] = iw_slots[projected_iw]
                watch_match = PROGRESS_WATCH_RE.match(event_id) or FLAGS_WATCH_RE.match(event_id)
                if watch_match and projected_iw is not None:
                    root_index = int(watch_match.group(1))
                    projected["_root_current_iw"] = current_root_iws.get(root_index)
                    projected["_watch_matches_current_root"] = (
                        current_root_iws.get(root_index) == projected_iw
                    )
                events.append(projected)
            if event_id == FRAME_ID:
                frame_window += 1

    return {
        "events": sorted(events, key=sequence),
        "frames": frames,
        "counts": counts,
        "event_count": event_count,
        "frame_count": frame_window,
        "list_snapshots": list_snapshots,
        "invalid_lists": invalid_lists,
        "false_samples": false_samples,
        "first_vi": first_vi,
        "last_vi": last_vi,
        "iw_slots": iw_slots,
    }


def event_prefix(event: dict[str, Any]) -> str | None:
    for prefix in PREFIXES:
        if f"{prefix}_iw_slot_0x00" in event:
            return prefix
    return None


def event_iw(event: dict[str, Any]) -> int | None:
    for key in ("instruction_worksheet", "origin_instruction"):
        value = integer(event.get(key))
        if value not in (None, 0):
            return value & 0xFFFFFFFF
    prefix = event_prefix(event)
    if prefix:
        address = integer(event.get(f"{prefix}_iw_slot_0x00_address"))
        if address not in (None, 0):
            return address & 0xFFFFFFFF
        pointer_value = integer(event.get(f"{prefix}_iw_ptr_0x4c"))
        if pointer_value not in (None, 0):
            return pointer_value & 0xFFFFFFFF
    event_id = checkpoint_id(event)
    address = integer(event.get("memwatch_addr"))
    if address is not None:
        if PROGRESS_WATCH_RE.match(event_id):
            return (address - 0x68) & 0xFFFFFFFF
        if FLAGS_WATCH_RE.match(event_id):
            return (address - 0xEC) & 0xFFFFFFFF
    return None


def event_slot(event: dict[str, Any]) -> int | None:
    resolved = integer(event.get("_resolved_slot"))
    if resolved is not None:
        return resolved
    prefix = event_prefix(event)
    if prefix:
        slot = integer(event.get(f"{prefix}_iw_slot_0x00"))
        if slot is not None:
            return slot
    for regex in (PROGRESS_WATCH_RE, FLAGS_WATCH_RE):
        match = regex.match(checkpoint_id(event))
        if match:
            return int(match.group(1))
    return None


def iw_value(event: dict[str, Any], suffix: str) -> Any:
    prefix = event_prefix(event)
    return event.get(f"{prefix}_{suffix}") if prefix else None


def normalized_event(job: int, event: dict[str, Any]) -> dict[str, Any]:
    progress_bits = iw_value(event, "iw_motion_progress_0x68")
    increment_bits = iw_value(event, "iw_motion_increment_0x6c")
    flags = iw_value(event, "iw_flags_0xec")
    return {
        "job": job,
        "capture_sequence": sequence(event),
        "vi": integer(event.get("vi_field_count")),
        "frame_window": integer(event.get("_frame_window")),
        "checkpoint_id": checkpoint_id(event),
        "pc": hex32(event.get("pc")),
        "slot": event_slot(event),
        "target_slot": integer(iw_value(event, "iw_target_0x04")),
        "instruction_worksheet": hex32(event_iw(event)),
        "mode": integer(iw_value(event, "iw_mode_0x06")),
        "staged_mode": integer(iw_value(event, "iw_staged_mode_0x0a")),
        "previous_mode": integer(iw_value(event, "iw_previous_mode_0x1c")),
        "control": integer(iw_value(event, "iw_control_0x12")),
        "motion_id": integer(iw_value(event, "iw_motion_id_0x64")),
        "action_row": integer(iw_value(event, "iw_action_row_0xe4")),
        "progress_bits": hex32(progress_bits),
        "progress": bits_float(progress_bits),
        "increment_bits": hex32(increment_bits),
        "increment": bits_float(increment_bits),
        "flags_0xec": hex32(flags),
        "bit31_set": bool((integer(flags) or 0) & 0x80000000),
        "result": integer(event.get("result")),
        "payload_primary": integer(event.get("record_payload_primary_0x00")),
        "payload_mode": integer(event.get("record_payload_mode_0x22")),
        "payload_flags": hex32(event.get("record_payload_flags_0x10")),
        "rng_draw_index_before": integer(event.get("rng_draw_index_before")),
    }


def queued_state_row(job: int, event: dict[str, Any]) -> dict[str, Any]:
    return {
        **normalized_event(job, event),
        "queued_state": integer(event.get("r4")),
        "state_array": hex32(event.get("r5")),
        "active_actor_slot": integer(event.get("active_actor_slot_80347334")),
        "r28": hex32(event.get("r28")),
        "r29": hex32(event.get("r29")),
        "r30": hex32(event.get("r30")),
        "r31": hex32(event.get("r31")),
    }


def writer_rows(job: int, events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        progress_match = PROGRESS_WATCH_RE.match(event_id)
        flags_match = FLAGS_WATCH_RE.match(event_id)
        if not progress_match and not flags_match:
            continue
        if event.get("memwatch_confirmed_current_instruction") is not True:
            continue
        pc = integer(event.get("decoded_pc")) or integer(event.get("pc"))
        post_bits = integer(event.get("decoded_memory_value"))
        kind = "progress" if progress_match else "flags"
        classification = (
            PROGRESS_WRITERS.get(pc, "unresolved_progress_writer")
            if kind == "progress"
            else FLAGS_WRITERS.get(pc, "other_flags_writer")
        )
        if event.get("_watch_matches_current_root") is False:
            classification = "stale_watch_after_root_rebinding"
        rows.append({
            "job": job,
            "capture_sequence": sequence(event),
            "vi": integer(event.get("vi_field_count")),
            "frame_window": integer(event.get("_frame_window")),
            "root_index": int((progress_match or flags_match).group(1)),
            "slot": event_slot(event),
            "instruction_worksheet": hex32(event_iw(event)),
            "current_root_instruction_worksheet": hex32(event.get("_root_current_iw")),
            "watch_matches_current_root": event.get("_watch_matches_current_root"),
            "field": kind,
            "writer_pc": hex32(pc),
            "classification": classification,
            "post_bits": hex32(post_bits),
            "post_float": bits_float(post_bits) if kind == "progress" else None,
            "post_value_authority": (
                "superseded_by_80018F9C_post_store_checkpoint"
                if pc == 0x80018F98
                else "normal_post_write_watchpoint"
            ),
            "bit31_set": bool((post_bits or 0) & 0x80000000) if kind == "flags" else None,
            "unattributed_extra_hits": integer(event.get("memwatch_unattributed_extra_hits")) or 0,
            "caller_callsite": hex32(event.get("caller_callsite_pc")),
            "stack": ">".join(
                hex32(event.get(f"stack_frame_{index}_callsite_pc"))
                for index in range(8)
                if integer(event.get(f"stack_frame_{index}_callsite_pc")) is not None
            ),
        })
    return rows


def preceding(
    events: Iterable[dict[str, Any]], event_id: str, iw: int, before: int
) -> dict[str, Any] | None:
    matches = [
        event for event in events
        if checkpoint_id(event) == event_id
        and event_iw(event) == iw
        and sequence(event) < before
    ]
    return max(matches, key=sequence) if matches else None


def expected_increment_bits(duration_bits: Any, flags_f0: Any) -> int | None:
    duration = bits_float(duration_bits)
    if duration is None or math.isnan(duration):
        return None
    if duration < 1.0:
        if (integer(flags_f0) or 0) & 0x00040000:
            duration = 5.0
    if duration == 0.0:
        return None
    return float_bits(f32(1.0 / duration))


def build_progress_chains(
    job: int,
    events: list[dict[str, Any]],
    writers: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    chains: list[dict[str, Any]] = []
    cadence: list[dict[str, Any]] = []
    installs = [event for event in events if checkpoint_id(event) == SET_ID]
    for install_ordinal, install in enumerate(installs):
        iw = event_iw(install)
        slot = event_slot(install)
        if iw is None or slot is None:
            continue
        next_install = min(
            (
                sequence(candidate) for candidate in installs
                if sequence(candidate) > sequence(install) and event_iw(candidate) == iw
            ),
            default=1 << 62,
        )
        gates = [
            event for event in events
            if checkpoint_id(event) == STATE6_ID
            and event_iw(event) == iw
            and sequence(install) < sequence(event) < next_install
        ]
        if not gates:
            continue
        true_gate = next((gate for gate in gates if integer(gate.get("result")) != 0), None)
        end_sequence = sequence(true_gate) if true_gate else next_install
        gates = [gate for gate in gates if sequence(gate) <= end_sequence]
        duration = preceding(events, DURATION_ID, iw, sequence(install))
        reset = max(
            (
                event for event in events
                if checkpoint_id(event) in RESET_IDS
                and event_iw(event) == iw
                and (sequence(duration) if duration else -1) < sequence(event) < sequence(install)
            ),
            key=sequence,
            default=None,
        )
        chain_writers = [
            row for row in writers
            if row["field"] == "progress"
            and integer(row["slot"]) == slot
            and integer(row["instruction_worksheet"]) == iw
            and sequence(install) < integer(row["capture_sequence"]) <= end_sequence
        ]
        progress_updates = [
            event for event in events
            if checkpoint_id(event) in {
                "motion_renderer_increment_complete_80018F9C",
                "motion_renderer_clamp_complete_80018FAC",
            }
            and event_iw(event) == iw
            and sequence(install) < sequence(event) <= end_sequence
        ]
        duration_bits = duration.get("selected_action_row_duration_0x10") if duration else None
        expected_bits = expected_increment_bits(
            duration_bits,
            iw_value(duration or {}, "iw_flags_0xf0"),
        )
        install_progress_bits = integer(iw_value(install, "iw_motion_progress_0x68"))
        install_increment_bits = integer(iw_value(install, "iw_motion_increment_0x6c"))
        install_flags = integer(iw_value(install, "iw_flags_0xec"))

        previous = bits_float(install_progress_bits)
        increment = bits_float(install_increment_bits)
        update_checks: list[bool] = []
        for update in progress_updates:
            observed_bits = integer(iw_value(update, "iw_motion_progress_0x68"))
            if (
                checkpoint_id(update) == "motion_renderer_increment_complete_80018F9C"
                and previous is not None
                and increment is not None
            ):
                expected_post = float_bits(f32_add(previous, increment))
                update_checks.append(observed_bits == expected_post)
            elif checkpoint_id(update) == "motion_renderer_clamp_complete_80018FAC":
                update_checks.append(observed_bits == 0x3F800000)
            previous = bits_float(observed_bits)
        unknown_progress_writers = sorted({
            str(row["writer_pc"])
            for row in chain_writers
            if integer(row["writer_pc"]) not in {0x80018F98, 0x80018FA8}
        })

        gate_results = [integer(gate.get("result")) for gate in gates]
        gate_progress = [integer(iw_value(gate, "iw_motion_progress_0x68")) for gate in gates]
        gate_flags = [integer(iw_value(gate, "iw_flags_0xec")) for gate in gates]
        false_gates = [gate for gate in gates if integer(gate.get("result")) == 0]
        updates_per_false: list[int] = []
        for index, gate in enumerate(false_gates):
            later_gate_sequence = min(
                (
                    sequence(candidate) for candidate in gates
                    if sequence(candidate) > sequence(gate)
                ),
                default=end_sequence + 1,
            )
            updates_per_false.append(sum(
                checkpoint_id(update) == "motion_renderer_increment_complete_80018F9C"
                and sequence(gate) < sequence(update) < later_gate_sequence
                for update in progress_updates
            ))

        initial_updates_before_first_gate = sum(
            checkpoint_id(update) == "motion_renderer_increment_complete_80018F9C"
            and sequence(install) < sequence(update) < sequence(gates[0])
            for update in progress_updates
        )

        gate_windows = [integer(gate.get("_frame_window")) for gate in gates]
        consecutive_gate_frames = all(
            left is not None and right is not None and right - left == 1
            for left, right in zip(gate_windows, gate_windows[1:])
        )
        false_flags_preserved = all(
            flags is not None and bool(flags & 0x80000000)
            for flags, result in zip(gate_flags, gate_results)
            if result == 0
        )
        true_flag_cleared = (
            true_gate is not None
            and gate_flags[-1] is not None
            and not bool(gate_flags[-1] & 0x80000000)
        )
        true_progress = bits_float(gate_progress[-1]) if true_gate else None
        progression_exact = bool(update_checks) and all(update_checks) and not unknown_progress_writers
        chain_id = f"{job}:{install_ordinal}"
        complete = all((
            duration is not None,
            reset is not None,
            expected_bits is not None,
            expected_bits == install_increment_bits,
            install_progress_bits == 0,
            bool((install_flags or 0) & 0x80000000),
            true_gate is not None,
            false_flags_preserved,
            true_flag_cleared,
            true_progress is not None and true_progress >= 1.0,
            progression_exact,
            initial_updates_before_first_gate == 1,
            all(count == 1 for count in updates_per_false),
            consecutive_gate_frames,
        ))

        chains.append({
            "job": job,
            "chain_id": chain_id,
            "install_ordinal": install_ordinal,
            "slot": slot,
            "instruction_worksheet": hex32(iw),
            "mode": integer(iw_value(install, "iw_mode_0x06")),
            "control": integer(iw_value(install, "iw_control_0x12")),
            "motion_id": integer(iw_value(install, "iw_motion_id_0x64")),
            "action_row": integer(iw_value(install, "iw_action_row_0xe4")),
            "duration_sequence": sequence(duration) if duration else None,
            "duration_bits": hex32(duration_bits),
            "duration": bits_float(duration_bits),
            "row_frame_step_bits": hex32(duration.get("selected_action_row_frame_step_0x14")) if duration else "",
            "reset_sequence": sequence(reset) if reset else None,
            "install_sequence": sequence(install),
            "install_frame_window": integer(install.get("_frame_window")),
            "install_progress_bits": hex32(install_progress_bits),
            "install_increment_bits": hex32(install_increment_bits),
            "expected_increment_bits": hex32(expected_bits),
            "expected_increment_matches": expected_bits == install_increment_bits,
            "install_flags_0xec": hex32(install_flags),
            "gate_sequences": ">".join(str(sequence(gate)) for gate in gates),
            "gate_frame_windows": ">".join(str(window) for window in gate_windows),
            "gate_results": ">".join(str(result) for result in gate_results),
            "gate_progress_bits": ">".join(hex32(value) for value in gate_progress),
            "gate_flags_0xec": ">".join(hex32(value) for value in gate_flags),
            "false_poll_count": len(false_gates),
            "progress_writer_sequences": ">".join(str(row["capture_sequence"]) for row in chain_writers),
            "progress_writer_pcs": ">".join(str(row["writer_pc"]) for row in chain_writers),
            "progress_writer_bits": ">".join(str(row["post_bits"]) for row in chain_writers),
            "authoritative_update_sequences": ">".join(
                str(sequence(update)) for update in progress_updates
            ),
            "authoritative_update_bits": ">".join(
                hex32(iw_value(update, "iw_motion_progress_0x68"))
                for update in progress_updates
            ),
            "update_arithmetic_matches": ">".join(
                "1" if value else "0" for value in update_checks
            ),
            "unknown_progress_writers": ">".join(unknown_progress_writers),
            "initial_updates_before_first_gate": initial_updates_before_first_gate,
            "updates_per_false_poll": ">".join(str(value) for value in updates_per_false),
            "progression_exact": progression_exact,
            "false_flags_preserved": false_flags_preserved,
            "true_flag_cleared": true_flag_cleared,
            "threshold_met": true_progress is not None and true_progress >= 1.0,
            "consecutive_gate_frames": consecutive_gate_frames,
            "complete": complete,
        })

        chain_events: list[tuple[str, dict[str, Any] | dict[str, Any]]] = [
            ("install", install),
            *(("state6_gate", gate) for gate in gates),
        ]
        for update in progress_updates:
            chain_events.append((checkpoint_id(update), update))
        for phase, item in sorted(
            chain_events,
            key=lambda value: integer(value[1].get("capture_sequence")) or 0,
        ):
            cadence.append({
                "job": job,
                "chain_id": chain_id,
                "phase": phase,
                "capture_sequence": integer(item.get("capture_sequence")),
                "vi": integer(item.get("vi_field_count")),
                "frame_window": integer(item.get("_frame_window")),
                "progress_bits": hex32(iw_value(item, "iw_motion_progress_0x68")),
                "increment_bits": hex32(iw_value(item, "iw_motion_increment_0x6c")),
                "flags_0xec": hex32(iw_value(item, "iw_flags_0xec")),
                "result": integer(item.get("result")),
                "writer_pc": "",
            })
    return chains, cadence


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields: list[str] = []
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def findings_text(
    run_root: Path,
    acceptance: list[dict[str, Any]],
    chains: list[dict[str, Any]],
    writers: list[dict[str, Any]],
) -> str:
    lines = [
        "# Mode-1 State-6 Motion Progress",
        "",
        "## Capture",
        "",
        f"- Raw run: `{run_root}`.",
    ]
    for row in acceptance:
        lines.append(
            f"- Job {row['job']}: terminal state `{row['terminal_state']}`, "
            f"{row['event_count']} events, {row['frame_end_count']} case-5 frame ends, "
            f"{row['complete_state6_chains']}/{row['state6_chains']} complete state-6 chains, "
            f"{row['invalid_thread_lists']} invalid thread-list snapshots."
        )
    progress_pcs = Counter(
        row["writer_pc"] for row in writers if row["field"] == "progress"
    )
    flag_pcs = Counter(
        row["writer_pc"] for row in writers if row["field"] == "flags"
    )
    lines.extend([
        "",
        "## Reduced Contract",
        "",
        f"- Confirmed progress writers: {dict(progress_pcs)}.",
        f"- Confirmed flag writers: {dict(flag_pcs)}.",
    ])
    for chain in chains:
        lines.append(
            f"- {chain['chain_id']}: slot {chain['slot']} mode {chain['mode']}, duration "
            f"`{chain['duration_bits']}`, increment `{chain['install_increment_bits']}`, "
            f"state-6 results `{chain['gate_results']}`, progress "
            f"`{chain['gate_progress_bits']}`, frame windows "
            f"`{chain['gate_frame_windows']}`, complete={chain['complete']}."
        )
    lines.extend([
        "",
        "## Scope",
        "",
        "- This reducer tests evidence only. It does not change predictor scheduling.",
        "- A failed terminal job may still contain a complete target chain; terminal acceptance and chain acceptance are reported separately.",
        "",
    ])
    return "\n".join(lines)


def reduce_capture(run_root: Path, analysis_root: Path) -> dict[str, Any]:
    manifest, captures = locate_captures(run_root)
    manifest_jobs = {
        integer(row.get("original_exec_job_id")): row
        for row in manifest.get("jobs", [])
        if isinstance(row, dict)
    }
    limits = profile_limits(run_root / "capture_profile.ini")
    acceptance: list[dict[str, Any]] = []
    checkpoint_counts: list[dict[str, Any]] = []
    frame_rows: list[dict[str, Any]] = []
    install_rows: list[dict[str, Any]] = []
    gate_rows: list[dict[str, Any]] = []
    publication_rows: list[dict[str, Any]] = []
    queued_state_rows: list[dict[str, Any]] = []
    all_writers: list[dict[str, Any]] = []
    all_chains: list[dict[str, Any]] = []
    all_cadence: list[dict[str, Any]] = []

    for job, path in sorted(captures.items()):
        capture = read_capture(job, path)
        events = capture["events"]
        writers = writer_rows(job, events)
        chains, cadence = build_progress_chains(job, events, writers)
        all_writers.extend(writers)
        all_chains.extend(chains)
        all_cadence.extend(cadence)
        frame_rows.extend(capture["frames"])
        install_rows.extend(
            normalized_event(job, event)
            | {
                "duration_bits": hex32(event.get("selected_action_row_duration_0x10")),
                "duration": bits_float(event.get("selected_action_row_duration_0x10")),
                "row_frame_step_bits": hex32(event.get("selected_action_row_frame_step_0x14")),
            }
            for event in events
            if checkpoint_id(event) in ({DURATION_ID, SET_ID} | RESET_IDS)
        )
        gate_rows.extend(
            normalized_event(job, event)
            for event in events
            if checkpoint_id(event).startswith("motion_gate_")
            or checkpoint_id(event).startswith("callback_gate_state")
        )
        publication_rows.extend(
            normalized_event(job, event)
            for event in events
            if checkpoint_id(event) in PUBLICATION_IDS
        )
        queued_state_rows.extend(
            queued_state_row(job, event)
            for event in events
            if checkpoint_id(event) == QUEUED_STATE_ID
        )
        for event_id, count in sorted(capture["counts"].items()):
            checkpoint_counts.append({
                "job": job,
                "checkpoint_id": event_id,
                "count": count,
                "max_hits": limits.get(event_id),
                "reached_local_cap": limits.get(event_id) == count,
            })
        manifest_job = manifest_jobs.get(job, {})
        unknown_progress = sorted({
            row["writer_pc"] for row in writers
            if row["classification"] == "unresolved_progress_writer"
        })
        acceptance.append({
            "job": job,
            "terminal_state": manifest_job.get("terminal_state", ""),
            "source_fake_attacks": manifest_job.get("source_fake_attacks_this_turn", ""),
            "event_count": capture["event_count"],
            "event_limit_headroom": GLOBAL_EVENT_LIMIT - capture["event_count"],
            "first_vi": capture["first_vi"],
            "last_vi": capture["last_vi"],
            "frame_end_count": capture["frame_count"],
            "frame_cap_reached": capture["counts"].get(FRAME_ID, 0) >= limits.get(FRAME_ID, 2400),
            "thread_list_snapshots": capture["list_snapshots"],
            "invalid_thread_lists": capture["invalid_lists"],
            "false_addrprog_samples": capture["false_samples"],
            "confirmed_progress_writes": sum(row["field"] == "progress" for row in writers),
            "confirmed_flags_writes": sum(row["field"] == "flags" for row in writers),
            "unresolved_progress_writer_pcs": ";".join(unknown_progress),
            "state6_chains": len(chains),
            "complete_state6_chains": sum(bool(chain["complete"]) for chain in chains),
            "all_target_chains_complete": bool(chains) and all(chain["complete"] for chain in chains),
        })

    analysis_root.mkdir(parents=True, exist_ok=True)
    write_csv(analysis_root / "capture_acceptance.csv", acceptance)
    write_csv(analysis_root / "checkpoint_counts.csv", checkpoint_counts)
    write_csv(analysis_root / "motion_install_timeline.csv", install_rows)
    write_csv(analysis_root / "motion_progress_writers.csv", all_writers)
    write_csv(analysis_root / "state6_gate_timeline.csv", gate_rows)
    write_csv(analysis_root / "motion_progress_chains.csv", all_chains)
    write_csv(analysis_root / "motion_frame_cadence.csv", all_cadence)
    write_csv(analysis_root / "queued_state_timeline.csv", queued_state_rows)
    write_csv(analysis_root / "mode1_publication_timeline.csv", publication_rows)
    write_csv(analysis_root / "frame_progress_timeline.csv", frame_rows)
    (analysis_root / "findings.md").write_text(
        findings_text(run_root, acceptance, all_chains, all_writers),
        encoding="utf-8",
    )
    return {"acceptance": acceptance, "chains": all_chains, "writers": all_writers}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--analysis-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = reduce_capture(args.run_root, args.analysis_root)
    print(json.dumps({
        "analysis_root": str(args.analysis_root),
        "acceptance": result["acceptance"],
        "chains": result["chains"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
