#!/usr/bin/env python3
"""Render first-turn combatant position captures as SVG path maps.

The default input is the render-focused movement capture under C:\\savor.  This
renderer intentionally consumes that capture schema directly: action windows
come from the captured selected-slot/action-sequence frame globals and movement
publish metadata comes from the captured FUN_8008178c rows.

Examples:
  python SavorPredict/tools/render_position_paths.py
  python SavorPredict/tools/render_position_paths.py --turn-progressions
  python SavorPredict/tools/render_position_paths.py --source-exec 1050837 --sequence 0
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont


DEFAULT_RUN_ROOT = Path(r"C:\savor\render_position_paths_corpus_rainbow_20260630_1")
DEFAULT_OUT_DIR = Path("Analyses") / "20260630_position_path_images_rainbow_corpus"
COMBINED_HEADER_PX = 76
COMBINED_FOOTER_PX = 310
COMBINED_ROW_TOP_PAD = 54
COMBINED_ROW_BOTTOM_PAD = 42

SLOTS = (0, 1, 4, 5)
SLOT_NAMES = {
    0: "slot 0 Vyse",
    1: "slot 1 Aika",
    4: "slot 4 Soldier",
    5: "slot 5 Soldier",
}
SLOT_COLORS = {
    0: "#0f766e",
    1: "#7c3aed",
    4: "#b45309",
    5: "#be123c",
}
TIME_GRADIENT_COLORS = (
    "#7c3aed",
    "#2563eb",
    "#0891b2",
    "#16a34a",
    "#eab308",
    "#f97316",
    "#dc2626",
)
ATTACK_ACTION_MODES = {0x04, 0x05}
DEATH_ACTION_MODE = 0x0E

ATTACK_ROUTE_WORKER_BY_PC = {
    "80086F48": ("pc_direct_attack_worker_call", 0),
    "80086F6C": ("pc_fallback_attack_worker_select", 1),
    "8008BDAC": ("enemy_direct_worker_select", 0),
    "8008BDDC": ("enemy_fallback_worker_select", 1),
}
ATTACK_ROUTE_CHECKPOINT_NAMES = {
    value[0] for value in ATTACK_ROUTE_WORKER_BY_PC.values()
}
ACTION_MODE_CHANGE_CHECKPOINT_NAMES = {
    "camode_writer_80022798",
    "camode_writer_800202FC",
    "camode_writer_8001C0D8",
}
ACTION_MODE_WRITER_LABEL_BY_PC = {
    "80022798": "queued_std_transition",
    "800202FC": "caller_requested_transition",
    "8001C0D8": "staged_mode_apply",
}
ACTION_MODE_NEW_MODE_FIELDS = (
    "new_mode_r4",
    "new_mode_r29",
    "new_mode_r0",
    "requested_mode_r4",
)
MOTION_APPLY_CALL_CHECKPOINT_NAMES = {
    "motion_apply_call_direct",
    "motion_apply_call_secondary",
}
ROTATION_FACING_STORE_CHECKPOINT_NAMES = {
    "rotation_facing_reached_store_after",
    "rotation_facing_step_store_after",
}

GRID_SIZE = 11
GRID_STAGE_STEP = 15.0
GRID_STAGE_ORIGIN = -75.0
PATH_MOVEMENT_EPSILON_PX = 0.05
PATH_MOVEMENT_EPSILON_STAGE = 0.001
RAW_MOTION_PATH_WIDTH = 4.2
FACING_RAY_LENGTH_PX = 13.0
FACING_RAY_WIDTH = 1.15
FACING_RAY_OPACITY = 0.78
RAW_MOTION_GRADIENT_SEGMENTS = 32
ATTACK_VECTOR_WIDTH = 1.6
ATTACK_VECTOR_TRIM_PX = 13.0
ATTACK_VECTOR_SEGMENTS = 36
ATTACK_COUNTER_VECTOR_OFFSET_PX = 8.0
ATTACK_ARROWHEAD_LENGTH_PX = 5.1
ATTACK_ARROWHEAD_WIDTH_PX = 3.05
DEATH_SKULL_OFFSET_PX = 15.0
DEATH_SKULL_RADIUS_PX = 6.0


@dataclass(frozen=True)
class PositionSample:
    seq: int
    frame: int
    vi_field_count: int
    positions: Dict[int, Tuple[float, float]]
    grids: Dict[int, Tuple[int, int]]
    active_actor_slot: Optional[int]
    action_sequence: Optional[int]


@dataclass(frozen=True)
class ActionBookmark:
    seq: int
    frame: int
    kind: str
    selected_slot: Optional[int]
    action_sequence: Optional[int]


@dataclass(frozen=True)
class ActionWindow:
    turn_index: int
    action_sequence: int
    active_slot: int
    start_seq: int
    end_seq: int
    first_frame_seq: int
    last_frame_seq: int
    selected_seq: Optional[int]
    scheduled_seq: Optional[int]


@dataclass(frozen=True)
class AttackRangeFact:
    seq: int
    actor_slot: int
    target_slot: Optional[int]
    instruction: Optional[int]
    instr_param_0x6: Optional[int]
    source_checkpoint: str
    worker_pc: Optional[str]


@dataclass(frozen=True)
class AttackVectorTiming:
    attacker_slot: int
    target_slot: int
    start_seq: int
    end_seq: int
    start_vi: int
    end_vi: int
    kind: str


@dataclass(frozen=True)
class RawMotionSegment:
    slot: int
    start_seq: int
    end_seq: int
    start_vi: int
    end_vi: int
    before_pos: Tuple[float, float]
    after_pos: Tuple[float, float]
    mode: Optional[int]
    increment: Tuple[float, float]
    target: Optional[Tuple[float, float]]
    reached: Optional[int]


@dataclass(frozen=True)
class FacingRaySample:
    slot: int
    seq: int
    vi_field_count: int
    position: Tuple[float, float]
    facing_degrees: float
    source_kind: str


@dataclass(frozen=True)
class DeathMarker:
    slot: int
    seq: int
    vi: int


@dataclass(frozen=True)
class ViTimeScale:
    min_vi: float
    max_vi: float

    def color(self, vi_field_count: float) -> str:
        return time_color(normalized_time(vi_field_count, self.min_vi, self.max_vi))


@dataclass(frozen=True)
class GridSnapshot:
    base_values: Tuple[int, ...]
    active_values: Tuple[int, ...]

    def terrain_value_at(self, x: int, z: int) -> int:
        index = z * GRID_SIZE + x
        return self.base_values[index]


@dataclass(frozen=True)
class TurnProgression:
    key: str
    title: str
    source_exec: str
    clone_exec: str
    turn_index: int
    action_sequence: int
    active_slot: int
    start_seq: int
    end_seq: int
    first_frame_seq: int
    last_frame_seq: int
    selected_seq: Optional[int]
    scheduled_seq: Optional[int]
    attack_range_kind: str
    attack_source_seq: Optional[int]
    attack_source_checkpoint: Optional[str]
    attack_worker_pc: Optional[str]
    attack_instruction: Optional[int]
    attack_target_slot: Optional[int]
    attack_instr_param_0x6: Optional[int]
    grid_snapshot: Optional[GridSnapshot]
    dead_slots: Tuple[int, ...]
    events: List[Dict[str, str]]
    capture_path: Path


TurnRecord = Tuple[TurnProgression, List[PositionSample]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render SVG maps of combatant raw-position paths from SavorPredict checkpoint captures."
    )
    parser.add_argument(
        "--run-root",
        type=Path,
        default=DEFAULT_RUN_ROOT,
        help=f"Checkpoint run root. Default: {DEFAULT_RUN_ROOT}",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help=f"Directory for generated SVGs. Default: {DEFAULT_OUT_DIR}",
    )
    parser.add_argument(
        "--examples",
        action="store_true",
        help="Render the default combined turn overview from the render-position capture root.",
    )
    parser.add_argument(
        "--turn-progressions",
        action="store_true",
        help=(
            "Also render one SVG per complete action window in the capture. "
            "If --source-exec is omitted, every row in summary.csv is rendered."
        ),
    )
    parser.add_argument(
        "--source-exec",
        help="Render only one source execution job.",
    )
    parser.add_argument(
        "--sequence",
        type=int,
        help="Render only this raw action_sequence value from the action-window stream.",
    )
    parser.add_argument(
        "--terrain-square",
        action="append",
        default=[],
        metavar="X,Z",
        help=(
            "Fallback SST terrain/blocked square to fill pink when a capture lacks "
            "a grid snapshot. Repeatable."
        ),
    )
    parser.add_argument(
        "--cell-px",
        type=int,
        default=52,
        help="SVG pixels per grid square.",
    )
    parser.add_argument(
        "--export-png",
        action="store_true",
        help="Also export PNGs next to generated SVGs using Pillow.",
    )
    return parser.parse_args()


def read_csv_rows(path: Path, delimiter: str = ",") -> List[Dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle, delimiter=delimiter))


def read_summary(run_root: Path) -> Dict[str, Dict[str, str]]:
    summary_path = run_root / "summary.csv"
    if not summary_path.exists():
        manifest_path = run_root / "manifest.json"
        if not manifest_path.exists():
            raise FileNotFoundError(f"Expected {summary_path} or {manifest_path}")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        row = {
            "original_exec_job_id": str(manifest["original_exec_job_id"]),
            "cloned_exec_job_id": str(manifest["cloned_exec_job_id"]),
            "stable_capture_path": str(manifest.get("stable_capture_path", "")),
        }
        return {row["original_exec_job_id"]: row}
    rows = read_csv_rows(summary_path)
    return {row["original_exec_job_id"]: row for row in rows}


def maybe_int(value: object) -> Optional[int]:
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text)
    except ValueError:
        return None


def normalized_pc(value: object) -> str:
    text = str(value or "").strip().upper()
    if text.startswith("0X"):
        text = text[2:]
    return text


def format_hex(value: Optional[int], width: int = 2) -> str:
    if value is None:
        return "?"
    return f"0x{value:0{width}X}"


def required_int(value: object, label: str) -> int:
    parsed = maybe_int(value)
    if parsed is None:
        raise ValueError(f"Missing integer value for {label}: {value!r}")
    return parsed


def decode_float_bits(value: object) -> Optional[float]:
    parsed = maybe_int(value)
    if parsed is None:
        return None
    data = parsed.to_bytes(4, byteorder="big", signed=False)
    return struct.unpack(">f", data)[0]


def decode_short_angle_degrees(value: object) -> Optional[float]:
    parsed = maybe_int(value)
    if parsed is None:
        return None
    return ((parsed & 0xFFFF) * 360.0) / 65536.0


def finite_float_degrees(value: object) -> Optional[float]:
    angle = decode_float_bits(value)
    if angle is None or not math.isfinite(angle):
        return None
    return angle


def parse_grid_pair(text: str) -> Optional[Tuple[int, int]]:
    cleaned = text.strip().strip("()")
    if not cleaned:
        return None
    pieces = cleaned.split(",")
    if len(pieces) != 2:
        return None
    try:
        return int(pieces[0]), int(pieces[1])
    except ValueError:
        return None


def parse_publish_grid(text: str) -> Tuple[Optional[Tuple[int, int]], Optional[Tuple[int, int]]]:
    if "->" not in text:
        return None, None
    left, right = text.split("->", 1)
    return parse_grid_pair(left), parse_grid_pair(right)


def parse_path_nodes(text: str) -> List[Tuple[int, int, int]]:
    nodes: List[Tuple[int, int, int]] = []
    for piece in text.split():
        if ":" not in piece:
            continue
        index_text, pair_text = piece.split(":", 1)
        try:
            index = int(index_text)
        except ValueError:
            continue
        pair = parse_grid_pair(pair_text)
        if pair is None:
            continue
        x, z = pair
        if x == 0xFF or z == 0xFF:
            break
        if x <= 0 or z <= 0 or x >= GRID_SIZE - 1 or z >= GRID_SIZE - 1:
            continue
        nodes.append((index, x, z))
    return nodes


def stage_from_grid(grid_value: int) -> float:
    return GRID_STAGE_ORIGIN + grid_value * GRID_STAGE_STEP


def grid_float_from_stage(stage_value: float) -> float:
    return (stage_value - GRID_STAGE_ORIGIN) / GRID_STAGE_STEP


def terrain_squares_from_args(values: Sequence[str]) -> set[Tuple[int, int]]:
    result: set[Tuple[int, int]] = set()
    for value in values:
        pair = parse_grid_pair(value)
        if pair is None:
            raise ValueError(f"Bad --terrain-square value: {value!r}")
        result.add(pair)
    return result


def is_grid_snapshot_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return checkpoint_name == "after_setup_battle_grid" or checkpoint == "grid_snapshot"


def decode_grid_values(row: Dict[str, object], prefix: str) -> Optional[Tuple[int, ...]]:
    values: List[int] = []
    for offset in range(0, GRID_SIZE * GRID_SIZE - 1, 8):
        chunk = maybe_int(row.get(f"{prefix}_bytes_{offset:03d}_{offset + 7:03d}"))
        if chunk is None:
            return None
        values.extend((chunk & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "big"))

    final_value = maybe_int(row.get(f"{prefix}_byte_120"))
    if final_value is None:
        return None
    values.append(final_value & 0xFF)
    if len(values) != GRID_SIZE * GRID_SIZE:
        return None
    return tuple(values)


def load_grid_snapshot(capture_path: Path) -> Optional[GridSnapshot]:
    for row in iter_capture_rows(capture_path):
        if not is_grid_snapshot_row(row):
            continue
        base_values = decode_grid_values(row, "base_grid")
        active_values = decode_grid_values(row, "active_grid")
        if base_values is None or active_values is None:
            continue
        return GridSnapshot(base_values=base_values, active_values=active_values)
    return None


def summary_capture_path(summary: Dict[str, Dict[str, str]], source_exec: str, run_root: Path) -> Tuple[str, Path]:
    row = summary.get(source_exec)
    if row is None:
        raise ValueError(f"Source exec {source_exec} is not present in {run_root / 'summary.csv'}")
    clone_exec = row["cloned_exec_job_id"]
    path_text = row.get("stable_capture_path", "")
    capture_path = Path(path_text) if path_text else run_root / "captures" / f"job-{clone_exec}.jsonl"
    return clone_exec, capture_path


def is_frame_position_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    if checkpoint_name == "battle_case5_after_threads":
        return True
    return checkpoint == "case5_after_runBattleThreads"


def is_action_bookmark_row(row: Dict[str, object], kind: str) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return checkpoint_name == kind or checkpoint == kind


def is_movement_publish_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return checkpoint_name == "movement_commit_entry" or checkpoint == "movement_publish"


def is_action_mode_change_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    if checkpoint == "action_mode_change":
        return True
    if checkpoint_name in ACTION_MODE_CHANGE_CHECKPOINT_NAMES:
        return True
    return normalized_pc(row.get("pc")) in ACTION_MODE_WRITER_LABEL_BY_PC


def is_attack_range_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    if normalized_pc(row.get("pc")) in ATTACK_ROUTE_WORKER_BY_PC:
        return True
    return checkpoint == "attack_route_used" or checkpoint_name in ATTACK_ROUTE_CHECKPOINT_NAMES


def is_motion_apply_call_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return (
        checkpoint_name in MOTION_APPLY_CALL_CHECKPOINT_NAMES
        or checkpoint in MOTION_APPLY_CALL_CHECKPOINT_NAMES
    )


def is_motion_apply_result_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return checkpoint_name == "motion_apply_result" or checkpoint == "motion_apply_result"


def is_action_motion_setup_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return (
        checkpoint_name == "action_motion_setup_after_increment"
        or checkpoint == "action_motion_setup_after_increment"
    )


def is_rotation_facing_store_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    return (
        checkpoint_name in ROTATION_FACING_STORE_CHECKPOINT_NAMES
        or checkpoint in ROTATION_FACING_STORE_CHECKPOINT_NAMES
    )


def slot_position_from_row(row: Dict[str, object], slot: int) -> Optional[Tuple[float, float]]:
    x = decode_float_bits(row.get(f"pos{slot}_x_bits"))
    z = decode_float_bits(row.get(f"pos{slot}_z_bits"))
    if x is None or z is None:
        return None
    if not math.isfinite(x) or not math.isfinite(z):
        return None
    return x, z


def slot_grid_from_row(row: Dict[str, object], slot: int) -> Optional[Tuple[int, int]]:
    x = maybe_int(row.get(f"slot{slot}_movement_worksheet_cur_grid_x_0x0c"))
    z = maybe_int(row.get(f"slot{slot}_movement_worksheet_cur_grid_z_0x0d"))
    if x is None or z is None:
        return None
    return x, z


def load_position_samples(capture_path: Path, start_seq: int, end_seq: int) -> List[PositionSample]:
    samples: List[PositionSample] = []
    with capture_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            seq = maybe_int(row.get("capture_sequence"))
            if seq is None:
                continue
            if seq < start_seq:
                continue
            if seq > end_seq:
                break
            if not is_frame_position_row(row):
                continue

            positions: Dict[int, Tuple[float, float]] = {}
            grids: Dict[int, Tuple[int, int]] = {}
            for slot in SLOTS:
                position = slot_position_from_row(row, slot)
                if position is not None:
                    positions[slot] = position
                grid = slot_grid_from_row(row, slot)
                if grid is not None:
                    grids[slot] = grid

            if positions:
                samples.append(
                    PositionSample(
                        seq=seq,
                        frame=required_int(row.get("frame_count"), "frame_count"),
                        vi_field_count=required_int(
                            row.get("vi_field_count"), "vi_field_count"
                        ),
                        positions=positions,
                        grids=grids,
                        active_actor_slot=maybe_int(row.get("selected_slot_80347334")),
                        action_sequence=maybe_int(row.get("action_sequence_80347335")),
                    )
                )
    return samples


def load_all_position_samples(capture_path: Path) -> List[PositionSample]:
    return load_position_samples(capture_path, 0, 2_147_483_647)


def iter_capture_rows(capture_path: Path) -> Iterable[Dict[str, object]]:
    with capture_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def finite_stage_pair(x_value: object, z_value: object) -> Optional[Tuple[float, float]]:
    x = decode_float_bits(x_value)
    z = decode_float_bits(z_value)
    if x is None or z is None:
        return None
    if not math.isfinite(x) or not math.isfinite(z):
        return None
    return x, z


def load_raw_motion_segments(capture_path: Path, start_seq: int, end_seq: int) -> List[RawMotionSegment]:
    segments: List[RawMotionSegment] = []
    pending_call: Optional[Dict[str, object]] = None

    for row in iter_capture_rows(capture_path):
        seq = maybe_int(row.get("capture_sequence"))
        if seq is None:
            continue
        if seq > end_seq:
            break

        if is_motion_apply_call_row(row):
            pending_call = row
            continue

        if not is_motion_apply_result_row(row):
            continue
        if pending_call is None:
            continue
        if seq < start_seq:
            pending_call = None
            continue

        call_seq = maybe_int(pending_call.get("capture_sequence"))
        call_vi = maybe_int(pending_call.get("vi_field_count"))
        result_vi = maybe_int(row.get("vi_field_count"))
        slot = slot_id_or_none(pending_call.get("inst_slot_0x00"))
        before_pos = finite_stage_pair(pending_call.get("cur_x_before"), pending_call.get("cur_z_before"))
        after_pos = finite_stage_pair(row.get("cur_x_after"), row.get("cur_z_after"))
        increment = finite_stage_pair(row.get("increment_x_bits"), row.get("increment_z_bits"))
        if increment is None:
            increment = finite_stage_pair(
                pending_call.get("increment_x_bits"),
                pending_call.get("increment_z_bits"),
            )
        if (
            call_seq is None
            or call_vi is None
            or result_vi is None
            or slot is None
            or before_pos is None
            or after_pos is None
            or increment is None
        ):
            pending_call = None
            continue

        reached = maybe_int(row.get("reached_flag_r8"))
        moved = math.hypot(after_pos[0] - before_pos[0], after_pos[1] - before_pos[1])
        if moved >= PATH_MOVEMENT_EPSILON_STAGE or reached:
            segments.append(
                RawMotionSegment(
                    slot=slot,
                    start_seq=call_seq,
                    end_seq=seq,
                    start_vi=call_vi,
                    end_vi=result_vi,
                    before_pos=before_pos,
                    after_pos=after_pos,
                    mode=maybe_int(pending_call.get("inst_action_mode_0x06")),
                    increment=increment,
                    target=finite_stage_pair(
                        pending_call.get("inst_target_x_0x110"),
                        pending_call.get("inst_target_z_0x118"),
                    ),
                    reached=reached,
                )
            )
        pending_call = None

    return sorted(segments, key=lambda item: (item.end_seq, item.slot))


def latest_frame_position_before(
    samples: Sequence[PositionSample],
    slot: int,
    seq: int,
) -> Optional[Tuple[float, float]]:
    candidates = [
        sample
        for sample in samples
        if sample.seq <= seq and slot in sample.positions
    ]
    if candidates:
        return max(candidates, key=lambda sample: (sample.seq, sample.vi_field_count)).positions[slot]
    candidates = [sample for sample in samples if slot in sample.positions]
    if not candidates:
        return None
    return min(candidates, key=lambda sample: (abs(sample.seq - seq), sample.seq)).positions[slot]


def load_facing_ray_samples(
    capture_path: Path,
    start_seq: int,
    end_seq: int,
    frame_samples: Sequence[PositionSample],
    raw_segments: Sequence[RawMotionSegment],
) -> List[FacingRaySample]:
    rays: List[FacingRaySample] = []
    latest_position_by_slot: Dict[int, Tuple[float, float]] = {}
    latest_facing_by_slot: Dict[int, float] = {}
    raw_by_end_seq: Dict[int, List[RawMotionSegment]] = {}
    for segment in raw_segments:
        raw_by_end_seq.setdefault(segment.end_seq, []).append(segment)

    for row in iter_capture_rows(capture_path):
        seq = maybe_int(row.get("capture_sequence"))
        if seq is None:
            continue
        if seq > end_seq:
            break
        if seq < start_seq:
            continue
        vi_field_count = maybe_int(row.get("vi_field_count"))
        if vi_field_count is None:
            continue

        if is_action_motion_setup_row(row):
            slot = slot_id_or_none(row.get("inst_slot_0x00"))
            position = finite_stage_pair(row.get("cw_cur_x_0x1c"), row.get("cw_cur_z_0x24"))
            facing = decode_short_angle_degrees(row.get("cw_facing_0x2c"))
            if facing is None:
                facing = finite_float_degrees(row.get("inst_turn_current_0x11c"))
            if slot is None or position is None:
                continue
            latest_position_by_slot[slot] = position
            if facing is not None:
                latest_facing_by_slot[slot] = facing
                rays.append(
                    FacingRaySample(
                        slot=slot,
                        seq=seq,
                        vi_field_count=vi_field_count,
                        position=position,
                        facing_degrees=facing,
                        source_kind="motion_setup",
                    )
                )
            continue

        if is_rotation_facing_store_row(row):
            slot = slot_id_or_none(row.get("inst_slot_0x00"))
            facing = decode_short_angle_degrees(row.get("cw_facing_after_0x2c"))
            if facing is None:
                facing = finite_float_degrees(row.get("inst_turn_current_0x11c"))
            if slot is None or facing is None:
                continue
            position = latest_position_by_slot.get(slot)
            if position is None:
                position = latest_frame_position_before(frame_samples, slot, seq)
            if position is None:
                continue
            latest_facing_by_slot[slot] = facing
            rays.append(
                FacingRaySample(
                    slot=slot,
                    seq=seq,
                    vi_field_count=vi_field_count,
                    position=position,
                    facing_degrees=facing,
                    source_kind=str(row.get("checkpoint_name") or "rotation_facing_store"),
                )
            )
            continue

        if is_motion_apply_result_row(row):
            for segment in raw_by_end_seq.get(seq, []):
                latest_position_by_slot[segment.slot] = segment.after_pos
                facing = latest_facing_by_slot.get(segment.slot)
                if facing is None:
                    continue
                rays.append(
                    FacingRaySample(
                        slot=segment.slot,
                        seq=segment.end_seq,
                        vi_field_count=segment.end_vi,
                        position=segment.after_pos,
                        facing_degrees=facing,
                        source_kind="raw_motion",
                    )
                )

    return sorted(rays, key=lambda item: (item.seq, item.slot, item.source_kind))


def load_action_bookmarks(capture_path: Path) -> List[ActionBookmark]:
    bookmarks: List[ActionBookmark] = []
    for row in iter_capture_rows(capture_path):
        if is_action_bookmark_row(row, "action_slot_selected"):
            kind = "selected"
        elif is_action_bookmark_row(row, "action_scheduled"):
            kind = "scheduled"
        else:
            continue

        seq = maybe_int(row.get("capture_sequence"))
        if seq is None:
            continue
        bookmarks.append(
            ActionBookmark(
                seq=seq,
                frame=required_int(row.get("frame_count"), "frame_count"),
                kind=kind,
                selected_slot=maybe_int(row.get("selected_slot_80347334")),
                action_sequence=maybe_int(row.get("action_sequence_80347335")),
            )
        )
    return sorted(bookmarks, key=lambda item: item.seq)


def queued_slot_field(row: Dict[str, object], slot: int, suffix: str) -> Optional[int]:
    return maybe_int(row.get(f"slot{slot}_{suffix}"))


def slot_id_or_none(value: object) -> Optional[int]:
    parsed = maybe_int(value)
    return parsed if parsed in SLOTS else None


def direct_or_queued_slot_field(
    row: Dict[str, object],
    actor_slot: int,
    direct_names: Sequence[str],
    suffix: str,
) -> Optional[int]:
    for name in direct_names:
        direct = maybe_int(row.get(name))
        if direct is not None:
            return direct
    return queued_slot_field(row, actor_slot, suffix)


def direct_or_queued_target_slot(row: Dict[str, object], actor_slot: int) -> Optional[int]:
    for name in ("target_slot", "target"):
        direct = slot_id_or_none(row.get(name))
        if direct is not None:
            return direct
    return queued_slot_field(row, actor_slot, "target_0x4")


def extract_attack_range_fact(row: Dict[str, object]) -> Optional[AttackRangeFact]:
    seq = maybe_int(row.get("capture_sequence"))
    actor_slot = slot_id_or_none(row.get("selected_slot_80347334"))
    if actor_slot is None:
        actor_slot = slot_id_or_none(row.get("actor_slot"))
    if seq is None or actor_slot is None:
        return None

    pc = normalized_pc(row.get("pc"))
    route = ATTACK_ROUTE_WORKER_BY_PC.get(pc)
    implied_param = route[1] if route is not None else None
    instruction = direct_or_queued_slot_field(
        row,
        actor_slot,
        ("instruction", "queued_instruction", "instruction_0x0"),
        "instruction_0x0",
    )
    target_slot = direct_or_queued_target_slot(row, actor_slot)
    instr_param = direct_or_queued_slot_field(
        row,
        actor_slot,
        ("instr_param_0x6", "instr_param", "param_0x6"),
        "instr_param_0x6",
    )
    if instr_param is None:
        instr_param = implied_param
    checkpoint = str(row.get("checkpoint_name") or row.get("checkpoint") or "attack_route_used")
    return AttackRangeFact(
        seq=seq,
        actor_slot=actor_slot,
        target_slot=target_slot,
        instruction=instruction,
        instr_param_0x6=instr_param,
        source_checkpoint=checkpoint,
        worker_pc=pc if pc else None,
    )


def load_attack_range_facts(capture_path: Path) -> List[AttackRangeFact]:
    facts: List[AttackRangeFact] = []
    for row in iter_capture_rows(capture_path):
        if not is_attack_range_row(row):
            continue
        fact = extract_attack_range_fact(row)
        if fact is not None:
            facts.append(fact)
    return sorted(facts, key=lambda item: item.seq)


def matching_bookmark_seq(
    bookmarks: Sequence[ActionBookmark],
    kind: str,
    action_sequence: int,
    active_slot: int,
    first_frame_seq: int,
    previous_frame_seq: Optional[int],
) -> Optional[int]:
    candidates = [
        bookmark
        for bookmark in bookmarks
        if bookmark.kind == kind
        and bookmark.action_sequence == action_sequence
        and bookmark.selected_slot == active_slot
        and bookmark.seq <= first_frame_seq
        and (previous_frame_seq is None or bookmark.seq > previous_frame_seq)
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda item: item.seq).seq


def build_action_windows(samples: Sequence[PositionSample], bookmarks: Sequence[ActionBookmark]) -> List[ActionWindow]:
    keyed_samples = [
        sample
        for sample in samples
        if sample.action_sequence is not None and sample.active_actor_slot is not None
    ]
    if not keyed_samples:
        return []

    windows: List[ActionWindow] = []
    scheduled_bookmarks = [
        bookmark
        for bookmark in bookmarks
        if bookmark.kind == "scheduled"
        and bookmark.action_sequence is not None
        and bookmark.selected_slot is not None
    ]

    def append_window(
        action_sequence: int,
        active_slot: int,
        start_boundary: int,
        end_boundary: int,
        selected_seq: Optional[int],
        scheduled_seq: Optional[int],
    ) -> None:
        frame_samples = [
            sample
            for sample in keyed_samples
            if start_boundary <= sample.seq <= end_boundary
        ]
        if not frame_samples:
            return

        first_sample = frame_samples[0]
        last_sample = frame_samples[-1]
        start_candidates = [start_boundary]
        if selected_seq is not None:
            start_candidates.append(selected_seq)
        if scheduled_seq is not None:
            start_candidates.append(scheduled_seq)
        windows.append(
            ActionWindow(
                turn_index=len(windows),
                action_sequence=action_sequence,
                active_slot=active_slot,
                start_seq=min(start_candidates),
                end_seq=end_boundary,
                first_frame_seq=first_sample.seq,
                last_frame_seq=last_sample.seq,
                selected_seq=selected_seq,
                scheduled_seq=scheduled_seq,
            )
        )

    if not scheduled_bookmarks:
        first_sample = keyed_samples[0]
        append_window(
            required_int(first_sample.action_sequence, "action_sequence"),
            required_int(first_sample.active_actor_slot, "active_actor_slot"),
            first_sample.seq,
            keyed_samples[-1].seq,
            None,
            None,
        )
        return windows

    accepted_actions: List[Tuple[ActionBookmark, int, int, Optional[int], int]] = []
    for scheduled in scheduled_bookmarks:
        action_sequence = required_int(scheduled.action_sequence, "action_sequence")
        active_slot = required_int(scheduled.selected_slot, "selected_slot")
        selected_seq = matching_bookmark_seq(
            bookmarks,
            "selected",
            action_sequence,
            active_slot,
            scheduled.seq,
            None,
        )
        action_start_seq = min(scheduled.seq, selected_seq) if selected_seq is not None else scheduled.seq
        accepted_actions.append((scheduled, action_sequence, active_slot, selected_seq, action_start_seq))

    for index, (scheduled, action_sequence, active_slot, selected_seq, _action_start_seq) in enumerate(accepted_actions):
        next_action_start = accepted_actions[index + 1][4] if index + 1 < len(accepted_actions) else None
        end_boundary = next_action_start - 1 if next_action_start is not None else keyed_samples[-1].seq
        append_window(
            action_sequence,
            active_slot,
            scheduled.seq,
            end_boundary,
            selected_seq,
            scheduled.seq,
        )
    return windows


def movement_path_node_value(row: Dict[str, object], index: int, axis: str) -> Optional[int]:
    offset = 0x17 + index * 2 if axis == "x" else 0x18 + index * 2
    return maybe_int(row.get(f"movement_path_node{index}_{axis}_0x{offset:x}"))


def normalize_movement_publish_row(row: Dict[str, object]) -> Optional[Dict[str, str]]:
    seq = maybe_int(row.get("capture_sequence"))
    slot = maybe_int(row.get("slot"))
    cur_x = maybe_int(row.get("movement_cur_x_0x0c"))
    cur_z = maybe_int(row.get("movement_cur_z_0x0d"))
    next_x = maybe_int(row.get("next_grid_x"))
    next_z = maybe_int(row.get("next_grid_z"))
    if seq is None or slot is None or cur_x is None or cur_z is None or next_x is None or next_z is None:
        return None

    path_nodes: List[str] = []
    for index in range(8):
        node_x = movement_path_node_value(row, index, "x")
        node_z = movement_path_node_value(row, index, "z")
        if node_x is None or node_z is None:
            continue
        path_nodes.append(f"{index}:({node_x},{node_z})")

    dx = next_x - cur_x
    dz = next_z - cur_z
    cheb = max(abs(dx), abs(dz))
    path_index = maybe_int(row.get("movement_path_index_0x15"))
    selected_node = f"({next_x},{next_z})"
    if path_index is not None and 0 <= path_index < 8:
        selected_x = movement_path_node_value(row, path_index, "x")
        selected_z = movement_path_node_value(row, path_index, "z")
        if selected_x is not None and selected_z is not None and selected_x > 0 and selected_z > 0:
            selected_node = f"({selected_x},{selected_z})"

    selected_slot = maybe_int(row.get("selected_slot_80347334"))
    action_sequence = maybe_int(row.get("action_sequence_80347335"))
    dist_to_target = maybe_int(row.get("movement_dist_to_target_0x14"))
    status = maybe_int(row.get("movement_status_0x16"))
    normalized = {
        "event_kind": "movement_publish",
        "capture_sequence": str(seq),
        "slot": str(slot),
        "selected_slot": str(selected_slot) if selected_slot is not None else "",
        "action_sequence": str(action_sequence) if action_sequence is not None else "",
        "publish_grid": f"({cur_x},{cur_z})->({next_x},{next_z})",
        "cur_grid_x": str(cur_x),
        "cur_grid_z": str(cur_z),
        "next_grid_x": str(next_x),
        "next_grid_z": str(next_z),
        "cheb": str(cheb),
        "axis": "true" if dx == 0 or dz == 0 else "false",
        "diagonal": "true" if abs(dx) == abs(dz) else "false",
        "path_index": str(path_index) if path_index is not None else "",
        "dist_to_target": str(dist_to_target) if dist_to_target is not None else "",
        "status": str(status) if status is not None else "",
        "selected_node": selected_node,
        "path_nodes": " ".join(path_nodes),
    }
    return normalized


def first_int_field(row: Dict[str, object], names: Sequence[str]) -> Tuple[Optional[int], str]:
    for name in names:
        parsed = maybe_int(row.get(name))
        if parsed is not None:
            return parsed, name
    return None, ""


def normalize_action_mode_change_row(row: Dict[str, object]) -> Optional[Dict[str, str]]:
    seq = maybe_int(row.get("capture_sequence"))
    vi_field_count = maybe_int(row.get("vi_field_count"))
    slot = maybe_int(row.get("target_slot_0x00"))
    old_mode = maybe_int(row.get("old_mode_0x06"))
    new_mode, new_mode_field = first_int_field(row, ACTION_MODE_NEW_MODE_FIELDS)
    if seq is None or vi_field_count is None or slot is None or old_mode is None or new_mode is None:
        return None
    if old_mode == new_mode:
        return None

    pc = normalized_pc(row.get("pc"))
    selected_slot = maybe_int(row.get("selected_slot_80347334"))
    action_sequence = maybe_int(row.get("action_sequence_80347335"))
    target_slot = maybe_int(row.get("target_target_0x04"))
    staged_mode = maybe_int(row.get("staged_mode_0x0a"))
    subtype = maybe_int(row.get("subtype_0x08"))
    control = maybe_int(row.get("control_0x12"))
    current_state = maybe_int(row.get("current_state_0x20"))
    previous_mode = maybe_int(row.get("previous_mode_0x1c"))
    queued_state = maybe_int(row.get("queued_state_0x24"))
    writer = ACTION_MODE_WRITER_LABEL_BY_PC.get(pc, str(row.get("checkpoint_name") or "action_mode_writer"))
    return {
        "event_kind": "action_mode_change",
        "capture_sequence": str(seq),
        "vi_field_count": str(vi_field_count),
        "slot": str(slot),
        "target_slot": str(target_slot) if target_slot is not None else "",
        "selected_slot": str(selected_slot) if selected_slot is not None else "",
        "action_sequence": str(action_sequence) if action_sequence is not None else "",
        "old_mode": format_hex(old_mode),
        "new_mode": format_hex(new_mode),
        "old_mode_raw": str(old_mode),
        "new_mode_raw": str(new_mode),
        "new_mode_field": new_mode_field,
        "previous_mode": format_hex(previous_mode),
        "staged_mode": format_hex(staged_mode),
        "subtype": format_hex(subtype),
        "control": format_hex(control),
        "current_state": format_hex(current_state),
        "queued_state": format_hex(queued_state),
        "writer": writer,
        "pc": pc,
    }


def load_timeline_events(capture_path: Path) -> List[Dict[str, str]]:
    events: List[Dict[str, str]] = []
    for row in iter_capture_rows(capture_path):
        normalized: Optional[Dict[str, str]] = None
        if is_movement_publish_row(row):
            normalized = normalize_movement_publish_row(row)
        elif is_action_mode_change_row(row):
            normalized = normalize_action_mode_change_row(row)
        if normalized is not None:
            events.append(normalized)
    return sorted(events, key=lambda item: event_sequence(item) or -1)


def rgb_from_hex(color: str) -> Tuple[int, int, int]:
    color = color.lstrip("#")
    return int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)


def hex_from_rgb(rgb: Tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def lerp_color(left: str, right: str, t: float) -> str:
    t = min(1.0, max(0.0, t))
    a = rgb_from_hex(left)
    b = rgb_from_hex(right)
    return hex_from_rgb(tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3)))  # type: ignore[arg-type]


def gradient_color(stops: Sequence[str], t: float) -> str:
    if not stops:
        return "#111827"
    if len(stops) == 1:
        return stops[0]
    t = min(1.0, max(0.0, t))
    scaled = t * (len(stops) - 1)
    left_index = min(len(stops) - 2, int(math.floor(scaled)))
    local_t = scaled - left_index
    return lerp_color(stops[left_index], stops[left_index + 1], local_t)


def time_color(t: float) -> str:
    return gradient_color(TIME_GRADIENT_COLORS, t)


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


class SvgMap:
    def __init__(self, cell_px: int, panel_px: int = 430, height_px: Optional[int] = None) -> None:
        self.cell_px = cell_px
        self.margin_left = 72
        self.margin_top = 112
        self.margin_right = 72
        self.margin_bottom = 260
        self.grid_px = GRID_SIZE * cell_px
        self.panel_px = panel_px
        self.width = self.margin_left + self.grid_px + self.panel_px + self.margin_right
        self.height = height_px if height_px is not None else self.margin_top + self.grid_px + self.margin_bottom
        self.parts: List[str] = []

    def px_from_grid_float(self, grid_x: float, grid_z: float) -> Tuple[float, float]:
        return self.margin_left + grid_x * self.cell_px, self.margin_top + grid_z * self.cell_px

    def px_from_stage(self, x: float, z: float) -> Tuple[float, float]:
        # Stage coordinates from this first-battle capture are square centers:
        # grid 0 -> stage -75, grid 1 -> -60, etc.  Shift by half a cell so
        # raw combatant positions render at grid-square centers, not corners.
        return self.px_from_grid_float(grid_float_from_stage(x) + 0.5, grid_float_from_stage(z) + 0.5)

    def px_from_grid(self, x: int, z: int) -> Tuple[float, float]:
        return self.px_from_grid_float(x + 0.5, z + 0.5)

    def add(self, text: str) -> None:
        self.parts.append(text)

    def line(
        self,
        x1: float,
        y1: float,
        x2: float,
        y2: float,
        stroke: str,
        width: float = 1.0,
        opacity: float = 1.0,
        dash: Optional[str] = None,
        marker_end: bool = False,
    ) -> None:
        dash_attr = f' stroke-dasharray="{esc(dash)}"' if dash else ""
        marker = ""
        if marker_end:
            marker = ' marker-end="url(#arrow)"'
        self.add(
            f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" '
            f'stroke="{stroke}" stroke-width="{width:.2f}" opacity="{opacity:.3f}" '
            f'stroke-linecap="round"{dash_attr}{marker}/>'
        )

    def polygon(
        self,
        points: Sequence[Tuple[float, float]],
        fill: str,
        opacity: float = 1.0,
    ) -> None:
        point_text = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
        self.add(f'<polygon points="{point_text}" fill="{fill}" opacity="{opacity:.3f}"/>')

    def circle(
        self,
        x: float,
        y: float,
        r: float,
        fill: str,
        stroke: str = "#111827",
        width: float = 1.0,
        opacity: float = 1.0,
    ) -> None:
        self.add(
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r:.2f}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{width:.2f}" opacity="{opacity:.3f}"/>'
        )

    def text(
        self,
        x: float,
        y: float,
        value: object,
        size: int = 13,
        fill: str = "#111827",
        weight: str = "400",
        anchor: str = "start",
    ) -> None:
        self.add(
            f'<text x="{x:.2f}" y="{y:.2f}" font-family="Segoe UI, Arial, sans-serif" '
            f'font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}">{esc(value)}</text>'
        )

    def rect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        fill: str,
        stroke: str = "#9ca3af",
        stroke_width: float = 1.0,
        opacity: float = 1.0,
    ) -> None:
        self.add(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{width:.2f}" height="{height:.2f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width:.2f}" '
            f'opacity="{opacity:.3f}"/>'
        )

    def start(self) -> None:
        self.add(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.width}" height="{self.height}" '
            f'viewBox="0 0 {self.width} {self.height}">'
        )
        self.add("<defs>")
        self.add(
            '<marker id="arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" '
            'orient="auto" markerUnits="strokeWidth">'
            '<path d="M 0 0 L 8 4 L 0 8 z" fill="#111827"/></marker>'
        )
        self.add("</defs>")
        self.rect(0, 0, self.width, self.height, "#ffffff", "#ffffff", 0)

    def end(self) -> str:
        self.add("</svg>")
        return "\n".join(self.parts) + "\n"


_FONT_CACHE: Dict[Tuple[int, str], ImageFont.ImageFont] = {}


def png_color(color: str, opacity: float = 1.0) -> Tuple[int, int, int, int]:
    r, g, b = rgb_from_hex(color)
    return r, g, b, max(0, min(255, round(opacity * 255)))


def png_font(size: int, weight: str) -> ImageFont.ImageFont:
    key = (size, weight)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached

    candidates = (
        Path(r"C:\Windows\Fonts\segoeuib.ttf") if weight in {"600", "700", "bold"} else Path(r"C:\Windows\Fonts\segoeui.ttf"),
        Path(r"C:\Windows\Fonts\arialbd.ttf") if weight in {"600", "700", "bold"} else Path(r"C:\Windows\Fonts\arial.ttf"),
    )
    for candidate in candidates:
        if candidate.exists():
            font = ImageFont.truetype(str(candidate), size)
            _FONT_CACHE[key] = font
            return font

    font = ImageFont.load_default()
    _FONT_CACHE[key] = font
    return font


def draw_dashed_png_line(
    draw: ImageDraw.ImageDraw,
    start: Tuple[float, float],
    end: Tuple[float, float],
    color: Tuple[int, int, int, int],
    width: int,
    dash: str,
) -> None:
    pattern = [float(piece) for piece in dash.split() if piece]
    if not pattern:
        draw.line([start, end], fill=color, width=width)
        return
    if len(pattern) % 2 == 1:
        pattern *= 2

    x1, y1 = start
    x2, y2 = end
    total = math.hypot(x2 - x1, y2 - y1)
    if total <= 0:
        return
    ux = (x2 - x1) / total
    uy = (y2 - y1) / total
    distance = 0.0
    pattern_index = 0
    draw_segment = True
    while distance < total:
        length = pattern[pattern_index % len(pattern)]
        next_distance = min(total, distance + length)
        if draw_segment:
            sx = x1 + ux * distance
            sy = y1 + uy * distance
            ex = x1 + ux * next_distance
            ey = y1 + uy * next_distance
            draw.line([(sx, sy), (ex, ey)], fill=color, width=width)
        distance = next_distance
        pattern_index += 1
        draw_segment = not draw_segment


class PngMap:
    def __init__(self, cell_px: int, panel_px: int = 430, height_px: Optional[int] = None) -> None:
        self.cell_px = cell_px
        self.margin_left = 72
        self.margin_top = 112
        self.margin_right = 72
        self.margin_bottom = 260
        self.grid_px = GRID_SIZE * cell_px
        self.panel_px = panel_px
        self.width = self.margin_left + self.grid_px + self.panel_px + self.margin_right
        self.height = height_px if height_px is not None else self.margin_top + self.grid_px + self.margin_bottom
        self.image = Image.new("RGBA", (self.width, self.height), (255, 255, 255, 255))
        self.draw = ImageDraw.Draw(self.image)

    def px_from_grid_float(self, grid_x: float, grid_z: float) -> Tuple[float, float]:
        return self.margin_left + grid_x * self.cell_px, self.margin_top + grid_z * self.cell_px

    def px_from_stage(self, x: float, z: float) -> Tuple[float, float]:
        return self.px_from_grid_float(grid_float_from_stage(x) + 0.5, grid_float_from_stage(z) + 0.5)

    def px_from_grid(self, x: int, z: int) -> Tuple[float, float]:
        return self.px_from_grid_float(x + 0.5, z + 0.5)

    def start(self) -> None:
        pass

    def line(
        self,
        x1: float,
        y1: float,
        x2: float,
        y2: float,
        stroke: str,
        width: float = 1.0,
        opacity: float = 1.0,
        dash: Optional[str] = None,
        marker_end: bool = False,
    ) -> None:
        color = png_color(stroke, opacity)
        line_width = max(1, round(width))
        if dash:
            draw_dashed_png_line(self.draw, (x1, y1), (x2, y2), color, line_width, dash)
        else:
            self.draw.line([(x1, y1), (x2, y2)], fill=color, width=line_width)

        if marker_end:
            dx = x2 - x1
            dy = y2 - y1
            length = math.hypot(dx, dy)
            if length > 0:
                ux = dx / length
                uy = dy / length
                px = -uy
                py = ux
                arrow_len = 7.0 + line_width * 2.0
                arrow_w = 4.5 + line_width
                points = [
                    (x2, y2),
                    (x2 - ux * arrow_len + px * arrow_w, y2 - uy * arrow_len + py * arrow_w),
                    (x2 - ux * arrow_len - px * arrow_w, y2 - uy * arrow_len - py * arrow_w),
                ]
                self.draw.polygon(points, fill=color)

    def polygon(
        self,
        points: Sequence[Tuple[float, float]],
        fill: str,
        opacity: float = 1.0,
    ) -> None:
        self.draw.polygon(points, fill=png_color(fill, opacity))

    def circle(
        self,
        x: float,
        y: float,
        r: float,
        fill: str,
        stroke: str = "#111827",
        width: float = 1.0,
        opacity: float = 1.0,
    ) -> None:
        bbox = (x - r, y - r, x + r, y + r)
        self.draw.ellipse(
            bbox,
            fill=png_color(fill, opacity),
            outline=png_color(stroke, opacity),
            width=max(1, round(width)),
        )

    def text(
        self,
        x: float,
        y: float,
        value: object,
        size: int = 13,
        fill: str = "#111827",
        weight: str = "400",
        anchor: str = "start",
    ) -> None:
        font = png_font(size, weight)
        text = str(value)
        try:
            ascent, _descent = font.getmetrics()  # type: ignore[attr-defined]
        except AttributeError:
            ascent = size
        bbox = self.draw.textbbox((0, 0), text, font=font)
        text_width = bbox[2] - bbox[0]
        if anchor == "middle":
            x -= text_width / 2.0
        elif anchor == "end":
            x -= text_width
        self.draw.text((x, y - ascent), text, fill=png_color(fill), font=font)

    def rect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        fill: str,
        stroke: str = "#9ca3af",
        stroke_width: float = 1.0,
        opacity: float = 1.0,
    ) -> None:
        line_width = round(stroke_width)
        outline = png_color(stroke, opacity) if line_width > 0 else None
        self.draw.rectangle(
            (x, y, x + width, y + height),
            fill=png_color(fill, opacity),
            outline=outline,
            width=max(1, line_width) if line_width > 0 else 1,
        )

    def save(self, output_path: Path) -> None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        self.image.convert("RGB").save(output_path)


def grid_byte_fill(value: int) -> str:
    if value == 0x7C:
        return "#fef9c3"
    if value == 0x7D:
        return "#fce7f3"
    if value == 0x7F:
        return "#fecaca"
    return "#ffffff"


def fallback_grid_value(x: int, z: int, terrain_squares: set[Tuple[int, int]]) -> int:
    if x == 0 or z == 0 or x == GRID_SIZE - 1 or z == GRID_SIZE - 1:
        return 0x7F
    if (x, z) in terrain_squares:
        return 0x7D
    return 0x00


def draw_grid(
    svg: SvgMap,
    grid_snapshot: Optional[GridSnapshot],
    terrain_squares: set[Tuple[int, int]],
) -> None:
    for z in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            left = svg.margin_left + x * svg.cell_px
            top = svg.margin_top + z * svg.cell_px
            if grid_snapshot is None:
                value = fallback_grid_value(x, z, terrain_squares)
            else:
                value = grid_snapshot.terrain_value_at(x, z)
            fill = grid_byte_fill(value)
            svg.rect(left, top, svg.cell_px, svg.cell_px, fill, "#9ca3af", 1.0, 1.0)

    for index in range(GRID_SIZE):
        center_x = svg.margin_left + (index + 0.5) * svg.cell_px
        center_y = svg.margin_top + (index + 0.5) * svg.cell_px
        svg.text(center_x, svg.margin_top - 14, index, 11, "#4b5563", anchor="middle")
        svg.text(svg.margin_left - 16, center_y + 4, index, 11, "#4b5563", anchor="middle")


def event_sequence(event: Dict[str, str]) -> Optional[int]:
    return maybe_int(event.get("capture_sequence")) or maybe_int(event.get("publish_seq")) or maybe_int(event.get("commit_seq"))


def is_timeline_movement_event(event: Dict[str, str]) -> bool:
    return event.get("event_kind") == "movement_publish" or "publish_grid" in event


def is_timeline_action_mode_event(event: Dict[str, str]) -> bool:
    return event.get("event_kind") == "action_mode_change"


def draw_event_nodes(svg: SvgMap, event: Dict[str, str], label: str = "grid publish") -> None:
    path_nodes = parse_path_nodes(event.get("path_nodes", ""))
    selected_index = maybe_int(event.get("path_index"))
    publish_start, publish_end = parse_publish_grid(event.get("publish_grid", ""))
    if publish_start is None and "cur_grid_x" in event:
        publish_start = (
            required_int(event["cur_grid_x"], "cur_grid_x"),
            required_int(event["cur_grid_z"], "cur_grid_z"),
        )
        publish_end = (
            required_int(event["next_grid_x"], "next_grid_x"),
            required_int(event["next_grid_z"], "next_grid_z"),
        )

    if path_nodes:
        last_px: Optional[Tuple[float, float]] = None
        for index, grid_x, grid_z in path_nodes:
            px = svg.px_from_grid(grid_x, grid_z)
            if last_px is not None:
                svg.line(last_px[0], last_px[1], px[0], px[1], "#374151", 1.5, 0.45, "5 5")
            fill = "#fde047" if selected_index == index else "#ffffff"
            stroke = "#111827" if selected_index == index else "#6b7280"
            svg.circle(px[0], px[1], 7.0 if selected_index == index else 5.0, fill, stroke, 1.6, 0.95)
            svg.text(px[0], px[1] - 10, index, 11, "#111827", "600", "middle")
            last_px = px

    if publish_start is not None and publish_end is not None:
        start_px = svg.px_from_grid(*publish_start)
        end_px = svg.px_from_grid(*publish_end)
        svg.line(start_px[0], start_px[1], end_px[0], end_px[1], "#111827", 2.2, 0.95, "7 4", True)
        svg.circle(start_px[0], start_px[1], 6.0, "#ffffff", "#111827", 1.5, 1.0)
        svg.circle(end_px[0], end_px[1], 8.0, "#fde047", "#111827", 2.0, 1.0)
        svg.text(end_px[0] - 10, end_px[1] + 18, label, 11, "#111827", "700", "end")


def normalized_time(value: float, min_value: float, max_value: float) -> float:
    if max_value <= min_value:
        return 0.0
    return (value - min_value) / (max_value - min_value)


def movement_vi_values(
    samples: Sequence[PositionSample],
    raw_segments: Sequence[RawMotionSegment],
    dead_slots: Sequence[int],
) -> List[int]:
    values: List[int] = []
    hidden_slots = set(dead_slots)
    for segment in raw_segments:
        if segment.slot in hidden_slots:
            continue
        if (
            math.hypot(
                segment.after_pos[0] - segment.before_pos[0],
                segment.after_pos[1] - segment.before_pos[1],
            )
            < PATH_MOVEMENT_EPSILON_STAGE
            and not segment.reached
        ):
            continue
        values.extend((segment.start_vi, segment.end_vi))
    if values:
        return values

    for slot in SLOTS:
        if slot in hidden_slots:
            continue
        points: List[Tuple[int, float, float]] = []
        for sample in samples:
            position = sample.positions.get(slot)
            if position is None:
                continue
            points.append((sample.vi_field_count, position[0], position[1]))
        for (vi_field_a, x1, z1), (vi_field_b, x2, z2) in zip(points, points[1:]):
            if math.hypot(x2 - x1, z2 - z1) < PATH_MOVEMENT_EPSILON_STAGE:
                continue
            values.extend((vi_field_a, vi_field_b))

    if values:
        return values
    return [sample.vi_field_count for sample in samples]


def action_vi_time_scale(
    samples: Sequence[PositionSample],
    raw_segments: Sequence[RawMotionSegment],
    attack_timings: Sequence[AttackVectorTiming],
    facing_samples: Sequence[FacingRaySample],
    dead_slots: Sequence[int],
) -> ViTimeScale:
    hidden_slots = set(dead_slots)
    vi_values = movement_vi_values(samples, raw_segments, dead_slots)
    for timing in attack_timings:
        if timing.attacker_slot in hidden_slots or timing.target_slot in hidden_slots:
            continue
        vi_values.extend((timing.start_vi, timing.end_vi))
    for sample in facing_samples:
        if sample.slot in hidden_slots:
            continue
        vi_values.append(sample.vi_field_count)
    if not vi_values:
        return ViTimeScale(0.0, 0.0)
    return ViTimeScale(float(min(vi_values)), float(max(vi_values)))


def event_int(event: Dict[str, str], field: str) -> Optional[int]:
    return maybe_int(event.get(field))


def event_vi_field_count(event: Dict[str, str]) -> Optional[int]:
    return event_int(event, "vi_field_count")


def event_slot(event: Dict[str, str]) -> Optional[int]:
    return event_int(event, "slot")


def event_target_slot(event: Dict[str, str]) -> Optional[int]:
    return event_int(event, "target_slot")


def event_old_mode(event: Dict[str, str]) -> Optional[int]:
    return event_int(event, "old_mode_raw")


def event_new_mode(event: Dict[str, str]) -> Optional[int]:
    return event_int(event, "new_mode_raw")


def is_attack_mode(mode: Optional[int]) -> bool:
    return mode in ATTACK_ACTION_MODES


def is_attack_enter_event(event: Dict[str, str], slot: int) -> bool:
    if not is_timeline_action_mode_event(event) or event_slot(event) != slot:
        return False
    return not is_attack_mode(event_old_mode(event)) and is_attack_mode(event_new_mode(event))


def is_attack_leave_event(event: Dict[str, str], slot: int) -> bool:
    if not is_timeline_action_mode_event(event) or event_slot(event) != slot:
        return False
    return is_attack_mode(event_old_mode(event)) and not is_attack_mode(event_new_mode(event))


def action_mode_events(events: Sequence[Dict[str, str]]) -> List[Dict[str, str]]:
    return sorted(
        [event for event in events if is_timeline_action_mode_event(event)],
        key=lambda event: event_sequence(event) or -1,
    )


def attack_vector_timing_from_events(
    attacker_slot: int,
    target_slot: int,
    start_event: Dict[str, str],
    end_event: Dict[str, str],
    kind: str,
) -> Optional[AttackVectorTiming]:
    start_seq = event_sequence(start_event)
    end_seq = event_sequence(end_event)
    start_vi = event_vi_field_count(start_event)
    end_vi = event_vi_field_count(end_event)
    if start_seq is None or end_seq is None or start_vi is None or end_vi is None:
        return None
    if end_seq < start_seq:
        return None
    return AttackVectorTiming(
        attacker_slot=attacker_slot,
        target_slot=target_slot,
        start_seq=start_seq,
        end_seq=end_seq,
        start_vi=start_vi,
        end_vi=end_vi,
        kind=kind,
    )


def active_attack_timing(progression: TurnProgression) -> Optional[AttackVectorTiming]:
    events = action_mode_events(progression.events)
    start_event = next(
        (event for event in events if is_attack_enter_event(event, progression.active_slot)),
        None,
    )
    if start_event is None:
        return None
    start_seq = event_sequence(start_event)
    if start_seq is None:
        return None
    end_event = next(
        (
            event
            for event in events
            if (event_sequence(event) or -1) > start_seq
            and is_attack_leave_event(event, progression.active_slot)
        ),
        None,
    )
    if end_event is None:
        return None
    target_slot = event_target_slot(start_event)
    if target_slot is None:
        target_slot = progression.attack_target_slot
    if target_slot is None or target_slot == progression.active_slot:
        return None
    return attack_vector_timing_from_events(
        progression.active_slot,
        target_slot,
        start_event,
        end_event,
        "active_attack",
    )


def counter_attack_timings(
    progression: TurnProgression,
    primary_timing: AttackVectorTiming,
) -> List[AttackVectorTiming]:
    events = action_mode_events(progression.events)
    timings: List[AttackVectorTiming] = []
    end_event = next(
        (
            event
            for event in events
            if event_sequence(event) == primary_timing.end_seq
            and is_attack_leave_event(event, progression.active_slot)
        ),
        None,
    )
    if end_event is None:
        return timings

    for event in events:
        seq = event_sequence(event)
        if seq is None:
            continue
        if seq <= primary_timing.start_seq or seq >= primary_timing.end_seq:
            continue
        if not is_attack_enter_event(event, primary_timing.target_slot):
            continue
        target_slot = event_target_slot(event)
        if target_slot is not None and target_slot != progression.active_slot:
            continue
        timing = attack_vector_timing_from_events(
            primary_timing.target_slot,
            progression.active_slot,
            event,
            end_event,
            "counter_attack",
        )
        if timing is not None:
            timings.append(timing)
    return timings


def attack_vector_timings(progression: TurnProgression) -> List[AttackVectorTiming]:
    primary = active_attack_timing(progression)
    if primary is None:
        return []
    counters = counter_attack_timings(progression, primary)
    if counters:
        first_counter = min(counters, key=lambda timing: (timing.start_seq, timing.start_vi))
        primary = AttackVectorTiming(
            attacker_slot=primary.attacker_slot,
            target_slot=primary.target_slot,
            start_seq=primary.start_seq,
            end_seq=first_counter.start_seq,
            start_vi=primary.start_vi,
            end_vi=first_counter.start_vi,
            kind=primary.kind,
        )
    return [primary, *counters]


def death_markers(progression: TurnProgression) -> List[DeathMarker]:
    marker_by_slot: Dict[int, DeathMarker] = {}
    for event in action_mode_events(progression.events):
        seq = event_sequence(event)
        vi = event_vi_field_count(event)
        slot = event_slot(event)
        if seq is None or vi is None or slot is None:
            continue
        if event_new_mode(event) != DEATH_ACTION_MODE:
            continue
        current = marker_by_slot.get(slot)
        if current is None or seq < current.seq:
            marker_by_slot[slot] = DeathMarker(slot=slot, seq=seq, vi=vi)
    return sorted(marker_by_slot.values(), key=lambda marker: marker.seq)


def draw_timed_line(
    svg: SvgMap,
    vector: Tuple[float, float, float, float],
    start_vi: int,
    end_vi: int,
    time_scale: ViTimeScale,
    width: float,
    opacity: float,
    segments: int = 1,
) -> None:
    x1, y1, x2, y2 = vector
    segment_count = max(1, segments)
    for index in range(segment_count):
        start_t = index / segment_count
        end_t = (index + 1) / segment_count
        color_t = 1.0 if segment_count == 1 else index / (segment_count - 1)
        segment_vi = start_vi + (end_vi - start_vi) * color_t
        sx = x1 + (x2 - x1) * start_t
        sy = y1 + (y2 - y1) * start_t
        ex = x1 + (x2 - x1) * end_t
        ey = y1 + (y2 - y1) * end_t
        svg.line(sx, sy, ex, ey, time_scale.color(segment_vi), width, opacity)


def draw_frame_sample_slot_paths(
    svg: SvgMap,
    samples: Sequence[PositionSample],
    time_scale: ViTimeScale,
    dead_slots: Sequence[int],
) -> None:
    points_by_slot: Dict[int, List[Tuple[int, int, float, float]]] = {}
    hidden_slots = set(dead_slots)

    for slot in SLOTS:
        if slot in hidden_slots:
            continue
        points: List[Tuple[int, int, float, float]] = []
        for sample in samples:
            position = sample.positions.get(slot)
            if position is None:
                continue
            x, y = svg.px_from_stage(position[0], position[1])
            points.append((sample.seq, sample.vi_field_count, x, y))
        points_by_slot[slot] = points

    for slot in SLOTS:
        points = points_by_slot.get(slot, [])
        moved = any(
            math.hypot(x2 - x1, y2 - y1) >= PATH_MOVEMENT_EPSILON_PX
            for (_seq_a, _vi_field_a, x1, y1), (_seq_b, _vi_field_b, x2, y2) in zip(
                points, points[1:]
            )
        )
        for (_seq_a, _vi_field_a, x1, y1), (_seq_b, vi_field_b, x2, y2) in zip(
            points, points[1:]
        ):
            distance = math.hypot(x2 - x1, y2 - y1)
            if distance < PATH_MOVEMENT_EPSILON_PX:
                continue
            draw_timed_line(
                svg,
                (x1, y1, x2, y2),
                _vi_field_a,
                vi_field_b,
                time_scale,
                RAW_MOTION_PATH_WIDTH,
                0.9,
            )

        if not points:
            continue
        if not moved:
            _seq, _vi_field, x, y = points[-1]
            svg.circle(x, y, 7.0, SLOT_COLORS[slot], "#ffffff", 1.6, 1.0)
            svg.text(x + 8, y - 8, f"{slot}", 12, SLOT_COLORS[slot], "700")
            continue

        start_seq, _start_vi_field, start_x, start_y = points[0]
        end_seq, _end_vi_field, end_x, end_y = points[-1]
        svg.circle(start_x, start_y, 6.5, "#ffffff", SLOT_COLORS[slot], 2.4, 1.0)
        svg.circle(end_x, end_y, 7.0, SLOT_COLORS[slot], "#ffffff", 1.6, 1.0)
        svg.text(end_x + 8, end_y - 8, f"{slot}", 12, SLOT_COLORS[slot], "700")
        svg.text(start_x + 8, start_y + 16, f"{start_seq}", 10, "#4b5563")
        svg.text(end_x + 8, end_y + 16, f"{end_seq}", 10, "#4b5563")


def motion_increment_key(segment: RawMotionSegment) -> Tuple[int, int]:
    return round(segment.increment[0] * 1000), round(segment.increment[1] * 1000)


def raw_motion_phases(segments: Sequence[RawMotionSegment]) -> List[List[RawMotionSegment]]:
    phases: List[List[RawMotionSegment]] = []
    for segment in sorted(segments, key=lambda item: (item.start_seq, item.end_seq)):
        if not phases:
            phases.append([segment])
            continue

        current = phases[-1]
        previous = current[-1]
        same_increment = motion_increment_key(previous) == motion_increment_key(segment)
        close_sequence = segment.start_seq - previous.end_seq <= 10
        connected_position = (
            math.hypot(
                segment.before_pos[0] - previous.after_pos[0],
                segment.before_pos[1] - previous.after_pos[1],
            )
            < PATH_MOVEMENT_EPSILON_STAGE * 4
        )
        if same_increment and close_sequence and connected_position:
            current.append(segment)
        else:
            phases.append([segment])
    return phases


def draw_slot_paths(
    svg: SvgMap,
    samples: Sequence[PositionSample],
    raw_segments: Sequence[RawMotionSegment],
    time_scale: ViTimeScale,
    dead_slots: Sequence[int],
) -> None:
    visible_segments = [
        segment
        for segment in raw_segments
        if segment.slot not in set(dead_slots)
        and (
            math.hypot(
                segment.after_pos[0] - segment.before_pos[0],
                segment.after_pos[1] - segment.before_pos[1],
            )
            >= PATH_MOVEMENT_EPSILON_STAGE
            or segment.reached
        )
    ]
    if not visible_segments:
        draw_frame_sample_slot_paths(svg, samples, time_scale, dead_slots)
        return

    hidden_slots = set(dead_slots)
    for slot in SLOTS:
        if slot in hidden_slots:
            continue
        slot_segments = sorted(
            [segment for segment in visible_segments if segment.slot == slot],
            key=lambda segment: (segment.end_seq, segment.end_vi),
        )
        if not slot_segments:
            fallback = latest_frame_position_before(samples, slot, samples[-1].seq) if samples else None
            if fallback is not None:
                x, y = svg.px_from_stage(fallback[0], fallback[1])
                svg.circle(x, y, 7.0, SLOT_COLORS[slot], "#ffffff", 1.6, 1.0)
                svg.text(x + 8, y - 8, f"{slot}", 12, SLOT_COLORS[slot], "700")
            continue

        phases = raw_motion_phases(slot_segments)
        for phase in phases:
            first = phase[0]
            last = phase[-1]
            start = svg.px_from_stage(first.before_pos[0], first.before_pos[1])
            end = svg.px_from_stage(last.after_pos[0], last.after_pos[1])
            if math.hypot(end[0] - start[0], end[1] - start[1]) < PATH_MOVEMENT_EPSILON_PX:
                continue
            draw_timed_line(
                svg,
                (start[0], start[1], end[0], end[1]),
                first.start_vi,
                last.end_vi,
                time_scale,
                RAW_MOTION_PATH_WIDTH,
                0.9,
                RAW_MOTION_GRADIENT_SEGMENTS,
            )

        first_segment = slot_segments[0]
        last_segment = slot_segments[-1]
        start_x, start_y = svg.px_from_stage(first_segment.before_pos[0], first_segment.before_pos[1])
        end_x, end_y = svg.px_from_stage(last_segment.after_pos[0], last_segment.after_pos[1])
        svg.circle(start_x, start_y, 6.5, "#ffffff", SLOT_COLORS[slot], 2.4, 1.0)
        svg.circle(end_x, end_y, 7.0, SLOT_COLORS[slot], "#ffffff", 1.6, 1.0)
        svg.text(end_x + 8, end_y - 8, f"{slot}", 12, SLOT_COLORS[slot], "700")
        svg.text(start_x + 8, start_y + 16, f"{first_segment.start_seq}", 10, "#4b5563")
        svg.text(end_x + 8, end_y + 16, f"{last_segment.end_seq}", 10, "#4b5563")


def facing_vector(
    x: float,
    y: float,
    facing_degrees: float,
    length: float = FACING_RAY_LENGTH_PX,
) -> Tuple[float, float, float, float]:
    radians = math.radians(facing_degrees)
    dx = math.sin(radians)
    dy = math.cos(radians)
    return x, y, x + dx * length, y + dy * length


def draw_facing_rays(
    svg: SvgMap,
    facing_samples: Sequence[FacingRaySample],
    time_scale: ViTimeScale,
    dead_slots: Sequence[int],
) -> None:
    hidden_slots = set(dead_slots)
    for sample in facing_samples:
        if sample.slot in hidden_slots:
            continue
        x, y = svg.px_from_stage(sample.position[0], sample.position[1])
        vector = facing_vector(x, y, sample.facing_degrees)
        svg.line(
            vector[0],
            vector[1],
            vector[2],
            vector[3],
            time_scale.color(sample.vi_field_count),
            FACING_RAY_WIDTH,
            FACING_RAY_OPACITY,
        )


def marker_position_sample(
    samples: Sequence[PositionSample],
    slot: int,
    vi: int,
) -> Optional[PositionSample]:
    before = [
        sample
        for sample in samples
        if sample.vi_field_count <= vi and slot in sample.positions
    ]
    if before:
        return max(before, key=lambda sample: (sample.vi_field_count, sample.seq))
    after = [sample for sample in samples if slot in sample.positions]
    if after:
        return min(after, key=lambda sample: (sample.vi_field_count, sample.seq))
    return None


def draw_death_skull(svg: SvgMap, dot_x: float, dot_y: float) -> None:
    x = dot_x - DEATH_SKULL_OFFSET_PX
    y = dot_y
    r = DEATH_SKULL_RADIUS_PX
    svg.circle(x, y - 2.4, r, "#111827", "#111827", 0.0, 1.0)
    svg.rect(x - r * 0.58, y + 1.2, r * 1.16, r * 0.82, "#111827", "#111827", 0.0, 1.0)
    svg.circle(x - r * 0.35, y - 3.6, r * 0.22, "#ffffff", "#ffffff", 0.0, 1.0)
    svg.circle(x + r * 0.35, y - 3.6, r * 0.22, "#ffffff", "#ffffff", 0.0, 1.0)
    svg.polygon(
        [
            (x, y - 1.5),
            (x - r * 0.18, y + 0.7),
            (x + r * 0.18, y + 0.7),
        ],
        "#ffffff",
        1.0,
    )
    for offset in (-0.3, 0.0, 0.3):
        tx = x + r * offset
        svg.line(tx, y + 2.5, tx, y + 5.8, "#ffffff", 0.9, 1.0)


def draw_death_markers(
    svg: SvgMap,
    progression: TurnProgression,
    samples: Sequence[PositionSample],
) -> None:
    for marker in death_markers(progression):
        sample = marker_position_sample(samples, marker.slot, marker.vi)
        if sample is None:
            continue
        position = sample.positions.get(marker.slot)
        if position is None:
            continue
        x, y = svg.px_from_stage(position[0], position[1])
        draw_death_skull(svg, x, y)


def trimmed_vector(
    start: Tuple[float, float],
    end: Tuple[float, float],
    trim_px: float,
) -> Optional[Tuple[float, float, float, float]]:
    x1, y1 = start
    x2, y2 = end
    dx = x2 - x1
    dy = y2 - y1
    distance = math.hypot(dx, dy)
    if distance < 2.0:
        return None
    trim = min(trim_px, max(0.0, (distance - 2.0) * 0.35))
    ux = dx / distance
    uy = dy / distance
    return x1 + ux * trim, y1 + uy * trim, x2 - ux * trim, y2 - uy * trim


def offset_vector(
    vector: Tuple[float, float, float, float],
    offset_px: float,
) -> Tuple[float, float, float, float]:
    if offset_px == 0:
        return vector
    x1, y1, x2, y2 = vector
    dx = x2 - x1
    dy = y2 - y1
    length = math.hypot(dx, dy)
    if length <= 0:
        return vector
    px = -dy / length
    py = dx / length
    ox = px * offset_px
    oy = py * offset_px
    return x1 + ox, y1 + oy, x2 + ox, y2 + oy


def attack_position_sample(
    samples: Sequence[PositionSample],
    timing: AttackVectorTiming,
) -> Optional[PositionSample]:
    slots = (timing.attacker_slot, timing.target_slot)
    before = [
        sample
        for sample in samples
        if sample.vi_field_count <= timing.start_vi
        and all(slot in sample.positions for slot in slots)
    ]
    if before:
        return max(before, key=lambda sample: (sample.vi_field_count, sample.seq))
    after = [sample for sample in samples if all(slot in sample.positions for slot in slots)]
    if after:
        return min(after, key=lambda sample: (sample.vi_field_count, sample.seq))
    return None


def arrowhead_points(
    x1: float,
    y1: float,
    x2: float,
    y2: float,
) -> Optional[List[Tuple[float, float]]]:
    dx = x2 - x1
    dy = y2 - y1
    length = math.hypot(dx, dy)
    if length <= 0:
        return None
    ux = dx / length
    uy = dy / length
    px = -uy
    py = ux
    return [
        (x2, y2),
        (
            x2 - ux * ATTACK_ARROWHEAD_LENGTH_PX + px * ATTACK_ARROWHEAD_WIDTH_PX,
            y2 - uy * ATTACK_ARROWHEAD_LENGTH_PX + py * ATTACK_ARROWHEAD_WIDTH_PX,
        ),
        (
            x2 - ux * ATTACK_ARROWHEAD_LENGTH_PX - px * ATTACK_ARROWHEAD_WIDTH_PX,
            y2 - uy * ATTACK_ARROWHEAD_LENGTH_PX - py * ATTACK_ARROWHEAD_WIDTH_PX,
        ),
    ]


def draw_timed_vector(
    svg: SvgMap,
    vector: Tuple[float, float, float, float],
    start_vi: int,
    end_vi: int,
    time_scale: ViTimeScale,
    width: float = ATTACK_VECTOR_WIDTH,
    opacity: float = 0.95,
) -> None:
    x1, y1, x2, y2 = vector
    draw_timed_line(
        svg,
        vector,
        start_vi,
        end_vi,
        time_scale,
        width,
        opacity,
        max(2, ATTACK_VECTOR_SEGMENTS),
    )

    final_color = time_scale.color(end_vi)
    points = arrowhead_points(x1, y1, x2, y2)
    if points is not None:
        svg.polygon(points, final_color, opacity)


def draw_attack_vector(
    svg: SvgMap,
    progression: TurnProgression,
    samples: Sequence[PositionSample],
    time_scale: ViTimeScale,
    timings: Sequence[AttackVectorTiming],
) -> None:
    if not samples or not timings:
        return

    hidden_slots = set(progression.dead_slots)
    has_counter_attack = any(timing.kind == "counter_attack" for timing in timings)
    for timing in timings:
        if timing.attacker_slot in hidden_slots or timing.target_slot in hidden_slots:
            continue
        sample = attack_position_sample(samples, timing)
        if sample is None:
            continue
        attacker = sample.positions.get(timing.attacker_slot)
        target = sample.positions.get(timing.target_slot)
        if attacker is None or target is None:
            continue
        start = svg.px_from_stage(attacker[0], attacker[1])
        end = svg.px_from_stage(target[0], target[1])
        vector = trimmed_vector(start, end, ATTACK_VECTOR_TRIM_PX)
        if vector is None:
            continue
        if has_counter_attack:
            vector = offset_vector(vector, ATTACK_COUNTER_VECTOR_OFFSET_PX)
        draw_timed_vector(svg, vector, timing.start_vi, timing.end_vi, time_scale)


def draw_legend(svg: SvgMap, x: float, y: float) -> None:
    svg.text(x, y, "Legend", 15, "#111827", "700")
    content_y = y + 23
    squares_x = x
    slots_x = x + 138
    delta_x = x + 272

    svg.text(squares_x, content_y, "Grid", 12, "#374151", "600")
    row_y = content_y + 22
    svg.rect(squares_x, row_y - 12, 16, 16, grid_byte_fill(0x7C), "#9ca3af", 1.0)
    svg.text(squares_x + 24, row_y, "special terrain square", 12, "#374151")
    row_y += 21
    svg.rect(squares_x, row_y - 12, 16, 16, grid_byte_fill(0x7D), "#9ca3af", 1.0)
    svg.text(squares_x + 24, row_y, "terrain square", 12, "#374151")
    row_y += 21
    svg.rect(squares_x, row_y - 12, 16, 16, grid_byte_fill(0x7F), "#9ca3af", 1.0)
    svg.text(squares_x + 24, row_y, "border square", 12, "#374151")
    row_y += 23
    draw_timed_line(
        svg,
        (squares_x, row_y - 5, squares_x + 40, row_y - 5),
        0,
        10,
        ViTimeScale(0.0, 10.0),
        RAW_MOTION_PATH_WIDTH,
        0.9,
        6,
    )
    svg.text(squares_x + 49, row_y, "raw motion path", 12, "#374151")
    row_y += 21
    svg.line(squares_x, row_y - 5, squares_x + 40, row_y - 5, "#111827", 2.0, 1.0, "7 4", True)
    svg.text(squares_x + 49, row_y, "publish vector", 12, "#374151")
    row_y += 21
    draw_timed_vector(
        svg,
        (squares_x, row_y - 5, squares_x + 40, row_y - 5),
        0,
        10,
        ViTimeScale(0.0, 10.0),
        ATTACK_VECTOR_WIDTH,
        1.0,
    )
    svg.text(squares_x + 49, row_y, "attack vector", 12, "#374151")
    row_y += 21
    ray = facing_vector(squares_x + 4, row_y - 5, 270.0, 28.0)
    svg.line(ray[0], ray[1], ray[2], ray[3], time_color(0.65), FACING_RAY_WIDTH, FACING_RAY_OPACITY)
    svg.text(squares_x + 49, row_y, "facing ray", 12, "#374151")
    row_y += 21
    svg.text(squares_x + 13, row_y, "0", 11, "#111827", "400", "end")
    svg.text(squares_x + 24, row_y, "path node", 12, "#374151")
    row_y += 19
    svg.text(squares_x + 18, row_y, "72", 11, "#111827", "700", "end")
    svg.text(squares_x + 24, row_y, "commit seq", 12, "#374151")

    svg.text(slots_x, content_y, "Slot colors", 12, "#374151", "600")
    row_y = content_y + 22
    for slot in SLOTS:
        svg.circle(slots_x + 8, row_y - 4, 6.0, SLOT_COLORS[slot], "#ffffff", 1.2)
        svg.text(slots_x + 24, row_y, SLOT_NAMES[slot], 12, "#374151")
        row_y += 19

    svg.text(delta_x, content_y, "VI field time", 12, "#374151", "600")
    ramp_y = content_y + 20
    ramp_width = 116
    ramp_steps = 29
    step_width = ramp_width / ramp_steps
    for index in range(ramp_steps):
        color = time_color(index / (ramp_steps - 1))
        svg.rect(delta_x + index * step_width, ramp_y, step_width + 0.8, 14, color, color, 0.0)
    svg.text(delta_x, ramp_y + 31, "early", 11, "#4b5563")
    svg.text(delta_x + ramp_width, ramp_y + 31, "late", 11, "#4b5563", anchor="end")


def compact_path(path: Path) -> str:
    text = str(path)
    if len(text) <= 76:
        return text
    return "..." + text[-73:]


def draw_capture_footer(svg: SvgMap, capture_path: Path) -> None:
    footer_x = svg.margin_left
    footer_y = svg.margin_top + svg.grid_px + 34
    svg.text(footer_x, footer_y, "Capture", 15, "#111827", "700")
    svg.text(footer_x, footer_y + 21, compact_path(capture_path), 10, "#4b5563")


def slot_short_name(slot: int) -> str:
    value = SLOT_NAMES.get(slot, f"slot {slot}")
    prefix = f"slot {slot} "
    if value.startswith(prefix):
        return value[len(prefix) :]
    return value


def optional_text(value: Optional[int]) -> str:
    return str(value) if value is not None else "?"


def attack_range_line(progression: TurnProgression) -> str:
    if progression.attack_range_kind == "melee":
        return f"attack_range: melee (final instrParam_0x6={optional_text(progression.attack_instr_param_0x6)})"
    if progression.attack_range_kind == "ranged":
        return f"attack_range: ranged (final instrParam_0x6={optional_text(progression.attack_instr_param_0x6)})"
    if progression.attack_range_kind == "non_attack":
        return f"attack_range: non_attack (instruction={optional_text(progression.attack_instruction)})"
    if progression.attack_source_seq is not None:
        return f"attack_range: unknown (final instrParam_0x6={optional_text(progression.attack_instr_param_0x6)})"
    return "attack_range: unknown"


def attack_range_source_line(progression: TurnProgression) -> str:
    if progression.attack_source_seq is None:
        return "attack_source: no worker-select route fact"
    worker = f"; worker_pc={progression.attack_worker_pc}" if progression.attack_worker_pc else ""
    return (
        f"attack_source: {progression.attack_source_checkpoint or 'attack_route_used'} "
        f"seq {progression.attack_source_seq}; target={optional_text(progression.attack_target_slot)}{worker}"
    )


def attack_vector_timing_line(progression: TurnProgression) -> str:
    timings = attack_vector_timings(progression)
    if not timings:
        return "attack_vector: none"
    parts = [
        (
            f"{timing.kind} slot{timing.attacker_slot}->slot{timing.target_slot} "
            f"seq {timing.start_seq}->{timing.end_seq} vi {timing.start_vi}->{timing.end_vi}"
        )
        for timing in timings
    ]
    return "attack_vector: " + "; ".join(parts)


def removed_slots_line(progression: TurnProgression) -> str:
    if not progression.dead_slots:
        return "removed_slots: none"
    slots = ", ".join(f"slot{slot}" for slot in progression.dead_slots)
    return f"removed_slots: {slots}"


def render_motion_streams(
    progression: TurnProgression,
    samples: Sequence[PositionSample],
) -> Tuple[List[RawMotionSegment], List[FacingRaySample]]:
    raw_segments = load_raw_motion_segments(
        progression.capture_path,
        progression.start_seq,
        progression.end_seq,
    )
    facing_samples = load_facing_ray_samples(
        progression.capture_path,
        progression.start_seq,
        progression.end_seq,
        samples,
        raw_segments,
    )
    return raw_segments, facing_samples


def samples_in_window(samples: Sequence[PositionSample], window: ActionWindow) -> List[PositionSample]:
    return [
        sample
        for sample in samples
        if window.first_frame_seq <= sample.seq <= window.last_frame_seq
    ]


def event_rows_in_window(events: Sequence[Dict[str, str]], window: ActionWindow) -> List[Dict[str, str]]:
    result = []
    for event in events:
        seq = event_sequence(event)
        if seq is None:
            continue
        if not (window.start_seq <= seq <= window.end_seq):
            continue
        result.append(event)
    return sorted(result, key=lambda row: event_sequence(row) or -1)


def death_sequence_by_slot(events: Sequence[Dict[str, str]]) -> Dict[int, int]:
    death_by_slot: Dict[int, int] = {}
    for event in action_mode_events(events):
        seq = event_sequence(event)
        slot = event_slot(event)
        if seq is None or slot is None:
            continue
        if event_new_mode(event) != DEATH_ACTION_MODE:
            continue
        current = death_by_slot.get(slot)
        if current is None or seq < current:
            death_by_slot[slot] = seq
    return death_by_slot


def dead_slots_before_window(death_by_slot: Dict[int, int], window: ActionWindow) -> Tuple[int, ...]:
    return tuple(
        slot
        for slot in SLOTS
        if (death_seq := death_by_slot.get(slot)) is not None and death_seq < window.start_seq
    )


def timeline_movement_count(events: Sequence[Dict[str, str]]) -> int:
    return sum(1 for event in events if is_timeline_movement_event(event))


def timeline_action_mode_count(events: Sequence[Dict[str, str]]) -> int:
    return sum(1 for event in events if is_timeline_action_mode_event(event))


def attack_range_kind(fact: Optional[AttackRangeFact]) -> str:
    if fact is None:
        return "unknown"
    if fact.instruction is not None and fact.instruction != 3:
        return "non_attack"
    if fact.instr_param_0x6 == 0:
        return "melee"
    if fact.instr_param_0x6 is not None:
        return "ranged"
    return "unknown"


def attack_range_fact_in_window(
    facts: Sequence[AttackRangeFact],
    window: ActionWindow,
) -> Optional[AttackRangeFact]:
    matching = [
        fact
        for fact in facts
        if window.start_seq <= fact.seq <= window.end_seq and fact.actor_slot == window.active_slot
    ]
    if not matching:
        return None
    attack_facts = [fact for fact in matching if fact.instruction == 3]
    if attack_facts:
        return attack_facts[0]
    return matching[0]


def build_turn_progressions(
    run_root: Path,
    source_filter: Optional[str],
    action_sequence_filter: Optional[int],
) -> List[Tuple[TurnProgression, List[PositionSample]]]:
    summary = read_summary(run_root)
    result: List[Tuple[TurnProgression, List[PositionSample]]] = []

    for source_exec in sorted(summary, key=lambda value: int(value)):
        if source_filter and source_exec != source_filter:
            continue

        clone_exec, capture_path = summary_capture_path(summary, source_exec, run_root)
        samples = load_all_position_samples(capture_path)
        bookmarks = load_action_bookmarks(capture_path)
        windows = build_action_windows(samples, bookmarks)
        events = load_timeline_events(capture_path)
        attack_facts = load_attack_range_facts(capture_path)
        grid_snapshot = load_grid_snapshot(capture_path)
        death_by_slot = death_sequence_by_slot(events)

        for window in windows:
            if action_sequence_filter is not None and window.action_sequence != action_sequence_filter:
                continue
            window_samples = samples_in_window(samples, window)
            if not window_samples:
                continue
            active_label = f"slot{window.active_slot}_{slot_short_name(window.active_slot).lower().replace(' ', '_')}"
            key = (
                f"source{source_exec}_clone{clone_exec}_"
                f"turn{window.turn_index + 1:02d}_actionseq{window.action_sequence}_{active_label}"
            )
            active_text = f"{window.active_slot} {slot_short_name(window.active_slot)}"
            title = f"Source {source_exec} action #{window.turn_index + 1}: active slot {active_text}"
            attack_fact = attack_range_fact_in_window(attack_facts, window)
            progression = TurnProgression(
                key=key,
                title=title,
                source_exec=source_exec,
                clone_exec=clone_exec,
                turn_index=window.turn_index,
                action_sequence=window.action_sequence,
                active_slot=window.active_slot,
                start_seq=window.start_seq,
                end_seq=window.end_seq,
                first_frame_seq=window.first_frame_seq,
                last_frame_seq=window.last_frame_seq,
                selected_seq=window.selected_seq,
                scheduled_seq=window.scheduled_seq,
                attack_range_kind=attack_range_kind(attack_fact),
                attack_source_seq=attack_fact.seq if attack_fact is not None else None,
                attack_source_checkpoint=attack_fact.source_checkpoint if attack_fact is not None else None,
                attack_worker_pc=attack_fact.worker_pc if attack_fact is not None else None,
                attack_instruction=attack_fact.instruction if attack_fact is not None else None,
                attack_target_slot=attack_fact.target_slot if attack_fact is not None else None,
                attack_instr_param_0x6=attack_fact.instr_param_0x6 if attack_fact is not None else None,
                grid_snapshot=grid_snapshot,
                dead_slots=dead_slots_before_window(death_by_slot, window),
                events=event_rows_in_window(events, window),
                capture_path=capture_path,
            )
            result.append((progression, window_samples))

    return result


def turn_progression_event_lines(events: Sequence[Dict[str, str]]) -> List[str]:
    if not events:
        return ["timeline: none in window"]
    movement_count = sum(1 for event in events if is_timeline_movement_event(event))
    mode_count = sum(1 for event in events if is_timeline_action_mode_event(event))
    lines = [f"timeline: {len(events)} ({movement_count} commits, {mode_count} modes)"]
    for event in events[:16]:
        seq = event_sequence(event)
        if is_timeline_movement_event(event):
            slot = event.get("slot", "?")
            publish_grid = event.get("publish_grid")
            if not publish_grid and "cur_grid_x" in event:
                publish_grid = (
                    f"({event.get('cur_grid_x')},{event.get('cur_grid_z')})"
                    f"->({event.get('next_grid_x')},{event.get('next_grid_z')})"
                )
            cheb = event.get("cheb", "?")
            path_index = event.get("path_index", "?")
            lines.append(f"commit {seq}: slot {slot} {publish_grid}; cheb={cheb}; path_index={path_index}")
        elif is_timeline_action_mode_event(event):
            slot = event.get("slot", "?")
            old_mode = event.get("old_mode", "?")
            new_mode = event.get("new_mode", "?")
            writer = event.get("writer", "action_mode_writer")
            pc = event.get("pc", "?")
            lines.append(f"mode {seq}: slot {slot} {old_mode}->{new_mode}; {writer}; pc={pc}")
        else:
            lines.append(f"event {seq}: {event.get('event_kind', 'unknown')}")
    if len(events) > 16:
        lines.append(f"... {len(events) - 16} more events")
    return lines


def render_turn_progression(
    progression: TurnProgression,
    samples: Sequence[PositionSample],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    svg = SvgMap(cell_px=cell_px)
    svg.start()
    attack_timings = attack_vector_timings(progression)
    raw_segments, facing_samples = render_motion_streams(progression, samples)
    time_scale = action_vi_time_scale(
        samples,
        raw_segments,
        attack_timings,
        facing_samples,
        progression.dead_slots,
    )
    draw_grid(svg, progression.grid_snapshot, terrain_squares)
    draw_facing_rays(svg, facing_samples, time_scale, progression.dead_slots)
    for event in progression.events:
        if not is_timeline_movement_event(event):
            continue
        seq = event_sequence(event)
        label = str(seq) if seq is not None else "?"
        draw_event_nodes(svg, event, label)
    draw_slot_paths(svg, samples, raw_segments, time_scale, progression.dead_slots)
    draw_attack_vector(svg, progression, samples, time_scale, attack_timings)
    draw_death_markers(svg, progression, samples)
    draw_capture_footer(svg, progression.capture_path)

    title_x = svg.margin_left
    svg.text(title_x, 32, progression.title, 20, "#111827", "700")
    subtitle = (
        f"source {progression.source_exec} -> clone {progression.clone_exec}; "
        f"seq window {progression.start_seq}-{progression.end_seq}; {len(samples)} frame samples"
    )
    svg.text(title_x, 54, subtitle, 12, "#374151")

    panel_x = svg.margin_left + svg.grid_px + 34
    panel_y = svg.margin_top + 8
    svg.text(panel_x, panel_y, "Action Window", 15, "#111827", "700")
    panel_y += 22
    svg.text(
        panel_x,
        panel_y,
        f"turn_order: #{progression.turn_index + 1}",
        11,
        "#374151",
    )
    panel_y += 17
    svg.text(panel_x, panel_y, f"action_sequence: {progression.action_sequence}", 11, "#374151")
    panel_y += 17
    svg.text(
        panel_x,
        panel_y,
        f"active_slot: {progression.active_slot} {slot_short_name(progression.active_slot)}",
        11,
        "#374151",
    )
    panel_y += 17
    svg.text(panel_x, panel_y, attack_range_line(progression), 11, "#374151")
    panel_y += 17
    svg.text(panel_x, panel_y, attack_range_source_line(progression), 11, "#374151")
    panel_y += 17
    svg.text(panel_x, panel_y, attack_vector_timing_line(progression), 11, "#374151")
    panel_y += 17
    svg.text(panel_x, panel_y, f"seq_window: {progression.start_seq}-{progression.end_seq}", 11, "#374151")
    panel_y += 17
    svg.text(
        panel_x,
        panel_y,
        f"frame_seq: {progression.first_frame_seq}-{progression.last_frame_seq}",
        11,
        "#374151",
    )
    panel_y += 17
    svg.text(
        panel_x,
        panel_y,
        f"bookmarks: selected={progression.selected_seq or '?'} scheduled={progression.scheduled_seq or '?'}",
        11,
        "#374151",
    )
    panel_y += 17
    svg.text(panel_x, panel_y, f"frame_samples: {len(samples)}", 11, "#374151")
    panel_y += 25
    svg.text(panel_x, panel_y, f"raw_motion: {len(raw_segments)} facing_rays: {len(facing_samples)}", 11, "#374151")
    panel_y += 17
    if progression.dead_slots:
        svg.text(panel_x, panel_y, removed_slots_line(progression), 11, "#374151")
        panel_y += 17

    svg.text(panel_x, panel_y, "Timeline", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_progression_event_lines(progression.events):
        if len(detail) > 68:
            detail = detail[:65] + "..."
        svg.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17

    panel_y += 16
    draw_legend(svg, panel_x, panel_y)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.end(), encoding="utf-8")


def render_turn_progression_png(
    progression: TurnProgression,
    samples: Sequence[PositionSample],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    png = PngMap(cell_px=cell_px)
    png.start()
    attack_timings = attack_vector_timings(progression)
    raw_segments, facing_samples = render_motion_streams(progression, samples)
    time_scale = action_vi_time_scale(
        samples,
        raw_segments,
        attack_timings,
        facing_samples,
        progression.dead_slots,
    )
    draw_grid(png, progression.grid_snapshot, terrain_squares)  # type: ignore[arg-type]
    draw_facing_rays(png, facing_samples, time_scale, progression.dead_slots)  # type: ignore[arg-type]
    for event in progression.events:
        if not is_timeline_movement_event(event):
            continue
        seq = event_sequence(event)
        label = str(seq) if seq is not None else "?"
        draw_event_nodes(png, event, label)  # type: ignore[arg-type]
    draw_slot_paths(png, samples, raw_segments, time_scale, progression.dead_slots)  # type: ignore[arg-type]
    draw_attack_vector(png, progression, samples, time_scale, attack_timings)  # type: ignore[arg-type]
    draw_death_markers(png, progression, samples)  # type: ignore[arg-type]
    draw_capture_footer(png, progression.capture_path)  # type: ignore[arg-type]

    title_x = png.margin_left
    png.text(title_x, 32, progression.title, 20, "#111827", "700")
    subtitle = (
        f"source {progression.source_exec} -> clone {progression.clone_exec}; "
        f"seq window {progression.start_seq}-{progression.end_seq}; {len(samples)} frame samples"
    )
    png.text(title_x, 54, subtitle, 12, "#374151")

    panel_x = png.margin_left + png.grid_px + 34
    panel_y = png.margin_top + 8
    png.text(panel_x, panel_y, "Action Window", 15, "#111827", "700")
    panel_y += 22
    png.text(
        panel_x,
        panel_y,
        f"turn_order: #{progression.turn_index + 1}",
        11,
        "#374151",
    )
    panel_y += 17
    png.text(panel_x, panel_y, f"action_sequence: {progression.action_sequence}", 11, "#374151")
    panel_y += 17
    png.text(
        panel_x,
        panel_y,
        f"active_slot: {progression.active_slot} {slot_short_name(progression.active_slot)}",
        11,
        "#374151",
    )
    panel_y += 17
    png.text(panel_x, panel_y, attack_range_line(progression), 11, "#374151")
    panel_y += 17
    png.text(panel_x, panel_y, attack_range_source_line(progression), 11, "#374151")
    panel_y += 17
    png.text(panel_x, panel_y, attack_vector_timing_line(progression), 11, "#374151")
    panel_y += 17
    png.text(panel_x, panel_y, f"seq_window: {progression.start_seq}-{progression.end_seq}", 11, "#374151")
    panel_y += 17
    png.text(
        panel_x,
        panel_y,
        f"frame_seq: {progression.first_frame_seq}-{progression.last_frame_seq}",
        11,
        "#374151",
    )
    panel_y += 17
    png.text(
        panel_x,
        panel_y,
        f"bookmarks: selected={progression.selected_seq or '?'} scheduled={progression.scheduled_seq or '?'}",
        11,
        "#374151",
    )
    panel_y += 17
    png.text(panel_x, panel_y, f"frame_samples: {len(samples)}", 11, "#374151")
    panel_y += 25
    png.text(panel_x, panel_y, f"raw_motion: {len(raw_segments)} facing_rays: {len(facing_samples)}", 11, "#374151")
    panel_y += 17
    if progression.dead_slots:
        png.text(panel_x, panel_y, removed_slots_line(progression), 11, "#374151")
        panel_y += 17

    png.text(panel_x, panel_y, "Timeline", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_progression_event_lines(progression.events):
        if len(detail) > 68:
            detail = detail[:65] + "..."
        png.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17

    panel_y += 16
    draw_legend(png, panel_x, panel_y)  # type: ignore[arg-type]

    png.save(output_path)


def combined_row_height(cell_px: int) -> int:
    return COMBINED_ROW_TOP_PAD + GRID_SIZE * cell_px + COMBINED_ROW_BOTTOM_PAD


def collect_turn_progression_records(
    run_root: Path,
    source_filter: Optional[str],
    action_sequence_filter: Optional[int],
) -> List[TurnRecord]:
    return list(build_turn_progressions(run_root, source_filter, action_sequence_filter))


def group_turn_records(records: Sequence[TurnRecord]) -> List[List[TurnRecord]]:
    grouped: Dict[Tuple[str, str, str], List[TurnRecord]] = {}
    for progression, samples in records:
        key = (progression.source_exec, progression.clone_exec, str(progression.capture_path))
        grouped.setdefault(key, []).append((progression, samples))

    groups = list(grouped.values())
    groups.sort(key=lambda group: (int(group[0][0].source_exec), int(group[0][0].clone_exec)))
    for group in groups:
        group.sort(key=lambda item: item[0].turn_index)
    return groups


def action_summary_parts(group: Sequence[TurnRecord]) -> Tuple[int, int, int]:
    action_count = len(group)
    first_seq = min(progression.start_seq for progression, _samples in group)
    last_seq = max(progression.end_seq for progression, _samples in group)
    return action_count, first_seq, last_seq


def turn_overview_key(group: Sequence[TurnRecord]) -> str:
    first = group[0][0]
    return f"source{first.source_exec}_clone{first.clone_exec}_turn_overview"


def draw_action_window_details(canvas: SvgMap, progression: TurnProgression, samples: Sequence[PositionSample]) -> None:
    panel_x = canvas.margin_left + canvas.grid_px + 34
    panel_y = canvas.margin_top + 8
    canvas.text(panel_x, panel_y, "Action Window", 15, "#111827", "700")
    panel_y += 22
    details = [
        f"turn_order: #{progression.turn_index + 1}",
        f"action_sequence: {progression.action_sequence}",
        f"active_slot: {progression.active_slot} {slot_short_name(progression.active_slot)}",
        attack_range_line(progression),
        attack_range_source_line(progression),
        attack_vector_timing_line(progression),
        f"seq_window: {progression.start_seq}-{progression.end_seq}",
        f"frame_seq: {progression.first_frame_seq}-{progression.last_frame_seq}",
        f"bookmarks: selected={progression.selected_seq or '?'} scheduled={progression.scheduled_seq or '?'}",
        f"frame_samples: {len(samples)}",
    ]
    if progression.dead_slots:
        details.insert(6, removed_slots_line(progression))
    for detail in details:
        if len(detail) > 72:
            detail = detail[:69] + "..."
        canvas.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17

    panel_y += 8
    canvas.text(panel_x, panel_y, "Timeline", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_progression_event_lines(progression.events):
        if len(detail) > 72:
            detail = detail[:69] + "..."
        canvas.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17


def draw_turn_overview_row(
    canvas: SvgMap,
    progression: TurnProgression,
    samples: Sequence[PositionSample],
    terrain_squares: set[Tuple[int, int]],
    row_top: int,
    row_height: int,
) -> None:
    old_margin_top = canvas.margin_top
    canvas.margin_top = row_top + COMBINED_ROW_TOP_PAD
    title_y = row_top + 24
    slot_label = f"{progression.active_slot} {slot_short_name(progression.active_slot)}"
    canvas.text(
        canvas.margin_left,
        title_y,
        f"#{progression.turn_index + 1}  {slot_label}  action_sequence {progression.action_sequence}",
        18,
        "#111827",
        "700",
    )
    canvas.text(canvas.margin_left + 360, title_y, attack_range_line(progression), 12, "#374151", "600")

    attack_timings = attack_vector_timings(progression)
    raw_segments, facing_samples = render_motion_streams(progression, samples)
    time_scale = action_vi_time_scale(
        samples,
        raw_segments,
        attack_timings,
        facing_samples,
        progression.dead_slots,
    )
    draw_grid(canvas, progression.grid_snapshot, terrain_squares)
    draw_facing_rays(canvas, facing_samples, time_scale, progression.dead_slots)
    for event in progression.events:
        if not is_timeline_movement_event(event):
            continue
        seq = event_sequence(event)
        label = str(seq) if seq is not None else "?"
        draw_event_nodes(canvas, event, label)
    draw_slot_paths(canvas, samples, raw_segments, time_scale, progression.dead_slots)
    draw_attack_vector(canvas, progression, samples, time_scale, attack_timings)
    draw_death_markers(canvas, progression, samples)
    draw_action_window_details(canvas, progression, samples)

    separator_y = row_top + row_height - 10
    canvas.line(canvas.margin_left, separator_y, canvas.width - canvas.margin_right, separator_y, "#e5e7eb", 1.0)
    canvas.margin_top = old_margin_top


def draw_turn_overview_footer(canvas: SvgMap, group: Sequence[TurnRecord], footer_y: int) -> None:
    first = group[0][0]
    draw_legend(canvas, canvas.margin_left, footer_y)
    capture_x = canvas.margin_left + canvas.grid_px + 34
    canvas.text(capture_x, footer_y, "Capture", 15, "#111827", "700")
    canvas.text(capture_x, footer_y + 21, compact_path(first.capture_path), 10, "#4b5563")


def draw_turn_overview_canvas(
    canvas: SvgMap,
    group: Sequence[TurnRecord],
    terrain_squares: set[Tuple[int, int]],
) -> None:
    canvas.start()
    first = group[0][0]
    action_count, first_seq, last_seq = action_summary_parts(group)
    title_x = canvas.margin_left
    canvas.text(title_x, 32, f"Source {first.source_exec} Turn Overview", 22, "#111827", "700")
    subtitle = (
        f"clone {first.clone_exec}; {action_count} complete actions; "
        f"seq window {first_seq}-{last_seq}"
    )
    canvas.text(title_x, 56, subtitle, 12, "#374151")

    row_height = combined_row_height(canvas.cell_px)
    for index, (progression, samples) in enumerate(group):
        row_top = COMBINED_HEADER_PX + index * row_height
        draw_turn_overview_row(canvas, progression, samples, terrain_squares, row_top, row_height)

    footer_y = COMBINED_HEADER_PX + len(group) * row_height + 16
    draw_turn_overview_footer(canvas, group, footer_y)


def render_turn_overview(
    group: Sequence[TurnRecord],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    height = COMBINED_HEADER_PX + len(group) * combined_row_height(cell_px) + COMBINED_FOOTER_PX
    svg = SvgMap(cell_px=cell_px, height_px=height)
    draw_turn_overview_canvas(svg, group, terrain_squares)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.end(), encoding="utf-8")


def render_turn_overview_png(
    group: Sequence[TurnRecord],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    height = COMBINED_HEADER_PX + len(group) * combined_row_height(cell_px) + COMBINED_FOOTER_PX
    png = PngMap(cell_px=cell_px, height_px=height)
    draw_turn_overview_canvas(png, group, terrain_squares)  # type: ignore[arg-type]
    png.save(output_path)


def write_turn_overview_manifest(rows: Sequence[Dict[str, object]], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "source_exec",
        "clone_exec",
        "action_count",
        "first_seq",
        "last_seq",
        "actions",
        "svg_path",
        "png_path",
    ]
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def render_turn_overviews(
    records: Sequence[TurnRecord],
    output_dir: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
    export_png_files: bool,
) -> List[Dict[str, object]]:
    rendered: List[Dict[str, object]] = []
    overview_dir = output_dir / "turn_overview"
    for group in group_turn_records(records):
        first = group[0][0]
        output_path = overview_dir / f"{turn_overview_key(group)}.svg"
        render_turn_overview(group, output_path, terrain_squares, cell_px)
        png_path = output_path.with_suffix(".png") if export_png_files else None
        if png_path is not None:
            render_turn_overview_png(group, png_path, terrain_squares, cell_px)
        action_count, first_seq, last_seq = action_summary_parts(group)
        action_summary = "; ".join(
            (
                f"#{progression.turn_index + 1}:slot{progression.active_slot}:"
                f"{progression.attack_range_kind}:seq{progression.start_seq}-{progression.end_seq}"
            )
            for progression, _samples in group
        )
        rendered.append(
            {
                "source_exec": first.source_exec,
                "clone_exec": first.clone_exec,
                "action_count": action_count,
                "first_seq": first_seq,
                "last_seq": last_seq,
                "actions": action_summary,
                "svg_path": str(output_path),
                "png_path": str(png_path) if png_path is not None else "",
            }
        )
        suffix = f", png {png_path}" if png_path is not None else ""
        print(f"{output_path} ({action_count} actions, seq {first_seq}-{last_seq}{suffix})")

    manifest_path = overview_dir / "turn_overview_manifest.tsv"
    write_turn_overview_manifest(rendered, manifest_path)
    print(f"{manifest_path} ({len(rendered)} rendered turn overviews)")
    return rendered


def write_turn_progression_manifest(rows: Sequence[Dict[str, object]], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "source_exec",
        "clone_exec",
        "turn_order",
        "action_sequence",
        "active_slot",
        "active_slot_name",
        "attack_range_kind",
        "attack_instr_param_0x6",
        "attack_source_seq",
        "attack_source_checkpoint",
        "attack_worker_pc",
        "attack_instruction",
        "attack_target_slot",
        "start_seq",
        "end_seq",
        "first_frame_seq",
        "last_frame_seq",
        "selected_seq",
        "scheduled_seq",
        "frame_samples",
        "timeline_event_count",
        "movement_commit_count",
        "action_mode_change_count",
        "svg_path",
        "png_path",
    ]
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def render_turn_progressions(
    records: Sequence[TurnRecord],
    output_dir: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
    export_png_files: bool,
) -> List[Dict[str, object]]:
    rendered: List[Dict[str, object]] = []
    turn_dir = output_dir / "turn_progression"
    for progression, samples in records:
        output_path = turn_dir / f"{progression.key}.svg"
        render_turn_progression(progression, samples, output_path, terrain_squares, cell_px)
        png_path = output_path.with_suffix(".png") if export_png_files else None
        if png_path is not None:
            render_turn_progression_png(progression, samples, png_path, terrain_squares, cell_px)
        active_name = slot_short_name(progression.active_slot)
        movement_count = timeline_movement_count(progression.events)
        action_mode_count = timeline_action_mode_count(progression.events)
        rendered.append(
            {
                "source_exec": progression.source_exec,
                "clone_exec": progression.clone_exec,
                "turn_order": progression.turn_index + 1,
                "action_sequence": progression.action_sequence,
                "active_slot": progression.active_slot,
                "active_slot_name": active_name,
                "attack_range_kind": progression.attack_range_kind,
                "attack_instr_param_0x6": (
                    progression.attack_instr_param_0x6
                    if progression.attack_instr_param_0x6 is not None
                    else ""
                ),
                "attack_source_seq": (
                    progression.attack_source_seq if progression.attack_source_seq is not None else ""
                ),
                "attack_source_checkpoint": progression.attack_source_checkpoint or "",
                "attack_worker_pc": progression.attack_worker_pc or "",
                "attack_instruction": (
                    progression.attack_instruction if progression.attack_instruction is not None else ""
                ),
                "attack_target_slot": (
                    progression.attack_target_slot if progression.attack_target_slot is not None else ""
                ),
                "start_seq": progression.start_seq,
                "end_seq": progression.end_seq,
                "first_frame_seq": progression.first_frame_seq,
                "last_frame_seq": progression.last_frame_seq,
                "selected_seq": progression.selected_seq if progression.selected_seq is not None else "",
                "scheduled_seq": progression.scheduled_seq if progression.scheduled_seq is not None else "",
                "frame_samples": len(samples),
                "timeline_event_count": len(progression.events),
                "movement_commit_count": movement_count,
                "action_mode_change_count": action_mode_count,
                "svg_path": str(output_path),
                "png_path": str(png_path) if png_path is not None else "",
            }
        )
        suffix = f", png {png_path}" if png_path is not None else ""
        print(
            f"{output_path} ({len(samples)} frame samples, {len(progression.events)} timeline events, "
            f"{movement_count} movement commits, {action_mode_count} mode changes{suffix})"
        )

    manifest_path = turn_dir / "turn_progression_manifest.tsv"
    write_turn_progression_manifest(rendered, manifest_path)
    print(f"{manifest_path} ({len(rendered)} rendered turns)")
    return rendered


def main() -> int:
    args = parse_args()
    run_root = args.run_root
    if not run_root.exists():
        raise FileNotFoundError(f"Run root does not exist: {run_root}")

    terrain_squares = terrain_squares_from_args(args.terrain_square)
    records = collect_turn_progression_records(
        run_root,
        args.source_exec,
        args.sequence,
    )
    if not records:
        raise RuntimeError("No action windows rendered.")

    rendered = render_turn_overviews(
        records,
        args.out_dir,
        terrain_squares,
        args.cell_px,
        args.export_png,
    )
    if args.turn_progressions:
        render_turn_progressions(
            records,
            args.out_dir,
            terrain_squares,
            args.cell_px,
            args.export_png,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
