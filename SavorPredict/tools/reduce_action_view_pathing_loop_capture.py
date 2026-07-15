#!/usr/bin/env python3
"""Reduce a focused FUN_8005174C/FUN_80011694 pathing-loop capture."""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


ACTOR_CALL = "pathing_actor_scan_call_800518A8"
TARGET_CALL = "pathing_target_scan_call_800518C4"
CANDIDATE_CALL = "pathing_candidate_geometry_call_80011724"
CANDIDATE_RETURN = "pathing_candidate_geometry_return_80011728"
FALLBACK = "pathing_scan_zero_score_fallback_80011794"
NONZERO = "pathing_scan_nonzero_return_800117C4"
OUTER_ENTRY = "pathing_outer_loop_entry_800526EC"
OUTER_RETURN = "pathing_outer_loop_return_800519F0"


def int_value(value: Any, default: int = 0) -> int:
    if value is None or value == "":
        return default
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def f32(value: float) -> float:
    return struct.unpack(">f", struct.pack(">f", value))[0]


def float_bits(value: Any) -> float:
    return struct.unpack(">f", int_value(value).to_bytes(4, "big"))[0]


def vector(event: dict[str, Any], prefix: str) -> tuple[float, float, float]:
    return (
        float_bits(event[f"{prefix}_x"]),
        float_bits(event[f"{prefix}_y"]),
        float_bits(event[f"{prefix}_z"]),
    )


def raw_angle_degrees_xz(
    start: tuple[float, float, float], end: tuple[float, float, float]
) -> float:
    degrees = math.degrees(math.atan2(end[2] - start[2], end[0] - start[0]))
    return degrees + 360.0 if degrees < 0.0 else degrees


@dataclass(frozen=True)
class GeometryResult:
    accepted: bool
    perpendicular_distance: float
    distance_base_to_candidate: float
    distance_base_to_input: float
    raw_angle_diff_degrees: float


def score_geometry(
    input_reference: tuple[float, float, float],
    candidate: tuple[float, float, float],
    path_base: tuple[float, float, float],
) -> GeometryResult:
    base = (path_base[0], input_reference[1], path_base[2])
    direction = tuple(input_reference[i] - base[i] for i in range(3))
    candidate_delta = tuple(candidate[i] - base[i] for i in range(3))
    direction_len_sq = sum(component * component for component in direction)
    if direction_len_sq > 0.0:
        scale = sum(direction[i] * candidate_delta[i] for i in range(3)) / direction_len_sq
        projection = tuple(base[i] + direction[i] * scale for i in range(3))
    else:
        projection = base
    perpendicular_sq = sum((candidate[i] - projection[i]) ** 2 for i in range(3))
    perpendicular = 0.0 if perpendicular_sq < 0.025 else math.sqrt(max(0.0, perpendicular_sq))
    distance_candidate = math.hypot(base[0] - candidate[0], base[2] - candidate[2])
    distance_input = math.hypot(base[0] - input_reference[0], base[2] - input_reference[2])
    candidate_angle = raw_angle_degrees_xz(candidate, base)
    input_angle = raw_angle_degrees_xz(input_reference, base)
    angle_diff = abs(candidate_angle - input_angle)
    return GeometryResult(
        accepted=distance_candidate <= distance_input and angle_diff < 45.0,
        perpendicular_distance=perpendicular,
        distance_base_to_candidate=distance_candidate,
        distance_base_to_input=distance_input,
        raw_angle_diff_degrees=angle_diff,
    )


def load_events(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
    events.sort(key=lambda event: int_value(event.get("capture_sequence")))
    return events


def reduce_events(
    events: Iterable[dict[str, Any]], source_exec_job_id: int | None = None
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any]]:
    scan_rows: list[dict[str, Any]] = []
    candidate_rows: list[dict[str, Any]] = []
    in_outer_loop = False
    loop_index = -1
    current_scan: dict[str, Any] | None = None
    current_candidate: dict[str, Any] | None = None
    outer_entries = 0
    outer_returns = 0

    for event in events:
        checkpoint_id = event.get("checkpoint_id", "")
        if checkpoint_id == OUTER_ENTRY:
            outer_entries += 1
            loop_index += 1
            in_outer_loop = True
            current_scan = None
            current_candidate = None
            continue
        if checkpoint_id == OUTER_RETURN:
            outer_returns += 1
            in_outer_loop = False
            current_scan = None
            current_candidate = None
            continue
        if not in_outer_loop:
            continue

        if checkpoint_id in (ACTOR_CALL, TARGET_CALL):
            if current_scan is not None:
                raise ValueError("new outer scan call arrived before the prior scan outcome")
            side = "actor" if checkpoint_id == ACTOR_CALL else "target"
            current_scan = {
                "source_exec_job_id": source_exec_job_id if source_exec_job_id is not None else "",
                "loop_index": loop_index,
                "yaw_iteration": int_value(event.get("yaw_iteration")),
                "side": side,
                "call_sequence": int_value(event.get("capture_sequence")),
                "outcome_sequence": "",
                "actor_slot": int_value(event.get("actor_slot"), -1),
                "target_slot": int_value(event.get("target_slot"), -1),
                "excluded_slot": int_value(event.get("excluded_slot"), -1),
                "rng_draw_index_before": int_value(event.get("rng_draw_index_before")),
                "input_x_bits": event.get("outer_input_x", ""),
                "input_y_bits": event.get("outer_input_y", ""),
                "input_z_bits": event.get("outer_input_z", ""),
                "path_x_bits": event.get("outer_path_x", ""),
                "path_y_bits": event.get("outer_path_y", ""),
                "path_z_bits": event.get("outer_path_z", ""),
                "yaw_bits": event.get("turn_yaw_0xd8", ""),
                "candidate_calls": 0,
                "accepted_candidates": 0,
                "modeled_accepted_candidates": 0,
                "accepted_slots": [],
                "modeled_aggregate": 0.0,
                "fallback_draw": False,
                "modeled_fallback_draw": False,
            }
            continue

        if checkpoint_id == CANDIDATE_CALL:
            if current_scan is None:
                raise ValueError("candidate geometry call arrived without an outer scan")
            current_candidate = {
                "event": event,
                "scan": current_scan,
                "geometry": score_geometry(
                    vector(event, "scan_input"),
                    vector(event, "candidate_position"),
                    vector(event, "scan_path"),
                ),
            }
            current_scan["candidate_calls"] += 1
            continue

        if checkpoint_id == CANDIDATE_RETURN:
            if current_candidate is None or current_scan is None:
                raise ValueError("candidate geometry return arrived without a call")
            call = current_candidate["event"]
            geometry: GeometryResult = current_candidate["geometry"]
            live_accepted = int_value(event.get("accepted_return")) != 0
            slot = int_value(call.get("candidate_slot_0x00"), -1)
            flags_ec = int_value(call.get("candidate_flags_0xec"))
            penalty = 22.5 if flags_ec & 0x00200000 else 7.5
            if live_accepted:
                current_scan["accepted_candidates"] += 1
                current_scan["accepted_slots"].append(slot)
            if geometry.accepted:
                current_scan["modeled_accepted_candidates"] += 1
                current_scan["modeled_aggregate"] = f32(
                    current_scan["modeled_aggregate"]
                    + f32(geometry.perpendicular_distance - penalty)
                )
            candidate_rows.append(
                {
                    "source_exec_job_id": current_scan["source_exec_job_id"],
                    "loop_index": current_scan["loop_index"],
                    "yaw_iteration": current_scan["yaw_iteration"],
                    "side": current_scan["side"],
                    "call_sequence": int_value(call.get("capture_sequence")),
                    "return_sequence": int_value(event.get("capture_sequence")),
                    "candidate_index": int_value(call.get("candidate_index")),
                    "candidate_slot": slot,
                    "excluded_slot": current_scan["excluded_slot"],
                    "live_accepted": int(live_accepted),
                    "modeled_accepted": int(geometry.accepted),
                    "acceptance_match": int(live_accepted == geometry.accepted),
                    "perpendicular_distance": geometry.perpendicular_distance,
                    "distance_base_to_candidate": geometry.distance_base_to_candidate,
                    "distance_base_to_input": geometry.distance_base_to_input,
                    "raw_angle_diff_degrees": geometry.raw_angle_diff_degrees,
                    "score_after_bits": event.get("scan_score_after", ""),
                    "candidate_flags_0xec": call.get("candidate_flags_0xec", ""),
                    "candidate_flags_0xf0": call.get("candidate_flags_0xf0", ""),
                    "candidate_extent_0x15c": call.get("candidate_extent_0x15c", ""),
                }
            )
            current_candidate = None
            continue

        if checkpoint_id in (FALLBACK, NONZERO):
            if current_scan is None:
                raise ValueError("scan outcome arrived without an outer scan")
            current_scan["outcome_sequence"] = int_value(event.get("capture_sequence"))
            current_scan["fallback_draw"] = checkpoint_id == FALLBACK
            current_scan["modeled_fallback_draw"] = current_scan["modeled_aggregate"] == 0.0
            current_scan["accepted_slots"] = ";".join(
                str(slot) for slot in current_scan["accepted_slots"]
            )
            current_scan["fallback_match"] = int(
                current_scan["fallback_draw"] == current_scan["modeled_fallback_draw"]
            )
            scan_rows.append(current_scan)
            current_scan = None
            current_candidate = None

    actor_scans = [row for row in scan_rows if row["side"] == "actor"]
    target_scans = [row for row in scan_rows if row["side"] == "target"]
    summary = {
        "source_exec_job_id": source_exec_job_id,
        "outer_entries": outer_entries,
        "outer_returns": outer_returns,
        "scan_rows": len(scan_rows),
        "actor_scans": len(actor_scans),
        "target_scans": len(target_scans),
        "actor_fallback_draws": sum(bool(row["fallback_draw"]) for row in actor_scans),
        "target_fallback_draws": sum(bool(row["fallback_draw"]) for row in target_scans),
        "total_fallback_draws": sum(bool(row["fallback_draw"]) for row in scan_rows),
        "candidate_rows": len(candidate_rows),
        "candidate_acceptance_matches": sum(row["acceptance_match"] for row in candidate_rows),
        "scan_fallback_matches": sum(row["fallback_match"] for row in scan_rows),
    }
    return scan_rows, candidate_rows, summary


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-exec-job-id", type=int)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    scans, candidates, summary = reduce_events(
        load_events(args.capture), args.source_exec_job_id
    )
    write_csv(args.output / "pathing_scan_timeline.csv", scans)
    write_csv(args.output / "pathing_candidate_geometry.csv", candidates)
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
