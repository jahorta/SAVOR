#!/usr/bin/env python3
"""Compare predictor RNG sources and frame movement with live captures."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import struct
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
RNG_WATCH_ID = "memwatch.rng_seed_write_803469A8"
ANCHOR_PC = 0x800145C8
FOCUS_SLOTS = (0, 1, 4, 5)
VISUAL_RNG_SOURCE_PCS = frozenset({
    0x800145C8,
    0x80012314,
    0x80013920,
    0x800513D4,
    0x80052BF0,
    0x80051BB0,
    0x80011794,
    0x8002EBDC,
})
SERIALIZED_ACTION_VIEW_PUBLICATION_ID = "serialized_action_view_publication_8003C738"
SYNTHETIC_ACTION_VIEW_PUBLICATION_ID = "synthetic_action_view_publication_800540BC"
ACTION_SERVICE_PUBLICATION_ID = "action_service_publication_8003B2B4"
POSITION_RE = re.compile(
    r"combatant_cur_pos_0x1c=\(([^)]*)\)->\(([^)]*)\)"
)

SOURCE_NAMES = {
    0x8001413C: "pre_ai_battle_start_camera",
    0x800608DC: "pre_ai_attack_targeting_camera",
    0x8008B428: "soldier_ai_action_decision",
    0x8008A0F0: "soldier_ai_random_pc_target",
    0x8008A618: "soldier_ai_attack_parameter",
    0x800711F8: "turn_order_priority_jitter",
    0x8008BC68: "enemy_attack_execution_setup",
    0x80052BF0: "mode0e_action_view_camera",
    0x800513D4: "mode0_action_view_camera_fallback",
    0x800145C8: "view_placement_direct_view",
    0x80012314: "view_placement_placement_function",
    0x80013920: "view_placement_runner",
    0x80051BB0: "action_view_pathing_record",
    # The seed-write watchpoint can retain the LR from FUN_80011694's
    # preceding geometry helper call while the function reaches its rand.
    0x80011724: "action_view_pathing_fallback",
    0x80011794: "action_view_pathing_fallback",
    # The RNG watchpoint's first saved LR can still reflect the preceding
    # action-row lookup when FUN_8002EB4C reaches its rand call at 0x8002EBDC.
    0x8002EBA4: "action_service_eb4c",
    0x8002EBDC: "action_service_eb4c",
    0x80010BDC: "attack_hit_dodge",
    0x80010C44: "attack_critical",
    0x80010958: "damage_spread",
    0x80010984: "damage_low_bit_bonus",
    0x80010628: "pc_status_attempt",
    0x80010774: "enemy_status_attempt",
    0x80081A88: "counter_roll",
    0x8002BAD8: "enemy_drop_roll",
    0x8006FF38: "end_turn_status_cleanup",
    0x801F2B6C: "level_up_stat_roll_1",
    0x801F2C00: "level_up_stat_roll_2",
    0x801F2D10: "level_up_stat_roll_3",
    0x80041F3C: "effect_emitter_spawn_variant",
    0x80041F60: "effect_emitter_spawn_offset_x",
    0x80041F88: "effect_emitter_spawn_offset_y",
    0x80041FB0: "effect_emitter_spawn_scale_a",
    0x80041FCC: "effect_emitter_spawn_scale_b",
    0x80041FE8: "effect_emitter_spawn_scale_c",
    0x80042020: "effect_emitter_axis_variant",
    0x800425A0: "effect_particle_motion_gate_x",
    0x800425E0: "effect_particle_motion_offset_x",
    0x80042630: "effect_particle_motion_gate_z",
    0x80042670: "effect_particle_motion_offset_z",
    0x80042F3C: "combat_effect_spawn_position_four_way",
    0x80042FBC: "combat_effect_spawn_position_binary",
    0x80043020: "combat_effect_spawn_scale_x",
    0x80043048: "combat_effect_spawn_scale_y",
    0x80043070: "combat_effect_spawn_scale_z",
    0x800430FC: "combat_effect_spawn_variant_index",
    0x80043200: "combat_effect_spawn_axis_assignment",
}

SOURCE_PC_ALIASES = {
    # FUN_80011694 calls its geometry helper at 0x80011724 before reaching
    # the zero-score fallback draw at 0x80011794.
    0x80011724: 0x80011794,
    # The watchpoint stack can retain the preceding row-lookup LR while
    # FUN_8002EB4C is in its retry loop. The draw itself is at 0x8002EBDC.
    0x8002EBA4: 0x8002EBDC,
}


def integer(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
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


def hex32(value: int | None) -> str:
    return "" if value is None else f"0x{value & 0xFFFFFFFF:08X}"


def advance_once(state: int) -> int:
    return (state * 0x41C64E6D + 0x3039) & 0xFFFFFFFF


def bits_to_float(bits: int | None) -> float | None:
    if bits is None:
        return None
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def sample_bits(event: dict[str, Any], name: str) -> int | None:
    if f"{name}_eval_ok" in event and boolean(event.get(f"{name}_eval_ok")) is not True:
        return None
    if f"{name}_read_ok" in event and boolean(event.get(f"{name}_read_ok")) is not True:
        return None
    return integer(event.get(name))


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: expected an object")
            rows.append(row)
    rows.sort(key=lambda row: integer(row.get("capture_sequence")) or 0)
    return rows


def source_name(pc: int | None) -> str:
    if pc is None:
        return "unknown"
    return SOURCE_NAMES.get(pc, f"unknown_{pc:08X}")


def parse_vector(text: str) -> tuple[float, float, float] | None:
    try:
        values = tuple(float(part.strip()) for part in text.split(","))
    except ValueError:
        return None
    return values if len(values) == 3 else None


def vector_delta(
    current: tuple[float, float, float] | None,
    previous: tuple[float, float, float] | None,
) -> tuple[float, float, float] | None:
    if current is None or previous is None:
        return None
    return tuple(current[index] - previous[index] for index in range(3))


def vector_magnitude(vector: tuple[float, float, float] | None) -> float | None:
    if vector is None:
        return None
    return math.sqrt(sum(component * component for component in vector))


def vector_matches(
    left: tuple[float, float, float] | None,
    right: tuple[float, float, float] | None,
    tolerance: float = 0.0001,
) -> bool | None:
    if left is None or right is None:
        return None
    return all(abs(a - b) <= tolerance for a, b in zip(left, right))


def vector_text(vector: tuple[float | None, float | None, float | None] | None) -> str:
    if vector is None or any(value is None for value in vector):
        return ""
    return ",".join(f"{value:.6f}" for value in vector if value is not None)


def predictor_sources(event: dict[str, Any]) -> list[int | None]:
    draws = integer(event.get("draws_consumed")) or 0
    if draws <= 0:
        return []
    phase = str(event.get("phase", ""))
    label = str(event.get("label", ""))

    if phase == "turn_order" and label == "resolve_turn_order":
        return []
    if phase == "pre_ai" and label == "camera_draws":
        return ([0x8001413C] + [0x800608DC] * max(0, draws - 1))[:draws]
    if phase == "enemy_ai" and label == "soldier_ai":
        return ([0x8008B428, 0x8008A0F0, 0x8008A618] + [None] * draws)[:draws]
    if phase == "turn_order" and label == "entry":
        return [0x800711F8] * draws
    if phase == "movement_setup" and label == "enemy_setup_draw":
        return [0x8008BC68] * draws
    if label == "view_placement_direct_view":
        return [0x800145C8] * draws
    if label == "view_placement_end_turn":
        return [0x80012314] * draws
    if label == "mode0e_action_view_camera":
        return [0x80052BF0] * draws
    if label in {"mode0_action_view_camera_fallback", "mode0_action_view_camera_rewrite_gate"}:
        return [0x800513D4] * draws
    if label == "mode1_pathing_record_draw":
        return [0x80051BB0] * draws
    if label in {
        "fun_80011694_target_side_fallback",
        "fun_8005174c_actor_target_fallbacks",
    }:
        return [0x80011794] * draws
    if label == "fun_8002eb4c_action_service":
        return [0x8002EBDC] * draws
    if label == "action_view_record_mode0":
        return [0x800513D4] * draws
    if phase == "attack_resolution" and label == "attack_miss":
        return [0x80010BDC] * draws
    if phase == "attack_resolution" and label in {"attack_hit", "attack_crit"}:
        if draws == 3:
            return [0x80010BDC, 0x80010958, 0x80010984]
        return ([0x80010BDC, 0x80010C44, 0x80010958, 0x80010984] + [None] * draws)[:draws]
    if phase == "counter" and label in {"counter_queued", "counter_not_queued"}:
        return [0x80081A88] * draws
    if phase == "counter_follow_up" and label == "forced_hit_damage":
        return ([0x80010958, 0x80010984] + [None] * draws)[:draws]
    if phase == "death_drop" and label in {"enemy_drop", "enemy_no_drop"}:
        return [0x8002BAD8] * draws
    if label in {"combat_effect_chunk", "combat_effect_burst"}:
        pattern = [0x80042FBC, 0x80043020, 0x80043048, 0x80043070, 0x800430FC]
        return (pattern * ((draws + len(pattern) - 1) // len(pattern)))[:draws]
    return [None] * draws


def expand_predictor_draws(predictor: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in predictor.get("events", []):
        if not isinstance(event, dict):
            continue
        sources = predictor_sources(event)
        if not sources:
            continue
        state = integer(event.get("rng_seed_before"))
        if state is None:
            continue
        for local_index, pc in enumerate(sources):
            seed_before = state
            state = advance_once(state)
            rows.append({
                "predictor_draw_ordinal": len(rows),
                "predictor_event_sequence": event.get("sequence", ""),
                "predictor_local_draw": local_index,
                "predictor_phase": event.get("phase", ""),
                "predictor_label": event.get("label", ""),
                "predictor_frame": event.get("frame_index", ""),
                "predictor_source_pc": pc,
                "predictor_source": source_name(pc),
                "predictor_seed_before": seed_before,
                "predictor_seed_after": state,
            })
        expected_after = integer(event.get("rng_seed_after"))
        if expected_after is not None and expected_after != state:
            raise ValueError(
                f"predictor event {event.get('sequence')} draw expansion ends at "
                f"{hex32(state)}, expected {hex32(expected_after)}"
            )
    return rows


def live_rng_draws(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    previous_post_seed: int | None = None
    pending_checkpoint_pc: int | None = None
    pending_checkpoint_id = ""
    pending_checkpoint_sequence: int | None = None
    pending_checkpoint_seed_before: int | None = None
    for event in events:
        if event.get("checkpoint_id") != RNG_WATCH_ID:
            event_pc = integer(event.get("pc"))
            if (
                event.get("stop_kind") == "pc_breakpoint"
                and event_pc in SOURCE_NAMES
                and boolean(event.get("owns_rng_draw")) is False
            ):
                pending_checkpoint_pc = event_pc
                pending_checkpoint_id = str(event.get("checkpoint_id", ""))
                pending_checkpoint_sequence = integer(event.get("capture_sequence"))
                pending_checkpoint_seed_before = integer(event.get("rng_seed_before"))
            continue
        capture_sequence = integer(event.get("capture_sequence"))
        post_seed = (
            integer(event.get("decoded_value"))
            or integer(event.get("decoded_memory_value"))
            or integer(event.get("rng_seed_after"))
        )
        stack_pc = integer(event.get("caller_callsite_pc"))
        adjacent_checkpoint = (
            pending_checkpoint_pc is not None
            and pending_checkpoint_sequence is not None
            and capture_sequence == pending_checkpoint_sequence + 1
            and pending_checkpoint_seed_before is not None
            and post_seed == advance_once(pending_checkpoint_seed_before)
        )
        raw_pc = pending_checkpoint_pc if adjacent_checkpoint else stack_pc
        pc = SOURCE_PC_ALIASES.get(raw_pc, raw_pc)
        trusted_source = adjacent_checkpoint or stack_pc in SOURCE_NAMES
        rows.append({
            "capture_sequence": capture_sequence or 0,
            "live_draw_index": integer(event.get("rng_draw_index_before")),
            "live_source_pc": pc,
            "live_source": source_name(pc),
            "live_source_trusted": trusted_source,
            "live_source_attribution": (
                "same_draw_checkpoint" if adjacent_checkpoint else "caller_stack"
            ),
            "live_source_checkpoint_id": pending_checkpoint_id if adjacent_checkpoint else "",
            "live_stack_source_pc": stack_pc,
            "live_stack_source_pc_hex": hex32(stack_pc),
            "live_seed_before_reconstructed": previous_post_seed,
            "live_seed_after": post_seed,
            "caller_source": event.get("caller_source", ""),
            "stack_frame_count": event.get("stack_frame_count", ""),
        })
        previous_post_seed = post_seed
        pending_checkpoint_pc = None
        pending_checkpoint_id = ""
        pending_checkpoint_sequence = None
        pending_checkpoint_seed_before = None
    return rows


def find_anchor(
    live: list[dict[str, Any]], predictor: list[dict[str, Any]]
) -> tuple[int, int] | None:
    for live_index, live_row in enumerate(live):
        if live_row["live_source_pc"] != ANCHOR_PC:
            continue
        for predictor_index, predictor_row in enumerate(predictor):
            if predictor_row["predictor_source_pc"] != ANCHOR_PC:
                continue
            if predictor_row["predictor_seed_after"] == live_row["live_seed_after"]:
                return live_index, predictor_index
    live_index = next(
        (index for index, row in enumerate(live) if row["live_source_pc"] == ANCHOR_PC),
        None,
    )
    predictor_index = next(
        (index for index, row in enumerate(predictor) if row["predictor_source_pc"] == ANCHOR_PC),
        None,
    )
    if live_index is None or predictor_index is None:
        return None
    return live_index, predictor_index


def compare_rng(
    job: int,
    live: list[dict[str, Any]],
    predictor: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    anchor = find_anchor(live, predictor)
    live_indices = [row["live_draw_index"] for row in live]
    indices_contiguous = all(
        value is not None and value == index
        for index, value in enumerate(live_indices)
    )
    if anchor is None:
        return [], {
            "rng_anchor_found": False,
            "rng_live_draws": len(live),
            "rng_predictor_draws": len(predictor),
            "rng_live_indices_contiguous": indices_contiguous,
        }
    live_anchor, predictor_anchor = anchor
    rows: list[dict[str, Any]] = []
    count = min(len(live) - live_anchor, len(predictor) - predictor_anchor)
    for relative in range(count):
        live_row = live[live_anchor + relative]
        predictor_row = predictor[predictor_anchor + relative]
        predicted_pc = predictor_row["predictor_source_pc"]
        source_match = (
            None
            if predicted_pc is None or not live_row["live_source_trusted"]
            else predicted_pc == live_row["live_source_pc"]
        )
        seed_match = predictor_row["predictor_seed_after"] == live_row["live_seed_after"]
        rows.append({
            "source_exec_job_id": job,
            "relative_draw": relative,
            **live_row,
            **predictor_row,
            "live_source_pc_hex": hex32(live_row["live_source_pc"]),
            "predictor_source_pc_hex": hex32(predicted_pc),
            "live_seed_before_reconstructed_hex": hex32(live_row["live_seed_before_reconstructed"]),
            "live_seed_after_hex": hex32(live_row["live_seed_after"]),
            "predictor_seed_before_hex": hex32(predictor_row["predictor_seed_before"]),
            "predictor_seed_after_hex": hex32(predictor_row["predictor_seed_after"]),
            "source_match": source_match,
            "seed_match": seed_match,
            "exact_match": source_match is True and seed_match,
        })
    known_source_rows = [row for row in rows if row["source_match"] is not None]
    first_source_mismatch = next(
        (row["relative_draw"] for row in known_source_rows if not row["source_match"]),
        None,
    )
    first_seed_mismatch = next(
        (row["relative_draw"] for row in rows if not row["seed_match"]),
        None,
    )
    return rows, {
        "rng_anchor_found": True,
        "rng_anchor_live_index": live[live_anchor]["live_draw_index"],
        "rng_anchor_predictor_ordinal": predictor[predictor_anchor]["predictor_draw_ordinal"],
        "rng_anchor_seed_match": (
            live[live_anchor]["live_seed_after"]
            == predictor[predictor_anchor]["predictor_seed_after"]
        ),
        "rng_live_draws": len(live),
        "rng_predictor_draws": len(predictor),
        "rng_live_indices_contiguous": indices_contiguous,
        "rng_compared_draws": len(rows),
        "rng_seed_matches": sum(bool(row["seed_match"]) for row in rows),
        "rng_known_source_draws": len(known_source_rows),
        "rng_source_matches": sum(bool(row["source_match"]) for row in known_source_rows),
        "first_rng_source_mismatch_relative_draw": first_source_mismatch,
        "first_rng_seed_mismatch_relative_draw": first_seed_mismatch,
    }


def compare_visual_rng_sources(
    job: int,
    live: list[dict[str, Any]],
    predictor: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Compare semantic visual callers by ordinal, never by RNG value."""
    live_visual = [
        row for row in live
        if row.get("live_source_pc") in VISUAL_RNG_SOURCE_PCS
    ]
    predictor_visual = [
        row for row in predictor
        if row.get("predictor_source_pc") in VISUAL_RNG_SOURCE_PCS
    ]
    rows: list[dict[str, Any]] = []
    for ordinal in range(max(len(live_visual), len(predictor_visual))):
        live_row = live_visual[ordinal] if ordinal < len(live_visual) else None
        predictor_row = (
            predictor_visual[ordinal] if ordinal < len(predictor_visual) else None
        )
        source_match = (
            live_row is not None
            and predictor_row is not None
            and live_row["live_source_pc"] == predictor_row["predictor_source_pc"]
        )
        rows.append({
            "source_exec_job_id": job,
            "visual_draw_ordinal": ordinal,
            "live_global_draw_index": "" if live_row is None else live_row["live_draw_index"],
            "live_capture_sequence": "" if live_row is None else live_row["capture_sequence"],
            "live_source_pc_hex": "" if live_row is None else hex32(live_row["live_source_pc"]),
            "live_source": "" if live_row is None else live_row["live_source"],
            "live_source_attribution": (
                "" if live_row is None else live_row["live_source_attribution"]
            ),
            "live_seed_after_hex": "" if live_row is None else hex32(live_row["live_seed_after"]),
            "predictor_global_draw_ordinal": (
                "" if predictor_row is None else predictor_row["predictor_draw_ordinal"]
            ),
            "predictor_event_sequence": (
                "" if predictor_row is None else predictor_row["predictor_event_sequence"]
            ),
            "predictor_frame": "" if predictor_row is None else predictor_row["predictor_frame"],
            "predictor_label": "" if predictor_row is None else predictor_row["predictor_label"],
            "predictor_source_pc_hex": (
                "" if predictor_row is None else hex32(predictor_row["predictor_source_pc"])
            ),
            "predictor_source": (
                "" if predictor_row is None else predictor_row["predictor_source"]
            ),
            "predictor_seed_after_hex": (
                "" if predictor_row is None else hex32(predictor_row["predictor_seed_after"])
            ),
            "source_match": source_match,
        })
    first_mismatch = next(
        (row["visual_draw_ordinal"] for row in rows if not row["source_match"]),
        None,
    )
    return rows, {
        "visual_live_rng_draws": len(live_visual),
        "visual_predictor_rng_draws": len(predictor_visual),
        "visual_rng_compared_draws": min(len(live_visual), len(predictor_visual)),
        "visual_rng_source_matches": sum(bool(row["source_match"]) for row in rows),
        "first_visual_rng_source_mismatch_ordinal": first_mismatch,
    }


def visual_publication_summary(
    events: list[dict[str, Any]],
    predictor_visual: list[dict[str, Any]],
) -> dict[str, Any]:
    live_ids = [str(event.get("checkpoint_id", "")) for event in events]
    predictor_publications = [
        row for row in predictor_visual if row.get("label") == "visual_command_publish"
    ]

    def predictor_count(command_kind: str) -> int:
        return sum(
            row.get("command_kind") == command_kind for row in predictor_publications
        )

    return {
        "visual_live_set_command_publications": live_ids.count(ACTION_SERVICE_PUBLICATION_ID),
        "visual_predictor_set_command_publications": predictor_count("SetCommand"),
        "visual_live_system_camera_publications": live_ids.count(
            SERIALIZED_ACTION_VIEW_PUBLICATION_ID
        ),
        "visual_predictor_system_camera_publications": predictor_count("SystemCamera"),
        "visual_live_synthetic_publications": live_ids.count(
            SYNTHETIC_ACTION_VIEW_PUBLICATION_ID
        ),
        "visual_predictor_synthetic_publications": predictor_count("SyntheticActionView"),
    }


def live_frames(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []
    for event in events:
        if event.get("checkpoint_id") != FRAME_ID:
            continue
        positions: dict[int, tuple[float, float, float] | None] = {}
        facings: dict[int, int] = {}
        motion: dict[int, dict[str, Any]] = {}
        for root_index in range(12):
            prefix = f"root{root_index}"
            worksheet = sample_bits(event, f"{prefix}_combatant_worksheet_ptr")
            slot = sample_bits(event, f"{prefix}_iw_slot_0x00")
            if (
                worksheet is None
                or not (0x80000000 <= worksheet < 0x81800000)
                or slot not in FOCUS_SLOTS
            ):
                continue
            position = tuple(
                bits_to_float(sample_bits(event, f"{prefix}_cw_cur_{axis}_{offset}"))
                for axis, offset in (("x", "0x1c"), ("y", "0x20"), ("z", "0x24"))
            )
            if any(value is None or not math.isfinite(value) for value in position):
                continue
            positions[slot] = position
            facing = sample_bits(event, f"{prefix}_cw_facing_angle_0x2c")
            if facing is not None:
                facings[slot] = facing & 0xFFFF
            motion[slot] = {
                "root_index": root_index,
                "action_mode": sample_bits(event, f"{prefix}_iw_action_mode_0x06"),
                "move_increment": tuple(
                    bits_to_float(sample_bits(event, f"{prefix}_iw_move_inc_{axis}_{offset}"))
                    for axis, offset in (("x", "0x104"), ("y", "0x108"), ("z", "0x10c"))
                ),
                "target": tuple(
                    bits_to_float(sample_bits(event, f"{prefix}_iw_target_{axis}_{offset}"))
                    for axis, offset in (("x", "0x110"), ("y", "0x114"), ("z", "0x118"))
                ),
                "speed_bits": sample_bits(event, f"{prefix}_iw_speed_0x12c"),
            }
        frames.append({
            "ordinal": len(frames) + 1,
            "capture_sequence": integer(event.get("capture_sequence")) or 0,
            "positions": positions,
            "facings": facings,
            "motion": motion,
            "turn_phase": integer(event.get("turn_phase_8034733c")),
            "active_actor_slot": integer(event.get("active_actor_slot_80347334")),
            "action_sequence": integer(event.get("action_sequence_80347335")),
            "rng_seed": integer(event.get("rng_seed_current")),
            "event": event,
        })
    return frames


def predictor_frames(predictor: dict[str, Any]) -> dict[int, dict[int, tuple[float, float, float]]]:
    states: dict[int, tuple[float, float, float]] = {}
    frames: dict[int, dict[int, tuple[float, float, float]]] = {}
    current_frame = -1
    for event in predictor.get("events", []):
        if not isinstance(event, dict) or event.get("phase") != "frame_scheduler":
            continue
        frame = integer(event.get("frame_index"))
        if frame is None:
            continue
        if current_frame != -1 and frame != current_frame:
            frames[current_frame] = dict(states)
        current_frame = frame
        match = POSITION_RE.search(str(event.get("detail", "")))
        slot = integer(event.get("actor_slot"))
        if match is None or slot is None:
            continue
        old_position = parse_vector(match.group(1))
        new_position = parse_vector(match.group(2))
        if slot not in states and old_position is not None:
            states[slot] = old_position
        if new_position is not None:
            states[slot] = new_position
    if current_frame != -1:
        frames[current_frame] = dict(states)
    return frames


def predictor_facing_frames(predictor: dict[str, Any]) -> dict[int, dict[int, int]]:
    states: dict[int, int] = {}
    frames: dict[int, dict[int, int]] = {}
    current_frame = -1
    for event in predictor.get("events", []):
        if not isinstance(event, dict):
            continue
        slot = integer(event.get("actor_slot"))
        facing = integer(event.get("facing_angle_0x2c"))
        if (
            event.get("label") == "initial_facing_seeded"
            and slot is not None
            and facing is not None
        ):
            states[slot] = facing & 0xFFFF
            continue
        if event.get("phase") != "frame_scheduler":
            continue
        frame = integer(event.get("frame_index"))
        if frame is None:
            continue
        if current_frame != -1 and frame != current_frame:
            frames[current_frame] = dict(states)
        current_frame = frame
        if slot is not None and facing is not None:
            states[slot] = facing & 0xFFFF
    if current_frame != -1:
        frames[current_frame] = dict(states)
    return frames


def frame_after_sequence(frames: list[dict[str, Any]], sequence: int) -> dict[str, Any] | None:
    return next((frame for frame in frames if frame["capture_sequence"] > sequence), None)


def movement_steps(
    frames: Iterable[tuple[int, dict[int, tuple[float, float, float] | None]]]
) -> dict[int, list[dict[str, Any]]]:
    previous: dict[int, tuple[float, float, float] | None] = {}
    steps = {slot: [] for slot in FOCUS_SLOTS}
    for frame_index, positions in frames:
        for slot in FOCUS_SLOTS:
            position = positions.get(slot)
            delta = vector_delta(position, previous.get(slot))
            speed = vector_magnitude(delta)
            if delta is not None and speed is not None and speed > 0.0001:
                steps[slot].append({
                    "frame": frame_index,
                    "position": position,
                    "delta": delta,
                    "speed": speed,
                })
            if position is not None:
                previous[slot] = position
    return steps


def movement_segments(steps: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    segments: list[list[dict[str, Any]]] = []
    for step in steps:
        if not segments or step["frame"] != segments[-1][-1]["frame"] + 1:
            segments.append([])
        segments[-1].append(step)
    return segments


def signed_short_delta(current: int, previous: int) -> int:
    delta = (current - previous) & 0xFFFF
    return delta - 0x10000 if delta >= 0x8000 else delta


def rotation_steps(
    frames: Iterable[tuple[int, dict[int, int]]]
) -> dict[int, list[dict[str, Any]]]:
    previous: dict[int, int] = {}
    steps = {slot: [] for slot in FOCUS_SLOTS}
    for frame_index, facings in frames:
        for slot in FOCUS_SLOTS:
            facing = facings.get(slot)
            if facing is None:
                continue
            prior = previous.get(slot)
            if prior is not None:
                delta = signed_short_delta(facing, prior)
                if delta != 0:
                    steps[slot].append({
                        "frame": frame_index,
                        "facing": facing,
                        "delta": delta,
                        "speed": abs(delta),
                    })
            previous[slot] = facing
    return steps


def compare_rotation_rates(
    job: int,
    live: list[dict[str, Any]],
    modeled: dict[int, dict[int, int]],
    live_anchor_frame: int,
    predictor_anchor_frame: int,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    live_steps = rotation_steps(
        (frame["ordinal"], frame["facings"]) for frame in live
    )
    predictor_steps = rotation_steps(
        (frame_index, modeled[frame_index]) for frame_index in sorted(modeled)
    )
    rows: list[dict[str, Any]] = []
    live_segment_count = 0
    predictor_segment_count = 0
    paired_segment_count = 0
    segment_length_matches = 0
    nominal_speed_pairs = 0
    nominal_speed_matches = 0
    start_offsets: list[str] = []
    for slot in FOCUS_SLOTS:
        live_segments = movement_segments(live_steps[slot])
        predictor_segments = movement_segments(predictor_steps[slot])
        live_segment_count += len(live_segments)
        predictor_segment_count += len(predictor_segments)
        paired_segment_count += min(len(live_segments), len(predictor_segments))
        for segment_index in range(max(len(live_segments), len(predictor_segments))):
            live_segment = live_segments[segment_index] if segment_index < len(live_segments) else []
            predictor_segment = (
                predictor_segments[segment_index]
                if segment_index < len(predictor_segments)
                else []
            )
            if live_segment and predictor_segment:
                nominal_speed_pairs += 1
                if abs(
                    max(step["speed"] for step in live_segment)
                    - max(step["speed"] for step in predictor_segment)
                ) <= 1:
                    nominal_speed_matches += 1
                if len(live_segment) == len(predictor_segment):
                    segment_length_matches += 1
                aligned_live_start = (
                    live_segment[0]["frame"] - live_anchor_frame + predictor_anchor_frame
                )
                start_offset = aligned_live_start - predictor_segment[0]["frame"]
                start_offsets.append(f"slot{slot}.segment{segment_index}={start_offset}")
            else:
                start_offset = None
            for step_index in range(max(len(live_segment), len(predictor_segment))):
                live_step = live_segment[step_index] if step_index < len(live_segment) else None
                predictor_step = (
                    predictor_segment[step_index]
                    if step_index < len(predictor_segment)
                    else None
                )
                speed_match = None
                full_step_pair = False
                full_step_speed_match = None
                direction_match = None
                delta_match = None
                if live_step is not None and predictor_step is not None:
                    speed_match = abs(live_step["speed"] - predictor_step["speed"]) <= 1
                    full_step_pair = (
                        step_index + 1 < len(live_segment)
                        and step_index + 1 < len(predictor_segment)
                    )
                    if full_step_pair:
                        full_step_speed_match = speed_match
                    direction_match = (
                        (live_step["delta"] > 0) == (predictor_step["delta"] > 0)
                    )
                    delta_match = live_step["delta"] == predictor_step["delta"]
                rows.append({
                    "source_exec_job_id": job,
                    "slot": slot,
                    "segment_index": segment_index,
                    "step_index": step_index,
                    "live_frame": "" if live_step is None else live_step["frame"],
                    "predictor_frame": "" if predictor_step is None else predictor_step["frame"],
                    "segment_start_frame_offset": "" if start_offset is None else start_offset,
                    "live_segment_steps": len(live_segment),
                    "predictor_segment_steps": len(predictor_segment),
                    "live_facing_hex": "" if live_step is None else hex32(live_step["facing"]),
                    "predictor_facing_hex": (
                        "" if predictor_step is None else hex32(predictor_step["facing"])
                    ),
                    "live_delta_short": "" if live_step is None else live_step["delta"],
                    "predictor_delta_short": (
                        "" if predictor_step is None else predictor_step["delta"]
                    ),
                    "speed_match": speed_match,
                    "full_step_pair": full_step_pair,
                    "full_step_speed_match": full_step_speed_match,
                    "direction_match": direction_match,
                    "delta_match": delta_match,
                })
    paired_rows = [row for row in rows if row["speed_match"] is not None]
    full_step_rows = [row for row in rows if row["full_step_pair"]]
    return rows, {
        "rotation_live_active_steps": sum(len(value) for value in live_steps.values()),
        "rotation_predictor_active_steps": sum(len(value) for value in predictor_steps.values()),
        "rotation_paired_rate_steps": len(paired_rows),
        "rotation_speed_matches": sum(row["speed_match"] is True for row in paired_rows),
        "rotation_full_step_pairs": len(full_step_rows),
        "rotation_full_step_speed_matches": sum(
            row["full_step_speed_match"] is True for row in full_step_rows
        ),
        "rotation_nominal_speed_pairs": nominal_speed_pairs,
        "rotation_nominal_speed_matches": nominal_speed_matches,
        "rotation_direction_matches": sum(
            row["direction_match"] is True for row in paired_rows
        ),
        "rotation_delta_matches": sum(row["delta_match"] is True for row in paired_rows),
        "rotation_live_segments": live_segment_count,
        "rotation_predictor_segments": predictor_segment_count,
        "rotation_paired_segments": paired_segment_count,
        "rotation_segment_length_matches": segment_length_matches,
        "rotation_segment_start_offsets": ";".join(start_offsets),
    }


def compare_movement_rates(
    job: int,
    live: list[dict[str, Any]],
    modeled: dict[int, dict[int, tuple[float, float, float]]],
    live_anchor_frame: int,
    predictor_anchor_frame: int,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    live_steps = movement_steps(
        (frame["ordinal"], frame["positions"]) for frame in live
    )
    predictor_steps = movement_steps(
        (frame_index, modeled[frame_index]) for frame_index in sorted(modeled)
    )
    rows: list[dict[str, Any]] = []
    live_segment_count = 0
    predictor_segment_count = 0
    paired_segment_count = 0
    segment_length_matches = 0
    start_offsets: list[str] = []
    for slot in FOCUS_SLOTS:
        live_segments = movement_segments(live_steps[slot])
        predictor_segments = movement_segments(predictor_steps[slot])
        live_segment_count += len(live_segments)
        predictor_segment_count += len(predictor_segments)
        paired_segment_count += min(len(live_segments), len(predictor_segments))
        for segment_index in range(max(len(live_segments), len(predictor_segments))):
            live_segment = live_segments[segment_index] if segment_index < len(live_segments) else []
            predictor_segment = (
                predictor_segments[segment_index]
                if segment_index < len(predictor_segments)
                else []
            )
            if live_segment and predictor_segment:
                if len(live_segment) == len(predictor_segment):
                    segment_length_matches += 1
                aligned_live_start = (
                    live_segment[0]["frame"] - live_anchor_frame + predictor_anchor_frame
                )
                start_offset = aligned_live_start - predictor_segment[0]["frame"]
                start_offsets.append(f"slot{slot}.segment{segment_index}={start_offset}")
            else:
                start_offset = None
            for step_index in range(max(len(live_segment), len(predictor_segment))):
                live_step = live_segment[step_index] if step_index < len(live_segment) else None
                predictor_step = (
                    predictor_segment[step_index]
                    if step_index < len(predictor_segment)
                    else None
                )
                speed_match = None
                vector_match = None
                if live_step is not None and predictor_step is not None:
                    speed_match = abs(live_step["speed"] - predictor_step["speed"]) <= 0.0001
                    vector_match = vector_matches(live_step["delta"], predictor_step["delta"])
                rows.append({
                    "source_exec_job_id": job,
                    "slot": slot,
                    "segment_index": segment_index,
                    "step_index": step_index,
                    "live_frame": "" if live_step is None else live_step["frame"],
                    "predictor_frame": "" if predictor_step is None else predictor_step["frame"],
                    "segment_start_frame_offset": "" if start_offset is None else start_offset,
                    "live_segment_steps": len(live_segment),
                    "predictor_segment_steps": len(predictor_segment),
                    "live_delta": "" if live_step is None else vector_text(live_step["delta"]),
                    "predictor_delta": (
                        "" if predictor_step is None else vector_text(predictor_step["delta"])
                    ),
                    "live_speed": "" if live_step is None else f"{live_step['speed']:.6f}",
                    "predictor_speed": (
                        "" if predictor_step is None else f"{predictor_step['speed']:.6f}"
                    ),
                    "speed_match": speed_match,
                    "vector_match": vector_match,
                })
    paired_rows = [row for row in rows if row["speed_match"] is not None]
    first_speed_mismatch = next(
        (
            f"slot={row['slot']};segment={row['segment_index']};step={row['step_index']}"
            for row in paired_rows
            if not row["speed_match"]
        ),
        None,
    )
    return rows, {
        "movement_live_active_steps": sum(len(value) for value in live_steps.values()),
        "movement_predictor_active_steps": sum(len(value) for value in predictor_steps.values()),
        "movement_paired_rate_steps": len(paired_rows),
        "movement_speed_matches": sum(row["speed_match"] is True for row in paired_rows),
        "movement_vector_matches": sum(row["vector_match"] is True for row in paired_rows),
        "movement_live_segments": live_segment_count,
        "movement_predictor_segments": predictor_segment_count,
        "movement_paired_segments": paired_segment_count,
        "movement_segment_length_matches": segment_length_matches,
        "movement_segment_start_offsets": ";".join(start_offsets),
        "first_movement_speed_mismatch": first_speed_mismatch,
    }


def compare_movement(
    job: int,
    events: list[dict[str, Any]],
    predictor: dict[str, Any],
    rng_rows: list[dict[str, Any]],
) -> tuple[
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    dict[str, Any],
]:
    live = live_frames(events)
    modeled = predictor_frames(predictor)
    modeled_facings = predictor_facing_frames(predictor)
    list_integrity_issues = sum(
        boolean(frame["event"].get("thread_list_read_ok")) is not True
        or boolean(frame["event"].get("thread_list_truncated")) is not False
        or boolean(frame["event"].get("thread_list_cycle_detected")) is not False
        for frame in live
    )
    root_mapping_missing_samples = sum(
        1
        for frame in live
        if frame["turn_phase"] is not None
        and frame["turn_phase"] >= 3
        for slot in FOCUS_SLOTS
        if frame["positions"].get(slot) is None
    )
    anchor_rng = next((row for row in rng_rows if row["relative_draw"] == 0), None)
    if anchor_rng is None:
        return [], [], [], {"movement_anchor_found": False, "live_case5_frames": len(live)}
    live_anchor_frame = frame_after_sequence(live, anchor_rng["capture_sequence"])
    predictor_anchor_frame = integer(anchor_rng.get("predictor_frame"))
    if live_anchor_frame is None or predictor_anchor_frame is None:
        return [], [], [], {"movement_anchor_found": False, "live_case5_frames": len(live)}

    live_by_ordinal = {frame["ordinal"]: frame for frame in live}
    rows: list[dict[str, Any]] = []
    previous_live: dict[int, tuple[float, float, float] | None] = {}
    previous_predictor: dict[int, tuple[float, float, float] | None] = {}
    for predictor_frame in sorted(modeled):
        live_ordinal = (
            live_anchor_frame["ordinal"]
            + predictor_frame
            - predictor_anchor_frame
        )
        live_frame = live_by_ordinal.get(live_ordinal)
        if live_frame is None:
            continue
        for slot in FOCUS_SLOTS:
            live_position = live_frame["positions"].get(slot)
            predictor_position = modeled[predictor_frame].get(slot)
            if live_position is None or predictor_position is None:
                continue
            live_delta = vector_delta(live_position, previous_live.get(slot))
            predictor_delta = vector_delta(predictor_position, previous_predictor.get(slot))
            live_speed = vector_magnitude(live_delta)
            predictor_speed = vector_magnitude(predictor_delta)
            active = (
                (live_speed is not None and live_speed > 0.0001)
                or (predictor_speed is not None and predictor_speed > 0.0001)
            )
            event = live_frame["event"]
            live_motion = live_frame["motion"].get(slot, {})
            rows.append({
                "source_exec_job_id": job,
                "predictor_frame": predictor_frame,
                "live_frame_ordinal": live_ordinal,
                "live_capture_sequence": live_frame["capture_sequence"],
                "slot": slot,
                "live_position": vector_text(live_position),
                "predictor_position": vector_text(predictor_position),
                "position_match": vector_matches(live_position, predictor_position),
                "live_delta": vector_text(live_delta),
                "predictor_delta": vector_text(predictor_delta),
                "live_speed": "" if live_speed is None else f"{live_speed:.6f}",
                "predictor_speed": "" if predictor_speed is None else f"{predictor_speed:.6f}",
                "delta_match": vector_matches(live_delta, predictor_delta),
                "movement_active": active,
                "turn_phase": live_frame["turn_phase"],
                "active_actor_slot": live_frame["active_actor_slot"],
                "action_sequence": live_frame["action_sequence"],
                "live_root_index": live_motion.get("root_index", ""),
                "live_action_mode_0x6": live_motion.get("action_mode", ""),
                "live_path_index_0x15": sample_bits(
                    event, f"slot{slot}_movement_worksheet_path_index_0x15"
                ),
                "live_move_increment": vector_text(live_motion.get("move_increment")),
                "live_target": vector_text(live_motion.get("target")),
                "live_speed_bits": hex32(live_motion.get("speed_bits")),
            })
            previous_live[slot] = live_position
            previous_predictor[slot] = predictor_position

    active_rows = [row for row in rows if row["movement_active"]]
    comparable_deltas = [row for row in active_rows if row["delta_match"] is not None]
    first_delta_mismatch = next(
        (
            f"frame={row['predictor_frame']};slot={row['slot']}"
            for row in comparable_deltas
            if not row["delta_match"]
        ),
        None,
    )
    rate_rows, rate_summary = compare_movement_rates(
        job,
        live,
        modeled,
        live_anchor_frame["ordinal"],
        predictor_anchor_frame,
    )
    rotation_rows, rotation_summary = compare_rotation_rates(
        job,
        live,
        modeled_facings,
        live_anchor_frame["ordinal"],
        predictor_anchor_frame,
    )
    return rows, rate_rows, rotation_rows, {
        "movement_anchor_found": True,
        "movement_anchor_live_frame": live_anchor_frame["ordinal"],
        "movement_anchor_predictor_frame": predictor_anchor_frame,
        "live_case5_frames": len(live),
        "frame_cap_reached": len(live) >= 2400,
        "list_integrity_issues": list_integrity_issues,
        "root_mapping_missing_samples": root_mapping_missing_samples,
        "predictor_frames": len(modeled),
        "movement_rows_compared": len(rows),
        "movement_active_rows": len(active_rows),
        "movement_position_matches": sum(row["position_match"] is True for row in rows),
        "movement_delta_matches": sum(row["delta_match"] is True for row in comparable_deltas),
        "movement_comparable_deltas": len(comparable_deltas),
        "first_movement_delta_mismatch": first_delta_mismatch,
        **rate_summary,
        **rotation_summary,
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
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def predictor_visual_timeline(
    job: int,
    predictor: dict[str, Any],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in predictor.get("events", []):
        if not isinstance(event, dict) or event.get("phase") != "frame_scheduler":
            continue
        label = str(event.get("label", ""))
        if not (
            label.startswith("visual_")
            or event.get("visual_command_kind")
            or event.get("visual_child_kind")
        ):
            continue
        rows.append({
            "source_exec_job_id": job,
            "event_sequence": event.get("sequence", ""),
            "frame_index": event.get("frame_index", ""),
            "action_ordinal": event.get("action_ordinal", ""),
            "label": label,
            "status": event.get("status", ""),
            "origin_slot": event.get("actor_slot", ""),
            "target_slot": event.get("target_slot", ""),
            "command_kind": event.get("visual_command_kind", ""),
            "resource": event.get("visual_resource", ""),
            "record_index": event.get("visual_record_index", ""),
            "epoch": event.get("visual_epoch", ""),
            "task_sequence": event.get("visual_task_sequence", ""),
            "child_kind": event.get("visual_child_kind", ""),
            "payload_mode": event.get("visual_payload_mode", ""),
            "effective_mode": event.get("visual_effective_mode", ""),
            "draws_consumed": event.get("draws_consumed", 0),
            "rand_value": event.get("rand_value", ""),
            "rng_seed_before": hex32(integer(event.get("rng_seed_before"))),
            "rng_seed_after": hex32(integer(event.get("rng_seed_after"))),
            "detail": event.get("detail", ""),
        })
    return rows


def predictor_visual_child_lifetimes(
    rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for row in rows:
        task = integer(row.get("task_sequence"))
        if task is None:
            continue
        grouped.setdefault((int(row["source_exec_job_id"]), task), []).append(row)

    lifetimes: list[dict[str, Any]] = []
    for (job, task), task_rows in sorted(grouped.items()):
        task_rows.sort(key=lambda row: integer(row.get("event_sequence")) or -1)
        first = task_rows[0]
        publications = [row for row in task_rows if row["label"] == "visual_command_publish"]
        state0 = [row for row in task_rows if row["label"] == "visual_child_state0"]
        cleanup = [row for row in task_rows if row["label"] == "visual_child_cleanup"]
        nested = [row for row in task_rows if row["label"] == "visual_child_nested"]
        rng_rows = [row for row in task_rows if integer(row.get("draws_consumed"))]
        lifetimes.append({
            "source_exec_job_id": job,
            "task_sequence": task,
            "action_ordinal": first.get("action_ordinal", ""),
            "origin_slot": first.get("origin_slot", ""),
            "target_slot": first.get("target_slot", ""),
            "command_kind": first.get("command_kind", ""),
            "child_kind": first.get("child_kind", ""),
            "resource": first.get("resource", ""),
            "record_index": first.get("record_index", ""),
            "epoch": first.get("epoch", ""),
            "payload_mode": first.get("payload_mode", ""),
            "effective_mode": next(
                (row["effective_mode"] for row in reversed(task_rows)
                 if row.get("effective_mode", "") != ""),
                "",
            ),
            "publication_sequence": publications[0]["event_sequence"] if publications else "",
            "publication_frame": publications[0]["frame_index"] if publications else "",
            "state0_sequence": state0[0]["event_sequence"] if state0 else "",
            "state0_frame": state0[0]["frame_index"] if state0 else "",
            "delay_visits": sum(row["label"] == "visual_child_delay" for row in task_rows),
            "nested_calls": len(nested),
            "rng_draws": sum(integer(row.get("draws_consumed")) or 0 for row in rng_rows),
            "cleanup_sequence": cleanup[-1]["event_sequence"] if cleanup else "",
            "cleanup_frame": cleanup[-1]["frame_index"] if cleanup else "",
            "complete": bool(cleanup),
        })
    return lifetimes


def divergence_snippets(rows: list[dict[str, Any]], radius: int = 3) -> list[dict[str, Any]]:
    snippets: list[dict[str, Any]] = []
    by_job: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        by_job.setdefault(int(row["source_exec_job_id"]), []).append(row)
    for job, job_rows in by_job.items():
        first = next(
            (
                index
                for index, row in enumerate(job_rows)
                if row["seed_match"] is False or row["source_match"] is False
            ),
            None,
        )
        if first is None:
            continue
        for row in job_rows[max(0, first - radius): first + radius + 1]:
            snippets.append({
                "source_exec_job_id": job,
                "relative_draw": row["relative_draw"],
                "live_draw_index": row["live_draw_index"],
                "live_source_pc": row["live_source_pc_hex"],
                "live_source": row["live_source"],
                "predictor_source_pc": row["predictor_source_pc_hex"],
                "predictor_source": row["predictor_source"],
                "live_seed_after": row["live_seed_after_hex"],
                "predictor_seed_after": row["predictor_seed_after_hex"],
                "source_match": row["source_match"],
                "seed_match": row["seed_match"],
            })
    return snippets


def movement_segment_summaries(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, int, int], list[dict[str, Any]]] = {}
    for row in rows:
        key = (
            int(row["source_exec_job_id"]),
            int(row["slot"]),
            int(row["segment_index"]),
        )
        grouped.setdefault(key, []).append(row)
    summaries: list[dict[str, Any]] = []
    for (job, slot, segment_index), segment_rows in sorted(grouped.items()):
        paired = [row for row in segment_rows if row["speed_match"] is not None]
        first = segment_rows[0]
        summaries.append({
            "source_exec_job_id": job,
            "slot": slot,
            "segment_index": segment_index,
            "segment_start_frame_offset": first["segment_start_frame_offset"],
            "live_segment_steps": first["live_segment_steps"],
            "predictor_segment_steps": first["predictor_segment_steps"],
            "paired_steps": len(paired),
            "speed_matches": sum(row["speed_match"] is True for row in paired),
            "vector_matches": sum(row["vector_match"] is True for row in paired),
            "first_live_speed": next(
                (row["live_speed"] for row in segment_rows if row["live_speed"] != ""), ""
            ),
            "first_predictor_speed": next(
                (
                    row["predictor_speed"]
                    for row in segment_rows
                    if row["predictor_speed"] != ""
                ),
                "",
            ),
        })
    return summaries


def predictor_path(directory: Path, job: int) -> Path:
    for name in (
        f"job_{job}.json",
        f"job_{job}.predictor.json",
        f"{job}.json",
        f"predictor_{job}.json",
    ):
        candidate = directory / name
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"no predictor JSON found for job {job} in {directory}")


def run_captures(run: Path) -> Iterable[tuple[int, Path, dict[str, Any]]]:
    manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8-sig"))
    jobs = manifest.get("jobs")
    if isinstance(jobs, list):
        for job_row in jobs:
            if not isinstance(job_row, dict):
                continue
            job = integer(job_row.get("original_exec_job_id"))
            capture = Path(str(job_row.get("stable_capture_path", "")))
            if job is not None and capture.exists():
                yield job, capture, job_row
        return
    job = integer(manifest.get("original_exec_job_id"))
    capture = run / "capture.jsonl"
    if job is not None and capture.exists():
        yield job, capture, manifest


def markdown_findings(summary: list[dict[str, Any]]) -> str:
    lines = [
        "# Predictor/live RNG, movement, and rotation comparison",
        "",
        "Full RNG and movement alignment uses the first `0x800145C8` direct-view placement caller, "
        "preferring a matching post-write seed and otherwise retaining caller-only alignment. Live "
        "seed-watch values are post-write because Dolphin mutates memory before breaking.",
        "",
        "| Job | RNG compared | Seed matches | Source matches | First source mismatch | First seed mismatch | Active movement rows | Delta matches | First movement mismatch |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in summary:
        def cell(key: str) -> Any:
            value = row.get(key, "")
            return "" if value is None else value

        lines.append(
            f"| {cell('source_exec_job_id')} | {cell('rng_compared_draws')} | "
            f"{cell('rng_seed_matches')} | {cell('rng_source_matches')}/"
            f"{cell('rng_known_source_draws')} | "
            f"{cell('first_rng_source_mismatch_relative_draw')} | "
            f"{cell('first_rng_seed_mismatch_relative_draw')} | "
            f"{cell('movement_active_rows')} | {cell('movement_delta_matches')}/"
            f"{cell('movement_comparable_deltas')} | "
            f"{cell('first_movement_delta_mismatch')} |"
        )
    lines.extend([
        "",
        "## Visual dispatcher",
        "",
        "Visual and placement callers are paired by subsystem ordinal, independently of RNG values. "
        "This keeps a missing or extra caller visible even after seed alignment has failed.",
        "",
        "| Job | SET live/predictor | SYSTEM CAMERA live/predictor | Synthetic live/predictor | Visual RNG live/predictor | Source matches | First mismatch |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for row in summary:
        def visual_cell(key: str) -> Any:
            value = row.get(key, "")
            return "" if value is None else value

        lines.append(
            f"| {visual_cell('source_exec_job_id')} | "
            f"{visual_cell('visual_live_set_command_publications')}/"
            f"{visual_cell('visual_predictor_set_command_publications')} | "
            f"{visual_cell('visual_live_system_camera_publications')}/"
            f"{visual_cell('visual_predictor_system_camera_publications')} | "
            f"{visual_cell('visual_live_synthetic_publications')}/"
            f"{visual_cell('visual_predictor_synthetic_publications')} | "
            f"{visual_cell('visual_live_rng_draws')}/"
            f"{visual_cell('visual_predictor_rng_draws')} | "
            f"{visual_cell('visual_rng_source_matches')} | "
            f"{visual_cell('first_visual_rng_source_mismatch_ordinal')} |"
        )
    lines.extend([
        "",
        "## Movement rate",
        "",
        "| Job | Live active steps | Predictor active steps | Paired steps | Speed matches | Vector matches | Segments live/predictor | Segment lengths | Start offsets (frames) |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ])
    for row in summary:
        def movement_cell(key: str) -> Any:
            value = row.get(key, "")
            return "" if value is None else value

        lines.append(
            f"| {movement_cell('source_exec_job_id')} | "
            f"{movement_cell('movement_live_active_steps')} | "
            f"{movement_cell('movement_predictor_active_steps')} | "
            f"{movement_cell('movement_paired_rate_steps')} | "
            f"{movement_cell('movement_speed_matches')} | "
            f"{movement_cell('movement_vector_matches')} | "
            f"{movement_cell('movement_live_segments')}/"
            f"{movement_cell('movement_predictor_segments')} | "
            f"{movement_cell('movement_segment_length_matches')}/"
            f"{movement_cell('movement_paired_segments')} | "
            f"{movement_cell('movement_segment_start_offsets')} |"
        )
    lines.extend([
        "",
        "## Rotation rate",
        "",
        "Rotation segments are paired by slot and segment ordinal after the same direct-view anchor. "
        "Speed compares absolute short-angle increments; direction and delta retain the sign.",
        "",
        "| Job | Live active steps | Predictor active steps | Paired steps | Full-step speed | Nominal segment speed | Direction matches | Exact deltas | Segments live/predictor | Segment lengths | Start offsets (frames) |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ])
    for row in summary:
        def rotation_cell(key: str) -> Any:
            value = row.get(key, "")
            return "" if value is None else value

        lines.append(
            f"| {rotation_cell('source_exec_job_id')} | "
            f"{rotation_cell('rotation_live_active_steps')} | "
            f"{rotation_cell('rotation_predictor_active_steps')} | "
            f"{rotation_cell('rotation_paired_rate_steps')} | "
            f"{rotation_cell('rotation_full_step_speed_matches')}/"
            f"{rotation_cell('rotation_full_step_pairs')} | "
            f"{rotation_cell('rotation_nominal_speed_matches')}/"
            f"{rotation_cell('rotation_nominal_speed_pairs')} | "
            f"{rotation_cell('rotation_direction_matches')} | "
            f"{rotation_cell('rotation_delta_matches')} | "
            f"{rotation_cell('rotation_live_segments')}/"
            f"{rotation_cell('rotation_predictor_segments')} | "
            f"{rotation_cell('rotation_segment_length_matches')}/"
            f"{rotation_cell('rotation_paired_segments')} | "
            f"{rotation_cell('rotation_segment_start_offsets')} |"
        )
    lines.extend([
        "",
        "Exact draw windows are in `rng_draw_comparison.csv`; the first mismatch window per job is in "
        "`rng_divergence_snippets.csv`. Frame positions, deltas, live increments, and worksheet state are in "
        "`movement_frame_comparison.csv`; segment-paired active-step rates are in "
        "`movement_rate_comparison.csv`; facing updates are in `rotation_rate_comparison.csv`. "
        "Predictor visual publications and child callbacks are in "
        "`visual_publication_timeline.csv`; reduced child lifetimes and visual-only draws are in "
        "`visual_child_lifetimes.csv` and `visual_rng_timeline.csv`. Caller-only live/predictor "
        "comparison is in `visual_rng_source_comparison.csv`.",
        "",
    ])
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="append", type=Path, required=True)
    parser.add_argument("--predictor-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-exec-job-id", action="append", type=int)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    all_rng: list[dict[str, Any]] = []
    all_movement: list[dict[str, Any]] = []
    all_movement_rates: list[dict[str, Any]] = []
    all_rotation_rates: list[dict[str, Any]] = []
    all_visual: list[dict[str, Any]] = []
    all_visual_rng_sources: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []
    selected_jobs = set(args.source_exec_job_id or [])
    for run in args.run:
        for job, capture, run_metadata in run_captures(run):
            if selected_jobs and job not in selected_jobs:
                continue
            events = load_jsonl(capture)
            predictor_document = json.loads(
                predictor_path(args.predictor_dir, job).read_text(encoding="utf-8-sig")
            )
            predictor = predictor_document.get("prediction", predictor_document)
            predicted_draws = expand_predictor_draws(predictor)
            live_draws = live_rng_draws(events)
            rng_rows, rng_summary = compare_rng(job, live_draws, predicted_draws)
            (
                movement_rows,
                movement_rate_rows,
                rotation_rate_rows,
                movement_summary,
            ) = compare_movement(job, events, predictor, rng_rows)
            visual_rows = predictor_visual_timeline(job, predictor)
            visual_source_rows, visual_source_summary = compare_visual_rng_sources(
                job, live_draws, predicted_draws
            )
            all_rng.extend(rng_rows)
            all_movement.extend(movement_rows)
            all_movement_rates.extend(movement_rate_rows)
            all_rotation_rates.extend(rotation_rate_rows)
            all_visual.extend(visual_rows)
            all_visual_rng_sources.extend(visual_source_rows)
            summaries.append({
                "source_exec_job_id": job,
                "run_root": str(run),
                "capture_path": str(capture),
                "terminal_state": run_metadata.get("terminal_state", ""),
                "capture_found": run_metadata.get("capture_found", False),
                **rng_summary,
                **movement_summary,
                **visual_publication_summary(events, visual_rows),
                **visual_source_summary,
            })

    write_csv(args.output / "rng_draw_comparison.csv", all_rng)
    write_csv(args.output / "rng_divergence_snippets.csv", divergence_snippets(all_rng))
    write_csv(args.output / "movement_frame_comparison.csv", all_movement)
    write_csv(args.output / "movement_rate_comparison.csv", all_movement_rates)
    write_csv(args.output / "rotation_rate_comparison.csv", all_rotation_rates)
    write_csv(
        args.output / "movement_segment_summary.csv",
        movement_segment_summaries(all_movement_rates),
    )
    write_csv(args.output / "summary.csv", summaries)
    write_csv(args.output / "visual_publication_timeline.csv", all_visual)
    write_csv(
        args.output / "visual_child_lifetimes.csv",
        predictor_visual_child_lifetimes(all_visual),
    )
    write_csv(
        args.output / "visual_rng_timeline.csv",
        [row for row in all_visual if integer(row.get("draws_consumed"))],
    )
    write_csv(
        args.output / "visual_rng_source_comparison.csv",
        all_visual_rng_sources,
    )
    (args.output / "findings.md").write_text(markdown_findings(summaries), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
