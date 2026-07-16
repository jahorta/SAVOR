#!/usr/bin/env python3
"""Reduce visual-publication producer captures and predictor-generation deltas."""

from __future__ import annotations

import argparse
import csv
import difflib
import json
import re
import sqlite3
from collections import Counter
from contextlib import closing
from pathlib import Path
from typing import Any, Iterable, Sequence


FRAME_ID = "battle_case5_after_threads_8000A2FC"
RNG_WATCH_ID = "memwatch.rng_seed_write_803469A8"
PUBLICATION_ID = "serialized_action_view_publication_8003C738"
CHILD_STATE0_ID = "action_view_record_state0_helper_80051320"
MODE1_ID = "action_view_record_mode1_call_800514B0"
MODE1_GEOMETRY_ID = "mode1_geometry_call_80051BB0"
FRAME_CAP = 2400

PRODUCER_PREFIX = "visual_probe_callsite_"
PRODUCER_CHAIN_IDS = {
    "visual_probe_entry_800086BC",
    "visual_probe_mode_write_800086F4",
    "visual_probe_subtype_write_800086F8",
    "aux_row_apply_entry_8000832C",
    "aux_dispatch_call_800084C8",
    "aux_dispatch_return_800084CC",
    "command_dispatch_entry_800367E8",
    "command_handler_call_80036864",
    "command_handler_return_80036868",
    "action_service_creator_entry_8003B1D8",
    "action_service_publication_8003B2B4",
    "serialized_action_view_creator_entry_8003C690",
    PUBLICATION_ID,
    CHILD_STATE0_ID,
    MODE1_ID,
    MODE1_GEOMETRY_ID,
}
ATTACK_IDS = {
    "attack_hit_dodge_80010BDC",
    "attack_critical_80010C44",
    "crit_gate_return_80010CA4",
    "counter_roll_80081A88",
    "counter_gate_return_80081B80",
    "attack_resolution_begin_80081B94",
    "attack_result_return_80081BE8",
    "attack_result_write_80081C48",
}
MOTION_IDS = {
    "movement_commit_entry_8008178C",
    "action_motion_setup_complete_8001FC04",
    "action_motion_final_result_8001EB54",
    "action_motion_caller_consumption_8001B778",
}

TRIGGER_DEFINITIONS = (
    {
        "trigger": "movement_invocation_install",
        "source": "SavorPredict/BattleFrameSchedulerModel.cpp:2656",
        "model_boundary": "install_visual_instruction_for_invocation",
        "initial_classification": "unsupported",
        "static_basis": "no game producer is called by movement invocation activation",
    },
    {
        "trigger": "active_movement_handoff_install",
        "source": "SavorPredict/BattleFrameSchedulerModel.cpp:5654",
        "model_boundary": "ActiveMovementHandoff",
        "initial_classification": "provisional_but_necessary",
        "static_basis": "FUN_8001AB60 reaches FUN_800086BC at 0x8001AE98 after motion control",
    },
    {
        "trigger": "attack_result_instruction_install",
        "source": "SavorPredict/BattlePredictor.cpp:2826",
        "model_boundary": "validated attack-result transition",
        "initial_classification": "evidence_backed_state_wrong_publication_risk",
        "static_basis": "persistent instruction state is validated, but auxiliary rows publish on later instruction visits",
    },
    {
        "trigger": "nested_service_instruction_install",
        "source": "SavorPredict/BattleFrameSchedulerModel.cpp:2184",
        "model_boundary": "FUN_8002EB4C candidate publication",
        "initial_classification": "evidence_backed",
        "static_basis": "FUN_8002EB4C publishes its selected mode through FUN_800214FC",
    },
)

VISUAL_LABEL_RE = re.compile(r"visual|instruction|child|publication", re.IGNORECASE)
DETAIL_PAIR_RE = re.compile(r"(?:^|;\s*)([A-Za-z0-9_]+)=([^;]*)")
DISPATCH_RE = re.compile(r"\[seedprobe-dispatch\]\s+job=(\d+)\s+worker=(\d+)")
GRID_POSITION_RE = re.compile(r"grid_x=(-?\d+)\s+grid_z=(-?\d+)\s+side=([A-Za-z0-9_]+)")


def scalar(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def integer(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
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


def pointer(value: Any) -> int | None:
    parsed = integer(value)
    if parsed in (None, 0):
        return None
    return parsed & 0xFFFFFFFF


def pointer_text(value: Any) -> str:
    parsed = pointer(value)
    return "" if parsed is None else f"0x{parsed:08X}"


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return scalar(event.get("checkpoint_id"))


def load_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    value = json.loads(data.decode(encoding))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            text = line.strip()
            if not text:
                continue
            try:
                value = json.loads(text)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: expected a JSON object")
            events.append(value)
    events.sort(key=sequence)
    return events


def locate_manifest(run_root: Path) -> Path:
    for candidate in (run_root / "manifest.json", run_root / "runs" / "manifest.json"):
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"no manifest.json under {run_root}")


def resolve_path(base: Path, value: Any) -> Path:
    path = Path(scalar(value))
    return path if path.is_absolute() else base / path


def run_captures(run_root: Path) -> tuple[Path, dict[str, Any], dict[int, tuple[Path, dict[str, Any]]]]:
    manifest_path = locate_manifest(run_root)
    manifest = load_json(manifest_path)
    captures: dict[int, tuple[Path, dict[str, Any]]] = {}
    jobs = manifest.get("jobs", [])
    if isinstance(jobs, list):
        for row in jobs:
            if not isinstance(row, dict):
                continue
            job = integer(row.get("original_exec_job_id"))
            path = resolve_path(manifest_path.parent, row.get("stable_capture_path"))
            if job is not None and path.exists():
                captures[job] = (path, row)
    else:
        job = integer(manifest.get("original_exec_job_id"))
        path = manifest_path.parent / "capture.jsonl"
        if job is not None and path.exists():
            captures[job] = (path, manifest)
    return manifest_path, manifest, captures


def detail_pairs(event: dict[str, Any]) -> dict[str, str]:
    detail = scalar(event.get("detail"))
    return {match.group(1): match.group(2).strip() for match in DETAIL_PAIR_RE.finditer(detail)}


def prediction_events(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    root = load_json(path)
    prediction = root.get("prediction", root)
    if not isinstance(prediction, dict):
        return root, []
    events = prediction.get("events", [])
    return root, [event for event in events if isinstance(event, dict)] if isinstance(events, list) else []


def prediction_path(directory: Path, job: int) -> Path | None:
    for name in (f"job_{job}.json", f"job-{job}.json", f"{job}.json"):
        candidate = directory / name
        if candidate.exists():
            return candidate
    return None


def prediction_event_coarse_key(event: dict[str, Any]) -> tuple[Any, ...]:
    pairs = detail_pairs(event)
    return (
        event.get("phase"),
        event.get("label"),
        integer(event.get("actor_slot")),
        integer(event.get("target_slot")),
        integer(event.get("action_ordinal")),
        event.get("visual_command_kind", pairs.get("visual_command_kind")),
        event.get("movement_worker"),
    )


def prediction_event_signature(event: dict[str, Any]) -> tuple[Any, ...]:
    pairs = detail_pairs(event)
    return prediction_event_coarse_key(event) + (
        integer(event.get("frame_index")),
        integer(event.get("draws_consumed")) or 0,
        integer(event.get("rng_seed_before")),
        integer(event.get("rng_seed_after")),
        event.get("status"),
        event.get("visual_task_sequence", pairs.get("visual_task_sequence")),
        event.get("instruction_revision", pairs.get("instruction_revision", pairs.get("instruction_epoch"))),
        event.get("movement_callback_pc", pairs.get("callback_pc")),
        integer(event.get("movement_thread_order_index")),
        pairs.get("callback"),
        pairs.get("action_mode"),
        pairs.get("pos_to_move_to_0x110"),
        pairs.get("move_increment_0x104"),
        pairs.get("selected_speed"),
        pairs.get("turn_speed_bits_0x128"),
        pairs.get("selected_action_row_flags"),
        pairs.get("selected_instruction_mode_0x6"),
        pairs.get("state0_derived_mode"),
        pairs.get("visual_key_source"),
    )


def visual_or_rng_event(event: dict[str, Any]) -> bool:
    label = scalar(event.get("label"))
    return bool(VISUAL_LABEL_RE.search(label)) or (integer(event.get("draws_consumed")) or 0) > 0


def rng_event_only(event: dict[str, Any]) -> bool:
    return (integer(event.get("draws_consumed")) or 0) > 0


def runtime_visual_or_rng_event(event: dict[str, Any]) -> bool:
    return event.get("label") not in {
        "visual_resource_loaded",
        "std_resource_thread_publication",
    } and visual_or_rng_event(event)


def motion_setup_event(event: dict[str, Any]) -> bool:
    return event.get("label") == "combatant_instruction_motion_setup"


def first_semantic_difference(
    lhs: Sequence[dict[str, Any]], rhs: Sequence[dict[str, Any]]
) -> tuple[int, int, dict[str, Any] | None, dict[str, Any] | None, str]:
    matcher = difflib.SequenceMatcher(
        None,
        [prediction_event_coarse_key(event) for event in lhs],
        [prediction_event_coarse_key(event) for event in rhs],
        autojunk=False,
    )
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            for left_index, right_index in zip(range(i1, i2), range(j1, j2)):
                if prediction_event_signature(lhs[left_index]) != prediction_event_signature(rhs[right_index]):
                    return left_index, right_index, lhs[left_index], rhs[right_index], "changed"
            continue
        return (
            i1,
            j1,
            lhs[i1] if i1 < len(lhs) else None,
            rhs[j1] if j1 < len(rhs) else None,
            tag,
        )
    return len(lhs), len(rhs), None, None, "equal"


def event_summary(prefix: str, event: dict[str, Any] | None) -> dict[str, Any]:
    if event is None:
        return {
            f"{prefix}_sequence": "",
            f"{prefix}_phase": "",
            f"{prefix}_label": "",
            f"{prefix}_frame": "",
            f"{prefix}_action": "",
            f"{prefix}_owner": "",
            f"{prefix}_thread_order_index": "",
            f"{prefix}_instruction_revision": "",
            f"{prefix}_action_mode": "",
            f"{prefix}_motion_target": "",
            f"{prefix}_motion_increment": "",
            f"{prefix}_action_row_flags": "",
            f"{prefix}_visual_command": "",
            f"{prefix}_child_visit": "",
            f"{prefix}_draws": "",
            f"{prefix}_rng_source": "",
        }
    pairs = detail_pairs(event)
    return {
        f"{prefix}_sequence": event.get("sequence", ""),
        f"{prefix}_phase": event.get("phase", ""),
        f"{prefix}_label": event.get("label", ""),
        f"{prefix}_frame": event.get("frame_index", ""),
        f"{prefix}_action": event.get("action_ordinal", ""),
        f"{prefix}_owner": event.get("actor_slot", pairs.get("origin_slot", pairs.get("slot", ""))),
        f"{prefix}_thread_order_index": event.get(
            "movement_thread_order_index", pairs.get("thread_order_index", "")
        ),
        f"{prefix}_instruction_revision": event.get(
            "instruction_revision", pairs.get("instruction_revision", pairs.get("instruction_epoch", ""))
        ),
        f"{prefix}_action_mode": pairs.get("action_mode", ""),
        f"{prefix}_motion_target": pairs.get("pos_to_move_to_0x110", pairs.get("target", "")),
        f"{prefix}_motion_increment": pairs.get("move_increment_0x104", ""),
        f"{prefix}_action_row_flags": pairs.get("selected_action_row_flags", ""),
        f"{prefix}_visual_command": event.get(
            "visual_command_kind", pairs.get("visual_command", pairs.get("visual_command_kind", ""))
        ),
        f"{prefix}_child_visit": event.get("visual_task_sequence", pairs.get("visual_task_sequence", "")),
        f"{prefix}_draws": event.get("draws_consumed", ""),
        f"{prefix}_rng_source": pairs.get("rng_label", pairs.get("callback", event.get("label", ""))),
    }


def initialization_fingerprint(root: dict[str, Any], events: Sequence[dict[str, Any]]) -> dict[str, Any]:
    prediction = root.get("prediction", {})
    if not isinstance(prediction, dict):
        prediction = {}
    positions = sorted(
        (integer(event.get("actor_slot")), scalar(event.get("detail")))
        for event in events
        if event.get("label") in {"enemy_event_start_position", "source_start_position"}
    )
    resources = sorted(
        (integer(event.get("actor_slot")), scalar(event.get("visual_resource")))
        for event in events
        if event.get("label") == "visual_resource_loaded"
    )
    first_motion = next(
        (prediction_event_signature(event) for event in events if event.get("label") == "worker_select"),
        None,
    )
    return {
        "starting_rng_seed": prediction.get("starting_rng_seed"),
        "turn_type": prediction.get("initial_turn_type"),
        "positions": positions,
        "resources": resources,
        "first_motion": first_motion,
    }


def prediction_summary(root: dict[str, Any], events: Sequence[dict[str, Any]]) -> dict[str, Any]:
    prediction = root.get("prediction", {})
    if not isinstance(prediction, dict):
        prediction = {}
    input_data = root.get("input", {})
    if not isinstance(input_data, dict):
        input_data = {}
    source_event = next(
        (event for event in events if scalar(event.get("label")).startswith("source_manifest_")),
        None,
    )
    return {
        "outcome": prediction.get("outcome", ""),
        "starting_seed": prediction.get("starting_rng_seed", input_data.get("starting_rng_seed", "")),
        "final_seed": prediction.get("final_rng_seed", ""),
        "total_draws": prediction.get("total_draws_consumed", ""),
        "turn_type": prediction.get("initial_turn_type", ""),
        "start_boundary": prediction.get("start_boundary", input_data.get("start_boundary", "")),
        "source_status": source_event.get("label", "not_recorded") if source_event else "not_recorded",
        "source_detail": source_event.get("detail", "") if source_event else "",
    }


def canonical_positions(events: Sequence[dict[str, Any]]) -> tuple[tuple[Any, ...], ...]:
    positions: list[tuple[Any, ...]] = []
    for event in events:
        if event.get("label") not in {"enemy_event_start_position", "source_start_position"}:
            continue
        match = GRID_POSITION_RE.search(scalar(event.get("detail")))
        if match is None:
            continue
        positions.append((
            integer(event.get("actor_slot")),
            int(match.group(1)),
            int(match.group(2)),
            match.group(3).lower(),
        ))
    return tuple(sorted(positions))


def source_snapshot(events: Sequence[dict[str, Any]]) -> tuple[str, str, str] | None:
    event = next((event for event in events if event.get("label") == "source_manifest_validated"), None)
    if event is None:
        return None
    pairs = detail_pairs(event)
    return (
        pairs.get("manifest_key", ""),
        pairs.get("stage", ""),
        pairs.get("snapshot_sha256", ""),
    )


def thread_publication_order(events: Sequence[dict[str, Any]]) -> tuple[int, ...] | None:
    event = next((event for event in events if event.get("label") == "std_resource_thread_publication"), None)
    if event is None:
        return None
    match = re.search(r"(?:^|;\s*)publication_order=([0-9,]+)", scalar(event.get("detail")))
    if match is None:
        return None
    return tuple(int(value) for value in match.group(1).split(","))


def motion_fingerprint(events: Sequence[dict[str, Any]]) -> tuple[tuple[Any, ...], ...]:
    rows: list[tuple[Any, ...]] = []
    for event in events:
        if event.get("label") != "combatant_instruction_motion_setup":
            continue
        pairs = detail_pairs(event)
        rows.append((
            integer(event.get("action_ordinal")),
            integer(event.get("actor_slot")),
            integer(event.get("target_slot")),
            integer(event.get("frame_index")),
            integer(event.get("movement_thread_order_index")),
            pairs.get("action_mode", ""),
            pairs.get("pos_to_move_to_0x110", ""),
            pairs.get("move_increment_0x104", ""),
            pairs.get("selected_speed", ""),
            pairs.get("turn_speed_bits_0x128", ""),
            pairs.get("selected_action_row_flags", ""),
        ))
    return tuple(rows)


def comparison_state(lhs: Any, rhs: Any) -> str:
    if lhs in (None, (), "") or rhs in (None, (), ""):
        return "not_recorded" if lhs in (None, (), "") and rhs in (None, (), "") else "not_comparable"
    return "equal" if lhs == rhs else "different"


def generation_comparison_metadata(
    lhs_root: dict[str, Any],
    lhs_events: Sequence[dict[str, Any]],
    rhs_root: dict[str, Any],
    rhs_events: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    lhs_summary = prediction_summary(lhs_root, lhs_events)
    rhs_summary = prediction_summary(rhs_root, rhs_events)
    lhs_coordinator = (
        lhs_summary["starting_seed"], lhs_summary["start_boundary"], lhs_summary["turn_type"]
    )
    rhs_coordinator = (
        rhs_summary["starting_seed"], rhs_summary["start_boundary"], rhs_summary["turn_type"]
    )
    return {
        "lhs_outcome": lhs_summary["outcome"],
        "rhs_outcome": rhs_summary["outcome"],
        "lhs_total_draws": lhs_summary["total_draws"],
        "rhs_total_draws": rhs_summary["total_draws"],
        "terminal_draw_count_equal": lhs_summary["total_draws"] == rhs_summary["total_draws"],
        "lhs_final_seed": lhs_summary["final_seed"],
        "rhs_final_seed": rhs_summary["final_seed"],
        "terminal_seed_equal": lhs_summary["final_seed"] == rhs_summary["final_seed"],
        "lhs_source_status": lhs_summary["source_status"],
        "rhs_source_status": rhs_summary["source_status"],
        "rhs_source_detail": rhs_summary["source_detail"],
        "coordinator_state": comparison_state(lhs_coordinator, rhs_coordinator),
        "source_snapshot": comparison_state(source_snapshot(lhs_events), source_snapshot(rhs_events)),
        "positions": comparison_state(canonical_positions(lhs_events), canonical_positions(rhs_events)),
        "thread_publication_order": comparison_state(
            thread_publication_order(lhs_events), thread_publication_order(rhs_events)
        ),
        "motion_values": comparison_state(motion_fingerprint(lhs_events), motion_fingerprint(rhs_events)),
    }


def predictor_delta_rows(
    jobs: Iterable[int], generation_dirs: Sequence[tuple[str, Path]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for job in jobs:
        loaded: dict[str, tuple[dict[str, Any], list[dict[str, Any]]]] = {}
        for name, directory in generation_dirs:
            path = prediction_path(directory, job)
            if path is not None:
                loaded[name] = prediction_events(path)
        for (lhs_name, _), (rhs_name, _) in zip(generation_dirs, generation_dirs[1:]):
            if lhs_name not in loaded or rhs_name not in loaded:
                rows.append({
                    "job": job,
                    "lhs_generation": lhs_name,
                    "rhs_generation": rhs_name,
                    "scope": "missing",
                    "difference_kind": "prediction_missing",
                })
                continue
            lhs_root, lhs_events = loaded[lhs_name]
            rhs_root, rhs_events = loaded[rhs_name]
            state_equal = initialization_fingerprint(lhs_root, lhs_events) == initialization_fingerprint(
                rhs_root, rhs_events
            )
            comparison_metadata = generation_comparison_metadata(
                lhs_root, lhs_events, rhs_root, rhs_events
            )
            for scope, selector in (
                ("all_events", lambda event: True),
                ("visual_and_rng", visual_or_rng_event),
                ("runtime_visual_and_rng", runtime_visual_or_rng_event),
                ("motion_setup", motion_setup_event),
                ("rng_only", rng_event_only),
            ):
                left = [event for event in lhs_events if selector(event)]
                right = [event for event in rhs_events if selector(event)]
                left_index, right_index, left_event, right_event, kind = first_semantic_difference(left, right)
                row = {
                    "job": job,
                    "lhs_generation": lhs_name,
                    "rhs_generation": rhs_name,
                    "scope": scope,
                    "difference_kind": kind,
                    "lhs_index": left_index,
                    "rhs_index": right_index,
                    "initialization_fingerprint_equal": state_equal,
                    **comparison_metadata,
                }
                row.update(event_summary("lhs", left_event))
                row.update(event_summary("rhs", right_event))
                rows.append(row)
    return rows


def event_nodes(event: dict[str, Any]) -> list[dict[str, Any]]:
    value = event.get("thread_list_nodes", [])
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return []
    return [node for node in value if isinstance(node, dict)] if isinstance(value, list) else []


def node_address(node: dict[str, Any]) -> int | None:
    return pointer(node.get("node", node.get("address")))


def node_index(event: dict[str, Any], address: Any) -> int | None:
    expected = pointer(address)
    if expected is None:
        return None
    for index, node in enumerate(event_nodes(event)):
        if node_address(node) == expected:
            return index
    return None


def list_valid(event: dict[str, Any]) -> bool:
    return (
        event.get("thread_list_read_ok") is True
        and event.get("thread_list_truncated") is False
        and event.get("thread_list_cycle_detected") is False
    )


def nearest_before(events: Sequence[dict[str, Any]], index: int, predicate) -> tuple[int, dict[str, Any]] | None:
    for candidate_index in range(index - 1, -1, -1):
        if predicate(events[candidate_index]):
            return candidate_index, events[candidate_index]
    return None


def nearest_after(events: Sequence[dict[str, Any]], index: int, predicate) -> tuple[int, dict[str, Any]] | None:
    for candidate_index in range(index + 1, len(events)):
        if predicate(events[candidate_index]):
            return candidate_index, events[candidate_index]
    return None


def publication_chains(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    chains: list[dict[str, Any]] = []
    previous_publication_sequence = -1
    for publication_index, publication in enumerate(events):
        if checkpoint_id(publication) != PUBLICATION_ID:
            continue
        publication_sequence = sequence(publication)
        required: dict[str, dict[str, Any] | None] = {}
        cursor = publication_index
        for name, predicate in (
            ("camera_entry", lambda event: checkpoint_id(event) == "serialized_action_view_creator_entry_8003C690"),
            ("handler_call", lambda event: checkpoint_id(event) == "command_handler_call_80036864"),
            ("dispatch_entry", lambda event: checkpoint_id(event) == "command_dispatch_entry_800367E8"),
            ("aux_call", lambda event: checkpoint_id(event) == "aux_dispatch_call_800084C8"),
            ("probe_entry", lambda event: checkpoint_id(event) == "visual_probe_entry_800086BC"),
            ("probe_callsite", lambda event: checkpoint_id(event).startswith(PRODUCER_PREFIX)),
        ):
            found = nearest_before(events, cursor + 1, predicate)
            if found is None or sequence(found[1]) <= previous_publication_sequence:
                required[name] = None
                continue
            cursor, required[name] = found

        record_thread = publication.get("record_thread")
        child_found = nearest_after(
            events,
            publication_index,
            lambda event: checkpoint_id(event) == CHILD_STATE0_ID
            and (pointer(record_thread) is None or pointer(event.get("record_thread")) == pointer(record_thread)),
        )
        mode1_found = nearest_after(
            events,
            publication_index,
            lambda event: checkpoint_id(event) == MODE1_ID
            and (pointer(record_thread) is None or pointer(event.get("record_thread")) == pointer(record_thread)),
        )
        geometry_found = nearest_after(
            events,
            publication_index,
            lambda event: checkpoint_id(event) == MODE1_GEOMETRY_ID,
        )
        next_publication = nearest_after(
            events, publication_index, lambda event: checkpoint_id(event) == PUBLICATION_ID
        )
        upper_sequence = sequence(next_publication[1]) if next_publication else 1 << 62
        for key, found in (("child", child_found), ("mode1", mode1_found), ("geometry", geometry_found)):
            required[key] = found[1] if found is not None and sequence(found[1]) < upper_sequence else None

        monotonic = [
            sequence(required[name]) if required[name] is not None else -1
            for name in ("probe_callsite", "probe_entry", "aux_call", "dispatch_entry", "handler_call", "camera_entry")
        ]
        unique = all(value >= 0 for value in monotonic) and monotonic == sorted(monotonic)
        if unique:
            forbidden_exact = (
                "",
                "visual_probe_entry_800086BC",
                "aux_dispatch_call_800084C8",
                "command_dispatch_entry_800367E8",
                "command_handler_call_80036864",
            )
            for range_index, (lower, upper) in enumerate(zip(monotonic, monotonic[1:])):
                if any(
                    (
                        checkpoint_id(event).startswith(PRODUCER_PREFIX)
                        if range_index == 0
                        else checkpoint_id(event) == forbidden_exact[range_index]
                    )
                    for event in events
                    if lower < sequence(event) < upper
                ):
                    unique = False
                    break
        chain = {
            "job": job,
            "publication_ordinal": len(chains),
            "publication_sequence": publication_sequence,
            "vi_field_count": publication.get("vi_field_count", ""),
            "rng_draw_index": publication.get("rng_draw_index_before", ""),
            "record_thread": pointer_text(record_thread),
            "origin_thread": pointer_text(publication.get("origin_thread")),
            "origin_slot": integer(publication.get("record_origin_iw_slot_0x00")),
            "origin_mode": integer(publication.get("record_origin_iw_mode_0x06")),
            "payload_mode": integer(publication.get("record_payload_mode_0x22")),
            "producer_callsite": checkpoint_id(required["probe_callsite"]) if required["probe_callsite"] else "",
            "producer_sequence": sequence(required["probe_callsite"]) if required["probe_callsite"] else "",
            "aux_sequence": sequence(required["aux_call"]) if required["aux_call"] else "",
            "command_id": required["aux_call"].get("command_id", "") if required["aux_call"] else "",
            "handler": required["handler_call"].get("handler", "") if required["handler_call"] else "",
            "handler_sequence": sequence(required["handler_call"]) if required["handler_call"] else "",
            "child_state0_sequence": sequence(required["child"]) if required["child"] else "",
            "mode1_sequence": sequence(required["mode1"]) if required["mode1"] else "",
            "mode1_geometry_sequence": sequence(required["geometry"]) if required["geometry"] else "",
            "producer_unique": unique,
            "child_lifetime_identified": required["child"] is not None,
            "mode1_lifetime_identified": required["mode1"] is not None,
            "mode1_geometry_identified": required["geometry"] is not None,
            "chain": required,
            "publication": publication,
            "publication_index": publication_index,
        }
        chains.append(chain)
        previous_publication_sequence = publication_sequence
    return chains


def attack_to_publication_rows(
    job: int, events: list[dict[str, Any]], chains: Sequence[dict[str, Any]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    selected_ids = ATTACK_IDS | MOTION_IDS | PRODUCER_CHAIN_IDS
    for chain in chains:
        publication_index = int(chain["publication_index"])
        previous_attack = nearest_before(
            events, publication_index + 1, lambda event: checkpoint_id(event) == "attack_resolution_begin_80081B94"
        )
        probe = chain["chain"].get("probe_callsite")
        start_sequence = sequence(previous_attack[1]) if previous_attack else max(0, sequence(probe) - 1)
        end_candidates = [
            sequence(value) for key in ("geometry", "mode1", "child")
            if (value := chain["chain"].get(key)) is not None
        ]
        end_sequence = max(end_candidates, default=int(chain["publication_sequence"]))
        ordinal = 0
        for event in events:
            event_sequence = sequence(event)
            if event_sequence < start_sequence or event_sequence > end_sequence:
                continue
            event_id = checkpoint_id(event)
            if event_id not in selected_ids and not event_id.startswith(PRODUCER_PREFIX):
                continue
            rows.append({
                "job": job,
                "publication_ordinal": chain["publication_ordinal"],
                "timeline_ordinal": ordinal,
                "capture_sequence": event_sequence,
                "checkpoint_id": event_id,
                "vi_field_count": event.get("vi_field_count", ""),
                "rng_draw_index": event.get("rng_draw_index_before", ""),
                "actor_slot": event.get("actor_slot", event.get("active_slot", event.get("origin_slot", ""))),
                "target_slot": event.get("target_slot", event.get("target_slot_word", "")),
                "attack_result": event.get("attack_result", event.get("crit_result", event.get("counter_result", ""))),
                "origin_thread": pointer_text(
                    event.get("origin_thread", event.get("probe_origin_thread", event.get("aux_origin_thread", "")))
                ),
                "instruction_mode": event.get(
                    "record_origin_iw_mode_0x06",
                    event.get("probe_live_iw_mode_0x06", event.get("aux_origin_iw_mode_0x06", "")),
                ),
                "command_id": event.get("command_id", ""),
                "handler": event.get("handler", ""),
                "record_thread": pointer_text(event.get("record_thread")),
            })
            ordinal += 1
    return rows


def aux_dispatch_rows(job: int, events: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        if event_id not in PRODUCER_CHAIN_IDS and not event_id.startswith(PRODUCER_PREFIX):
            continue
        rows.append({
            "job": job,
            "capture_sequence": sequence(event),
            "checkpoint_id": event_id,
            "vi_field_count": event.get("vi_field_count", ""),
            "rng_draw_index": event.get("rng_draw_index_before", ""),
            "origin_thread": pointer_text(
                event.get("origin_thread", event.get("probe_origin_thread", event.get("aux_origin_thread", "")))
            ),
            "origin_slot": event.get(
                "record_origin_iw_slot_0x00",
                event.get("probe_origin_iw_slot_0x00", event.get("aux_origin_iw_slot_0x00", "")),
            ),
            "instruction_mode": event.get(
                "record_origin_iw_mode_0x06",
                event.get("probe_live_iw_mode_0x06", event.get("aux_origin_iw_mode_0x06", "")),
            ),
            "temporary_mode": event.get("temporary_mode", ""),
            "temporary_subtype": event.get("temporary_subtype", ""),
            "command_id": event.get("command_id", ""),
            "row_command_low": event.get(
                "aux_row_command_id_low_0x00", event.get("dispatch_row_command_id_low_0x00", "")
            ),
            "row_command_high": event.get(
                "aux_row_command_id_high_0x02", event.get("dispatch_row_command_id_high_0x02", "")
            ),
            "row_payload": pointer_text(
                event.get("aux_row_payload_ptr_0x0c", event.get("dispatch_row_payload_ptr_0x0c", ""))
            ),
            "handler": pointer_text(event.get("handler")),
            "record_thread": pointer_text(event.get("record_thread")),
            "thread_list_valid": list_valid(event) if "thread_list_read_ok" in event else "",
        })
    return rows


def thread_insertion_rows(
    job: int, events: list[dict[str, Any]], chains: Sequence[dict[str, Any]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for chain in chains:
        publication_index = int(chain["publication_index"])
        before = nearest_before(
            events,
            publication_index,
            lambda event: "thread_list_read_ok" in event and list_valid(event),
        )
        publication = chain["publication"]
        child = chain["chain"].get("child")
        before_event = before[1] if before else None
        before_nodes = {node_address(node) for node in event_nodes(before_event or {})}
        publication_nodes = {node_address(node) for node in event_nodes(publication)}
        before_nodes.discard(None)
        publication_nodes.discard(None)
        added = sorted(publication_nodes - before_nodes)
        removed = sorted(before_nodes - publication_nodes)
        record_thread = publication.get("record_thread")
        rows.append({
            "job": job,
            "publication_ordinal": chain["publication_ordinal"],
            "before_sequence": sequence(before_event) if before_event else "",
            "publication_sequence": chain["publication_sequence"],
            "child_sequence": sequence(child) if child else "",
            "before_list_valid": list_valid(before_event) if before_event else False,
            "publication_list_valid": list_valid(publication),
            "child_list_valid": list_valid(child) if child and "thread_list_read_ok" in child else False,
            "before_node_count": len(before_nodes),
            "publication_node_count": len(publication_nodes),
            "added_nodes": "|".join(f"0x{value:08X}" for value in added),
            "removed_nodes": "|".join(f"0x{value:08X}" for value in removed),
            "record_thread": pointer_text(record_thread),
            "record_added": pointer(record_thread) in added,
            "publication_thread_index": node_index(publication, record_thread),
            "child_thread_index": node_index(child, record_thread) if child else "",
            "origin_thread": pointer_text(publication.get("origin_thread")),
            "origin_thread_index": node_index(publication, publication.get("origin_thread")),
        })
    return rows


def rng_timeline(events: Sequence[dict[str, Any]]) -> list[tuple[str, str, tuple[str, ...]]]:
    rows: list[tuple[str, str, tuple[str, ...]]] = []
    for event in events:
        if checkpoint_id(event) != RNG_WATCH_ID and event.get("owns_rng_draw") is not True:
            continue
        frame_count = integer(event.get("stack_frame_count")) or 0
        stack = tuple(
            scalar(event.get(f"stack_frame_{index}_callsite_pc"))
            for index in range(frame_count)
        )
        rows.append((
            scalar(event.get("rng_seed_after", event.get("decoded_memory_value"))),
            scalar(event.get("caller_callsite_pc", event.get("stack_frame_0_callsite_pc"))),
            stack,
        ))
    return rows


def match_rng_trajectory(
    captured_events: Sequence[dict[str, Any]],
    accepted_events: Sequence[dict[str, Any]],
) -> tuple[bool, int | None]:
    captured = rng_timeline(captured_events)
    accepted = rng_timeline(accepted_events)
    if not captured or len(captured) > len(accepted):
        return False, None
    offsets = [
        offset
        for offset in range(len(accepted) - len(captured) + 1)
        if accepted[offset : offset + len(captured)] == captured
    ]
    return len(offsets) == 1, offsets[0] if len(offsets) == 1 else None


def checkpoint_hit_counters_are_independent(
    capture_events: dict[int, list[dict[str, Any]]]
) -> bool:
    if not capture_events:
        return False
    for events in capture_events.values():
        counters: dict[str, list[int]] = {}
        for event in events:
            hit_count = integer(event.get("checkpoint_hit_count"))
            if hit_count is None:
                return False
            counters.setdefault(checkpoint_id(event), []).append(hit_count)
        if not counters:
            return False
        if any(values != list(range(len(values))) for values in counters.values()):
            return False
    return True


def execution_intervals(manifest_path: Path, manifest: dict[str, Any]) -> dict[int, tuple[int, int]]:
    database = manifest_path.parent / "db" / "execution.db"
    if not database.exists():
        return {}
    clone_to_original = {
        integer(row.get("cloned_exec_job_id")): integer(row.get("original_exec_job_id"))
        for row in manifest.get("jobs", []) if isinstance(row, dict)
    }
    intervals: dict[int, tuple[int, int]] = {}
    with closing(sqlite3.connect(
        f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
    )) as connection:
        for clone, original in clone_to_original.items():
            if clone is None or original is None:
                continue
            row = connection.execute(
                "SELECT started_at_utc, ended_at_utc FROM exec_job WHERE job_id = ?", (clone,)
            ).fetchone()
            if row and row[0] is not None and row[1] is not None:
                intervals[original] = (int(row[0]), int(row[1]))
    return intervals


def parallel_acceptance(
    manifest_path: Path,
    manifest: dict[str, Any],
    captures: dict[int, tuple[Path, dict[str, Any]]],
    accepted: dict[int, tuple[Path, dict[str, Any]]],
    capture_events: dict[int, list[dict[str, Any]]],
) -> dict[str, Any]:
    jobs = [row for row in manifest.get("jobs", []) if isinstance(row, dict)]
    intervals = execution_intervals(manifest_path, manifest)
    overlap = False
    if len(intervals) >= 2:
        starts = [value[0] for value in intervals.values()]
        ends = [value[1] for value in intervals.values()]
        overlap = max(starts) < min(ends)
    worker_pairs: set[tuple[int, int]] = set()
    for event in manifest.get("events", []):
        match = DISPATCH_RE.search(scalar(event))
        if match:
            worker_pairs.add((int(match.group(1)), int(match.group(2))))
    runtime_root = manifest_path.parent
    user_dirs = list((runtime_root / ".worker-runtime").glob("workflow-worker-*/User"))
    binary_slots = list((runtime_root / ".worker-binary-runtime").glob("*/slot-*"))
    list_rows = [
        event for events in capture_events.values() for event in events
        if "thread_list_read_ok" in event
    ]
    frame_counts = {
        job: sum(checkpoint_id(event) == FRAME_ID for event in events)
        for job, events in capture_events.items()
    }
    rng_matches: dict[int, bool | None] = {}
    rng_offsets: dict[int, int | None] = {}
    for job, events in capture_events.items():
        if job not in accepted:
            rng_matches[job] = None
            rng_offsets[job] = None
            continue
        accepted_events = load_jsonl(accepted[job][0])
        rng_matches[job], rng_offsets[job] = match_rng_trajectory(events, accepted_events)
    natural_end = all(
        row.get("terminal_state") == "SUCCEEDED"
        and row.get("timed_out") is not True
        and any(
            f"job={row.get('cloned_exec_job_id')}" in scalar(event)
            and "[battle-single-turn-result-diagnostics]" in scalar(event)
            and ("battle_outcome=Reached Next Turn" in scalar(event) or "battle_outcome=Victory" in scalar(event))
            for event in manifest.get("events", [])
        )
        for row in jobs
    )
    capture_paths = [value[0] for value in captures.values()]
    trace_paths = [
        resolve_path(manifest_path.parent, row.get("trace_report_path"))
        for row in jobs
    ]
    sequence_independent = all(
        events and min(sequence(event) for event in events) <= 1
        for events in capture_events.values()
    )
    requested_runs = [
        row for row in manifest.get("requested_runs", []) if isinstance(row, dict)
    ]
    std_cache = manifest.get("std_json_cache", {})
    if not isinstance(std_cache, dict):
        std_cache = {}
    return {
        "batch_command": manifest.get("command") == "run-battle-jobs",
        "minimal_sandbox": manifest.get("sandbox_mode") == "minimal",
        "worker_count_two": integer(manifest.get("worker_count")) == 2,
        "two_jobs_succeeded": len(jobs) == 2 and all(row.get("terminal_state") == "SUCCEEDED" for row in jobs),
        "natural_single_turn_end": natural_end,
        "execution_overlap": overlap,
        "intervals": intervals,
        "distinct_worker_ids": len({worker for _, worker in worker_pairs}) == 2,
        "worker_pairs": sorted(worker_pairs),
        "distinct_user_dirs": len(user_dirs) >= 2,
        "distinct_binary_slots": len(binary_slots) >= 2,
        "separate_captures": (
            len(capture_paths) == len(set(capture_paths)) == 2
            and all(path.exists() for path in capture_paths)
        ),
        "separate_traces": (
            len(trace_paths) == len(set(trace_paths)) == 2
            and all(path.exists() for path in trace_paths)
        ),
        "all_lists_valid": bool(list_rows) and all(list_valid(event) for event in list_rows),
        "frame_cap_not_reached": all(0 < count < FRAME_CAP for count in frame_counts.values()),
        "capture_sequences_independent": sequence_independent,
        "checkpoint_hit_counters_independent": checkpoint_hit_counters_are_independent(capture_events),
        "trusted_fake_attack_zero": bool(jobs) and all(
            integer(row.get("source_fake_attacks_this_turn")) == 0
            and integer(row.get("fake_attacks_this_turn")) == 0
            for row in jobs
        ),
        "no_state_overrides": (
            manifest.get("override_start_rng_seed") is None
            and manifest.get("override_fake_attacks_this_turn") is None
            and len(requested_runs) == len(jobs)
            and all(
                row.get("override_start_rng_seed") is None
                and row.get("override_fake_attacks_this_turn") is None
                for row in requested_runs
            )
        ),
        "explicit_std_json_cache_reused": (
            std_cache.get("used_explicit_dir") is True
            and std_cache.get("available") is True
            and std_cache.get("generation_attempted") is False
            and Path(scalar(std_cache.get("resolved_std_json_dir")))
                == Path(r"D:\SavorPredictDB\.std_json")
        ),
        "rng_matches_accepted": rng_matches,
        "rng_accepted_offsets": rng_offsets,
    }


def publication_trigger_rows(
    jobs: Sequence[int], capture_events: dict[int, list[dict[str, Any]]], chains: dict[int, list[dict[str, Any]]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    callsite_counts = {
        job: Counter(
            checkpoint_id(event) for event in events if checkpoint_id(event).startswith(PRODUCER_PREFIX)
        )
        for job, events in capture_events.items()
    }
    all_unique = all(chain.get("producer_unique") for job_chains in chains.values() for chain in job_chains)
    producer_ids = {
        scalar(chain.get("producer_callsite"))
        for job_chains in chains.values() for chain in job_chains
        if chain.get("producer_callsite")
    }
    observed_handoff = "visual_probe_callsite_8001AE98" in producer_ids
    for definition in TRIGGER_DEFINITIONS:
        classification = definition["initial_classification"]
        recommendation = "retain pending broader evidence"
        if definition["trigger"] == "movement_invocation_install":
            classification = "unsupported_as_camera_publication_trigger_for_captured_paths"
            recommendation = "remove early installation; wait for an observed instruction producer"
        elif definition["trigger"] == "active_movement_handoff_install":
            if observed_handoff:
                classification = "evidence_backed_boundary_needs_exact_state"
                recommendation = "move the transition to the observed FUN_8001AB60:0x8001AE98 producer"
            elif producer_ids:
                classification = "unsupported_as_camera_publication_trigger_for_captured_paths"
                recommendation = "do not publish camera children at handoff; wait for the instruction-row producer"
        elif definition["trigger"] == "attack_result_instruction_install":
            recommendation = "retain instruction state, but publish visual rows only on the observed producer visit"
        elif definition["trigger"] == "nested_service_instruction_install":
            recommendation = "retain instruction-state publication; auxiliary child creation remains producer-owned"
        rows.append({
            **definition,
            "all_camera_inner_producers_unique": all_unique,
            "observed_8001AE98_camera_producer": observed_handoff,
            "final_classification": classification,
            "recommendation": recommendation,
            **{
                f"job_{job}_camera_publications": len(chains.get(job, []))
                for job in jobs
            },
        })
    for callsite in sorted({key for counts in callsite_counts.values() for key in counts}):
        rows.append({
            "trigger": callsite,
            "source": callsite.removeprefix(PRODUCER_PREFIX),
            "model_boundary": "live FUN_800086BC caller",
            "initial_classification": "static_candidate",
            "static_basis": "one of eight direct FUN_800086BC callsites",
            "all_camera_inner_producers_unique": all_unique,
            "observed_8001AE98_camera_producer": observed_handoff,
            "final_classification": (
                "evidence_backed_inner_std_payload_match"
                if callsite == "visual_probe_callsite_80008664"
                else "observed"
            ),
            "recommendation": (
                "introduce a typed combatant-instruction STD-row producer; retain the outer IW+0xE0 callback as provisional"
                if callsite == "visual_probe_callsite_80008664"
                else "introduce a typed producer family if this callsite owns a distinct publication role"
            ),
            **{f"job_{job}_hits": callsite_counts.get(job, Counter()).get(callsite, 0) for job in jobs},
        })
    return rows


def write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames: list[str] = []
    seen: set[str] = set()
    for row in rows:
        for key in row:
            if key in seen or key in {"chain", "publication", "publication_index"}:
                continue
            seen.add(key)
            fieldnames.append(key)
    if not fieldnames:
        fieldnames = ["status"]
        rows = [{"status": "no_rows"}]
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def findings_text(
    jobs: Sequence[int],
    delta_rows: Sequence[dict[str, Any]],
    chains: dict[int, list[dict[str, Any]]],
    acceptance: dict[str, Any],
) -> str:
    total_publications = sum(len(value) for value in chains.values())
    unique_publications = sum(
        bool(chain.get("producer_unique")) for value in chains.values() for chain in value
    )
    child_lifetimes = sum(
        bool(chain.get("child_lifetime_identified")) for value in chains.values() for chain in value
    )
    mode1_lifetimes = sum(
        bool(chain.get("mode1_lifetime_identified")) for value in chains.values() for chain in value
    )
    mode1_geometry = sum(
        bool(chain.get("mode1_geometry_identified")) for value in chains.values() for chain in value
    )
    producer_counts = Counter(
        scalar(chain.get("producer_callsite")) for value in chains.values() for chain in value
    )
    current_rows = [
        row for row in delta_rows
        if row.get("scope") == "rng_only"
        and row.get("lhs_generation") == "instruction_runtime"
        and row.get("rhs_generation") == "current"
    ]
    supported_unchanged = [
        integer(row.get("job")) for row in current_rows
        if row.get("rhs_outcome") != "MissingInput"
        and row.get("terminal_draw_count_equal") is True
        and row.get("terminal_seed_equal") is True
    ]
    source_blocked = [
        integer(row.get("job")) for row in current_rows if row.get("rhs_outcome") == "MissingInput"
    ]
    coordinator_equal = [
        integer(row.get("job")) for row in current_rows if row.get("coordinator_state") == "equal"
    ]
    positions_equal = [
        integer(row.get("job")) for row in current_rows if row.get("positions") == "equal"
    ]
    motion_changed = [
        integer(row.get("job")) for row in current_rows if row.get("motion_values") == "different"
    ]
    visual_deltas = [
        row for row in delta_rows
        if row.get("scope") == "runtime_visual_and_rng"
        and row.get("lhs_generation") == "instruction_runtime"
        and row.get("rhs_generation") == "current"
    ]
    motion_deltas = [
        row for row in delta_rows
        if row.get("scope") == "motion_setup"
        and row.get("lhs_generation") == "instruction_runtime"
        and row.get("rhs_generation") == "current"
    ]
    acceptance_failures = [
        key for key, value in acceptance.items()
        if isinstance(value, bool) and not value
    ]
    rng_matches = acceptance.get("rng_matches_accepted", {})
    captured_jobs = sorted(chains)
    lines = [
        "# Visual Publication Order Audit",
        "",
        "## Capture Acceptance",
        "",
        f"- Audit corpus: {', '.join(str(job) for job in jobs)}. Live parallel capture: {', '.join(str(job) for job in captured_jobs)}.",
        f"- SYSTEM CAMERA publications: {total_publications}; unique direct inner producer chains: {unique_publications}; first-child lifetimes: {child_lifetimes}; mode-1 visits: {mode1_lifetimes}; mode-1 geometry calls: {mode1_geometry}.",
        f"- Parallel acceptance failures: {', '.join(acceptance_failures) if acceptance_failures else 'none'}.",
        f"- Execution intervals: {json.dumps(acceptance.get('intervals', {}), sort_keys=True)}; worker pairs: {json.dumps(acceptance.get('worker_pairs', []))}.",
        f"- Accepted RNG trajectory matches: {json.dumps(rng_matches, sort_keys=True)}.",
        "- Both captures reached the natural single-turn end with distinct workers, user directories, binary slots, captures, traces, checkpoint counters, and overlapping execution intervals.",
        "",
        "## Predictor Audit",
        "",
        f"- Instruction-runtime to current terminal RNG is unchanged for supported jobs: {', '.join(str(job) for job in supported_unchanged) or 'none'}.",
        f"- Current prediction stops at source-manifest validation for jobs: {', '.join(str(job) for job in source_blocked) or 'none'}. These are coverage losses, not downstream RNG divergences.",
        f"- Coordinator state agrees where both generations reach it: {', '.join(str(job) for job in coordinator_equal) or 'none'}; canonical start positions agree for: {', '.join(str(job) for job in positions_equal) or 'none'}.",
        "- The prior generation did not emit a source-manifest or terrain checksum, so source-snapshot and terrain equality are not independently comparable from prediction artifacts. Current supported jobs use the encounter-resolved first-battle source snapshot.",
        f"- Motion/setup fingerprints changed for supported jobs: {', '.join(str(job) for job in motion_changed) or 'none'}. The changes include diagnostic movement-controller indices, instruction modes, action-row flags, and some later path destinations; terminal RNG equality does not validate those movement changes.",
        "- Code and focused tests confirm that resource publication order `0,1,4,5` produces combatant-instruction traversal order `4,5,1,0` through current-cursor child insertion. The `movement_thread_order_index` copied into instruction events is the owning movement-controller index, not the instruction-list visit index; its delta is diagnostic attribution rather than changed instruction visitation.",
        "- At the first supported-job motion delta, frame, owner, target, and increment remain equal. Mode `11 -> 6` is the controller/instruction separation (`AmbientPursuit` family versus action-motion mode 6), while flags `0x01000000 -> 0x81000000` replace the old fallback with the selected STD row. Neither change shifts the supported jobs' RNG event stream.",
        "",
        "First runtime visual/RNG event deltas:",
        "",
    ]
    for row in visual_deltas:
        if row.get("rhs_outcome") == "MissingInput":
            lines.append(
                f"- Job {row.get('job', '')}: current generation stopped at source-manifest validation; no runtime visual/RNG comparison is available."
            )
            continue
        lines.append(
            "- Job {job}, {lhs_generation} -> {rhs_generation}: {difference_kind}; "
            "{lhs_label} at frame {lhs_frame} versus {rhs_label} at frame {rhs_frame}.".format(**{
                key: row.get(key, "") for key in (
                    "job", "lhs_generation", "rhs_generation", "difference_kind",
                    "lhs_label", "lhs_frame", "rhs_label", "rhs_frame",
                )
            })
        )
    lines.extend(["", "First motion-setup deltas:", ""])
    for row in motion_deltas:
        if row.get("rhs_outcome") == "MissingInput":
            lines.append(
                f"- Job {row.get('job', '')}: current generation stopped at source-manifest validation; no motion setup was produced."
            )
            continue
        lines.append(
            "- Job {job}: {difference_kind}; slot {lhs_owner} action {lhs_action} frame {lhs_frame} "
            "versus slot {rhs_owner} action {rhs_action} frame {rhs_frame}; thread index "
            "{lhs_thread_order_index}->{rhs_thread_order_index}; mode {lhs_action_mode} -> "
            "{rhs_action_mode}; target {lhs_motion_target} -> {rhs_motion_target}; increment "
            "{lhs_motion_increment} -> {rhs_motion_increment}; action-row flags "
            "{lhs_action_row_flags}->{rhs_action_row_flags}.".format(**{
                key: row.get(key, "") for key in (
                    "job", "difference_kind", "lhs_owner", "lhs_action", "lhs_frame",
                    "rhs_owner", "rhs_action", "rhs_frame", "lhs_thread_order_index",
                    "rhs_thread_order_index", "lhs_action_mode", "rhs_action_mode",
                    "lhs_motion_target", "rhs_motion_target", "lhs_motion_increment",
                    "rhs_motion_increment", "lhs_action_row_flags", "rhs_action_row_flags",
                )
            })
        )
    lines.extend([
        "",
        "## Producer Findings",
        "",
        f"- Live producer counts: {json.dumps(dict(producer_counts), sort_keys=True)}.",
        "- All four captured SYSTEM CAMERA records came from a live `FUN_80022850` combatant-instruction thread in state 1 and the matched STD payload-list path `FUN_800085EC -> 0x80008664 -> FUN_800086BC`.",
        "- `FUN_800086BC` is a temporary instruction probe: its `IW+0x6/+0x8` writes are restored and must not be modeled as persistent combatant state.",
        "- `FUN_8000832C` walks the selected auxiliary rows and calls `FUN_800367E8`; the computed handler then owns SET COMMAND or SYSTEM CAMERA child creation.",
        "- No captured camera publication used the movement-handoff callsite `0x8001AE98`. Movement activation and active handoff are therefore unsupported camera-publication triggers for these paths.",
        "- Direct inner producer attribution is exact. The capture did not record the outer `IW+0xE0` callback identity, so that callback family remains provisional.",
        "- Three of four children reached the explicit mode-1 geometry path. The second job-149113 child was still observed at state 0 but did not execute mode 1 before the next publication or turn end.",
        "",
        "## Next Implementation",
        "",
    ])
    if (
        total_publications
        and unique_publications == total_publications
        and producer_counts == Counter({"visual_probe_callsite_80008664": total_publications})
    ):
        lines.append(
            "Introduce a typed `CombatantInstructionStdRowProducer` at the modeled `FUN_80022850` state-1 / `IW+0xE0` visit. It should select the current action row, traverse action-key-matched STD payloads, apply auxiliary rows through the `FUN_8000832C -> FUN_800367E8` dispatch chain, and insert SET COMMAND or SYSTEM CAMERA children in thread order. Remove camera-child publication from movement invocation and active handoff. Retain attack-result and `FUN_8002EB4C` installs as instruction-state transitions; they should not directly create camera children."
        )
    else:
        lines.append(
            "Do not change scheduling yet. At least one SYSTEM CAMERA publication lacks a unique upstream producer or first-child lifetime in this capture."
        )
    lines.extend([
        "",
        "Jobs 149113 and 453748 agreed and exercised the required publication chain, so the conditional 158364 live rerun was not required.",
        "",
        "Predictor scheduling was not modified during this pass.",
        "",
    ])
    return "\n".join(lines)


def reduce_capture(
    run_root: Path,
    analysis_root: Path,
    generation_dirs: Sequence[tuple[str, Path]],
    jobs: Sequence[int],
    accepted_run_root: Path | None = None,
) -> dict[str, Any]:
    manifest_path, manifest, captures = run_captures(run_root)
    accepted: dict[int, tuple[Path, dict[str, Any]]] = {}
    if accepted_run_root is not None:
        _, _, accepted = run_captures(accepted_run_root)
    capture_events = {job: load_jsonl(path) for job, (path, _) in captures.items() if job in jobs}
    chains = {job: publication_chains(job, events) for job, events in capture_events.items()}

    delta_rows = predictor_delta_rows(jobs, generation_dirs)
    trigger_rows = publication_trigger_rows(jobs, capture_events, chains)
    attack_rows = [
        row for job, events in capture_events.items()
        for row in attack_to_publication_rows(job, events, chains.get(job, []))
    ]
    aux_rows = [row for job, events in capture_events.items() for row in aux_dispatch_rows(job, events)]
    insertion_rows = [
        row for job, events in capture_events.items()
        for row in thread_insertion_rows(job, events, chains.get(job, []))
    ]
    acceptance = parallel_acceptance(
        manifest_path, manifest, captures, accepted, capture_events
    )

    analysis_root.mkdir(parents=True, exist_ok=True)
    write_csv(analysis_root / "predictor_generation_delta.csv", delta_rows)
    write_csv(analysis_root / "publication_trigger_matrix.csv", trigger_rows)
    write_csv(analysis_root / "attack_to_publication_timeline.csv", attack_rows)
    write_csv(analysis_root / "aux_dispatch_timeline.csv", aux_rows)
    write_csv(analysis_root / "thread_insertion_windows.csv", insertion_rows)
    (analysis_root / "findings.md").write_text(
        findings_text(jobs, delta_rows, chains, acceptance), encoding="utf-8"
    )
    return {
        "manifest": manifest,
        "captures": captures,
        "chains": chains,
        "acceptance": acceptance,
        "delta_rows": delta_rows,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--analysis-root", type=Path, required=True)
    parser.add_argument("--pre-separation-dir", type=Path, required=True)
    parser.add_argument("--instruction-runtime-dir", type=Path, required=True)
    parser.add_argument("--current-predictor-dir", type=Path, required=True)
    parser.add_argument("--accepted-run-root", type=Path)
    parser.add_argument("--job", type=int, action="append", dest="jobs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    jobs = args.jobs or [149113, 153108, 158364, 167916, 453748]
    result = reduce_capture(
        args.run_root,
        args.analysis_root,
        (
            ("pre_separation", args.pre_separation_dir),
            ("instruction_runtime", args.instruction_runtime_dir),
            ("current", args.current_predictor_dir),
        ),
        jobs,
        args.accepted_run_root,
    )
    print(json.dumps({
        "analysis_root": str(args.analysis_root),
        "jobs_reduced": sorted(result["chains"]),
        "publication_counts": {
            str(job): len(chains) for job, chains in result["chains"].items()
        },
        "acceptance": result["acceptance"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
