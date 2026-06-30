#!/usr/bin/env python3
"""Render first-turn combatant position captures as SVG path maps.

The default input is the current path-list/bend capture under C:\\savor.  The
script uses the reduced TSV files to choose representative action windows and
streams the raw JSONL capture only for the per-frame battle-controller position
rows.

Examples:
  python SavorPredict/tools/render_position_paths.py --examples
  python SavorPredict/tools/render_position_paths.py --source-exec 1050837 --sequence 260
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw, ImageFont


DEFAULT_RUN_ROOT = Path(r"C:\savor\path_list_bend_capture_20260630_release")
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
class RenderExample:
    key: str
    title: str
    source_exec: str
    clone_exec: str
    event_seq: int
    start_seq: int
    end_seq: int
    event: Dict[str, str]
    event_kind: str
    capture_path: Path


@dataclass(frozen=True)
class TurnProgression:
    key: str
    title: str
    source_exec: str
    clone_exec: str
    turn_index: int
    active_slot: Optional[int]
    start_seq: int
    end_seq: int
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
        help="Render the built-in representative examples from the reduced TSV evidence.",
    )
    parser.add_argument(
        "--turn-progressions",
        action="store_true",
        help=(
            "Render one SVG per source job and action_sequence in the capture. "
            "If --source-exec is omitted, every row in summary.csv is rendered."
        ),
    )
    parser.add_argument(
        "--source-exec",
        help="Render one source execution job. Requires --sequence unless --examples is used.",
    )
    parser.add_argument(
        "--sequence",
        type=int,
        help="Publish/commit capture_sequence to render for --source-exec.",
    )
    parser.add_argument(
        "--label",
        help="Optional label for a single rendered example.",
    )
    parser.add_argument(
        "--window-start",
        type=int,
        help="Override start capture_sequence for a single rendered example.",
    )
    parser.add_argument(
        "--window-end",
        type=int,
        help="Override end capture_sequence for a single rendered example.",
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


def parse_bool(value: object) -> bool:
    return str(value).strip().lower() == "true"


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


def find_row(rows: Iterable[Dict[str, str]], source_exec: str, seq_key: str, seq: int) -> Optional[Dict[str, str]]:
    seq_text = str(seq)
    for row in rows:
        if row.get("source_exec") == source_exec and row.get(seq_key) == seq_text:
            return row
    return None


def choose_window_for_followup(row: Dict[str, str]) -> Tuple[int, int]:
    publish_seq = required_int(row["publish_seq"], "publish_seq")
    setup_seq = maybe_int(row.get("follow_setup_seq"))
    first_move_seq = maybe_int(row.get("first_move_seq"))
    last_move_seq = maybe_int(row.get("last_move_seq"))
    next_commit_seq = maybe_int(row.get("next_commit_seq"))

    starts = [publish_seq]
    if setup_seq is not None:
        starts.append(setup_seq)
    if first_move_seq is not None:
        starts.append(first_move_seq)

    ends = [publish_seq]
    if last_move_seq is not None:
        ends.append(last_move_seq)
    if next_commit_seq is not None and next_commit_seq - publish_seq <= 90:
        ends.append(next_commit_seq)
    elif last_move_seq is None and next_commit_seq is not None:
        ends.append(min(next_commit_seq, publish_seq + 90))
    if setup_seq is not None:
        ends.append(setup_seq + 40)

    return max(0, min(starts) - 10), max(ends) + 10


def choose_window_for_commit(row: Dict[str, str]) -> Tuple[int, int]:
    seq = required_int(row["capture_sequence"], "capture_sequence")
    return max(0, seq - 35), seq + 35


def make_followup_example(
    summary: Dict[str, Dict[str, str]],
    row: Dict[str, str],
    run_root: Path,
    key: str,
    title: str,
) -> RenderExample:
    source_exec = row["source_exec"]
    clone_exec, capture_path = summary_capture_path(summary, source_exec, run_root)
    start_seq, end_seq = choose_window_for_followup(row)
    return RenderExample(
        key=key,
        title=title,
        source_exec=source_exec,
        clone_exec=clone_exec,
        event_seq=required_int(row["publish_seq"], "publish_seq"),
        start_seq=start_seq,
        end_seq=end_seq,
        event=row,
        event_kind="multi-square publish follow-up",
        capture_path=capture_path,
    )


def make_commit_example(
    summary: Dict[str, Dict[str, str]],
    row: Dict[str, str],
    run_root: Path,
    key: str,
    title: str,
) -> RenderExample:
    source_exec = row["source_exec"]
    clone_exec, capture_path = summary_capture_path(summary, source_exec, run_root)
    start_seq, end_seq = choose_window_for_commit(row)
    return RenderExample(
        key=key,
        title=title,
        source_exec=source_exec,
        clone_exec=clone_exec,
        event_seq=required_int(row["capture_sequence"], "capture_sequence"),
        start_seq=start_seq,
        end_seq=end_seq,
        event=row,
        event_kind="movement commit",
        capture_path=capture_path,
    )


def choose_default_examples(run_root: Path) -> List[RenderExample]:
    summary = read_summary(run_root)
    analysis_dir = run_root / "analysis"
    followup_rows = read_csv_rows(analysis_dir / "multi_square_publish_followup_segments.tsv", delimiter="\t")
    commit_rows = read_csv_rows(analysis_dir / "movement_commit_pathlist.tsv", delimiter="\t")

    examples: List[RenderExample] = []
    selections = [
        ("single_square_axis_aika_1050837_seq098", "Single-square axial movement, Aika", "1050837", "capture_sequence", 98),
        ("single_square_diagonal_slot5_1050837_seq135", "Single-square diagonal movement, slot 5", "1050837", "capture_sequence", 135),
    ]
    for key, title, source_exec, seq_key, seq in selections:
        row = find_row(commit_rows, source_exec, seq_key, seq)
        if row is not None:
            examples.append(make_commit_example(summary, row, run_root, key, title))

    multi_selections = [
        (
            "multi_square_non_axis_aika_1050837_seq260",
            "Multi-square non-axis path-index target, Aika",
            "1050837",
            260,
        ),
        (
            "multi_square_non_axis_aika_1221189_seq260",
            "Multi-square non-axis repeat, Aika",
            "1221189",
            260,
        ),
        (
            "multi_square_axis_aika_1755292_seq260",
            "Multi-square axial path-index target, Aika",
            "1755292",
            260,
        ),
    ]
    for key, title, source_exec, seq in multi_selections:
        row = find_row(followup_rows, source_exec, "publish_seq", seq)
        if row is not None:
            examples.append(make_followup_example(summary, row, run_root, key, title))

    return examples


def make_single_example(args: argparse.Namespace, run_root: Path) -> RenderExample:
    if not args.source_exec or args.sequence is None:
        raise ValueError("A single render requires --source-exec and --sequence, or use --examples.")

    summary = read_summary(run_root)
    analysis_dir = run_root / "analysis"
    followup_rows = read_csv_rows(analysis_dir / "multi_square_publish_followup_segments.tsv", delimiter="\t")
    commit_rows = read_csv_rows(analysis_dir / "movement_commit_pathlist.tsv", delimiter="\t")

    source_exec = args.source_exec
    seq = args.sequence
    row = find_row(followup_rows, source_exec, "publish_seq", seq)
    if row is not None:
        example = make_followup_example(
            summary,
            row,
            run_root,
            f"source{source_exec}_publish{seq}",
            args.label or f"source {source_exec} publish {seq}",
        )
    else:
        row = find_row(commit_rows, source_exec, "capture_sequence", seq)
        if row is None:
            raise ValueError(f"No follow-up publish or movement commit row for source {source_exec}, seq {seq}")
        example = make_commit_example(
            summary,
            row,
            run_root,
            f"source{source_exec}_commit{seq}",
            args.label or f"source {source_exec} commit {seq}",
        )

    if args.window_start is None and args.window_end is None:
        return example
    return RenderExample(
        key=example.key,
        title=example.title,
        source_exec=example.source_exec,
        clone_exec=example.clone_exec,
        event_seq=example.event_seq,
        start_seq=args.window_start if args.window_start is not None else example.start_seq,
        end_seq=args.window_end if args.window_end is not None else example.end_seq,
        event=example.event,
        event_kind=example.event_kind,
        capture_path=example.capture_path,
    )


def is_frame_position_row(row: Dict[str, object]) -> bool:
    checkpoint_name = str(row.get("checkpoint_name", ""))
    checkpoint = str(row.get("checkpoint", ""))
    if checkpoint_name == "battle_case5_after_threads":
        return True
    return checkpoint == "case5_after_runBattleThreads"


def slot_position_from_row(row: Dict[str, object], slot: int) -> Optional[Tuple[float, float]]:
    thread_ptr = maybe_int(row.get(f"slot{slot}_combatant_thread_ptr"))
    if thread_ptr is not None and thread_ptr != 0:
        x = decode_float_bits(row.get(f"slot{slot}_cw_cur_x_0x1c"))
        z = decode_float_bits(row.get(f"slot{slot}_cw_cur_z_0x24"))
        if x is not None and z is not None and math.isfinite(x) and math.isfinite(z):
            return x, z

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
                        active_actor_slot=maybe_int(row.get("active_actor_slot_80347334")),
                        action_sequence=maybe_int(row.get("action_sequence_80347335")),
                    )
                )
    return samples


def load_all_position_samples(capture_path: Path) -> List[PositionSample]:
    return load_position_samples(capture_path, 0, 2_147_483_647)


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


def turn_order_details(samples: Sequence[PositionSample], event_seq: int) -> List[str]:
    candidates = [
        sample
        for sample in samples
        if sample.action_sequence is not None or sample.active_actor_slot is not None
    ]
    if not candidates:
        return ["turn_order: missing from frame rows"]

    before = [sample for sample in candidates if sample.seq <= event_seq]
    if before:
        sample = max(before, key=lambda item: item.seq)
    else:
        sample = min(candidates, key=lambda item: abs(item.seq - event_seq))

    details: List[str] = []
    if sample.action_sequence is not None:
        details.append(f"turn_order: #{sample.action_sequence + 1} (action_sequence={sample.action_sequence})")
    else:
        details.append("turn_order: missing action_sequence")

    if sample.active_actor_slot is not None:
        details.append(
            f"active_slot: {sample.active_actor_slot} {slot_short_name(sample.active_actor_slot)}"
        )
    else:
        details.append("active_slot: missing")

    details.append(f"turn_order_source_seq: {sample.seq}, frame {sample.frame}")
    return details


def active_slot_for_samples(samples: Sequence[PositionSample]) -> Optional[int]:
    counts: Counter[int] = Counter(
        sample.active_actor_slot for sample in samples if sample.active_actor_slot is not None
    )
    if not counts:
        return None
    return counts.most_common(1)[0][0]


def samples_by_action_sequence(samples: Sequence[PositionSample]) -> Dict[int, List[PositionSample]]:
    grouped: Dict[int, List[PositionSample]] = {}
    for sample in samples:
        if sample.action_sequence is None:
            continue
        grouped.setdefault(sample.action_sequence, []).append(sample)
    return grouped


def commit_rows_by_source(run_root: Path) -> Dict[str, List[Dict[str, str]]]:
    path = run_root / "analysis" / "movement_commit_pathlist.tsv"
    if not path.exists():
        return {}
    rows = read_csv_rows(path, delimiter="\t")
    grouped: Dict[str, List[Dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault(row.get("source_exec", ""), []).append(row)
    return grouped


def event_rows_in_window(events: Sequence[Dict[str, str]], start_seq: int, end_seq: int) -> List[Dict[str, str]]:
    result = []
    for event in events:
        seq = event_sequence(event)
        if seq is None:
            continue
        if start_seq <= seq <= end_seq:
            result.append(event)
    return sorted(result, key=lambda row: event_sequence(row) or -1)


def build_turn_progressions(run_root: Path, source_filter: Optional[str]) -> List[Tuple[TurnProgression, List[PositionSample]]]:
    summary = read_summary(run_root)
    source_events = commit_rows_by_source(run_root)
    result: List[Tuple[TurnProgression, List[PositionSample]]] = []

    for source_exec in sorted(summary, key=lambda value: int(value)):
        if source_filter and source_exec != source_filter:
            continue

        clone_exec, capture_path = summary_capture_path(summary, source_exec, run_root)
        samples = load_all_position_samples(capture_path)
        grouped = samples_by_action_sequence(samples)
        events = source_events.get(source_exec, [])

        for action_sequence in sorted(grouped):
            turn_samples = grouped[action_sequence]
            start_seq = min(sample.seq for sample in turn_samples)
            end_seq = max(sample.seq for sample in turn_samples)
            active_slot = active_slot_for_samples(turn_samples)
            active_label = (
                f"slot{active_slot}_{slot_short_name(active_slot).lower().replace(' ', '_')}"
                if active_slot is not None
                else "slot_unknown"
            )
            key = (
                f"source{source_exec}_clone{clone_exec}_"
                f"turn{action_sequence + 1:02d}_actionseq{action_sequence}_{active_label}"
            )
            active_text = (
                f"{active_slot} {slot_short_name(active_slot)}" if active_slot is not None else "unknown"
            )
            title = f"Source {source_exec} turn #{action_sequence + 1}: active slot {active_text}"
            progression = TurnProgression(
                key=key,
                title=title,
                source_exec=source_exec,
                clone_exec=clone_exec,
                turn_index=action_sequence,
                active_slot=active_slot,
                start_seq=start_seq,
                end_seq=end_seq,
                events=event_rows_in_window(events, start_seq, end_seq),
                capture_path=capture_path,
            )
            result.append((progression, turn_samples))

    return result


def event_details(event: Dict[str, str], kind: str) -> List[str]:
    details = [f"event kind: {kind}"]
    for key in (
        "slot",
        "publish_grid",
        "capture_sequence",
        "cheb",
        "axis",
        "diagonal",
        "path_index",
        "dist_to_target",
        "status",
        "selected_node",
        "follow_setup_seq",
        "follow_target",
        "follow_inc",
        "next_commit_seq",
        "direction_change_count",
        "bend_detected",
    ):
        value = event.get(key)
        if value:
            details.append(f"{key}: {value}")
    first_move_seq = event.get("first_move_seq")
    last_move_seq = event.get("last_move_seq")
    if first_move_seq or last_move_seq:
        details.append(f"move_seq: {first_move_seq or '?'} -> {last_move_seq or '?'}")
    path_nodes = event.get("path_nodes")
    if path_nodes:
        visible_nodes = parse_path_nodes(path_nodes)
        if visible_nodes:
            rendered = " ".join(f"{index}:{x},{z}" for index, x, z in visible_nodes)
            details.append(f"path_nodes_rendered: {rendered}")
    return details


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


def render_example(
    example: RenderExample,
    samples: Sequence[PositionSample],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    svg = SvgMap(cell_px=cell_px)
    svg.start()
    draw_grid(svg, terrain_squares)
    draw_event_nodes(svg, example.event)
    draw_slot_paths(svg, samples)
    draw_capture_footer(svg, example.capture_path)

    title_x = svg.margin_left
    svg.text(title_x, 32, example.title, 20, "#111827", "700")
    subtitle = (
        f"source {example.source_exec} -> clone {example.clone_exec}; "
        f"seq window {example.start_seq}-{example.end_seq}; {len(samples)} frame samples"
    )
    svg.text(title_x, 54, subtitle, 12, "#374151")

    panel_x = svg.margin_left + svg.grid_px + 34
    panel_y = svg.margin_top + 8
    svg.text(panel_x, panel_y, "Event", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_order_details(samples, example.event_seq):
        svg.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17
    panel_y += 5
    for detail in event_details(example.event, example.event_kind):
        if len(detail) > 68:
            detail = detail[:65] + "..."
        svg.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17
    panel_y += 16
    draw_legend(svg, panel_x, panel_y)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.end(), encoding="utf-8")


def render_example_png(
    example: RenderExample,
    samples: Sequence[PositionSample],
    output_path: Path,
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
) -> None:
    png = PngMap(cell_px=cell_px)
    png.start()
    draw_grid(png, terrain_squares)  # type: ignore[arg-type]
    draw_event_nodes(png, example.event)  # type: ignore[arg-type]
    draw_slot_paths(png, samples)  # type: ignore[arg-type]
    draw_capture_footer(png, example.capture_path)  # type: ignore[arg-type]

    title_x = png.margin_left
    png.text(title_x, 32, example.title, 20, "#111827", "700")
    subtitle = (
        f"source {example.source_exec} -> clone {example.clone_exec}; "
        f"seq window {example.start_seq}-{example.end_seq}; {len(samples)} frame samples"
    )
    png.text(title_x, 54, subtitle, 12, "#374151")

    panel_x = png.margin_left + png.grid_px + 34
    panel_y = png.margin_top + 8
    png.text(panel_x, panel_y, "Event", 15, "#111827", "700")
    panel_y += 22
    for detail in turn_order_details(samples, example.event_seq):
        png.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17
    panel_y += 5
    for detail in event_details(example.event, example.event_kind):
        if len(detail) > 68:
            detail = detail[:65] + "..."
        png.text(panel_x, panel_y, detail, 11, "#374151")
        panel_y += 17
    panel_y += 16
    draw_legend(png, panel_x, panel_y)  # type: ignore[arg-type]

    png.save(output_path)


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
    svg.text(panel_x, panel_y, "Turn", 15, "#111827", "700")
    panel_y += 22
    svg.text(
        panel_x,
        panel_y,
        f"turn_order: #{progression.turn_index + 1} (action_sequence={progression.turn_index})",
        11,
        "#374151",
    )
    panel_y += 17
    if progression.active_slot is not None:
        svg.text(
            panel_x,
            panel_y,
            f"active_slot: {progression.active_slot} {slot_short_name(progression.active_slot)}",
            11,
            "#374151",
        )
    else:
        svg.text(panel_x, panel_y, "active_slot: unknown", 11, "#374151")
    panel_y += 17
    svg.text(panel_x, panel_y, f"seq_window: {progression.start_seq}-{progression.end_seq}", 11, "#374151")
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
    png.text(panel_x, panel_y, "Turn", 15, "#111827", "700")
    panel_y += 22
    png.text(
        panel_x,
        panel_y,
        f"turn_order: #{progression.turn_index + 1} (action_sequence={progression.turn_index})",
        11,
        "#374151",
    )
    panel_y += 17
    if progression.active_slot is not None:
        png.text(
            panel_x,
            panel_y,
            f"active_slot: {progression.active_slot} {slot_short_name(progression.active_slot)}",
            11,
            "#374151",
        )
    else:
        png.text(panel_x, panel_y, "active_slot: unknown", 11, "#374151")
    panel_y += 17
    png.text(panel_x, panel_y, f"seq_window: {progression.start_seq}-{progression.end_seq}", 11, "#374151")
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
    terrain_squares: set[Tuple[int, int]],
    cell_px: int,
    export_png_files: bool,
) -> List[Dict[str, object]]:
    rendered: List[Dict[str, object]] = []
    turn_dir = output_dir / "turn_progression"
    for progression, samples in build_turn_progressions(run_root, source_filter):
        output_path = turn_dir / f"{progression.key}.svg"
        render_turn_progression(progression, samples, output_path, terrain_squares, cell_px)
        png_path = output_path.with_suffix(".png") if export_png_files else None
        if png_path is not None:
            render_turn_progression_png(progression, samples, png_path, terrain_squares, cell_px)
        active_name = (
            slot_short_name(progression.active_slot) if progression.active_slot is not None else ""
        )
        rendered.append(
            {
                "source_exec": progression.source_exec,
                "clone_exec": progression.clone_exec,
                "turn_order": progression.turn_index + 1,
                "action_sequence": progression.turn_index,
                "active_slot": progression.active_slot if progression.active_slot is not None else "",
                "active_slot_name": active_name,
                "start_seq": progression.start_seq,
                "end_seq": progression.end_seq,
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
    if args.turn_progressions:
        rendered = render_turn_progressions(
            run_root,
            args.out_dir,
            args.source_exec,
            terrain_squares,
            args.cell_px,
            args.export_png,
        )
        if not rendered:
            raise RuntimeError("No turn progressions rendered.")
        return 0

    if args.examples or not args.source_exec:
        examples = choose_default_examples(run_root)
    else:
        examples = [make_single_example(args, run_root)]

    if not examples:
        raise RuntimeError("No examples selected.")

    output_dir = args.out_dir
    for example in examples:
        samples = load_position_samples(example.capture_path, example.start_seq, example.end_seq)
        output_path = output_dir / f"{example.key}.svg"
        render_example(example, samples, output_path, terrain_squares, args.cell_px)
        png_path = output_path.with_suffix(".png") if args.export_png else None
        if png_path is not None:
            render_example_png(example, samples, png_path, terrain_squares, args.cell_px)
        suffix = f", png {png_path}" if png_path is not None else ""
        print(f"{output_path} ({len(samples)} frame samples{suffix})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
