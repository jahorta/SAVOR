#!/usr/bin/env python3
"""Reduce view-placement semantic-hook captures into integration evidence."""

from __future__ import annotations

import argparse
import csv
import json
import re
import struct
from bisect import bisect_left
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
CONTROL_WATCH_ID = "memwatch.view_cache_control_write_8030A062"
RNG_WATCH_ID = "memwatch.rng_seed_write_803469A8"
FRAME_CAP = 2400
WORKER_LOG_TAIL_BYTES = 8 * 1024 * 1024

FINAL_HIT_PATTERN = re.compile(
    r"\[VM\] run_until_bp outcome=(?P<outcome>\d+) "
    r"pc=(?P<pc>[0-9A-Fa-f]+) bp_key=(?P<key>\d+) "
    r"bp_symbol=(?P<symbol>\S+) expected_match=(?P<expected>\d+)"
)
RESULT_DIAGNOSTICS_PATTERN = re.compile(
    r"battle_outcome=(?P<outcome>.*?)\((?P<outcome_code>\d+)\) "
    r"vi_start=(?P<vi_start>\d+) vi_end=(?P<vi_end>\d+).*?"
    r"pred_abort_run=(?P<pred_abort>\d+)"
)

TERMINAL_BREAKPOINTS = {
    203: "next_turn_inputs",
    208: "end_turn",
    209: "victory",
    210: "defeat",
}

CACHE_FIELDS = (
    "view_cache_control_8030A062",
    "view_cache_distance_bits_80309FD8",
    "view_cache_center_x_bits_80309F88",
    "view_cache_center_y_bits_80309F8C",
    "view_cache_center_z_bits_80309F90",
    "view_cache_angle_bits_8030A028",
)

GEOMETRY_RETURN_PATHS = {
    "placement_function_geometry_return_80012254": {
        "path": "placement_function",
        "key_event": "placement_function_cache_decision_80012308",
        "local_prefix": "placement_function",
        "publisher": "view_placement.publish.placement_function",
    },
    "runner_geometry_return_800138DC": {
        "path": "runner",
        "key_event": "runner_cache_draw_80013920",
        "local_prefix": "runner",
        "publisher": "view_placement.publish.runner",
    },
    "direct_view_geometry_return_80014508": {
        "path": "direct_view",
        "key_event": "view_placement_geometry_result_80014548",
        "local_prefix": "direct_view",
        "publisher": "view_placement.publish.direct_view",
    },
}

WRITER_SPECS = {
    "0x8001250c": (
        "view_placement.publish.placement_function",
        "view_cache_publish_from_121D8_80012530",
    ),
    "0x800139bc": (
        "view_placement.publish.runner",
        "view_cache_publish_from_136DC_800139D8",
    ),
    "0x800146e8": (
        "view_placement.publish.direct_view",
        "view_cache_publish_from_14474_80014704",
    ),
    "0x80052af0": (
        "view_placement.copy.workspace_snapshot",
        "workspace_copy_after_loop_80052B04",
    ),
}

TIMELINE_IDS = {
    "placement_function_cache_decision_80012308": ("placement_function", "decision"),
    "placement_function_cache_miss_call_80012314": ("placement_function", "miss_draw_call"),
    "view_cache_publish_from_121D8_80012530": ("placement_function", "publication"),
    "runner_cache_draw_80013920": ("runner", "draw_call"),
    "view_cache_publish_from_136DC_800139D8": ("runner", "publication"),
    "view_placement_readiness_return_800144EC": ("direct_view", "readiness"),
    "view_placement_cache_decision_800145BC": ("direct_view", "decision"),
    "view_placement_rng_call_800145C8": ("direct_view", "miss_draw_call"),
    "view_placement_rng_value_800608E0": ("angle_selector", "rand_value"),
    "view_placement_rng_angle_selected_8006093C": ("angle_selector", "angle_result"),
    "view_placement_post_selection_800145CC": ("direct_view", "selection_complete"),
    "view_cache_publish_from_14474_80014704": ("direct_view", "publication"),
    "active_record_reset_before_memset_80014B68": ("active_record", "reset_begin"),
    "active_record_reset_after_memset_80014B6C": ("active_record", "reset_complete"),
    "workspace_copy_before_loop_80052AE8": ("workspace_copy", "copy_begin"),
    "workspace_copy_after_loop_80052B04": ("workspace_copy", "copy_complete"),
}


def scalar(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def integer(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return None


def hex32(value: int | None) -> str:
    return "" if value is None else f"0x{value & 0xFFFFFFFF:08X}"


def normalized_pc(value: Any) -> str:
    parsed = integer(value)
    return "" if parsed is None else f"0x{parsed & 0xFFFFFFFF:08x}"


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return scalar(event.get("checkpoint_id"))


def bool_value(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    text = scalar(value).lower()
    if text == "true":
        return True
    if text == "false":
        return False
    return None


def bits_value(event: dict[str, Any], field: str) -> int | None:
    read_ok_name = f"{field}_read_ok"
    eval_ok_name = f"{field}_eval_ok"
    if read_ok_name in event and bool_value(event.get(read_ok_name)) is not True:
        return None
    if eval_ok_name in event and bool_value(event.get(eval_ok_name)) is not True:
        return None
    return integer(event.get(field))


def f32(value: float) -> float:
    return struct.unpack(">f", struct.pack(">f", value))[0]


def bits_to_f32(value: int) -> float:
    return struct.unpack(">f", struct.pack(">I", value & 0xFFFFFFFF))[0]


def f32_to_bits(value: float) -> int:
    return struct.unpack(">I", struct.pack(">f", f32(value)))[0]


def signed_byte(value: int | None) -> int | None:
    if value is None:
        return None
    low = value & 0xFF
    return low - 0x100 if low >= 0x80 else low


def stack_contains_callsite(event: dict[str, Any], pc: str) -> bool:
    wanted = normalized_pc(pc)
    count = integer(event.get("stack_frame_count")) or 0
    return any(
        normalized_pc(event.get(f"stack_frame_{index}_callsite_pc")) == wanted
        for index in range(count)
    )


def load_events(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            text = line.strip()
            if not text:
                continue
            try:
                row = json.loads(text)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: JSON row is not an object")
            rows.append(row)
    rows.sort(key=sequence)
    return rows


def load_run(path: Path) -> dict[str, Any]:
    capture = path if path.is_file() else path / "capture.jsonl"
    root = capture.parent
    manifest_path = root / "manifest.json"
    manifest: dict[str, Any] = {}
    if manifest_path.exists():
        parsed = json.loads(manifest_path.read_text(encoding="utf-8"))
        if isinstance(parsed, dict):
            manifest = parsed
    return {
        "root": root,
        "capture": capture,
        "run": root.name,
        "job": manifest.get("original_exec_job_id", ""),
        "manifest": manifest,
        "events": load_events(capture),
    }


def manifest_result_diagnostics(manifest: dict[str, Any]) -> str:
    events = manifest.get("events")
    if not isinstance(events, list):
        return ""
    matches = [
        event
        for event in events
        if isinstance(event, str)
        and event.startswith("[battle-single-turn-result-diagnostics]")
    ]
    return matches[-1] if matches else ""


def worker_log_tail(root: Path) -> tuple[Path | None, str]:
    worker_root = root / ".worker-runtime"
    if not worker_root.exists():
        return None, ""
    logs = sorted(
        worker_root.rglob("worker-*.log"),
        key=lambda candidate: candidate.stat().st_mtime_ns,
    )
    if not logs:
        return None, ""
    path = logs[-1]
    with path.open("rb") as source:
        source.seek(0, 2)
        size = source.tell()
        source.seek(max(0, size - WORKER_LOG_TAIL_BYTES))
        text = source.read().decode("utf-8", errors="replace")
    return path, text


def reduce_capture_coverage(run: dict[str, Any]) -> dict[str, Any]:
    diagnostics = manifest_result_diagnostics(run["manifest"])
    diagnostics_match = RESULT_DIAGNOSTICS_PATTERN.search(diagnostics)
    worker_log, tail = worker_log_tail(run["root"])
    final_hits = list(FINAL_HIT_PATTERN.finditer(tail))
    final_hit = final_hits[-1] if final_hits else None
    final_key = int(final_hit.group("key")) if final_hit else None
    terminal_kind = TERMINAL_BREAKPOINTS.get(final_key, "unknown")

    frames = [event for event in run["events"] if checkpoint_id(event) == FRAME_ID]
    last_event = run["events"][-1] if run["events"] else None
    vi_end = (
        int(diagnostics_match.group("vi_end"))
        if diagnostics_match is not None
        else None
    )
    last_capture_vi = integer(last_event.get("vi_field_count")) if last_event else None
    tail_vi_gap = (
        vi_end - last_capture_vi
        if vi_end is not None and last_capture_vi is not None
        else None
    )
    timed_out = bool(run["manifest"].get("timed_out", False))
    frame_cap_reached = len(frames) >= FRAME_CAP
    recognized_terminal = terminal_kind != "unknown"

    return {
        "run": run["run"],
        "source_exec_job_id": run["job"],
        "manifest_outcome": (
            diagnostics_match.group("outcome").strip()
            if diagnostics_match is not None
            else "unknown"
        ),
        "manifest_outcome_code": (
            int(diagnostics_match.group("outcome_code"))
            if diagnostics_match is not None
            else None
        ),
        "pred_abort_run": (
            int(diagnostics_match.group("pred_abort"))
            if diagnostics_match is not None
            else None
        ),
        "timed_out": timed_out,
        "worker_log": str(worker_log) if worker_log else "",
        "final_breakpoint_pc": (
            f"0x{int(final_hit.group('pc'), 16):08X}" if final_hit else ""
        ),
        "final_breakpoint_key": final_key,
        "final_breakpoint_symbol": final_hit.group("symbol") if final_hit else "",
        "terminal_kind": terminal_kind,
        "recognized_terminal": recognized_terminal,
        "case5_rows": len(frames),
        "frame_cap": FRAME_CAP,
        "frame_cap_reached": frame_cap_reached,
        "last_capture_sequence": sequence(last_event) if last_event else None,
        "last_capture_vi": last_capture_vi,
        "terminal_vi": vi_end,
        "terminal_vi_gap": tail_vi_gap,
        "runner_draw_checkpoint_hits": sum(
            checkpoint_id(event) == "runner_cache_draw_80013920"
            for event in run["events"]
        ),
        "runner_publication_checkpoint_hits": sum(
            checkpoint_id(event) == "view_cache_publish_from_136DC_800139D8"
            for event in run["events"]
        ),
        "coverage_pass": (
            recognized_terminal
            and not timed_out
            and bool(frames)
            and not frame_cap_reached
        ),
    }


def write_csv(path: Path, rows: Iterable[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: scalar(row.get(field)) for field in fields})


def prior_event_with_field(
    events: list[dict[str, Any]], index: int, field: str
) -> dict[str, Any] | None:
    for candidate_index in range(index - 1, -1, -1):
        candidate = events[candidate_index]
        if field in candidate:
            return candidate
    return None


def prior_event_with_id(
    events: list[dict[str, Any]], index: int, expected_id: str
) -> dict[str, Any] | None:
    for candidate_index in range(index - 1, -1, -1):
        candidate = events[candidate_index]
        if checkpoint_id(candidate) == expected_id:
            return candidate
    return None


def next_event_with_id(
    events: list[dict[str, Any]], index: int, expected_id: str, stop_at_control: bool = False
) -> dict[str, Any] | None:
    for candidate in events[index + 1 :]:
        if stop_at_control and checkpoint_id(candidate) == CONTROL_WATCH_ID:
            return None
        if checkpoint_id(candidate) == expected_id:
            return candidate
    return None


def nearest_frames(
    frames: list[dict[str, Any]], frame_sequences: list[int], event_sequence: int
) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    index = bisect_left(frame_sequences, event_sequence)
    previous = frames[index - 1] if index > 0 else None
    following = frames[index] if index < len(frames) else None
    return previous, following


def cache_snapshot(event: dict[str, Any] | None, prefix: str) -> dict[str, str]:
    if event is None:
        return {f"{prefix}_{field}": "" for field in CACHE_FIELDS}
    return {f"{prefix}_{field}": scalar(event.get(field)) for field in CACHE_FIELDS}


def classify_writer(event: dict[str, Any]) -> tuple[str, str]:
    pc = normalized_pc(event.get("memwatch_source_pc"))
    if pc in {"", "0x00000000"}:
        pc = normalized_pc(event.get("pc"))
    if pc == "0x800054d4":
        if (
            stack_contains_callsite(event, "0x80014B68")
            and normalized_pc(event.get("decoded_effective_addr")) == "0x8030a060"
            and integer(event.get("decoded_access_size")) == 4
            and integer(event.get("decoded_memory_value")) == 0
        ):
            return "view_placement.reset.active_record", "active_record_reset_after_memset_80014B6C"
        return "unresolved", ""
    return WRITER_SPECS.get(pc, ("unresolved", ""))


def reduce_writers(run: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    events = run["events"]
    for index, event in enumerate(events):
        if checkpoint_id(event) != CONTROL_WATCH_ID:
            continue
        source_pc = normalized_pc(event.get("memwatch_source_pc"))
        if source_pc in {"", "0x00000000"}:
            source_pc = normalized_pc(event.get("pc"))
        semantic_source, expected_post_id = classify_writer(event)
        post = (
            next_event_with_id(events, index, expected_post_id, stop_at_control=True)
            if expected_post_id
            else None
        )
        before = prior_event_with_field(events, index, "view_cache_control_8030A062")
        source = None
        if semantic_source == "view_placement.copy.workspace_snapshot":
            source = prior_event_with_id(events, index, "workspace_copy_before_loop_80052AE8")
        row = {
            "run": run["run"],
            "source_exec_job_id": run["job"],
            "capture_sequence": sequence(event),
            "source_pc": source_pc,
            "semantic_source": semantic_source,
            "classification": "known" if semantic_source != "unresolved" else "unresolved",
            "expected_post_checkpoint": expected_post_id,
            "post_sequence": sequence(post) if post else "",
            "post_matched": bool(post),
            "rng_draw_index_before": event.get("rng_draw_index_before"),
            "decoded_effective_addr": event.get("decoded_effective_addr"),
            "decoded_access_size": event.get("decoded_access_size"),
            "decoded_value": event.get("decoded_value"),
            "post_write_value": event.get("decoded_memory_value"),
            "caller_callsite_pc": event.get("caller_callsite_pc"),
            "stack_has_reset_callsite_80014B68": stack_contains_callsite(event, "0x80014B68"),
            "source_control": source.get("workspace_copy_source_control_0x112") if source else "",
            "source_distance": source.get("workspace_copy_source_distance_0x88") if source else "",
            "source_center_x": source.get("workspace_copy_source_center_x_0x38") if source else "",
            "source_center_y": source.get("workspace_copy_source_center_y_0x3c") if source else "",
            "source_center_z": source.get("workspace_copy_source_center_z_0x40") if source else "",
            "source_angle": source.get("workspace_copy_source_angle_0xd8") if source else "",
            **cache_snapshot(before, "before"),
            **cache_snapshot(post, "after"),
        }
        rows.append(row)
    return rows


def model_geometry(candidates: list[dict[str, int]]) -> dict[str, int]:
    max_x = f32(-500.0)
    min_x = f32(500.0)
    max_z = f32(-500.0)
    min_z = f32(500.0)
    max_extent = f32(0.0)
    max_extent_bits = f32_to_bits(max_extent)
    for candidate in candidates:
        radius = f32(22.5 if candidate["flags"] & 0x200000 else 7.5)
        x = bits_to_f32(candidate["x_bits"])
        z = bits_to_f32(candidate["z_bits"])
        value = f32(x + radius)
        if value > max_x:
            max_x = value
        value = f32(x - radius)
        if value < min_x:
            min_x = value
        value = f32(z + radius)
        if value > max_z:
            max_z = value
        value = f32(z - radius)
        if value < min_z:
            min_z = value
        extent = bits_to_f32(candidate["extent_bits"])
        if extent > max_extent:
            max_extent = extent
            max_extent_bits = candidate["extent_bits"]

    width = f32(max_x - min_x)
    if width < 0.0:
        width = f32(-1.0 * width)
    depth = f32(max_z - min_z)
    if depth < 0.0:
        depth = f32(-1.0 * depth)
    half_x = f32(width * 0.5)
    half_z = f32(depth * 0.5)
    center_x = f32(min_x + half_x)
    center_z = f32(min_z + half_z)
    largest = half_z if half_x <= half_z else half_x
    distance = f32(2.0 * largest)
    return {
        "half_x": f32_to_bits(half_x),
        "half_z": f32_to_bits(half_z),
        "center_x": f32_to_bits(center_x),
        "center_y": f32_to_bits(0.0),
        "center_z": f32_to_bits(center_z),
        "distance": f32_to_bits(distance),
        "max_extent": max_extent_bits,
    }


def candidate_inputs(
    event: dict[str, Any], use_saved: bool, mld_results: dict[int, int]
) -> tuple[list[dict[str, int]], list[int], list[int], list[str]]:
    candidates: list[dict[str, int]] = []
    included: list[int] = []
    excluded: list[int] = []
    missing: list[str] = []
    for slot in range(12):
        root = integer(event.get(f"slot{slot}_combatant_thread_ptr"))
        if root in {None, 0}:
            continue
        flags = bits_value(event, f"slot{slot}_iw_geometry_flags_0xec")
        if flags is None:
            missing.append(f"slot{slot}.instruction_flags")
            continue
        if flags & 0x100:
            mld = mld_results.get(slot)
            if mld is None:
                missing.append(f"slot{slot}.mld_slot_result")
                continue
            if mld < 0:
                excluded.append(slot)
                continue
        prefix = "iw_saved" if use_saved else "cw_current"
        x = bits_value(event, f"slot{slot}_{prefix}_x_0x{'13c' if use_saved else '1c'}")
        z = bits_value(event, f"slot{slot}_{prefix}_z_0x{'144' if use_saved else '24'}")
        extent = bits_value(event, f"slot{slot}_iw_geometry_extent_0x15c")
        if x is None or z is None:
            missing.append(f"slot{slot}.{'saved' if use_saved else 'current'}_position")
        if extent is None:
            missing.append(f"slot{slot}.geometry_extent")
        if x is None or z is None or extent is None:
            continue
        included.append(slot)
        candidates.append({
            "slot": slot,
            "flags": flags,
            "x_bits": x,
            "z_bits": z,
            "extent_bits": extent,
        })
    return candidates, included, excluded, missing


def geometry_target_values(event: dict[str, Any] | None, prefix: str) -> dict[str, int | None]:
    if event is None:
        return {name: None for name in ("distance", "center_x", "center_y", "center_z")}
    return {
        "distance": bits_value(event, f"{prefix}_distance_bits_0x0c"),
        "center_x": bits_value(event, f"{prefix}_center_x_bits_0x38"),
        "center_y": bits_value(event, f"{prefix}_center_y_bits_0x3c"),
        "center_z": bits_value(event, f"{prefix}_center_z_bits_0x40"),
    }


def reduce_geometry(run: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    events = run["events"]
    current_entry: dict[str, Any] | None = None
    mld_results: dict[int, int] = {}
    for index, event in enumerate(events):
        event_id = checkpoint_id(event)
        if event_id == "view_geometry_entry_800114AC":
            current_entry = event
            mld_results = {}
            continue
        if event_id == "view_geometry_mld_slot_return_80011544" and current_entry is not None:
            slot = integer(event.get("geometry_slot_index"))
            result = signed_byte(integer(event.get("mld_slot_return")))
            if slot is not None and result is not None:
                mld_results[slot & 0xFF] = result
            continue
        path_spec = GEOMETRY_RETURN_PATHS.get(event_id)
        if path_spec is None:
            continue

        use_saved_raw = integer(current_entry.get("geometry_use_saved_position")) if current_entry else None
        use_saved = use_saved_raw not in {None, 0}
        candidates, included, excluded, missing = candidate_inputs(event, use_saved, mld_results)
        if current_entry is None:
            missing.append("geometry_entry")
        modeled = model_geometry(candidates) if not missing else {}
        target = next_event_with_id(events, index, path_spec["key_event"])
        observed_geometry = {
            "half_x": bits_value(event, "geometry_half_x_bits"),
            "half_z": bits_value(event, "geometry_half_z_bits"),
            "center_x": bits_value(event, "geometry_center_x_bits"),
            "center_y": bits_value(event, "geometry_center_y_bits"),
            "center_z": bits_value(event, "geometry_center_z_bits"),
        }
        observed_key = geometry_target_values(target, path_spec["local_prefix"])
        geometry_match = bool(modeled) and all(
            modeled[name] == observed_geometry[name]
            for name in ("half_x", "half_z", "center_x", "center_y", "center_z")
        )
        key_match = bool(modeled) and all(
            modeled[name] == observed_key[name]
            for name in ("distance", "center_x", "center_y", "center_z")
        )
        vectors = ";".join(
            f"slot{candidate['slot']}:flags={hex32(candidate['flags'])}|"
            f"x={hex32(candidate['x_bits'])}|z={hex32(candidate['z_bits'])}|"
            f"extent={hex32(candidate['extent_bits'])}"
            for candidate in candidates
        )
        constants_match = all(
            integer(event.get(field)) == expected
            for field, expected in {
                "view_geometry_zero_bits_803481B8": 0x00000000,
                "view_geometry_radius_normal_bits_803481BC": 0x40F00000,
                "view_geometry_negate_bits_803481CC": 0xBF800000,
                "view_geometry_min_seed_bits_803481D0": 0xC3FA0000,
                "view_geometry_max_seed_bits_803481D4": 0x43FA0000,
                "view_geometry_radius_large_bits_803481D8": 0x41B40000,
                "view_geometry_half_bits_803481DC": 0x3F000000,
                "view_geometry_distance_scale_bits_80348210": 0x40000000,
            }.items()
        )
        rows.append({
            "run": run["run"],
            "source_exec_job_id": run["job"],
            "capture_sequence": sequence(event),
            "entry_sequence": sequence(current_entry) if current_entry else "",
            "path": path_spec["path"],
            "publisher_source": path_spec["publisher"],
            "use_saved_position": use_saved,
            "included_slots": "|".join(map(str, included)),
            "excluded_slots": "|".join(map(str, excluded)),
            "candidate_vectors": vectors,
            "missing_inputs": "|".join(missing),
            "status": "complete" if not missing else "unknown",
            "constants_match": constants_match,
            "modeled_half_x": hex32(modeled.get("half_x")),
            "observed_half_x": hex32(observed_geometry["half_x"]),
            "modeled_half_z": hex32(modeled.get("half_z")),
            "observed_half_z": hex32(observed_geometry["half_z"]),
            "modeled_center_x": hex32(modeled.get("center_x")),
            "observed_center_x": hex32(observed_geometry["center_x"]),
            "modeled_center_y": hex32(modeled.get("center_y")),
            "observed_center_y": hex32(observed_geometry["center_y"]),
            "modeled_center_z": hex32(modeled.get("center_z")),
            "observed_center_z": hex32(observed_geometry["center_z"]),
            "modeled_max_extent": hex32(modeled.get("max_extent")),
            "modeled_distance": hex32(modeled.get("distance")),
            "observed_distance": hex32(observed_key["distance"]),
            "key_event_sequence": sequence(target) if target else "",
            "geometry_bits_match": geometry_match,
            "cache_key_bits_match": key_match,
            "complete_match": geometry_match and key_match and constants_match,
        })
        current_entry = None
        mld_results = {}
    return rows


def runner_rng_write_between(
    events: list[dict[str, Any]], start_sequence: int, end_sequence: int
) -> list[dict[str, Any]]:
    return [
        event
        for event in events
        if start_sequence < sequence(event) < end_sequence
        and checkpoint_id(event) == RNG_WATCH_ID
        and stack_contains_callsite(event, "0x80013920")
    ]


def reduce_timeline(run: dict[str, Any]) -> list[dict[str, Any]]:
    events = run["events"]
    frames = [event for event in events if checkpoint_id(event) == FRAME_ID]
    frame_sequences = [sequence(event) for event in frames]
    rows: list[dict[str, Any]] = []
    previous_runner_publication_sequence = 0
    last_runner_draw_sequence = 0
    for event in events:
        event_id = checkpoint_id(event)
        spec = TIMELINE_IDS.get(event_id)
        if spec is None:
            continue
        source, event_type = spec
        previous_frame, next_frame = nearest_frames(frames, frame_sequences, sequence(event))
        runner_draws_since_previous_publication: int | str = ""
        runner_rng_writes: int | str = ""
        if event_id == "runner_cache_draw_80013920":
            last_runner_draw_sequence = sequence(event)
        elif event_id == "view_cache_publish_from_136DC_800139D8":
            runner_draws_since_previous_publication = (
                1 if last_runner_draw_sequence > previous_runner_publication_sequence else 0
            )
            start = max(previous_runner_publication_sequence, last_runner_draw_sequence - 1)
            runner_rng_writes = len(runner_rng_write_between(events, start, sequence(event)))
            previous_runner_publication_sequence = sequence(event)
        rows.append({
            "run": run["run"],
            "source_exec_job_id": run["job"],
            "capture_sequence": sequence(event),
            "checkpoint_id": event_id,
            "source": source,
            "event_type": event_type,
            "rng_draw_index_before": event.get("rng_draw_index_before"),
            "rng_seed_current": event.get("rng_seed_current"),
            "cache_hit_flag": event.get("cache_hit_flag_r0"),
            "readiness_result": event.get("view_eligibility_result"),
            "rand15_value": event.get("rng_rand15_value"),
            "selected_angle_bits": event.get("selected_angle_bits"),
            "runner_thread_context": event.get("runner_thread_context"),
            "runner_state": event.get("runner_state_0x30"),
            "runner_draws_since_previous_publication": runner_draws_since_previous_publication,
            "runner_rng_seed_writes": runner_rng_writes,
            "previous_frame_sequence": sequence(previous_frame) if previous_frame else "",
            "next_frame_sequence": sequence(next_frame) if next_frame else "",
            **cache_snapshot(event, "cache"),
        })
    return rows


def list_issues(run: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    for event in run["events"]:
        if "thread_list_read_ok" not in event:
            continue
        if bool_value(event.get("thread_list_read_ok")) is not True:
            issues.append(f"{run['run']} seq {sequence(event)} read_ok=false")
        if bool_value(event.get("thread_list_truncated")) is not False:
            issues.append(f"{run['run']} seq {sequence(event)} truncated")
        if bool_value(event.get("thread_list_cycle_detected")) is not False:
            issues.append(f"{run['run']} seq {sequence(event)} cycle")
    frames = [event for event in run["events"] if checkpoint_id(event) == FRAME_ID]
    if len(frames) >= FRAME_CAP:
        issues.append(f"{run['run']} reached frame cap {FRAME_CAP}")
    return issues


def integration_rows(
    writer_rows: list[dict[str, Any]],
    geometry_rows: list[dict[str, Any]],
    timeline_rows: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], bool]:
    writers = Counter(row["semantic_source"] for row in writer_rows)
    geometry = Counter(row["path"] for row in geometry_rows if row["complete_match"])
    runner_publications = [
        row for row in timeline_rows
        if row["checkpoint_id"] == "view_cache_publish_from_136DC_800139D8"
    ]
    runner_initial = [
        row for row in runner_publications
        if row["runner_draws_since_previous_publication"] == 1
        and row["runner_rng_seed_writes"] == 1
    ]
    runner_republications = [
        row for row in runner_publications
        if row["runner_draws_since_previous_publication"] == 0
        and row["runner_rng_seed_writes"] == 0
    ]
    runner_confirmed = bool(runner_initial and runner_republications)
    rows = [
        {
            "semantic_source": "view_placement.reset.active_record",
            "static_contract": "mode-1 SetActiveRecord clears complete 0x17C workspace",
            "runtime_examples": writers["view_placement.reset.active_record"],
            "integration": "coherent zero snapshot mutation",
            "status": "integrated",
        },
        {
            "semantic_source": "view_placement.copy.workspace_snapshot",
            "static_contract": "FUN_8005259C copies complete 0x17C worksheet snapshot",
            "runtime_examples": writers["view_placement.copy.workspace_snapshot"],
            "integration": "complete snapshot publish; incomplete input indeterminate",
            "status": "integrated",
        },
        {
            "semantic_source": "view_placement.publish.placement_function",
            "static_contract": "geometry lookup plus hit/miss publication",
            "runtime_examples": geometry["placement_function"],
            "integration": "geometry-backed explicit resolver",
            "status": "integrated" if geometry["placement_function"] else "awaiting runtime vector",
        },
        {
            "semantic_source": "view_placement.publish.direct_view",
            "static_contract": "readiness-gated geometry lookup plus hit/miss publication",
            "runtime_examples": geometry["direct_view"],
            "integration": "geometry-backed explicit resolver",
            "status": "integrated" if geometry["direct_view"] else "awaiting runtime vector",
        },
        {
            "semantic_source": "view_placement.publish.runner",
            "static_contract": "persistent controller state-0 draw and per-tick publication",
            "runtime_examples": len(runner_publications),
            "integration": "snapshot hook only" if not runner_confirmed else "randomized operation eligible",
            "status": "runtime confirmed" if runner_confirmed else "static-only RNG orchestration",
        },
        {
            "semantic_source": "battle_view_controller.schedule",
            "static_contract": "persistent FUN_800136DC controller state and thread order",
            "runtime_examples": len(runner_publications),
            "integration": "not automatically scheduled",
            "status": "next scheduling boundary",
        },
    ]
    return rows, runner_confirmed


def write_findings(
    output_dir: Path,
    runs: list[dict[str, Any]],
    writers: list[dict[str, Any]],
    geometry: list[dict[str, Any]],
    timeline: list[dict[str, Any]],
    coverage: list[dict[str, Any]],
    list_problem_rows: list[str],
    runner_confirmed: bool,
    acceptance: dict[str, bool],
) -> None:
    writer_counts = Counter(row["semantic_source"] for row in writers)
    complete_geometry = [row for row in geometry if row["status"] == "complete"]
    matched_geometry = [row for row in complete_geometry if row["complete_match"]]
    runner_publications = [
        row for row in timeline
        if row["checkpoint_id"] == "view_cache_publish_from_136DC_800139D8"
    ]
    end_turn_rows = [row for row in coverage if row["terminal_kind"] == "end_turn"]
    next_turn_rows = [
        row for row in coverage if row["terminal_kind"] == "next_turn_inputs"
    ]
    battle_terminal_rows = [
        row for row in coverage if row["terminal_kind"] in {"victory", "defeat"}
    ]
    lines = [
        "# View-Placement Semantic Hook Findings",
        "",
        "## Corpus",
        "",
        *[
            f"- `{run['run']}`: source exec job `{run['job']}`, raw `{run['capture']}`"
            for run in runs
        ],
        "",
        "## Acceptance",
        "",
        f"- Thread-list integrity and frame cap: {'PASS' if acceptance['list_integrity'] else 'FAIL'}",
        f"- Control writers classified and paired: {'PASS' if acceptance['writers_classified'] else 'FAIL'}",
        f"- Complete geometry vectors bit exact: {'PASS' if acceptance['geometry_bit_exact'] else 'FAIL'}",
        f"- Capture reached a recognized turn or battle terminal: {'PASS' if acceptance['terminal_coverage'] else 'FAIL'}",
        f"- Overall evidence pass: {'PASS' if acceptance['overall'] else 'FAIL'}",
        "",
        "## Capture Coverage",
        "",
        "The common guaranteed interval begins at post-input `TurnIsReady`, where capture memory watchpoints are armed. Explicit PC checkpoints remain active through the worker terminal.",
        "",
        *[
            f"- Job `{row['source_exec_job_id']}`: `{row['final_breakpoint_symbol'] or 'unknown'}` at `{row['final_breakpoint_pc'] or 'unknown'}`; manifest outcome `{row['manifest_outcome']}`; {row['case5_rows']} case-5 rows; terminal VI gap {row['terminal_vi_gap'] if row['terminal_vi_gap'] is not None else 'unknown'}; runner draw/publication hits `{row['runner_draw_checkpoint_hits']}/{row['runner_publication_checkpoint_hits']}`."
            for row in coverage
        ],
        "",
        f"- `{len(end_turn_rows)}` runs reached `EndTurn`; these prove the applied turn through that boundary but do not include post-EndTurn cleanup through the next input gate.",
        f"- `{len(next_turn_rows)}` run reached the next `TurnInputs` gate and covers the inter-turn transition.",
        f"- `{len(battle_terminal_rows)}` run reached a victory or defeat terminal.",
        "- Therefore the zero runner checkpoint count excludes `0x80013920` and `0x800139D8` only within these captured intervals, not during the input macro, later turns, or uncaptured post-terminal processing.",
        "",
        "## Writers",
        "",
        f"- Active-record reset: {writer_counts['view_placement.reset.active_record']} writes.",
        f"- Workspace snapshot copy: {writer_counts['view_placement.copy.workspace_snapshot']} writes.",
        f"- Placement-function publication: {writer_counts['view_placement.publish.placement_function']} writes.",
        f"- Runner publication: {writer_counts['view_placement.publish.runner']} writes.",
        f"- Direct-view publication: {writer_counts['view_placement.publish.direct_view']} writes.",
        f"- Unresolved: {writer_counts['unresolved']} writes.",
        "",
        "## Geometry",
        "",
        f"- Complete rows: {len(complete_geometry)}.",
        f"- Complete bit-exact rows: {len(matched_geometry)}.",
        f"- Unknown/incomplete rows: {len(geometry) - len(complete_geometry)}.",
        "- Slot presence is derived from the live static root array. Derived address-program reads are ignored when the corresponding root is null.",
        "",
        "## Runner",
        "",
        f"- Publications captured: {len(runner_publications)}.",
        f"- Initial one-draw plus later zero-draw publication pattern: {'CONFIRMED' if runner_confirmed else 'NOT CONFIRMED'}.",
        "- Randomized runner orchestration is eligible for integration only when that pattern is confirmed naturally.",
        "",
        "## Predictor Boundary",
        "",
        "The cache and geometry APIs remain shadow-driven. Automatic scheduling is still blocked on a persistent battle-view controller model for `FUN_800136DC`; this pass does not merge placement draws into `BattleVisualRngModel` or its mode-0xE camera draw.",
        "",
        "## Open Evidence",
        "",
    ]
    if list_problem_rows:
        lines.extend(f"- {issue}" for issue in list_problem_rows)
    unresolved = [row for row in writers if row["semantic_source"] == "unresolved"]
    lines.extend(
        f"- Unresolved writer `{row['source_pc']}` in `{row['run']}` sequence {row['capture_sequence']}."
        for row in unresolved
    )
    if not runner_confirmed:
        lines.append(
            "- The runner's initial one-draw publication and later zero-draw republication remain unobserved; randomized runner orchestration is not integrated."
        )
    if end_turn_rows:
        lines.append(
            "- Post-EndTurn cleanup through the next `TurnInputs` gate remains uncovered for the EndTurn-terminated runs."
        )
    if not list_problem_rows and not unresolved:
        lines.append(
            "- No unresolved control writer, list-integrity, or complete-geometry defect remains in the reduced corpus."
        )
    lines.extend([
        "",
        "## Outputs",
        "",
        "- `cache_writer_matrix.csv`",
        "- `geometry_vectors.csv`",
        "- `publisher_timeline.csv`",
        "- `integration_matrix.csv`",
        "- `summary.json`",
        "",
    ])
    (output_dir / "findings.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_roots", nargs="+", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    runs = [load_run(path) for path in args.run_roots]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    writer_rows = [row for run in runs for row in reduce_writers(run)]
    geometry_rows = [row for run in runs for row in reduce_geometry(run)]
    timeline_rows = [row for run in runs for row in reduce_timeline(run)]
    coverage_rows = [reduce_capture_coverage(run) for run in runs]
    list_problem_rows = [issue for run in runs for issue in list_issues(run)]
    integration, runner_confirmed = integration_rows(writer_rows, geometry_rows, timeline_rows)

    writers_classified = bool(writer_rows) and all(
        row["semantic_source"] != "unresolved" and row["post_matched"]
        for row in writer_rows
    )
    complete_geometry = [row for row in geometry_rows if row["status"] == "complete"]
    geometry_bit_exact = bool(complete_geometry) and all(
        row["complete_match"] for row in complete_geometry
    )
    terminal_coverage = bool(coverage_rows) and all(
        row["coverage_pass"] for row in coverage_rows
    )
    acceptance = {
        "list_integrity": not list_problem_rows,
        "writers_classified": writers_classified,
        "geometry_bit_exact": geometry_bit_exact,
        "terminal_coverage": terminal_coverage,
    }
    acceptance["overall"] = all(acceptance.values())

    writer_fields = [
        "run", "source_exec_job_id", "capture_sequence", "source_pc", "semantic_source",
        "classification", "expected_post_checkpoint", "post_sequence", "post_matched",
        "rng_draw_index_before", "decoded_effective_addr", "decoded_access_size",
        "decoded_value", "post_write_value", "caller_callsite_pc",
        "stack_has_reset_callsite_80014B68", "source_control", "source_distance",
        "source_center_x", "source_center_y", "source_center_z", "source_angle",
        *[f"before_{field}" for field in CACHE_FIELDS],
        *[f"after_{field}" for field in CACHE_FIELDS],
    ]
    geometry_fields = [
        "run", "source_exec_job_id", "capture_sequence", "entry_sequence", "path",
        "publisher_source", "use_saved_position", "included_slots", "excluded_slots",
        "candidate_vectors", "missing_inputs", "status", "constants_match",
        "modeled_half_x", "observed_half_x", "modeled_half_z", "observed_half_z",
        "modeled_center_x", "observed_center_x", "modeled_center_y", "observed_center_y",
        "modeled_center_z", "observed_center_z", "modeled_max_extent", "modeled_distance",
        "observed_distance", "key_event_sequence", "geometry_bits_match",
        "cache_key_bits_match", "complete_match",
    ]
    timeline_fields = [
        "run", "source_exec_job_id", "capture_sequence", "checkpoint_id", "source",
        "event_type", "rng_draw_index_before", "rng_seed_current", "cache_hit_flag",
        "readiness_result", "rand15_value", "selected_angle_bits", "runner_thread_context",
        "runner_state", "runner_draws_since_previous_publication", "runner_rng_seed_writes",
        "previous_frame_sequence", "next_frame_sequence",
        *[f"cache_{field}" for field in CACHE_FIELDS],
    ]
    write_csv(args.output_dir / "cache_writer_matrix.csv", writer_rows, writer_fields)
    write_csv(args.output_dir / "geometry_vectors.csv", geometry_rows, geometry_fields)
    write_csv(args.output_dir / "publisher_timeline.csv", timeline_rows, timeline_fields)
    write_csv(
        args.output_dir / "integration_matrix.csv",
        integration,
        ["semantic_source", "static_contract", "runtime_examples", "integration", "status"],
    )
    summary = {
        "runs": [
            {"run": run["run"], "source_exec_job_id": run["job"], "capture": str(run["capture"])}
            for run in runs
        ],
        "writer_count": len(writer_rows),
        "geometry_count": len(geometry_rows),
        "timeline_count": len(timeline_rows),
        "list_issues": list_problem_rows,
        "runner_publication_pattern_confirmed": runner_confirmed,
        "capture_coverage": coverage_rows,
        "acceptance": acceptance,
    }
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_findings(
        args.output_dir,
        runs,
        writer_rows,
        geometry_rows,
        timeline_rows,
        coverage_rows,
        list_problem_rows,
        runner_confirmed,
        acceptance,
    )
    print(json.dumps({"output_dir": str(args.output_dir), "acceptance": acceptance}, sort_keys=True))
    return 1 if args.strict and not acceptance["overall"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
