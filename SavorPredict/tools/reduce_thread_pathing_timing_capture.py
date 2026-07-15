#!/usr/bin/env python3
"""Reduce the combined frame/thread/position-write/pathing timing capture."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable, Iterator


FRAME_ID = "battle_case5_after_threads_8000A2FC"
PATHING_ID = "pathing_outer_loop_entry_800526EC"
ACTION_VIEW_CALLBACK = 0x80051264
THREAD_RUNNER_CALLSITE = 0x802265BC
EXACT_WRITE_PREFIX = "memwatch.packed"
DELTA_WRITE_PREFIX = "memwatch_delta.packed"


def int_value(value: Any, default: int = 0) -> int:
    if value is None or value == "":
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def hex32(value: Any) -> str:
    if value is None or value == "":
        return ""
    return f"0x{int_value(value) & 0xFFFFFFFF:08X}"


def valid_address(value: Any) -> bool:
    return value not in (None, "") and (int_value(value) & 0x80000000) != 0


def iter_jsonl(path: Path) -> Iterator[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error


def compact_thread_list(event: dict[str, Any]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for node in event.get("thread_list_nodes", []) or []:
        result.append(
            {
                "index": int_value(node.get("index"), -1),
                "node": hex32(node.get("node")),
                "callback": hex32(node.get("callback")),
                "state": hex32(node.get("state")),
                "flags": hex32(node.get("flags")),
                "payload": hex32(node.get("payload_word")),
            }
        )
    return result


def extract_positions(event: dict[str, Any]) -> dict[int, dict[str, Any]]:
    """Resolve packed-root samples to their live logical instruction slots."""
    positions: dict[int, dict[str, Any]] = {}
    for packed_index in range(12):
        prefix = f"slot{packed_index}"
        slot_value = event.get(f"{prefix}_iw_slot_0x00")
        if slot_value in (None, ""):
            continue
        logical_slot = int_value(slot_value, -1)
        if not 0 <= logical_slot < 12:
            continue
        x_addr = event.get(f"{prefix}_cw_cur_x_0x1c_address")
        z_addr = event.get(f"{prefix}_cw_cur_z_0x24_address")
        positions[logical_slot] = {
            "logical_slot": logical_slot,
            "packed_index": packed_index,
            "x_bits": hex32(event.get(f"{prefix}_cw_cur_x_0x1c")),
            "z_bits": hex32(event.get(f"{prefix}_cw_cur_z_0x24")),
            "x_addr": hex32(x_addr) if valid_address(x_addr) else "",
            "z_addr": hex32(z_addr) if valid_address(z_addr) else "",
            "callback": hex32(event.get(f"{prefix}_thread_callback_0x00")),
            "facing_bits": hex32(event.get(f"{prefix}_cw_facing_0x2c")),
        }
    return positions


def compact_snapshot(event: dict[str, Any]) -> dict[str, Any]:
    return {
        "sequence": int_value(event.get("capture_sequence")),
        "vi_field": int_value(event.get("vi_field_count"), -1),
        "movie_input": int_value(event.get("movie_input_count"), -1),
        "positions": extract_positions(event),
        "threads": compact_thread_list(event),
        "thread_list_read_ok": bool(event.get("thread_list_read_ok", False)),
        "thread_list_truncated": bool(event.get("thread_list_truncated", False)),
        "thread_list_cycle_detected": bool(event.get("thread_list_cycle_detected", False)),
        "thread_list_node_count": int_value(event.get("thread_list_node_count"), -1),
        "thread_runner_previous": hex32(event.get("thread_runner_previous_80311A78")),
        "thread_runner_current": hex32(event.get("thread_runner_current_80311A7C")),
    }


def stack_callsites(event: dict[str, Any]) -> list[str]:
    count = min(int_value(event.get("stack_frame_count")), 32)
    return [
        hex32(event.get(f"stack_frame_{index}_callsite_pc"))
        for index in range(count)
        if event.get(f"stack_frame_{index}_callsite_pc") not in (None, "")
    ]


def owner_callsite(callsites: list[str]) -> str:
    runner = hex32(THREAD_RUNNER_CALLSITE)
    for index, callsite in enumerate(callsites):
        if callsite == runner and index > 0:
            return callsites[index - 1]
    return ""


def compact_write(event: dict[str, Any], kind: str) -> dict[str, Any]:
    checkpoint_id = str(event.get("checkpoint_id", ""))
    label = str(event.get("memwatch_label", ""))
    if not label:
        label = checkpoint_id.split(".", 1)[1] if "." in checkpoint_id else checkpoint_id
    packed_match = re.search(r"packed(\d+)_cw_cur_([xz])_write", label)
    callsites = stack_callsites(event) if kind == "exact" else []
    hits_before = int_value(event.get("memwatch_hits_before"), 0)
    hits_after = int_value(event.get("memwatch_hits_after"), hits_before)
    return {
        "sequence": int_value(event.get("capture_sequence")),
        "vi_field": int_value(event.get("vi_field_count"), -1),
        "movie_input": int_value(event.get("movie_input_count"), -1),
        "kind": kind,
        "checkpoint_id": checkpoint_id,
        "watch_label": label,
        "watch_address": hex32(event.get("memwatch_addr")),
        "packed_label_index": int(packed_match.group(1)) if packed_match else -1,
        "component_from_label": packed_match.group(2) if packed_match else "",
        "value_bits": hex32(event.get("decoded_memory_value")) if kind == "exact" else "",
        "source_pc": hex32(event.get("pc")) if kind == "exact" else "",
        "bracket_pc": hex32(event.get("pc")) if kind == "delta" else "",
        "confirmed_current_instruction": bool(
            event.get("memwatch_confirmed_current_instruction", False)
        ),
        "hits_before": hits_before if kind == "delta" else "",
        "hits_after": hits_after if kind == "delta" else "",
        "hit_delta": max(0, hits_after - hits_before) if kind == "delta" else 1,
        "owner_callsite": owner_callsite(callsites),
        "stack_callsites": ";".join(callsites),
    }


def add_address_candidates(
    candidates: dict[str, set[tuple[int, str]]], positions: dict[int, dict[str, Any]]
) -> None:
    for slot, position in positions.items():
        for component in ("x", "z"):
            address = position[f"{component}_addr"]
            if address:
                candidates[address].add((slot, component))


def thread_indices(snapshot: dict[str, Any], callback: int) -> str:
    callback_hex = hex32(callback)
    return ";".join(
        str(node["index"])
        for node in snapshot.get("threads", [])
        if node["callback"] == callback_hex
    )


def node_index(snapshot: dict[str, Any], node_address: str) -> int | str:
    if not node_address:
        return ""
    for node in snapshot.get("threads", []):
        if node["node"] == node_address:
            return node["index"]
    return ""


def combatant_thread_order(snapshot: dict[str, Any]) -> str:
    payload_to_slot: dict[str, int] = {}
    for slot, position in snapshot.get("positions", {}).items():
        if position["x_addr"]:
            payload_to_slot[hex32(int_value(position["x_addr"]) - 0x1C)] = slot
    return ";".join(
        f"{node['index']}:{payload_to_slot[node['payload']]}"
        for node in snapshot.get("threads", [])
        if node["payload"] in payload_to_slot
    )


def combatant_slot_order(snapshot: dict[str, Any]) -> str:
    indexed = combatant_thread_order(snapshot)
    return ";".join(item.split(":", 1)[1] for item in indexed.split(";") if item)


def position_bits(snapshot: dict[str, Any] | None, slot: int, component: str) -> str:
    if snapshot is None:
        return ""
    return snapshot.get("positions", {}).get(slot, {}).get(f"{component}_bits", "")


def position_summary(snapshot: dict[str, Any]) -> str:
    return ";".join(
        f"{slot}:{position['x_bits']},{position['z_bits']}"
        for slot, position in sorted(snapshot["positions"].items())
    )


def callback_summary(snapshot: dict[str, Any]) -> str:
    return ";".join(
        f"{node['index']}:{node['callback']}:{node['state']}:{node['payload']}"
        for node in snapshot["threads"]
    )


def detail_value(detail: str, key: str) -> str:
    match = re.search(rf"(?:^|; ){re.escape(key)}=([^;]+)", detail)
    return match.group(1) if match else ""


def reduce_predictor(path: Path, source_job: int) -> list[dict[str, Any]]:
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    events = document.get("prediction", {}).get("events", [])
    rows: list[dict[str, Any]] = []
    for event in events:
        if event.get("label") != "mode1_pathing_record_draw":
            continue
        sequence = int_value(event.get("sequence"))
        frame_index = int_value(event.get("frame_index"), -1)
        same_frame = [
            candidate
            for candidate in events
            if candidate.get("label") == "worker_frame"
            and int_value(candidate.get("frame_index"), -2) == frame_index
        ]
        before = [candidate for candidate in same_frame if int_value(candidate.get("sequence")) < sequence]
        after = [candidate for candidate in same_frame if int_value(candidate.get("sequence")) > sequence]
        previous = before[-1] if before else {}
        following = after[0] if after else {}
        previous_detail = str(previous.get("detail", ""))
        following_detail = str(following.get("detail", ""))
        rows.append(
            {
                "source_exec_job_id": source_job,
                "predictor_sequence": sequence,
                "predictor_frame_index": frame_index,
                "action_ordinal": int_value(event.get("action_ordinal"), -1),
                "actor_slot": int_value(event.get("actor_slot"), -1),
                "target_slot": int_value(event.get("target_slot"), -1),
                "worker_visits_before_pathing": len(before),
                "worker_visits_after_pathing": len(after),
                "worker_slots_before_pathing": ";".join(str(item.get("actor_slot", "")) for item in before),
                "worker_slots_after_pathing": ";".join(str(item.get("actor_slot", "")) for item in after),
                "previous_worker_sequence": previous.get("sequence", ""),
                "previous_worker_slot": previous.get("actor_slot", ""),
                "previous_worker_kind": detail_value(previous_detail, "worker_kind"),
                "previous_controller_family": detail_value(previous_detail, "controller_family"),
                "previous_callback_pc": detail_value(previous_detail, "callback_pc"),
                "next_worker_sequence": following.get("sequence", ""),
                "next_worker_slot": following.get("actor_slot", ""),
                "next_worker_kind": detail_value(following_detail, "worker_kind"),
                "next_controller_family": detail_value(following_detail, "controller_family"),
                "next_callback_pc": detail_value(following_detail, "callback_pc"),
            }
        )
    return rows


def reduce_event_stream(
    events: Iterable[dict[str, Any]], source_job: int
) -> dict[str, Any]:
    frames: list[dict[str, Any]] = []
    pathing: list[dict[str, Any]] = []
    writes: list[dict[str, Any]] = []
    candidates: dict[str, set[tuple[int, str]]] = defaultdict(set)
    checkpoint_counts: Counter[str] = Counter()
    list_failures: list[dict[str, Any]] = []

    for event in events:
        checkpoint_id = str(event.get("checkpoint_id", ""))
        checkpoint_counts[checkpoint_id] += 1
        if "slot0_iw_slot_0x00" in event:
            add_address_candidates(candidates, extract_positions(event))
        if checkpoint_id == FRAME_ID:
            snapshot = compact_snapshot(event)
            frames.append(snapshot)
        elif checkpoint_id == PATHING_ID:
            snapshot = compact_snapshot(event)
            snapshot["actor_slot"] = int_value(event.get("turn_actor_slot_0x00"), -1)
            snapshot["target_slot"] = int_value(event.get("turn_target_slot_0x04"), -1)
            snapshot["turn_yaw_bits"] = hex32(event.get("turn_yaw_0xd8"))
            pathing.append(snapshot)
        elif checkpoint_id.startswith(EXACT_WRITE_PREFIX):
            writes.append(compact_write(event, "exact"))
        elif checkpoint_id.startswith(DELTA_WRITE_PREFIX):
            writes.append(compact_write(event, "delta"))

        if "thread_list_read_ok" in event and (
            not bool(event.get("thread_list_read_ok"))
            or bool(event.get("thread_list_truncated"))
            or bool(event.get("thread_list_cycle_detected"))
        ):
            list_failures.append(
                {
                    "sequence": int_value(event.get("capture_sequence")),
                    "checkpoint_id": checkpoint_id,
                    "read_ok": bool(event.get("thread_list_read_ok")),
                    "truncated": bool(event.get("thread_list_truncated")),
                    "cycle_detected": bool(event.get("thread_list_cycle_detected")),
                    "error": event.get("thread_list_error", ""),
                }
            )

    frames.sort(key=lambda item: item["sequence"])
    pathing.sort(key=lambda item: item["sequence"])
    writes.sort(key=lambda item: item["sequence"])

    address_map: dict[str, tuple[int, str] | None] = {}
    for address, labels in candidates.items():
        address_map[address] = next(iter(labels)) if len(labels) == 1 else None

    frame_sequences = [frame["sequence"] for frame in frames]
    for write in writes:
        mapping = address_map.get(write["watch_address"])
        write["logical_slot"] = mapping[0] if mapping else ""
        write["component"] = mapping[1] if mapping else ""
        insertion = bisect.bisect_left(frame_sequences, write["sequence"])
        write["previous_frame_index"] = insertion - 1 if insertion > 0 else ""
        write["next_frame_index"] = insertion if insertion < len(frames) else ""

    frame_rows: list[dict[str, Any]] = []
    for frame_index, frame in enumerate(frames):
        frame_rows.append(
            {
                "source_exec_job_id": source_job,
                "frame_index": frame_index,
                "capture_sequence": frame["sequence"],
                "vi_field": frame["vi_field"],
                "movie_input": frame["movie_input"],
                "thread_list_read_ok": int(frame["thread_list_read_ok"]),
                "thread_list_truncated": int(frame["thread_list_truncated"]),
                "thread_list_cycle_detected": int(frame["thread_list_cycle_detected"]),
                "thread_list_node_count": frame["thread_list_node_count"],
                "action_view_thread_indices": thread_indices(frame, ACTION_VIEW_CALLBACK),
                "thread_runner_previous": frame["thread_runner_previous"],
                "thread_runner_previous_index": node_index(frame, frame["thread_runner_previous"]),
                "thread_runner_current": frame["thread_runner_current"],
                "thread_runner_current_index": node_index(frame, frame["thread_runner_current"]),
                "combatant_thread_order": combatant_thread_order(frame),
                "combatant_slot_order": combatant_slot_order(frame),
                "positions": position_summary(frame),
                "thread_order": callback_summary(frame),
            }
        )

    write_rows: list[dict[str, Any]] = []
    for write in writes:
        write_rows.append({"source_exec_job_id": source_job, **write})

    invocation_rows: list[dict[str, Any]] = []
    visibility_rows: list[dict[str, Any]] = []
    for invocation_index, invocation in enumerate(pathing):
        insertion = bisect.bisect_left(frame_sequences, invocation["sequence"])
        previous_index = insertion - 1
        next_index = insertion
        previous = frames[previous_index] if previous_index >= 0 else None
        following = frames[next_index] if next_index < len(frames) else None
        lower_sequence = previous["sequence"] if previous else -1
        window_writes = [
            write
            for write in writes
            if lower_sequence < write["sequence"] <= invocation["sequence"]
        ]
        exact_window = [write for write in window_writes if write["kind"] == "exact"]
        delta_window = [write for write in window_writes if write["kind"] == "delta"]
        invocation_rows.append(
            {
                "source_exec_job_id": source_job,
                "invocation_index": invocation_index,
                "capture_sequence": invocation["sequence"],
                "vi_field": invocation["vi_field"],
                "actor_slot": invocation["actor_slot"],
                "target_slot": invocation["target_slot"],
                "turn_yaw_bits": invocation["turn_yaw_bits"],
                "previous_frame_index": previous_index if previous else "",
                "previous_frame_sequence": previous["sequence"] if previous else "",
                "previous_frame_vi": previous["vi_field"] if previous else "",
                "next_frame_index": next_index if following else "",
                "next_frame_sequence": following["sequence"] if following else "",
                "next_frame_vi": following["vi_field"] if following else "",
                "pathing_action_view_thread_indices": thread_indices(invocation, ACTION_VIEW_CALLBACK),
                "pathing_thread_runner_previous": invocation["thread_runner_previous"],
                "pathing_thread_runner_previous_index": node_index(
                    invocation, invocation["thread_runner_previous"]
                ),
                "pathing_thread_runner_current": invocation["thread_runner_current"],
                "pathing_thread_runner_current_index": node_index(
                    invocation, invocation["thread_runner_current"]
                ),
                "pathing_combatant_thread_order": combatant_thread_order(invocation),
                "pathing_combatant_slot_order": combatant_slot_order(invocation),
                "previous_action_view_thread_indices": thread_indices(previous or {}, ACTION_VIEW_CALLBACK),
                "next_action_view_thread_indices": thread_indices(following or {}, ACTION_VIEW_CALLBACK),
                "exact_position_writes_since_previous_frame": len(exact_window),
                "position_watch_delta_rows_since_previous_frame": len(delta_window),
                "position_watch_delta_hits_since_previous_frame": sum(
                    int_value(write["hit_delta"]) for write in delta_window
                ),
                "thread_list_read_ok": int(invocation["thread_list_read_ok"]),
                "thread_list_truncated": int(invocation["thread_list_truncated"]),
                "thread_list_cycle_detected": int(invocation["thread_list_cycle_detected"]),
                "thread_list_node_count": invocation["thread_list_node_count"],
            }
        )

        slots = set(invocation["positions"])
        if previous:
            slots.update(previous["positions"])
        if following:
            slots.update(following["positions"])
        slots.update(slot for slot in (invocation["actor_slot"], invocation["target_slot"]) if slot >= 0)
        for slot in sorted(slots):
            slot_writes = [write for write in window_writes if write["logical_slot"] == slot]
            all_prior_slot_writes = [
                write
                for write in writes
                if write["sequence"] <= invocation["sequence"]
                and write["logical_slot"] == slot
            ]
            row: dict[str, Any] = {
                "source_exec_job_id": source_job,
                "invocation_index": invocation_index,
                "capture_sequence": invocation["sequence"],
                "vi_field": invocation["vi_field"],
                "actor_slot": invocation["actor_slot"],
                "target_slot": invocation["target_slot"],
                "logical_slot": slot,
                "role": "actor" if slot == invocation["actor_slot"] else "target" if slot == invocation["target_slot"] else "candidate",
            }
            for component in ("x", "z"):
                component_writes = [write for write in slot_writes if write["component"] == component]
                all_prior_component_writes = [
                    write
                    for write in all_prior_slot_writes
                    if write["component"] == component
                ]
                exact = [write for write in component_writes if write["kind"] == "exact"]
                delta = [write for write in component_writes if write["kind"] == "delta"]
                last_exact = exact[-1] if exact else {}
                all_prior_exact = [
                    write for write in all_prior_component_writes if write["kind"] == "exact"
                ]
                last_global_exact = all_prior_exact[-1] if all_prior_exact else {}
                previous_bits = position_bits(previous, slot, component)
                pathing_bits = position_bits(invocation, slot, component)
                row[f"previous_{component}_bits"] = previous_bits
                row[f"pathing_{component}_bits"] = pathing_bits
                row[f"next_{component}_bits"] = position_bits(following, slot, component)
                row[f"changed_before_pathing_{component}"] = int(
                    bool(previous_bits and pathing_bits and previous_bits != pathing_bits)
                )
                row[f"exact_{component}_writes"] = len(exact)
                row[f"delta_{component}_rows"] = len(delta)
                row[f"delta_{component}_hits"] = sum(int_value(write["hit_delta"]) for write in delta)
                row[f"last_exact_{component}_sequence"] = last_exact.get("sequence", "")
                row[f"last_exact_{component}_vi"] = last_exact.get("vi_field", "")
                row[f"last_exact_{component}_value_bits"] = last_exact.get("value_bits", "")
                row[f"last_exact_{component}_source_pc"] = last_exact.get("source_pc", "")
                row[f"last_exact_{component}_owner_callsite"] = last_exact.get("owner_callsite", "")
                row[f"last_global_exact_{component}_sequence"] = last_global_exact.get("sequence", "")
                row[f"last_global_exact_{component}_vi"] = last_global_exact.get("vi_field", "")
                row[f"last_global_exact_{component}_value_bits"] = last_global_exact.get("value_bits", "")
                row[f"last_global_exact_{component}_source_pc"] = last_global_exact.get("source_pc", "")
                row[f"last_global_exact_{component}_owner_callsite"] = last_global_exact.get("owner_callsite", "")
            visibility_rows.append(row)

    exact_writes = [write for write in writes if write["kind"] == "exact"]
    unresolved_exact = [write for write in exact_writes if write["logical_slot"] == ""]
    summary = {
        "source_exec_job_id": source_job,
        "frames": len(frames),
        "pathing_invocations": len(pathing),
        "movement_commits": checkpoint_counts["movement_commit_entry_8008178C"],
        "exact_position_writes": len(exact_writes),
        "position_watch_delta_rows": sum(write["kind"] == "delta" for write in writes),
        "unresolved_exact_position_writes": len(unresolved_exact),
        "ambiguous_live_addresses": sum(value is None for value in address_map.values()),
        "thread_list_failures": len(list_failures),
        "rng_seed_writes": checkpoint_counts["memwatch.rng_seed_write_803469A8"],
        "exact_write_source_pcs": dict(Counter(write["source_pc"] for write in exact_writes)),
        "exact_write_owner_callsites": dict(Counter(write["owner_callsite"] for write in exact_writes)),
        "combatant_thread_orders": dict(
            Counter(combatant_slot_order(frame) for frame in frames)
        ),
    }
    return {
        "frame_rows": frame_rows,
        "write_rows": write_rows,
        "invocation_rows": invocation_rows,
        "visibility_rows": visibility_rows,
        "summary": summary,
        "list_failures": list_failures,
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def parse_job_path(value: str) -> tuple[int, Path]:
    job_text, separator, path_text = value.partition("=")
    if not separator or not job_text or not path_text:
        raise argparse.ArgumentTypeError("expected SOURCE_JOB=PATH")
    return int(job_text, 0), Path(path_text)


def build_cross_rows(
    live_rows: list[dict[str, Any]], predictor_rows: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    live_groups: dict[tuple[int, int, int], list[dict[str, Any]]] = defaultdict(list)
    predictor_groups: dict[tuple[int, int, int], list[dict[str, Any]]] = defaultdict(list)
    for row in live_rows:
        live_groups[(row["source_exec_job_id"], row["actor_slot"], row["target_slot"])].append(row)
    for row in predictor_rows:
        predictor_groups[(row["source_exec_job_id"], row["actor_slot"], row["target_slot"])].append(row)
    result: list[dict[str, Any]] = []
    for key in sorted(set(live_groups) | set(predictor_groups)):
        live = live_groups.get(key, [])
        predictor = predictor_groups.get(key, [])
        for occurrence in range(max(len(live), len(predictor))):
            live_row = live[occurrence] if occurrence < len(live) else {}
            predictor_row = predictor[occurrence] if occurrence < len(predictor) else {}
            result.append(
                {
                    "source_exec_job_id": key[0],
                    "actor_slot": key[1],
                    "target_slot": key[2],
                    "occurrence": occurrence,
                    "pair_status": "matched" if live_row and predictor_row else "live_only" if live_row else "predictor_only",
                    "live_invocation_index": live_row.get("invocation_index", ""),
                    "live_sequence": live_row.get("capture_sequence", ""),
                    "live_vi_field": live_row.get("vi_field", ""),
                    "live_previous_frame_index": live_row.get("previous_frame_index", ""),
                    "live_action_view_thread_indices": live_row.get("pathing_action_view_thread_indices", ""),
                    "live_exact_writes_since_previous_frame": live_row.get("exact_position_writes_since_previous_frame", ""),
                    "predictor_action_ordinal": predictor_row.get("action_ordinal", ""),
                    "predictor_sequence": predictor_row.get("predictor_sequence", ""),
                    "predictor_frame_index": predictor_row.get("predictor_frame_index", ""),
                    "predictor_worker_visits_before_pathing": predictor_row.get("worker_visits_before_pathing", ""),
                    "predictor_worker_slots_before_pathing": predictor_row.get("worker_slots_before_pathing", ""),
                    "predictor_previous_worker_slot": predictor_row.get("previous_worker_slot", ""),
                    "predictor_next_worker_slot": predictor_row.get("next_worker_slot", ""),
                }
            )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", action="append", required=True, type=parse_job_path)
    parser.add_argument("--predictor", action="append", default=[], type=parse_job_path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    tables: dict[str, list[dict[str, Any]]] = {
        "frame_rows": [],
        "write_rows": [],
        "invocation_rows": [],
        "visibility_rows": [],
    }
    summaries: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    for source_job, capture_path in args.capture:
        reduced = reduce_event_stream(iter_jsonl(capture_path), source_job)
        for name in tables:
            tables[name].extend(reduced[name])
        summaries.append(reduced["summary"])
        failures.extend(
            {"source_exec_job_id": source_job, **failure}
            for failure in reduced["list_failures"]
        )

    predictor_rows: list[dict[str, Any]] = []
    for source_job, predictor_path in args.predictor:
        predictor_rows.extend(reduce_predictor(predictor_path, source_job))

    write_csv(args.output / "frame_thread_timeline.csv", tables["frame_rows"])
    write_csv(args.output / "position_write_timeline.csv", tables["write_rows"])
    write_csv(args.output / "pathing_invocation_windows.csv", tables["invocation_rows"])
    write_csv(args.output / "pathing_slot_visibility.csv", tables["visibility_rows"])
    write_csv(args.output / "predictor_pathing_cursor.csv", predictor_rows)
    write_csv(
        args.output / "pathing_live_predictor_pairing.csv",
        build_cross_rows(tables["invocation_rows"], predictor_rows),
    )
    write_csv(args.output / "thread_list_failures.csv", failures)
    aggregate = {
        "jobs": summaries,
        "total_frames": sum(item["frames"] for item in summaries),
        "total_pathing_invocations": sum(item["pathing_invocations"] for item in summaries),
        "total_exact_position_writes": sum(item["exact_position_writes"] for item in summaries),
        "total_unresolved_exact_position_writes": sum(item["unresolved_exact_position_writes"] for item in summaries),
        "total_thread_list_failures": sum(item["thread_list_failures"] for item in summaries),
    }
    (args.output / "summary.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(aggregate, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
