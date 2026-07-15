#!/usr/bin/env python3
"""Reduce battle thread/resource producer captures into semantic timelines."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable, Iterator


FRAME_ID = "battle_case5_after_threads_8000A2FC"
MOVEMENT_PREFIXES = ("setup_grid_", "setup_combatant_", "movement_root")
INSTRUCTION_PREFIXES = ("create_std_", "instruction_root")
RESOURCE_PREFIXES = (
    "load_movement_std_",
    "load_combatant_std_",
    "resource_queue_",
    "character_mlk_queue_",
)


def int_value(value: Any, default: int = 0) -> int:
    if value in (None, ""):
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def hex32(value: Any) -> str:
    if value in (None, ""):
        return ""
    return f"0x{int_value(value) & 0xFFFFFFFF:08X}"


def iter_jsonl(path: Path) -> Iterator[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error


def checkpoint_id(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id", ""))


def semantic_kind(event_id: str) -> str:
    normalized = event_id.removeprefix("memwatch.")
    if event_id == FRAME_ID:
        return "frame_boundary"
    if normalized.startswith(MOVEMENT_PREFIXES):
        return "movement_thread"
    if normalized.startswith(INSTRUCTION_PREFIXES):
        return "instruction_thread"
    if normalized.startswith(RESOURCE_PREFIXES):
        return "resource_queue"
    if "thread_list_head_write" in normalized or event_id == "mkchild_entry_802268E8":
        return "thread_list"
    if normalized.startswith("thread_remove_"):
        return "thread_remove"
    return ""


def compact_nodes(event: dict[str, Any]) -> list[dict[str, Any]]:
    nodes: list[dict[str, Any]] = []
    for item in event.get("thread_list_nodes", []) or []:
        nodes.append(
            {
                "index": int_value(item.get("index"), -1),
                "node": int_value(item.get("node"), -1),
                "callback": int_value(item.get("callback"), 0),
                "next": int_value(item.get("next"), 0),
                "parent": int_value(item.get("parent"), 0),
                "flags": int_value(item.get("flags"), 0),
                "state": int_value(item.get("state"), 0),
                "payload": int_value(item.get("payload_word"), 0),
            }
        )
    return nodes


def root_rows(event: dict[str, Any]) -> list[dict[str, Any]]:
    nodes = compact_nodes(event)
    by_address = {node["node"]: node for node in nodes if node["node"] >= 0}
    by_index = {node["index"]: node for node in nodes if node["index"] >= 0}
    result: list[dict[str, Any]] = []
    for family, prefix in (
        ("movement", "movement_root"),
        ("instruction", "instruction_root"),
    ):
        for root_index in range(12):
            root = int_value(event.get(f"{prefix}{root_index}"), 0)
            if root == 0:
                continue
            node = by_address.get(root, {})
            predecessor = by_index.get(int_value(node.get("index"), -1) - 1, {})
            owner = event.get(f"{prefix}{root_index}_owner_slot")
            result.append(
                {
                    "family": family,
                    "root_index": root_index,
                    "owner_slot": int_value(owner, -1),
                    "node_address": root,
                    "list_index": int_value(node.get("index"), -1),
                    "callback": int_value(node.get("callback"), 0),
                    "parent": int_value(node.get("parent"), 0),
                    "payload": int_value(node.get("payload"), 0),
                    "predecessor_node": int_value(predecessor.get("node"), 0),
                    "predecessor_callback": int_value(
                        predecessor.get("callback"), 0
                    ),
                    "predecessor_payload": int_value(predecessor.get("payload"), 0),
                }
            )
    return result


def producer_row(source_job: int, event: dict[str, Any]) -> dict[str, Any]:
    event_id = checkpoint_id(event)
    nodes = compact_nodes(event)
    return {
        "source_exec_job_id": source_job,
        "capture_sequence": int_value(event.get("capture_sequence"), -1),
        "vi_field_count": int_value(event.get("vi_field_count"), -1),
        "movie_input_count": int_value(event.get("movie_input_count"), -1),
        "checkpoint_id": event_id,
        "semantic_kind": semantic_kind(event_id),
        "pc": hex32(event.get("pc")),
        "source_pc": hex32(event.get("memwatch_source_pc", event.get("pc"))),
        "post_write_value": hex32(event.get("decoded_memory_value")),
        "thread_node_count": int_value(event.get("thread_list_node_count"), len(nodes)),
        "thread_list_read_ok": bool(event.get("thread_list_read_ok", False)),
        "thread_list_truncated": bool(event.get("thread_list_truncated", False)),
        "thread_list_cycle_detected": bool(
            event.get("thread_list_cycle_detected", False)
        ),
        "gpr_slot": int_value(event.get("slot"), -1),
        "gpr_thread": hex32(event.get("thread")),
        "gpr_parent": hex32(event.get("parent")),
        "gpr_callback": hex32(event.get("callback")),
        "gpr_load_context": hex32(event.get("load_context")),
        "thread_runner_current": hex32(event.get("thread_runner_current")),
    }


def reduce_events(
    events: Iterable[dict[str, Any]], source_job: int
) -> dict[str, Any]:
    producers: list[dict[str, Any]] = []
    resources: list[dict[str, Any]] = []
    thread_rows: list[dict[str, Any]] = []
    first_seen: dict[tuple[str, int], int] = {}
    list_failures: list[dict[str, Any]] = []
    frame_count = 0
    frame_instruction_rows: dict[int, list[dict[str, Any]]] = defaultdict(list)
    frame_movement_rows: dict[int, list[dict[str, Any]]] = defaultdict(list)

    for event in events:
        event_id = checkpoint_id(event)
        kind = semantic_kind(event_id)
        if not kind:
            continue
        row = producer_row(source_job, event)
        producers.append(row)
        if kind == "resource_queue" or event_id.startswith("create_std_"):
            resources.append(row.copy())
        if event_id == FRAME_ID:
            frame_count += 1

        if "thread_list_read_ok" in event and (
            not bool(event.get("thread_list_read_ok"))
            or bool(event.get("thread_list_truncated"))
            or bool(event.get("thread_list_cycle_detected"))
        ):
            list_failures.append(row.copy())

        for root in root_rows(event):
            key = (root["family"], root["node_address"])
            first = key not in first_seen
            if first:
                first_seen[key] = row["capture_sequence"]
            thread_rows.append(
                {
                    "source_exec_job_id": source_job,
                    "capture_sequence": row["capture_sequence"],
                    "checkpoint_id": event_id,
                    "semantic_kind": kind,
                    "family": root["family"],
                    "root_index": root["root_index"],
                    "observed_owner_slot": root["owner_slot"],
                    "owner_slot": root["owner_slot"],
                    "node_address": hex32(root["node_address"]),
                    "list_index": root["list_index"],
                    "callback": hex32(root["callback"]),
                    "parent": hex32(root["parent"]),
                    "payload": hex32(root["payload"]),
                    "predecessor_node_address": hex32(
                        root["predecessor_node"]
                    ),
                    "predecessor_callback": hex32(
                        root["predecessor_callback"]
                    ),
                    "predecessor_payload": hex32(
                        root["predecessor_payload"]
                    ),
                    "thread_runner_current": row["thread_runner_current"],
                    "first_seen": first,
                    "first_seen_sequence": first_seen[key],
                }
            )
            if event_id == FRAME_ID:
                target = (
                    frame_instruction_rows
                    if root["family"] == "instruction"
                    else frame_movement_rows
                )
                target[row["capture_sequence"]].append(thread_rows[-1])

    resolved_owner_by_node: dict[tuple[str, str], int] = {}
    for row in thread_rows:
        if row["observed_owner_slot"] >= 0:
            resolved_owner_by_node[(row["family"], row["node_address"])] = row[
                "observed_owner_slot"
            ]
    for row in thread_rows:
        row["owner_slot"] = resolved_owner_by_node.get(
            (row["family"], row["node_address"]),
            row["observed_owner_slot"],
        )

    first_by_family: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in thread_rows:
        if row["first_seen"]:
            first_by_family[row["family"]].append(row)
    for rows in first_by_family.values():
        rows.sort(key=lambda item: (item["first_seen_sequence"], item["root_index"]))

    def first_maximum_frame_order(
        frame_rows: dict[int, list[dict[str, Any]]]
    ) -> list[int]:
        if not frame_rows:
            return []
        maximum = max(len(rows) for rows in frame_rows.values())
        sequence = min(
            capture_sequence
            for capture_sequence, rows in frame_rows.items()
            if len(rows) == maximum
        )
        return [
            row["owner_slot"]
            for row in sorted(
                frame_rows[sequence], key=lambda item: item["list_index"]
            )
        ]

    movement_creation_order = [
        row["owner_slot"] for row in first_by_family.get("movement", [])
    ]
    instruction_creation_order = [
        row["owner_slot"] for row in first_by_family.get("instruction", [])
    ]
    movement_traversal_order = first_maximum_frame_order(frame_movement_rows)
    instruction_traversal_order = first_maximum_frame_order(
        frame_instruction_rows
    )

    return {
        "producer_rows": producers,
        "resource_rows": resources,
        "thread_rows": thread_rows,
        "summary": {
            "source_exec_job_id": source_job,
            "producer_event_count": len(producers),
            "resource_event_count": len(resources),
            "frame_count": frame_count,
            "frame_cap_reached": frame_count >= 2400,
            "thread_list_failure_count": len(list_failures),
            "movement_creation_order": movement_creation_order,
            "movement_traversal_order": movement_traversal_order,
            "instruction_creation_order": instruction_creation_order,
            "instruction_traversal_order": instruction_traversal_order,
            "movement_publication_order": movement_creation_order,
            "instruction_publication_order": instruction_creation_order,
        },
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields: list[str] = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def parse_capture(value: str) -> tuple[int, Path]:
    job, separator, path = value.partition("=")
    if not separator:
        raise argparse.ArgumentTypeError("--capture must be EXEC_JOB_ID=PATH")
    try:
        source_job = int(job, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid exec job ID: {job}") from error
    return source_job, Path(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--capture",
        action="append",
        required=True,
        type=parse_capture,
        metavar="EXEC_JOB_ID=CAPTURE_JSONL",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    all_producers: list[dict[str, Any]] = []
    all_resources: list[dict[str, Any]] = []
    all_threads: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []
    for source_job, path in args.capture:
        reduced = reduce_events(iter_jsonl(path), source_job)
        all_producers.extend(reduced["producer_rows"])
        all_resources.extend(reduced["resource_rows"])
        all_threads.extend(reduced["thread_rows"])
        summaries.append(reduced["summary"])

    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.output_dir / "thread_producer_timeline.csv", all_producers)
    write_csv(args.output_dir / "resource_publication_timeline.csv", all_resources)
    write_csv(args.output_dir / "thread_order_reconstruction.csv", all_threads)
    (args.output_dir / "summary.json").write_text(
        json.dumps({"jobs": summaries}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
