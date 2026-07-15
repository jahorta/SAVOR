#!/usr/bin/env python3
"""Reduce movement destination, direction, and stop-frame capture evidence."""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
from pathlib import Path
from typing import Any, Iterable

import reduce_predictor_live_comparison as common


FRAME_ID = "battle_case5_after_threads_8000A2FC"
COMMIT_PREFIX = "movement_commit_callsite_"
SETUP_ID = "action_motion_setup_complete_8001FC04"
TARGET_ID = "action_motion_target_return_8001FADC"
FOCUS_SLOTS = (0, 1, 4, 5)
PHASE_PATTERNS = (
    ("frame_end", re.compile(r"battle_case5")),
    ("path_reachability", re.compile(r"movement_reachability")),
    ("path_extract", re.compile(r"movement_path_extract")),
    ("path_select", re.compile(r"movement_path_index_adjust")),
    ("grid_commit", re.compile(r"movement_commit_(?:callsite|post_call)")),
    ("posholder_publish", re.compile(r"movement_posholder")),
    ("motion_target", re.compile(r"action_motion_target")),
    ("motion_setup", re.compile(r"action_motion_setup")),
    ("motion_clamp", re.compile(r"action_motion_clamp")),
    ("motion_stop", re.compile(r"action_motion_(?:final_result|caller_consumption)")),
)
DESTINATION_SOURCE_BY_CALLSITE = {
    "0X80086480": "SelectedPathNode",
    "0X80086698": "SelectedPathNode",
    "0X8008816C": "SelectedPathNode",
    "0X800883CC": "SelectedPathNode",
    "0X8008C67C": "GeneratedSingleSquare",
    "0X8008C844": "GeneratedSingleSquare",
    "0X8008C920": "GeneratedSingleSquare",
    "0X800871C4": "ExplicitGrid",
    "0X800879A8": "ExplicitGrid",
    "0X8008D56C": "ExplicitGrid",
}
CONTROLLER_FAMILY_BY_CALLSITE = {
    0x80086480: "ActivePcDirect",
    0x80086698: "ActivePcDirect",
    0x8008816C: "EnemyDirect",
    0x800883CC: "EnemyDirect",
    0x80088434: "EnemyDirect",
    0x800879A8: "EnemyFallback",
    0x8008C67C: "AmbientPursuit",
    0x8008C844: "AmbientFormation",
    0x8008C920: "AmbientFormation",
    0x8008D56C: "UnsupportedSpecialReaction",
}
EXPECTED_CALLBACKS_BY_FAMILY = {
    "ActivePcDirect": (0x80086308, 0x80086308),
    "EnemyDirect": (0x8008B9E0, 0x80087F6C),
    "EnemyFallback": (0x80087844, 0x80087844),
    "AmbientPursuit": (0x8008C21C, 0x8008C21C),
    "AmbientFormation": (0x8008C7B0, 0x8008C7B0),
}


def integer(value: Any) -> int | None:
    return common.integer(value)


def boolean(value: Any) -> bool | None:
    return common.boolean(value)


def bits_to_float(bits: int | None) -> float | None:
    if bits is None:
        return None
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def hex32(value: int | None) -> str:
    return common.hex32(value)


def sample(event: dict[str, Any], name: str) -> int | None:
    return common.sample_bits(event, name)


def phase_name(checkpoint_id: str) -> str | None:
    for name, pattern in PHASE_PATTERNS:
        if pattern.search(checkpoint_id):
            return name
    return None


def active_slots(event: dict[str, Any]) -> list[int]:
    slots: list[int] = []
    for root in range(12):
        slot = sample(event, f"root{root}_iw_slot_0x00")
        worksheet = sample(event, f"root{root}_combatant_worksheet_ptr")
        if (
            slot in FOCUS_SLOTS
            and worksheet is not None
            and 0x80000000 <= worksheet < 0x81800000
            and slot not in slots
        ):
            slots.append(slot)
    return slots


def slot_for_event(event: dict[str, Any]) -> int | None:
    for key in ("slot", "inst_slot_0x00"):
        value = sample(event, key)
        if value in range(12):
            return value
    checkpoint_id = str(event.get("checkpoint_id", ""))
    if checkpoint_id == FRAME_ID:
        return None
    candidates = active_slots(event)
    if len(candidates) == 1:
        return candidates[0]
    return None


def path_entries(event: dict[str, Any]) -> list[tuple[int, int]]:
    distance = sample(event, "movement_dist_to_target_0x14") or 0
    entries: list[tuple[int, int]] = []
    for index in range(min(distance, 11)):
        offset = 0x17 + index * 2
        x = sample(event, f"movement_path_node{index}_x_0x{offset:02x}")
        z = sample(event, f"movement_path_node{index}_z_0x{offset + 1:02x}")
        if x is None or z is None or x == 0xFF:
            break
        entries.append((x, z))
    return entries


def slot_path_entries(event: dict[str, Any], slot: int) -> list[tuple[int, int]]:
    distance = sample(event, f"slot{slot}_movement_distance_0x14") or 0
    entries: list[tuple[int, int]] = []
    for index in range(min(distance, 11)):
        x = sample(event, f"slot{slot}_movement_path_node{index}_x")
        z = sample(event, f"slot{slot}_movement_path_node{index}_z")
        if x is None or z is None or x == 0xFF:
            break
        entries.append((x, z))
    return entries


def movement_worksheet_slot(event: dict[str, Any]) -> int | None:
    pointer = sample(event, "movement_worksheet")
    if pointer is None:
        return None
    for slot in range(12):
        if sample(event, f"slot{slot}_movement_worksheet_ptr") == pointer:
            return slot
    return None


def root_motion(event: dict[str, Any], slot: int) -> dict[str, Any]:
    for root in range(12):
        if sample(event, f"root{root}_iw_slot_0x00") != slot:
            continue
        worksheet = sample(event, f"root{root}_combatant_worksheet_ptr")
        if worksheet is None or not 0x80000000 <= worksheet < 0x81800000:
            continue
        return {
            "root": root,
            "mode": sample(event, f"root{root}_iw_action_mode_0x06"),
            "current_bits": tuple(
                sample(event, f"root{root}_cw_cur_{axis}_{offset}")
                for axis, offset in (("x", "0x1c"), ("y", "0x20"), ("z", "0x24"))
            ),
            "increment_bits": tuple(
                sample(event, f"root{root}_iw_move_inc_{axis}_{offset}")
                for axis, offset in (("x", "0x104"), ("y", "0x108"), ("z", "0x10c"))
            ),
            "target_bits": tuple(
                sample(event, f"root{root}_iw_target_{axis}_{offset}")
                for axis, offset in (("x", "0x110"), ("y", "0x114"), ("z", "0x118"))
            ),
            "speed_bits": sample(event, f"root{root}_iw_speed_0x12c"),
            "alt_speed_bits": sample(event, f"root{root}_iw_alt_speed_0x130"),
        }
    return {}


def vector_bits_text(bits: tuple[int | None, ...] | None) -> str:
    if bits is None:
        return ""
    return ",".join(hex32(value) for value in bits)


def vector_float_text(bits: tuple[int | None, ...] | None) -> str:
    if bits is None or any(value is None for value in bits):
        return ""
    values = tuple(bits_to_float(value) for value in bits)
    if any(value is None or not math.isfinite(value) for value in values):
        return ""
    return ",".join(f"{value:.6f}" for value in values if value is not None)


def event_timeline(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    frame_ordinal = 0
    current_path_actor: int | None = None
    for event in events:
        checkpoint_id = str(event.get("checkpoint_id", ""))
        phase = phase_name(checkpoint_id)
        if phase is None:
            continue
        actor_slot_arg = sample(event, "actor_slot")
        worksheet_slot = movement_worksheet_slot(event)
        if checkpoint_id == "movement_reachability_entry_80083728" and actor_slot_arg in range(12):
            current_path_actor = actor_slot_arg
        elif worksheet_slot is not None:
            current_path_actor = worksheet_slot
        if checkpoint_id == FRAME_ID:
            frame_ordinal += 1
            slots: Iterable[int | None] = active_slots(event)
        else:
            slot = slot_for_event(event)
            if slot is None and phase in ("path_reachability", "path_extract", "path_select"):
                slot = current_path_actor
            slots = (slot,)
        for slot in slots:
            motion = {} if slot is None else root_motion(event, slot)
            path_index = sample(event, "movement_path_index_0x15")
            entries = path_entries(event)
            current_grid_x = sample(event, "movement_cur_x_0x0c")
            current_grid_z = sample(event, "movement_cur_z_0x0d")
            distance = sample(event, "movement_dist_to_target_0x14")
            reachability = sample(event, "movement_status_0x16")
            if slot is not None:
                if current_grid_x is None:
                    current_grid_x = sample(event, f"slot{slot}_movement_current_x_0x0c")
                if current_grid_z is None:
                    current_grid_z = sample(event, f"slot{slot}_movement_current_z_0x0d")
                if distance is None:
                    distance = sample(event, f"slot{slot}_movement_distance_0x14")
                if path_index is None:
                    path_index = sample(event, f"slot{slot}_movement_path_index_0x15")
                if reachability is None:
                    reachability = sample(event, f"slot{slot}_movement_reachability_status_0x16")
                if not entries:
                    entries = slot_path_entries(event, slot)
            selected = (
                entries[path_index]
                if path_index is not None and 0 <= path_index < len(entries)
                else None
            )
            rows.append({
                "source_exec_job_id": job,
                "capture_sequence": integer(event.get("capture_sequence")),
                "frame_count": integer(event.get("frame_count")),
                "case5_frame_ordinal": frame_ordinal if checkpoint_id == FRAME_ID else "",
                "phase": phase,
                "checkpoint_id": checkpoint_id,
                "pc": event.get("pc", ""),
                "slot": "" if slot is None else slot,
                "actor_slot_arg": actor_slot_arg,
                "target_slot_arg": sample(event, "target_slot"),
                "current_grid_x": current_grid_x,
                "current_grid_z": current_grid_z,
                "next_grid_x": sample(event, "next_grid_x"),
                "next_grid_z": sample(event, "next_grid_z"),
                "distance": distance,
                "path_index": path_index,
                "reachability_status": reachability,
                "selected_path_node": "" if selected is None else f"{selected[0]},{selected[1]}",
                "destination_source": (
                    classify_destination(str(event.get("pc", "")), (None, None), (None, None), selected)
                    if checkpoint_id.startswith(COMMIT_PREFIX)
                    else ""
                ),
                "action_mode": motion.get("mode", ""),
                "current_position_bits": vector_bits_text(motion.get("current_bits")),
                "move_increment_bits": vector_bits_text(motion.get("increment_bits")),
                "motion_target_bits": vector_bits_text(motion.get("target_bits")),
                "thread_list_node_count": integer(event.get("thread_list_node_count")) if checkpoint_id == FRAME_ID else "",
                "thread_list_read_ok": boolean(event.get("thread_list_read_ok")) if checkpoint_id == FRAME_ID else "",
                "thread_list_truncated": boolean(event.get("thread_list_truncated")) if checkpoint_id == FRAME_ID else "",
                "thread_list_cycle_detected": boolean(event.get("thread_list_cycle_detected")) if checkpoint_id == FRAME_ID else "",
            })
    return rows


def next_same_slot_event(
    events: list[dict[str, Any]],
    start_index: int,
    slot: int,
    checkpoint_id: str,
) -> dict[str, Any] | None:
    for event in events[start_index + 1:]:
        candidate_id = str(event.get("checkpoint_id", ""))
        if candidate_id.startswith(COMMIT_PREFIX) and slot_for_event(event) == slot:
            return None
        if candidate_id == checkpoint_id:
            candidate_slot = sample(event, "inst_slot_0x00")
            if candidate_slot is None:
                candidate_slot = slot_for_event(event)
            if candidate_slot == slot:
                return event
    return None


def grid_to_raw_bits(grid: int, footprint: int) -> int:
    adjustment = 0 if footprint <= 1 else 7 if footprint == 2 else 15 if footprint == 3 else (footprint - 1) * 7
    value = float(grid * 15 - 75 + adjustment)
    return struct.unpack(">I", struct.pack(">f", value))[0]


def classify_destination(
    callsite_pc: str,
    current: tuple[int | None, int | None],
    destination: tuple[int | None, int | None],
    selected: tuple[int, int] | None,
) -> str:
    del current, destination, selected
    return DESTINATION_SOURCE_BY_CALLSITE.get(callsite_pc.upper(), "Unknown")


def capture_integrity(job: int, events: list[dict[str, Any]]) -> dict[str, Any]:
    frames = [event for event in events if event.get("checkpoint_id") == FRAME_ID]
    snapshots = [event for event in frames if "thread_list_read_ok" in event]
    return {
        "source_exec_job_id": job,
        "frame_hits": len(frames),
        "frame_cap_reached": len(frames) >= 2400,
        "list_snapshots": len(snapshots),
        "list_read_failures": sum(boolean(event.get("thread_list_read_ok")) is not True for event in snapshots),
        "list_truncations": sum(boolean(event.get("thread_list_truncated")) is True for event in snapshots),
        "list_cycles": sum(boolean(event.get("thread_list_cycle_detected")) is True for event in snapshots),
        "max_list_nodes": max((integer(event.get("thread_list_node_count")) or 0 for event in snapshots), default=0),
    }


def thread_payload_matches(
    frame: dict[str, Any] | None,
    slot: int,
) -> tuple[int | None, list[dict[str, Any]]]:
    if frame is None:
        return None, []
    worksheet = sample(frame, f"slot{slot}_movement_worksheet_ptr")
    if worksheet is None:
        return None, []
    nodes = frame.get("thread_list_nodes", [])
    if not isinstance(nodes, list):
        return worksheet, []
    matches = [
        node
        for node in nodes
        if isinstance(node, dict)
        and boolean(node.get("payload_word_read_ok")) is True
        and integer(node.get("payload_word")) == worksheet
    ]
    return worksheet, matches


def prior_path_request(
    events: list[dict[str, Any]],
    commit_index: int,
    slot: int,
) -> dict[str, Any] | None:
    for candidate in reversed(events[:commit_index]):
        checkpoint_id = str(candidate.get("checkpoint_id", ""))
        if checkpoint_id.startswith(COMMIT_PREFIX) and slot_for_event(candidate) == slot:
            break
        if checkpoint_id != "movement_reachability_entry_80083728":
            continue
        if sample(candidate, "actor_slot") == slot:
            return candidate
    return None


def invocation_windows(
    job: int,
    events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    frames: list[tuple[int, dict[str, Any]]] = []
    for event in events:
        if event.get("checkpoint_id") == FRAME_ID:
            frames.append((len(frames) + 1, event))

    rows: list[dict[str, Any]] = []
    leg_counts: dict[tuple[int, int, str, int], int] = {}
    last_target_by_controller: dict[tuple[int, int, str], int] = {}
    for event_index, event in enumerate(events):
        checkpoint_id = str(event.get("checkpoint_id", ""))
        if not checkpoint_id.startswith(COMMIT_PREFIX):
            continue
        sequence = integer(event.get("capture_sequence")) or 0
        slot = slot_for_event(event)
        callsite = integer(event.get("pc"))
        if slot is None:
            continue
        previous = next(
            (
                (ordinal, frame)
                for ordinal, frame in reversed(frames)
                if (integer(frame.get("capture_sequence")) or 0) < sequence
            ),
            None,
        )
        following = next(
            (
                (ordinal, frame)
                for ordinal, frame in frames
                if (integer(frame.get("capture_sequence")) or 0) > sequence
            ),
            None,
        )
        previous_ordinal, previous_frame = previous or (None, None)
        following_ordinal, following_frame = following or (None, None)
        previous_worksheet, previous_matches = thread_payload_matches(
            previous_frame, slot
        )
        following_worksheet, following_matches = thread_payload_matches(
            following_frame, slot
        )
        before_node = previous_matches[0] if len(previous_matches) == 1 else None
        after_node = following_matches[0] if len(following_matches) == 1 else None
        before_callback = None if before_node is None else integer(before_node.get("callback"))
        after_callback = None if after_node is None else integer(after_node.get("callback"))
        family = CONTROLLER_FAMILY_BY_CALLSITE.get(callsite, "Unknown")
        expected_callbacks = EXPECTED_CALLBACKS_BY_FAMILY.get(family)
        action_ordinal = sample(event, "action_sequence_80347335")
        if action_ordinal is None:
            action_ordinal = -1
        path_request = prior_path_request(events, event_index, slot)
        semantic_target = None if path_request is None else sample(path_request, "target_slot")
        if family == "AmbientFormation":
            semantic_target = None
        controller_key = (action_ordinal, slot, family)
        if family == "AmbientFormation":
            semantic_target = None
        elif semantic_target is None:
            semantic_target = last_target_by_controller.get(controller_key)
        else:
            last_target_by_controller[controller_key] = semantic_target
        key = (action_ordinal, slot, family, semantic_target if semantic_target is not None else -1)
        leg = leg_counts.get(key, 0)
        leg_counts[key] = leg + 1
        callback_match = (
            None
            if expected_callbacks is None
            else before_callback == expected_callbacks[0]
            and after_callback == expected_callbacks[1]
        )
        rows.append({
            "source_exec_job_id": job,
            "commit_sequence": sequence,
            "callsite_pc": hex32(callsite),
            "action_ordinal": action_ordinal,
            "active_actor_slot": sample(event, "active_actor_slot_80347334"),
            "slot": slot,
            "semantic_target_slot": "" if semantic_target is None else semantic_target,
            "controller_family": family,
            "leg": leg,
            "destination_source": DESTINATION_SOURCE_BY_CALLSITE.get(
                hex32(callsite).upper(), "Unknown"
            ),
            "previous_frame_ordinal": "" if previous_ordinal is None else previous_ordinal,
            "previous_frame_sequence": "" if previous_frame is None else integer(previous_frame.get("capture_sequence")),
            "next_frame_ordinal": "" if following_ordinal is None else following_ordinal,
            "next_frame_sequence": "" if following_frame is None else integer(following_frame.get("capture_sequence")),
            "frame_bracketed": previous is not None and following is not None,
            "previous_worksheet_ptr": hex32(previous_worksheet),
            "next_worksheet_ptr": hex32(following_worksheet),
            "previous_payload_match_count": len(previous_matches),
            "next_payload_match_count": len(following_matches),
            "unique_payload_to_slot_mapping": len(previous_matches) == 1 and len(following_matches) == 1,
            "previous_thread_index": "" if before_node is None else integer(before_node.get("index")),
            "next_thread_index": "" if after_node is None else integer(after_node.get("index")),
            "previous_callback": hex32(before_callback),
            "next_callback": hex32(after_callback),
            "callback_transition": f"{hex32(before_callback)}->{hex32(after_callback)}",
            "callback_transition_matches_family": callback_match,
            "assignment_status": (
                "Matched"
                if family in ("ActivePcDirect", "AmbientPursuit")
                and callback_match is True
                else "Provisional"
                if family in ("EnemyDirect", "AmbientFormation", "EnemyFallback")
                and callback_match is True
                else "Unknown"
            ),
        })
    return rows


def destination_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event_index, event in enumerate(events):
        checkpoint_id = str(event.get("checkpoint_id", ""))
        if not checkpoint_id.startswith(COMMIT_PREFIX):
            continue
        slot = slot_for_event(event)
        if slot is None:
            continue
        current = (
            sample(event, "movement_cur_x_0x0c"),
            sample(event, "movement_cur_z_0x0d"),
        )
        destination = (sample(event, "next_grid_x"), sample(event, "next_grid_z"))
        index = sample(event, "movement_path_index_0x15")
        entries = path_entries(event)
        selected = entries[index] if index is not None and 0 <= index < len(entries) else None
        setup = next_same_slot_event(events, event_index, slot, SETUP_ID)
        target = next_same_slot_event(events, event_index, slot, TARGET_ID)
        width = sample(event, f"slot{slot}_movement_width_0xac") or 1
        depth = sample(event, f"slot{slot}_movement_depth_0xad") or width
        expected_target_bits = None
        if None not in destination:
            expected_target_bits = (
                grid_to_raw_bits(int(destination[0]), width),
                0,
                grid_to_raw_bits(int(destination[1]), depth),
            )
        setup_bits = None if setup is None else (
            sample(setup, "inst_target_x_0x110"),
            sample(setup, "inst_target_y_0x114"),
            sample(setup, "inst_target_z_0x118"),
        )
        target_bits = None if target is None else (
            sample(target, "target_vector_x_bits"),
            sample(target, "target_vector_y_bits"),
            sample(target, "target_vector_z_bits"),
        )
        posholder_bits = None if setup is None else (
            sample(setup, f"slot{slot}_posholder_x_bits"),
            0,
            sample(setup, f"slot{slot}_posholder_z_bits"),
        )
        rows.append({
            "source_exec_job_id": job,
            "commit_sequence": integer(event.get("capture_sequence")),
            "callsite_pc": event.get("pc", ""),
            "slot": slot,
            "current_grid": f"{current[0]},{current[1]}",
            "destination_grid": f"{destination[0]},{destination[1]}",
            "distance": sample(event, "movement_dist_to_target_0x14"),
            "path_index": index,
            "selected_path_node": "" if selected is None else f"{selected[0]},{selected[1]}",
            "destination_source": classify_destination(
                str(event.get("pc", "")), current, destination, selected),
            "selected_path_value_matches": selected is not None and destination == selected,
            "setup_sequence": "" if setup is None else integer(setup.get("capture_sequence")),
            "setup_mode": "" if setup is None else sample(setup, "inst_action_mode_0x06"),
            "expected_target_bits": vector_bits_text(expected_target_bits),
            "posholder_bits_at_setup": vector_bits_text(posholder_bits),
            "target_helper_bits": vector_bits_text(target_bits),
            "setup_target_bits": vector_bits_text(setup_bits),
            "posholder_matches_expected": None if expected_target_bits is None or posholder_bits is None else posholder_bits == expected_target_bits,
            "target_helper_matches_expected": None if expected_target_bits is None or target_bits is None else target_bits == expected_target_bits,
            "setup_target_matches_expected": None if expected_target_bits is None or setup_bits is None else setup_bits == expected_target_bits,
        })
    return rows


def stop_rows(
    job: int,
    live_frames: list[dict[str, Any]],
    predictor: dict[str, Any],
) -> list[dict[str, Any]]:
    modeled = common.predictor_frames(predictor)
    live_steps = common.movement_steps(
        (frame["ordinal"], frame["positions"]) for frame in live_frames
    )
    predictor_steps = common.movement_steps(
        (frame, modeled[frame]) for frame in sorted(modeled)
    )
    rows: list[dict[str, Any]] = []
    for slot in FOCUS_SLOTS:
        live_segments = common.movement_segments(live_steps[slot])
        predictor_segments = common.movement_segments(predictor_steps[slot])
        for segment_index in range(max(len(live_segments), len(predictor_segments))):
            live = live_segments[segment_index] if segment_index < len(live_segments) else []
            predicted = predictor_segments[segment_index] if segment_index < len(predictor_segments) else []
            live_terminal = None if not live else live[-1]["position"]
            predicted_terminal = None if not predicted else predicted[-1]["position"]
            rows.append({
                "source_exec_job_id": job,
                "slot": slot,
                "segment_index": segment_index,
                "live_start_frame": "" if not live else live[0]["frame"],
                "predictor_start_frame": "" if not predicted else predicted[0]["frame"],
                "onset_frame_offset": "" if not live or not predicted else live[0]["frame"] - predicted[0]["frame"],
                "live_stop_frame": "" if not live else live[-1]["frame"],
                "predictor_stop_frame": "" if not predicted else predicted[-1]["frame"],
                "live_step_count": len(live),
                "predictor_step_count": len(predicted),
                "step_count_match": len(live) == len(predicted),
                "live_terminal_position": common.vector_text(live_terminal),
                "predictor_terminal_position": common.vector_text(predicted_terminal),
                "terminal_position_match": common.vector_matches(live_terminal, predicted_terminal),
                "fully_matches_after_onset_alignment": (
                    len(live) == len(predicted)
                    and common.vector_matches(live_terminal, predicted_terminal) is True
                    and all(
                        common.vector_matches(a["delta"], b["delta"]) is True
                        for a, b in zip(live, predicted)
                    )
                ),
            })
    return rows


SemanticKey = tuple[int, int, str, int, int]


def float_vector_bits_text(
    vector: tuple[float, float, float] | None,
) -> str:
    if vector is None:
        return ""
    return ",".join(
        hex32(struct.unpack(">I", struct.pack(">f", value))[0])
        for value in vector
    )


def live_semantic_segments(
    live_frames: list[dict[str, Any]],
    windows: list[dict[str, Any]],
) -> dict[SemanticKey, list[dict[str, Any]]]:
    live_steps = common.movement_steps(
        (frame["ordinal"], frame["positions"]) for frame in live_frames
    )
    available = [dict(row, _used=False) for row in windows]
    result: dict[SemanticKey, list[dict[str, Any]]] = {}
    for slot in FOCUS_SLOTS:
        for segment in common.movement_segments(live_steps[slot]):
            if not segment:
                continue
            start_frame = segment[0]["frame"]
            candidates = [
                row
                for row in available
                if not row["_used"]
                and integer(row.get("slot")) == slot
                and integer(row.get("next_frame_ordinal")) is not None
                and int(row["next_frame_ordinal"]) <= start_frame
            ]
            window = max(
                candidates,
                key=lambda row: (
                    int(row["next_frame_ordinal"]),
                    integer(row.get("commit_sequence")) or 0,
                ),
                default=None,
            )
            if window is None:
                continue
            window["_used"] = True
            semantic_target = integer(window.get("semantic_target_slot"))
            key: SemanticKey = (
                integer(window.get("action_ordinal")) or 0,
                slot,
                str(window.get("controller_family", "Unknown")),
                -1 if semantic_target is None else semantic_target,
                integer(window.get("leg")) or 0,
            )
            result[key] = segment
    return result


def predictor_semantic_segments(
    predictor: dict[str, Any],
) -> dict[SemanticKey, list[dict[str, Any]]]:
    leg_by_base: dict[tuple[int, int, str, int], int] = {}
    result: dict[SemanticKey, list[dict[str, Any]]] = {}
    for event in predictor.get("events", []):
        if not isinstance(event, dict) or event.get("phase") != "frame_scheduler":
            continue
        family = str(event.get("movement_controller_family", "Unknown"))
        action_ordinal = integer(event.get("action_ordinal"))
        slot = integer(event.get("actor_slot"))
        target = integer(event.get("target_slot"))
        if family == "Unknown" or action_ordinal is None or slot is None or target is None:
            continue
        base = (action_ordinal, slot, family, target)
        detail = str(event.get("detail", ""))
        if "step_kind=MovementCommit" in detail:
            leg_by_base[base] = leg_by_base.get(base, -1) + 1
            continue
        if "step_kind=MoveIncrementApply_80061340" not in detail:
            continue
        match = common.POSITION_RE.search(detail)
        if match is None:
            continue
        old_position = common.parse_vector(match.group(1))
        new_position = common.parse_vector(match.group(2))
        delta = common.vector_delta(new_position, old_position)
        speed = common.vector_magnitude(delta)
        if old_position is None or new_position is None or speed is None or speed <= 0.0001:
            continue
        leg = leg_by_base.get(base, 0)
        key: SemanticKey = (*base, leg)
        result.setdefault(key, []).append({
            "frame": integer(event.get("frame_index")),
            "position": new_position,
            "delta": delta,
            "speed": speed,
        })
    return result


def compare_semantic_segment_maps(
    job: int,
    live: dict[SemanticKey, list[dict[str, Any]]],
    predictor: dict[SemanticKey, list[dict[str, Any]]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for key in sorted(set(live) | set(predictor)):
        action_ordinal, slot, family, target, leg = key
        live_segment = live.get(key, [])
        predictor_segment = predictor.get(key, [])
        paired = bool(live_segment and predictor_segment)
        covered_family = family in ("ActivePcDirect", "AmbientPursuit")
        vectors_match = (
            paired
            and len(live_segment) == len(predictor_segment)
            and all(
                common.vector_matches(left["delta"], right["delta"]) is True
                for left, right in zip(live_segment, predictor_segment)
            )
        )
        live_terminal = None if not live_segment else live_segment[-1]["position"]
        predictor_terminal = (
            None if not predictor_segment else predictor_segment[-1]["position"]
        )
        terminal_match = common.vector_matches(live_terminal, predictor_terminal)
        terminal_bits_match = (
            paired
            and float_vector_bits_text(live_terminal)
            == float_vector_bits_text(predictor_terminal)
        )
        step_count_match = paired and len(live_segment) == len(predictor_segment)
        fully_matches = step_count_match and vectors_match and terminal_bits_match
        rows.append({
            "source_exec_job_id": job,
            "action_ordinal": action_ordinal,
            "slot": slot,
            "controller_family": family,
            "semantic_target_slot": target,
            "leg": leg,
            "status": (
                "Matched"
                if paired and covered_family and fully_matches
                else "Mismatch"
                if paired and covered_family
                else "Provisional"
                if paired
                else "Unpaired"
            ),
            "model_coverage": "Covered" if covered_family else "Provisional",
            "live_start_frame": "" if not live_segment else live_segment[0]["frame"],
            "predictor_start_frame": "" if not predictor_segment else predictor_segment[0]["frame"],
            "onset_frame_offset": (
                ""
                if not paired
                else live_segment[0]["frame"] - predictor_segment[0]["frame"]
            ),
            "live_step_count": len(live_segment),
            "predictor_step_count": len(predictor_segment),
            "step_count_match": step_count_match,
            "full_step_vectors_match": vectors_match,
            "live_terminal_position": common.vector_text(live_terminal),
            "predictor_terminal_position": common.vector_text(predictor_terminal),
            "terminal_position_match": terminal_match,
            "live_terminal_bits": float_vector_bits_text(live_terminal),
            "predictor_terminal_bits": float_vector_bits_text(predictor_terminal),
            "terminal_bits_match": terminal_bits_match,
            "fully_matches_after_onset_alignment": (
                fully_matches
            ),
        })
    return rows


def semantic_segment_rows(
    job: int,
    events: list[dict[str, Any]],
    predictor: dict[str, Any],
    windows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return compare_semantic_segment_maps(
        job,
        live_semantic_segments(common.live_frames(events), windows),
        predictor_semantic_segments(predictor),
    )


def affected_target_reaction_rows(
    job: int,
    events: list[dict[str, Any]],
    predictor: dict[str, Any],
) -> list[dict[str, Any]]:
    live_groups: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for frame in common.live_frames(events):
        action = integer(frame.get("action_sequence"))
        if action is None:
            continue
        event = frame.get("event")
        if not isinstance(event, dict):
            continue
        for slot in FOCUS_SLOTS:
            _, matches = thread_payload_matches(event, slot)
            if len(matches) != 1:
                continue
            callback = integer(matches[0].get("callback"))
            if callback not in (0x8008D3B0, 0x8008CDA8):
                continue
            live_groups.setdefault((action, slot), []).append(
                (integer(frame.get("ordinal")) or 0, callback)
            )

    predictor_groups: dict[tuple[int, int], list[dict[str, Any]]] = {}
    predictor_events = [
        event for event in predictor.get("events", []) if isinstance(event, dict)
    ]
    for event in predictor_events:
        if event.get("movement_controller_family") != "AffectedTargetReaction" \
                and event.get("movement_relation_route") != "AffectedTarget1":
            continue
        action = integer(event.get("action_ordinal"))
        slot = integer(event.get("actor_slot"))
        if action is None or slot is None:
            continue
        predictor_groups.setdefault((action, slot), []).append(event)

    rows: list[dict[str, Any]] = []
    for action, slot in sorted(set(live_groups) | set(predictor_groups)):
        live = live_groups.get((action, slot), [])
        modeled = predictor_groups.get((action, slot), [])
        completion = next(
            (
                event for event in predictor_events
                if integer(event.get("action_ordinal")) == action
                and event.get("label") == "action_complete"
            ),
            None,
        )
        labels = [str(event.get("label", "")) for event in modeled]
        rows.append({
            "source_exec_job_id": job,
            "action_ordinal": action,
            "slot": slot,
            "relation_route": "AffectedTarget1",
            "callback_family": "AffectedTargetReaction",
            "live_first_frame": live[0][0] if live else "",
            "live_last_frame": live[-1][0] if live else "",
            "live_callbacks": "|".join(hex32(callback) for _, callback in live),
            "live_action_bounded": bool(live),
            "predictor_first_frame": (
                integer(modeled[0].get("frame_index")) if modeled else ""
            ),
            "predictor_last_frame": (
                integer(modeled[-1].get("frame_index")) if modeled else ""
            ),
            "predictor_labels": "|".join(labels),
            "predictor_waited_for_result": "passive_completion_deferred" in labels,
            "predictor_death_clear": "passive_death_clear" in labels,
            "predictor_action_complete_frame": (
                integer(completion.get("frame_index")) if completion else ""
            ),
            "predictor_action_bounded": bool(modeled) and completion is not None,
            "pairing_status": (
                "Paired" if live and modeled else "LiveOnly" if live else "PredictorOnly"
            ),
        })
    return rows


def predictor_path(directory: Path, job: int) -> Path:
    return common.predictor_path(directory, job)


def load_predictor(directory: Path, job: int) -> dict[str, Any]:
    document = json.loads(predictor_path(directory, job).read_text(encoding="utf-8-sig"))
    return document.get("prediction", document)


def findings_text(
    destinations: list[dict[str, Any]],
    steps: list[dict[str, Any]],
    stops: list[dict[str, Any]],
    integrity: list[dict[str, Any]],
    invocations: list[dict[str, Any]],
    semantic_segments: list[dict[str, Any]],
    affected_reactions: list[dict[str, Any]],
) -> str:
    selected = sum(row["destination_source"] == "SelectedPathNode" for row in destinations)
    generated = sum(row["destination_source"] == "GeneratedSingleSquare" for row in destinations)
    explicit = sum(row["destination_source"] == "ExplicitGrid" for row in destinations)
    unknown = sum(row["destination_source"] == "Unknown" for row in destinations)
    semantic_non_path = [row for row in destinations if row["destination_source"] != "SelectedPathNode"]
    comparable_steps = [row for row in steps if row.get("vector_match") is not None]
    comparable_stops = [row for row in stops if row.get("terminal_position_match") is not None]
    posholder_matches = sum(row.get("posholder_matches_expected") is True for row in destinations)
    setup_matches = sum(row.get("setup_target_matches_expected") is True for row in destinations)
    mode6 = sum(row.get("setup_mode") == 6 for row in destinations)
    mode13 = sum(row.get("setup_mode") == 0x13 for row in destinations)
    integrity_failures = sum(
        row["list_read_failures"] + row["list_truncations"] + row["list_cycles"]
        for row in integrity
    )
    family_counts = {
        family: sum(row.get("controller_family") == family for row in invocations)
        for family in (
            "ActivePcDirect",
            "AmbientPursuit",
            "AmbientFormation",
            "EnemyDirect",
        )
    }
    expected_family_counts = {
        "ActivePcDirect": 9,
        "AmbientPursuit": 50,
        "AmbientFormation": 13,
        "EnemyDirect": 5,
    }
    mapped = sum(
        row.get("unique_payload_to_slot_mapping") is True for row in invocations
    )
    bracketed = sum(row.get("frame_bracketed") is True for row in invocations)
    transition_matches = sum(
        row.get("callback_transition_matches_family") is True for row in invocations
    )
    matched_semantic = [
        row for row in semantic_segments if row.get("status") == "Matched"
    ]
    covered_semantic = [
        row for row in semantic_segments if row.get("model_coverage") == "Covered"
        and row.get("status") != "Unpaired"
    ]
    lines = [
        "# Movement destination, direction, and stop-frame reduction",
        "",
        f"- Invocation attribution: {mapped}/{len(invocations)} commits map uniquely from the current frame's slot worksheet pointer to one thread payload; {bracketed}/{len(invocations)} are bracketed by frame snapshots; {transition_matches}/{len(invocations)} callback transitions match their family.",
        "- Invocation family counts: "
        + ", ".join(f"{family}={count}" for family, count in family_counts.items())
        + f"; expected corpus counts matched={family_counts == expected_family_counts}.",
        f"- Semantic movement pairing: {len(semantic_segments)} rows; {len(covered_semantic)} paired evidence-covered rows; {len(matched_semantic)}/{len(covered_semantic)} retain exact step count, vectors, and terminal bits after onset alignment and are labeled `Matched`.",
        f"- Affected-target reactions: {len(affected_reactions)} action/slot lifetimes reported separately from movement commits; {sum(row.get('live_action_bounded') is True for row in affected_reactions)} live and {sum(row.get('predictor_action_bounded') is True for row in affected_reactions)} predictor lifetimes are action-bounded.",
        f"- Capture integrity: {sum(row['frame_hits'] for row in integrity)} frame-end snapshots; {integrity_failures} list failures; maximum list size {max((row['max_list_nodes'] for row in integrity), default=0)}; frame cap reached by {sum(row['frame_cap_reached'] for row in integrity)} jobs.",
        f"- Commit rows: {len(destinations)}; semantic sources are {selected} selected path nodes, {generated} generated single squares, {explicit} explicit grids, and {unknown} unknown.",
        f"- Commit propagation: posHolder {posholder_matches}/{len(destinations)} and completed setup target {setup_matches}/{len(destinations)} match the committed raw-stage destination; modes are {mode6} mode-6 and {mode13} mode-0x13.",
        f"- Order-paired movement steps: {len(comparable_steps)} comparable; {sum(row.get('vector_match') is True for row in comparable_steps)} exact vectors (current baseline 103/224).",
        f"- Order-paired segments: {len(comparable_stops)} comparable; {sum(row.get('step_count_match') is True for row in comparable_stops)} equal step counts (current baseline 18/31); {sum(row.get('fully_matches_after_onset_alignment') is True for row in comparable_stops)} fully matching (current baseline 14/31).",
        "",
        "Generated or explicit commit sources by callsite:",
        "",
    ]
    if not semantic_non_path:
        lines.append("- None observed.")
    else:
        for callsite in sorted({row["callsite_pc"] for row in semantic_non_path}):
            rows = [row for row in semantic_non_path if row["callsite_pc"] == callsite]
            value_matches = sum(row["selected_path_value_matches"] is True for row in rows)
            jobs = ", ".join(str(job) for job in sorted({row["source_exec_job_id"] for row in rows}))
            lines.append(
                f"- {callsite}: {rows[0]['destination_source']}; {len(rows)} commits; "
                f"{value_matches} happened to equal the worksheet path-index value; "
                f"jobs {jobs}."
            )
    lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="append", type=Path, required=True)
    parser.add_argument("--predictor-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    timelines: list[dict[str, Any]] = []
    destinations: list[dict[str, Any]] = []
    steps: list[dict[str, Any]] = []
    stops: list[dict[str, Any]] = []
    integrity: list[dict[str, Any]] = []
    invocations: list[dict[str, Any]] = []
    semantic_segments: list[dict[str, Any]] = []
    affected_reactions: list[dict[str, Any]] = []
    for run in args.run:
        for job, capture, _ in common.run_captures(run):
            events = common.load_jsonl(capture)
            integrity.append(capture_integrity(job, events))
            predictor = load_predictor(args.predictor_dir, job)
            job_invocations = invocation_windows(job, events)
            invocations.extend(job_invocations)
            timelines.extend(event_timeline(job, events))
            destinations.extend(destination_rows(job, events))
            live = common.live_frames(events)
            modeled = common.predictor_frames(predictor)
            step_rows, _ = common.compare_movement_rates(job, live, modeled, 0, 0)
            steps.extend(step_rows)
            stops.extend(stop_rows(job, live, predictor))
            semantic_segments.extend(
                semantic_segment_rows(job, events, predictor, job_invocations)
            )
            affected_reactions.extend(
                affected_target_reaction_rows(job, events, predictor)
            )

    common.write_csv(args.output / "movement_phase_timeline.csv", timelines)
    common.write_csv(args.output / "movement_destination_comparison.csv", destinations)
    common.write_csv(args.output / "movement_step_comparison.csv", steps)
    common.write_csv(args.output / "movement_stop_comparison.csv", stops)
    common.write_csv(args.output / "movement_invocation_windows.csv", invocations)
    common.write_csv(
        args.output / "movement_semantic_segment_comparison.csv",
        semantic_segments,
    )
    common.write_csv(
        args.output / "movement_affected_target_reactions.csv",
        affected_reactions,
    )
    (args.output / "findings.md").write_text(
        findings_text(
            destinations,
            steps,
            stops,
            integrity,
            invocations,
            semantic_segments,
            affected_reactions,
        ),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
