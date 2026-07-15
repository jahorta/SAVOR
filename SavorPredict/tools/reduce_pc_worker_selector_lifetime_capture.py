#!/usr/bin/env python3
"""Reduce PC direct/fallback selector and movement-worker lifetime captures."""

from __future__ import annotations

import argparse
import csv
import json
from bisect import bisect_left
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
RNG_ID = "memwatch.rng_seed_write_803469A8"
FRAME_CAP = 2400

SELECTOR_IDS = {
    "pc_selector_entry_800855AC",
    "pc_selector_reachability_return_80085670",
    "pc_selector_path_shape_return_80085680",
    "pc_selector_distance_loaded_8008569C",
    "pc_selector_direct_branch_800856A8",
    "pc_selector_fallback_branch_800856B0",
    "pc_selector_fallback_write_complete_800856C8",
    "pc_selector_return_80085708",
}

ACTIVE_WORKER_IDS = {
    "pc_fallback_worker_entry_80085CE0": "pc_fallback",
    "pc_direct_worker_entry_80086308": "pc_direct",
    "pc_worker_terminal_entry_80086C48": "pc_terminal",
}

PASSIVE_PREFIXES = (
    "setup_action_",
    "passive_initial_relay_",
    "passive_relay_",
    "passive_dispatch_",
    "passive_ambient_",
    "passive_affected_",
    "passive_pursuit_",
    "passive_special_",
    "passive_status_",
)


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


def boolean(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        if value.lower() == "true":
            return True
        if value.lower() == "false":
            return False
    return None


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id") or "")


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            text = line.strip()
            if not text:
                continue
            try:
                row = json.loads(text)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: row is not an object")
            events.append(row)
    events.sort(key=sequence)
    return events


def locate_manifest(run: Path) -> Path:
    for candidate in (run / "manifest.json", run / "runs" / "manifest.json"):
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"no manifest.json under {run}")


def resolve_capture(manifest_path: Path, raw: Any) -> Path:
    path = Path(str(raw or "capture.jsonl"))
    return path if path.is_absolute() else manifest_path.parent / path


def run_captures(run: Path) -> Iterable[tuple[int, Path, dict[str, Any]]]:
    manifest_path = locate_manifest(run)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    jobs = manifest.get("jobs")
    if isinstance(jobs, list):
        for row in jobs:
            if not isinstance(row, dict):
                continue
            job = integer(row.get("original_exec_job_id"))
            capture = resolve_capture(manifest_path, row.get("stable_capture_path"))
            if job is not None and capture.exists():
                yield job, capture, row
        return
    job = integer(manifest.get("original_exec_job_id"))
    capture = manifest_path.parent / "capture.jsonl"
    if job is not None and capture.exists():
        yield job, capture, manifest


def event_nodes(event: dict[str, Any]) -> list[dict[str, Any]]:
    nodes = event.get("thread_list_nodes")
    if isinstance(nodes, str):
        try:
            nodes = json.loads(nodes)
        except json.JSONDecodeError:
            return []
    if not isinstance(nodes, list):
        return []
    return [node for node in nodes if isinstance(node, dict)]


def frame_context(
    events: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[int]]:
    frames = [event for event in events if checkpoint_id(event) == FRAME_ID]
    return frames, [sequence(frame) for frame in frames]


def frame_bracket(
    frames: list[dict[str, Any]], frame_sequences: list[int], event_sequence: int
) -> tuple[int | None, dict[str, Any] | None, int | None, dict[str, Any] | None]:
    index = bisect_left(frame_sequences, event_sequence)
    previous = frames[index - 1] if index > 0 else None
    following = frames[index] if index < len(frames) else None
    return (
        index - 1 if previous is not None else None,
        previous,
        index if following is not None else None,
        following,
    )


def slot_from_event(event: dict[str, Any]) -> int | None:
    thread = integer(event.get("thread"))
    if thread not in (None, 0):
        for slot in range(12):
            if integer(event.get(f"slot{slot}_movement_thread_ptr")) == thread:
                return slot
    for key in ("actor_slot", "slot", "worker_iw_slot_0x00", "fallback_iw_slot_0x00"):
        value = integer(event.get(key))
        if value is not None and 0 <= value < 12:
            return value
    return None


def frame_slot_node(frame: dict[str, Any], slot: int) -> tuple[int | None, dict[str, Any] | None]:
    thread = integer(frame.get(f"slot{slot}_movement_thread_ptr"))
    if thread in (None, 0):
        return None, None
    for index, node in enumerate(event_nodes(frame)):
        if integer(node.get("node")) == thread:
            return index, node
    return None, None


def selector_rows(events: list[dict[str, Any]], job: int) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for event in events:
        event_id = checkpoint_id(event)
        if event_id == "pc_selector_entry_800855AC":
            if current is not None:
                current["complete"] = False
                rows.append(current)
            actor = integer(event.get("actor_slot"))
            path_nodes: list[str] = []
            if actor is not None:
                for node_index in range(11):
                    x = integer(
                        event.get(
                            f"slot{actor}_movement_path_node{node_index}_x"
                        )
                    )
                    z = integer(
                        event.get(
                            f"slot{actor}_movement_path_node{node_index}_z"
                        )
                    )
                    if x is None or z is None:
                        break
                    if x == 0xFF:
                        path_nodes.append("FF")
                        break
                    path_nodes.append(f"{x}:{z}")
            current = {
                "job": job,
                "selector_sequence": sequence(event),
                "rng_draw_index": integer(event.get("rng_draw_index_before")),
                "actor_slot": actor,
                "target_slot": integer(event.get(f"slot{actor}_queued_target_0x04"))
                if actor is not None
                else None,
                "initial_instr_param": integer(
                    event.get(f"slot{actor}_queued_instr_param_0x06")
                )
                if actor is not None
                else None,
                "selector_path": "|".join(path_nodes),
                "reachability_result": None,
                "path_shape_result": None,
                "distance": None,
                "decision": "unknown",
                "final_instr_param": None,
                "return_value": None,
                "published_callback": None,
                "complete": False,
            }
            continue
        if current is None or event_id not in SELECTOR_IDS:
            continue
        if event_id == "pc_selector_reachability_return_80085670":
            current["target_slot"] = integer(event.get("target_slot"))
            current["reachability_result"] = integer(event.get("result"))
        elif event_id == "pc_selector_path_shape_return_80085680":
            current["path_shape_result"] = integer(event.get("result"))
        elif event_id == "pc_selector_distance_loaded_8008569C":
            current["distance"] = integer(event.get("distance"))
        elif event_id == "pc_selector_direct_branch_800856A8":
            current["decision"] = "direct"
        elif event_id == "pc_selector_fallback_branch_800856B0":
            current["decision"] = "fallback"
        elif event_id == "pc_selector_return_80085708":
            actor = current["actor_slot"]
            current["return_value"] = integer(event.get("result"))
            current["final_instr_param"] = integer(
                event.get(f"slot{actor}_queued_instr_param_0x06")
            )
            current["complete"] = True
            rows.append(current)
            current = None
    if current is not None:
        rows.append(current)

    for row in rows:
        start = row["selector_sequence"]
        actor = row["actor_slot"]
        for event in events:
            if sequence(event) <= start:
                continue
            event_id = checkpoint_id(event)
            if event_id == "pc_selector_entry_800855AC":
                break
            if event_id == "handle_pc_direct_publish_complete_80086F40":
                row["published_callback"] = "0x80086308"
                break
            if event_id == "handle_pc_fallback_publish_complete_80086F70":
                row["published_callback"] = "0x80085CE0"
                break
        reachability = row["reachability_result"]
        shape = row["path_shape_result"]
        distance = row["distance"]
        reasons: list[str] = []
        if reachability == 0:
            reasons.append("reachability_zero")
        if shape not in (None, 0):
            reasons.append("path_shape_nonzero")
        if distance is not None and distance > 4:
            reasons.append("distance_gt_4")
        row["expected_decision"] = "fallback" if reasons else "direct"
        row["fallback_reasons"] = "+".join(reasons)
        row["truth_table_match"] = row["decision"] == row["expected_decision"]
        if row["published_callback"] is None and actor is not None:
            row["published_callback"] = ""
    return rows


def active_lifetime_rows(
    events: list[dict[str, Any]], job: int
) -> list[dict[str, Any]]:
    frames, sequences = frame_context(events)
    rows: list[dict[str, Any]] = []
    for event in events:
        family = ACTIVE_WORKER_IDS.get(checkpoint_id(event))
        if family is None:
            continue
        slot = slot_from_event(event)
        previous_ordinal, previous, next_ordinal, following = frame_bracket(
            frames, sequences, sequence(event)
        )
        rows.append(
            {
                "job": job,
                "capture_sequence": sequence(event),
                "rng_draw_index": integer(event.get("rng_draw_index_before")),
                "family": family,
                "slot": slot,
                "thread_state": integer(event.get("worker_thread_state_0x19")),
                "callback": event.get("worker_thread_callback_0x00", ""),
                "distance": integer(event.get("worker_iw_distance_0x14")),
                "previous_frame_ordinal": previous_ordinal,
                "previous_action_sequence": integer(previous.get("action_sequence_80347335"))
                if previous
                else None,
                "next_frame_ordinal": next_ordinal,
                "next_action_sequence": integer(following.get("action_sequence_80347335"))
                if following
                else None,
            }
        )
    return rows


def passive_rows(events: list[dict[str, Any]], job: int) -> list[dict[str, Any]]:
    frames, sequences = frame_context(events)
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        if not event_id.startswith(PASSIVE_PREFIXES):
            continue
        slot = slot_from_event(event)
        previous_ordinal, previous, next_ordinal, following = frame_bracket(
            frames, sequences, sequence(event)
        )
        rows.append(
            {
                "job": job,
                "capture_sequence": sequence(event),
                "checkpoint_id": event_id,
                "slot": slot,
                "callback": event.get("worker_thread_callback_0x00", ""),
                "thread_state": integer(event.get("worker_thread_state_0x19")),
                "deferred_callback": event.get("deferred_callback", ""),
                "completion_mask": integer(event.get("movement_completion_mask_80347374")),
                "previous_frame_ordinal": previous_ordinal,
                "previous_action_sequence": integer(previous.get("action_sequence_80347335"))
                if previous
                else None,
                "next_frame_ordinal": next_ordinal,
                "next_action_sequence": integer(following.get("action_sequence_80347335"))
                if following
                else None,
            }
        )
    return rows


def frame_rows(events: list[dict[str, Any]], job: int) -> list[dict[str, Any]]:
    frames, _ = frame_context(events)
    rows: list[dict[str, Any]] = []
    for ordinal, frame in enumerate(frames):
        row: dict[str, Any] = {
            "job": job,
            "frame_ordinal": ordinal,
            "capture_sequence": sequence(frame),
            "rng_draw_index": integer(frame.get("rng_draw_index_before")),
            "turn_phase": integer(frame.get("turn_phase_8034733c")),
            "action_sequence": integer(frame.get("action_sequence_80347335")),
            "active_actor": integer(frame.get("active_actor_slot_80347334")),
            "completion_mask": integer(frame.get("movement_completion_mask_80347374")),
            "node_count": integer(frame.get("thread_list_node_count")),
            "read_ok": boolean(frame.get("thread_list_read_ok")),
            "truncated": boolean(frame.get("thread_list_truncated")),
            "cycle_detected": boolean(frame.get("thread_list_cycle_detected")),
        }
        for slot in (0, 1, 4, 5):
            index, node = frame_slot_node(frame, slot)
            row[f"slot{slot}_thread_index"] = index
            row[f"slot{slot}_callback"] = node.get("callback", "") if node else ""
            row[f"slot{slot}_state"] = integer(node.get("state")) if node else None
            row[f"slot{slot}_grid_x"] = integer(frame.get(f"slot{slot}_movement_current_x_0x0c"))
            row[f"slot{slot}_grid_z"] = integer(frame.get(f"slot{slot}_movement_current_z_0x0d"))
            row[f"slot{slot}_distance"] = integer(frame.get(f"slot{slot}_movement_distance_0x14"))
        rows.append(row)
    return rows


def rng_rows(events: list[dict[str, Any]], job: int) -> list[dict[str, Any]]:
    frames, sequences = frame_context(events)
    rows: list[dict[str, Any]] = []
    for event in events:
        if checkpoint_id(event) != RNG_ID:
            continue
        previous_ordinal, previous, next_ordinal, following = frame_bracket(
            frames, sequences, sequence(event)
        )
        rows.append(
            {
                "job": job,
                "capture_sequence": sequence(event),
                "draw_index_before": integer(event.get("rng_draw_index_before")),
                "seed_after": event.get("rng_seed_after", ""),
                "caller_callsite_pc": event.get("caller_callsite_pc", ""),
                "caller_return_pc": event.get("caller_return_pc", ""),
                "previous_frame_ordinal": previous_ordinal,
                "previous_action_sequence": integer(previous.get("action_sequence_80347335"))
                if previous
                else None,
                "next_frame_ordinal": next_ordinal,
                "next_action_sequence": integer(following.get("action_sequence_80347335"))
                if following
                else None,
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def reduce_run(run: Path, output: Path) -> dict[str, Any]:
    selector: list[dict[str, Any]] = []
    active: list[dict[str, Any]] = []
    passive: list[dict[str, Any]] = []
    frames: list[dict[str, Any]] = []
    rng: list[dict[str, Any]] = []
    jobs: list[dict[str, Any]] = []
    for job, capture, manifest in run_captures(run):
        events = load_jsonl(capture)
        job_frames = frame_rows(events, job)
        job_selectors = selector_rows(events, job)
        selector.extend(job_selectors)
        active.extend(active_lifetime_rows(events, job))
        passive.extend(passive_rows(events, job))
        frames.extend(job_frames)
        rng.extend(rng_rows(events, job))
        jobs.append(
            {
                "job": job,
                "fake_attacks": integer(manifest.get("source_fake_attacks_this_turn")),
                "terminal_state": manifest.get("terminal_state"),
                "events": len(events),
                "frames": len(job_frames),
                "frame_cap_reached": len(job_frames) >= FRAME_CAP,
                "bad_list_rows": sum(
                    1
                    for row in job_frames
                    if row["read_ok"] is not True
                    or row["truncated"] is True
                    or row["cycle_detected"] is True
                ),
                "selectors": len(job_selectors),
                "selector_truth_table_matches": sum(
                    1 for row in job_selectors if row["truth_table_match"]
                ),
            }
        )

    write_csv(output / "selector_matrix.csv", selector)
    write_csv(output / "active_worker_lifetime.csv", active)
    write_csv(output / "passive_activation_timeline.csv", passive)
    write_csv(output / "frame_thread_timeline.csv", frames)
    write_csv(output / "rng_source_window.csv", rng)
    summary = {
        "jobs": jobs,
        "selector_rows": len(selector),
        "direct_selectors": sum(1 for row in selector if row["decision"] == "direct"),
        "fallback_selectors": sum(1 for row in selector if row["decision"] == "fallback"),
        "distance_gt_4_fallbacks": sum(
            1 for row in selector if "distance_gt_4" in row["fallback_reasons"]
        ),
        "active_worker_visits": len(active),
        "passive_events": len(passive),
        "frame_rows": len(frames),
        "rng_draws": len(rng),
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    summary = reduce_run(args.run, args.output)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
