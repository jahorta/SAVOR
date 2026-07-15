#!/usr/bin/env python3
"""Reduce copied combat-effect buffers and their FUN_80042B10 RNG cardinality."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any, Iterable


COPY = "effect_record_copy_complete"
LOOP_GATE = "combat_effect_loop_gate"
POSITION_BINARY = "combat_effect_spawn_position_binary"
POSITION_FOUR_WAY = "combat_effect_spawn_position_four_way"
SCALE_X = "combat_effect_spawn_scale_x"
SCALE_Y = "combat_effect_spawn_scale_y"
SCALE_Z = "combat_effect_spawn_scale_z"
VARIANT = "combat_effect_spawn_variant_index"
AXIS = "combat_effect_spawn_axis_assignment"

DRAW_CHECKPOINTS = (
    POSITION_BINARY,
    POSITION_FOUR_WAY,
    SCALE_X,
    SCALE_Y,
    SCALE_Z,
    VARIANT,
    AXIS,
)


def int_value(value: Any, default: int = 0) -> int:
    if value is None or value == "":
        return default
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def load_events(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: expected an object")
            events.append(row)
    events.sort(key=lambda row: int_value(row.get("capture_sequence")))
    return events


def _buffer_key(event: dict[str, Any]) -> int:
    if event.get("checkpoint_name") == COPY:
        return int_value(event.get("r6_effect_buffer"), -1)
    return int_value(event.get("r29_effect_buffer"), -1)


def reduce_events(
    events: Iterable[dict[str, Any]],
    source_exec_job_id: int | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any]]:
    ordered_events = list(events)
    buffers: dict[int, dict[str, Any]] = {}
    copy_order: list[int] = []

    for event in ordered_events:
        if event.get("checkpoint_name") != COPY:
            continue
        buffer_key = _buffer_key(event)
        if buffer_key < 0 or buffer_key in buffers:
            raise ValueError("effect copy has a missing or duplicate live buffer")
        flags = int_value(event.get("effect_flags_0x38"))
        loop_count = int_value(event.get("effect_loop_count_0x5c"))
        variant_count = int_value(event.get("effect_variant_count_0x5e"))
        axis_mode = int_value(event.get("effect_axis_mode_0x60"))
        expected_position = POSITION_FOUR_WAY if flags & 0x80000 else POSITION_BINARY
        expected_axis_draws = loop_count if flags & 0x200 and axis_mode == 3 else 0
        expected_variant_draws = loop_count if variant_count != 0 else 0
        buffers[buffer_key] = {
            "source_exec_job_id": source_exec_job_id if source_exec_job_id is not None else "",
            "copy_sequence": int_value(event.get("capture_sequence")),
            "rng_draw_index_before": int_value(event.get("rng_draw_index_before")),
            "effect_buffer": f"0x{buffer_key:08X}",
            "parent_action_thread": event.get("effect_parent_action_thread_0x04", ""),
            "source_record": event.get("r31_source_record", ""),
            "source_key": int_value(event.get("effect_source_key_0x28"), -1),
            "loop_count": loop_count,
            "flags": f"0x{flags:08X}",
            "variant_count": variant_count,
            "axis_mode": axis_mode,
            "expected_position_checkpoint": expected_position,
            "expected_draws": loop_count * (4 + (variant_count != 0) + (expected_axis_draws != 0)),
            "expected_loop_gates": loop_count + 1,
            "expected_binary_draws": loop_count if expected_position == POSITION_BINARY else 0,
            "expected_four_way_draws": loop_count if expected_position == POSITION_FOUR_WAY else 0,
            "expected_scale_x_draws": loop_count,
            "expected_scale_y_draws": loop_count,
            "expected_scale_z_draws": loop_count,
            "expected_variant_draws": expected_variant_draws,
            "expected_axis_draws": expected_axis_draws,
            "loop_gates": 0,
            "binary_draws": 0,
            "four_way_draws": 0,
            "scale_x_draws": 0,
            "scale_y_draws": 0,
            "scale_z_draws": 0,
            "variant_draws": 0,
            "axis_draws": 0,
            "first_effect_draw_index": "",
            "last_effect_draw_index": "",
        }
        copy_order.append(buffer_key)

    counter_fields = {
        POSITION_BINARY: "binary_draws",
        POSITION_FOUR_WAY: "four_way_draws",
        SCALE_X: "scale_x_draws",
        SCALE_Y: "scale_y_draws",
        SCALE_Z: "scale_z_draws",
        VARIANT: "variant_draws",
        AXIS: "axis_draws",
    }
    for event in ordered_events:
        checkpoint = str(event.get("checkpoint_name", ""))
        if checkpoint != LOOP_GATE and checkpoint not in DRAW_CHECKPOINTS:
            continue
        buffer_key = _buffer_key(event)
        row = buffers.get(buffer_key)
        if row is None:
            continue
        if checkpoint == LOOP_GATE:
            row["loop_gates"] += 1
            continue
        row[counter_fields[checkpoint]] += 1
        draw_index = int_value(event.get("rng_draw_index_before"))
        if row["first_effect_draw_index"] == "":
            row["first_effect_draw_index"] = draw_index
        row["last_effect_draw_index"] = draw_index

    buffer_rows: list[dict[str, Any]] = []
    for buffer_key in copy_order:
        row = buffers[buffer_key]
        row["observed_draws"] = sum(row[counter_fields[name]] for name in DRAW_CHECKPOINTS)
        row["cardinality_exact"] = int(
            row["observed_draws"] == row["expected_draws"]
            and row["loop_gates"] == row["expected_loop_gates"]
            and row["binary_draws"] == row["expected_binary_draws"]
            and row["four_way_draws"] == row["expected_four_way_draws"]
            and row["scale_x_draws"] == row["expected_scale_x_draws"]
            and row["scale_y_draws"] == row["expected_scale_y_draws"]
            and row["scale_z_draws"] == row["expected_scale_z_draws"]
            and row["variant_draws"] == row["expected_variant_draws"]
            and row["axis_draws"] == row["expected_axis_draws"]
        )
        buffer_rows.append(row)

    grouped: dict[tuple[int, str, int], list[dict[str, Any]]] = {}
    for row in buffer_rows:
        group_key = (
            int_value(row["rng_draw_index_before"]),
            str(row["parent_action_thread"]),
            int_value(row["source_key"], -1),
        )
        grouped.setdefault(group_key, []).append(row)

    pair_rows: list[dict[str, Any]] = []
    for pair_index, (_, rows) in enumerate(sorted(grouped.items(), key=lambda item: item[0][0])):
        rows.sort(key=lambda row: int_value(row["copy_sequence"]))
        pair_rows.append({
            "source_exec_job_id": source_exec_job_id if source_exec_job_id is not None else "",
            "pair_index": pair_index,
            "rng_draw_index_before": rows[0]["rng_draw_index_before"],
            "parent_action_thread": rows[0]["parent_action_thread"],
            "source_key": rows[0]["source_key"],
            "buffer_count": len(rows),
            "loop_counts": "+".join(str(row["loop_count"]) for row in rows),
            "expected_draws": sum(int_value(row["expected_draws"]) for row in rows),
            "observed_draws": sum(int_value(row["observed_draws"]) for row in rows),
            "cardinality_exact": int(len(rows) == 2 and all(row["cardinality_exact"] for row in rows)),
        })

    summary = {
        "source_exec_job_id": source_exec_job_id,
        "copied_buffers": len(buffer_rows),
        "exact_buffers": sum(row["cardinality_exact"] for row in buffer_rows),
        "effect_pairs": len(pair_rows),
        "exact_effect_pairs": sum(row["cardinality_exact"] for row in pair_rows),
        "observed_draws": sum(int_value(row["observed_draws"]) for row in buffer_rows),
        "expected_draws": sum(int_value(row["expected_draws"]) for row in buffer_rows),
    }
    return buffer_rows, pair_rows, summary


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-exec-job-id", type=int)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    buffers, pairs, summary = reduce_events(
        load_events(args.capture), args.source_exec_job_id
    )
    write_csv(args.output / "effect_buffer_cardinality.csv", buffers)
    write_csv(args.output / "effect_pair_cardinality.csv", pairs)
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
