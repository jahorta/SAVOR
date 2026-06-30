#!/usr/bin/env python3
"""Render first-turn combatant position captures as SVG path maps.

The default input is the render-focused movement capture under C:\\savor.  This
renderer intentionally consumes that capture schema directly: action windows
come from the captured selected-slot/action-sequence frame globals and movement
publish metadata comes from the captured FUN_8008178c rows.

Examples:
  python SavorPredict/tools/render_position_paths.py --examples
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


DEFAULT_RUN_ROOT = Path(r"C:\savor\render_position_paths_corpus_20260630_release")
DEFAULT_OUT_DIR = Path("Analyses") / "20260630_position_path_images"

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

GRID_SIZE = 11
GRID_STAGE_STEP = 15.0
GRID_STAGE_ORIGIN = -75.0


@dataclass(frozen=True)
class PositionSample:
    seq: int
    frame: int
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
    events: List[Dict[str, str]]
    capture_path: Path


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
        help="Render all complete action windows from the render-position capture root.",
    )
    parser.add_argument(
        "--turn-progressions",
        action="store_true",
        help=(
            "Render one SVG per complete action window in the capture. "
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
            "SST terrain/blocked square to fill pink. Repeatable. "
            "If omitted, no interior squares are marked as terrain."
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
    rows = read_csv_rows(run_root / "summary.csv")
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

    first_action_start = accepted_actions[0][4]
    initial_frames = [sample for sample in keyed_samples if sample.seq < first_action_start]
    if initial_frames:
        first_initial = initial_frames[0]
        append_window(
            required_int(first_initial.action_sequence, "action_sequence"),
            required_int(first_initial.active_actor_slot, "active_actor_slot"),
            first_initial.seq,
            first_action_start - 1,
            None,
            None,
        )

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


def load_movement_publish_events(capture_path: Path) -> List[Dict[str, str]]:
    events: List[Dict[str, str]] = []
    for row in iter_capture_rows(capture_path):
        if not is_movement_publish_row(row):
            continue
        normalized = normalize_movement_publish_row(row)
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


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


class SvgMap:
    def __init__(self, cell_px: int, panel_px: int = 430) -> None:
        self.cell_px = cell_px
        self.margin_left = 72
        self.margin_top = 112
        self.margin_right = 72
        self.margin_bottom = 132
        self.grid_px = GRID_SIZE * cell_px
        self.panel_px = panel_px
        self.width = self.margin_left + self.grid_px + self.panel_px + self.margin_right
        self.height = self.margin_top + self.grid_px + self.margin_bottom
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
        marker = ' marker-end="url(#arrow)"' if marker_end else ""
        self.add(
            f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" '
            f'stroke="{stroke}" stroke-width="{width:.2f}" opacity="{opacity:.3f}" '
            f'stroke-linecap="round"{dash_attr}{marker}/>'
        )

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
    def __init__(self, cell_px: int, panel_px: int = 430) -> None:
        self.cell_px = cell_px
        self.margin_left = 72
        self.margin_top = 112
        self.margin_right = 72
        self.margin_bottom = 132
        self.grid_px = GRID_SIZE * cell_px
        self.panel_px = panel_px
        self.width = self.margin_left + self.grid_px + self.panel_px + self.margin_right
        self.height = self.margin_top + self.grid_px + self.margin_bottom
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


def draw_grid(svg: SvgMap, terrain_squares: set[Tuple[int, int]]) -> None:
    for z in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            left = svg.margin_left + x * svg.cell_px
            top = svg.margin_top + z * svg.cell_px
            border = x == 0 or z == 0 or x == GRID_SIZE - 1 or z == GRID_SIZE - 1
            if border:
                fill = "#fecaca"
            elif (x, z) in terrain_squares:
                fill = "#fce7f3"
            else:
                fill = "#ffffff"
            svg.rect(left, top, svg.cell_px, svg.cell_px, fill, "#9ca3af", 1.0, 1.0)

    for index in range(GRID_SIZE):
        center_x = svg.margin_left + (index + 0.5) * svg.cell_px
        center_y = svg.margin_top + (index + 0.5) * svg.cell_px
        svg.text(center_x, svg.margin_top - 14, index, 11, "#4b5563", anchor="middle")
        svg.text(svg.margin_left - 16, center_y + 4, index, 11, "#4b5563", anchor="middle")


def event_sequence(event: Dict[str, str]) -> Optional[int]:
    return maybe_int(event.get("capture_sequence")) or maybe_int(event.get("publish_seq")) or maybe_int(event.get("commit_seq"))


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
        mid_x = (start_px[0] + end_px[0]) * 0.5
        mid_y = (start_px[1] + end_px[1]) * 0.5
        svg.text(mid_x + 8, mid_y - 8, label, 10, "#111827", "600")


def normalized_time(seq: int, min_seq: int, max_seq: int) -> float:
    if max_seq <= min_seq:
        return 0.0
    return (seq - min_seq) / (max_seq - min_seq)


def draw_slot_paths(svg: SvgMap, samples: Sequence[PositionSample]) -> None:
    if not samples:
        return
    min_seq = min(sample.seq for sample in samples)
    max_seq = max(sample.seq for sample in samples)

    for slot in SLOTS:
        points: List[Tuple[int, float, float]] = []
        for sample in samples:
            position = sample.positions.get(slot)
            if position is None:
                continue
            x, y = svg.px_from_stage(position[0], position[1])
            points.append((sample.seq, x, y))

        for (seq_a, x1, y1), (seq_b, x2, y2) in zip(points, points[1:]):
            distance = math.hypot(x2 - x1, y2 - y1)
            if distance < 0.05:
                continue
            color = lerp_color("#2563eb", "#f97316", normalized_time(seq_b, min_seq, max_seq))
            svg.line(x1, y1, x2, y2, color, 4.2, 0.9)

        if not points:
            continue
        for index, (seq, x, y) in enumerate(points):
            if index == 0 or index == len(points) - 1 or index % 5 == 0:
                color = lerp_color("#2563eb", "#f97316", normalized_time(seq, min_seq, max_seq))
                svg.circle(x, y, 3.2, color, "#ffffff", 0.8, 0.85)

        start_seq, start_x, start_y = points[0]
        end_seq, end_x, end_y = points[-1]
        svg.circle(start_x, start_y, 6.5, "#ffffff", SLOT_COLORS[slot], 2.4, 1.0)
        svg.circle(end_x, end_y, 7.0, SLOT_COLORS[slot], "#ffffff", 1.6, 1.0)
        svg.text(end_x + 8, end_y - 8, f"{slot}", 12, SLOT_COLORS[slot], "700")
        svg.text(start_x + 8, start_y + 16, f"{start_seq}", 10, "#4b5563")
        svg.text(end_x + 8, end_y + 16, f"{end_seq}", 10, "#4b5563")


def draw_legend(svg: SvgMap, x: float, y: float) -> None:
    svg.text(x, y, "Legend", 15, "#111827", "700")
    y += 20
    svg.rect(x, y - 12, 16, 16, "#fecaca", "#9ca3af", 1.0)
    svg.text(x + 24, y, "border square", 12, "#374151")
    y += 22
    svg.rect(x, y - 12, 16, 16, "#fce7f3", "#9ca3af", 1.0)
    svg.text(x + 24, y, "terrain square", 12, "#374151")
    y += 26
    svg.line(x, y - 5, x + 50, y - 5, "#111827", 2.0, 1.0, "7 4", True)
    svg.text(x + 62, y, "grid publish vector", 12, "#374151")
    y += 26
    for slot in SLOTS:
        svg.circle(x + 8, y - 4, 6.0, SLOT_COLORS[slot], "#ffffff", 1.2)
        svg.text(x + 24, y, SLOT_NAMES[slot], 12, "#374151")
        y += 21

    y += 6
    svg.text(x, y, "delta-time color", 12, "#374151", "600")
    y += 10
    for index in range(20):
        color = lerp_color("#2563eb", "#f97316", index / 19.0)
        svg.rect(x + index * 8, y, 8, 14, color, color, 0.0)
    svg.text(x, y + 30, "early", 11, "#4b5563")
    svg.text(x + 160, y + 30, "late", 11, "#4b5563", anchor="end")


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
        events = load_movement_publish_events(capture_path)

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
                events=event_rows_in_window(events, window),
                capture_path=capture_path,
            )
            result.append((progression, window_samples))

    return result


def turn_progression_event_lines(events: Sequence[Dict[str, str]]) -> List[str]:
    if not events:
        return ["movement_commits: none in window"]
    lines = [f"movement_commits: {len(events)}"]
    for event in events[:14]:
        seq = event_sequence(event)
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
    if len(events) > 14:
        lines.append(f"... {len(events) - 14} more commits")
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
    draw_grid(svg, terrain_squares)
    for event in progression.events:
        seq = event_sequence(event)
        label = f"commit {seq}" if seq is not None else "commit"
        draw_event_nodes(svg, event, label)
    draw_slot_paths(svg, samples)
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

    svg.text(panel_x, panel_y, "Movement Commits", 15, "#111827", "700")
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
    draw_grid(png, terrain_squares)  # type: ignore[arg-type]
    for event in progression.events:
        seq = event_sequence(event)
        label = f"commit {seq}" if seq is not None else "commit"
        draw_event_nodes(png, event, label)  # type: ignore[arg-type]
    draw_slot_paths(png, samples)  # type: ignore[arg-type]
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

    png.text(panel_x, panel_y, "Movement Commits", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_progression_event_lines(progression.events):
        if len(detail) > 68:
            detail = detail[:65] + "..."
        png.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17

    panel_y += 16
    draw_legend(png, panel_x, panel_y)  # type: ignore[arg-type]

    png.save(output_path)


def write_turn_progression_manifest(rows: Sequence[Dict[str, object]], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "source_exec",
        "clone_exec",
        "turn_order",
        "action_sequence",
        "active_slot",
        "active_slot_name",
        "start_seq",
        "end_seq",
        "first_frame_seq",
        "last_frame_seq",
        "selected_seq",
        "scheduled_seq",
        "frame_samples",
        "movement_commit_count",
        "svg_path",
        "png_path",
    ]
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def render_turn_progressions(
    run_root: Path,
    output_dir: Path,
    source_filter: Optional[str],
    action_sequence_filter: Optional[int],
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
    export_png_files: bool,
) -> List[Dict[str, object]]:
    rendered: List[Dict[str, object]] = []
    turn_dir = output_dir / "turn_progression"
    for progression, samples in build_turn_progressions(run_root, source_filter, action_sequence_filter):
        output_path = turn_dir / f"{progression.key}.svg"
        render_turn_progression(progression, samples, output_path, terrain_squares, cell_px)
        png_path = output_path.with_suffix(".png") if export_png_files else None
        if png_path is not None:
            render_turn_progression_png(progression, samples, png_path, terrain_squares, cell_px)
        active_name = slot_short_name(progression.active_slot)
        rendered.append(
            {
                "source_exec": progression.source_exec,
                "clone_exec": progression.clone_exec,
                "turn_order": progression.turn_index + 1,
                "action_sequence": progression.action_sequence,
                "active_slot": progression.active_slot,
                "active_slot_name": active_name,
                "start_seq": progression.start_seq,
                "end_seq": progression.end_seq,
                "first_frame_seq": progression.first_frame_seq,
                "last_frame_seq": progression.last_frame_seq,
                "selected_seq": progression.selected_seq if progression.selected_seq is not None else "",
                "scheduled_seq": progression.scheduled_seq if progression.scheduled_seq is not None else "",
                "frame_samples": len(samples),
                "movement_commit_count": len(progression.events),
                "svg_path": str(output_path),
                "png_path": str(png_path) if png_path is not None else "",
            }
        )
        suffix = f", png {png_path}" if png_path is not None else ""
        print(f"{output_path} ({len(samples)} frame samples, {len(progression.events)} movement commits{suffix})")

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
    rendered = render_turn_progressions(
        run_root,
        args.out_dir,
        args.source_exec,
        args.sequence,
        terrain_squares,
        args.cell_px,
        args.export_png,
    )
    if not rendered:
        raise RuntimeError("No action windows rendered.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
