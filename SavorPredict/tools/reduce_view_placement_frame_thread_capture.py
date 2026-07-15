#!/usr/bin/env python3
"""Reduce view-placement cache frame/thread capture evidence."""

from __future__ import annotations

import argparse
import csv
import json
from bisect import bisect_left
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
CONTROL_WATCH_ID = "memwatch.view_cache_control_write_8030A062"
FRAME_CAP = 2400

LIST_EVENT_IDS = {
    FRAME_ID,
    "view_placement_cache_decision_800145BC",
    "view_placement_rng_call_800145C8",
    "view_placement_post_selection_800145CC",
    "view_cache_publish_from_14474_80014704",
    "view_cache_publish_from_136DC_800139D8",
    "view_cache_publish_from_121D8_80012530",
}

TIMELINE_EVENT_IDS = {
    "view_placement_cache_decision_800145BC",
    "view_placement_rng_call_800145C8",
    "view_placement_rng_value_800608E0",
    "view_placement_rng_angle_selected_8006093C",
    "view_placement_post_selection_800145CC",
    "view_cache_publish_from_14474_80014704",
    "view_cache_publish_from_136DC_800139D8",
    "view_cache_publish_from_121D8_80012530",
}

PUBLICATION_BY_WRITER_PC = {
    "0x8001250c": "view_cache_publish_from_121D8_80012530",
    "0x800139bc": "view_cache_publish_from_136DC_800139D8",
    "0x800146e8": "view_cache_publish_from_14474_80014704",
}

CACHE_FIELDS = (
    "view_cache_control_8030A062",
    "view_cache_distance_bits_80309FD8",
    "view_cache_center_x_bits_80309F88",
    "view_cache_center_y_bits_80309F8C",
    "view_cache_center_z_bits_80309F90",
    "view_cache_angle_bits_8030A028",
)

THREAD_FIELDS = (
    "callback",
    "next",
    "parent",
    "flags",
    "depth",
    "order_bits",
    "payload_word",
)


def scalar(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def sequence(event: dict[str, Any]) -> int:
    value = event.get("capture_sequence")
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def checkpoint_id(event: dict[str, Any]) -> str:
    return scalar(event.get("checkpoint_id"))


def normalized_pc(value: Any) -> str:
    text = scalar(value).strip().lower()
    if not text:
        return ""
    try:
        return f"0x{int(text, 0) & 0xFFFFFFFF:08x}"
    except ValueError:
        return text


def is_zero_pc(value: Any) -> bool:
    return normalized_pc(value) in {"", "0x00000000"}


def writer_pc(event: dict[str, Any]) -> str:
    source = event.get("memwatch_source_pc")
    if not is_zero_pc(source):
        return normalized_pc(source)
    return normalized_pc(event.get("pc"))


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


def bool_value(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered == "true":
            return True
        if lowered == "false":
            return False
    return None


def list_snapshot_issues(event: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    read_ok = bool_value(event.get("thread_list_read_ok"))
    truncated = bool_value(event.get("thread_list_truncated"))
    cycle_detected = bool_value(event.get("thread_list_cycle_detected"))
    if read_ok is not True:
        issues.append("read_ok is not true")
    if truncated is not False:
        issues.append("truncated is not false")
    if cycle_detected is not False:
        issues.append("cycle_detected is not false")
    return issues


def cache_values(event: dict[str, Any] | None) -> dict[str, str]:
    if event is None:
        return {name: "" for name in CACHE_FIELDS}
    return {name: scalar(event.get(name)) for name in CACHE_FIELDS}


def order(nodes: list[dict[str, Any]]) -> list[str]:
    return [scalar(node.get("node")) for node in nodes]


def node_map(nodes: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {scalar(node.get("node")): node for node in nodes if scalar(node.get("node"))}


def summarize_node_delta(before: list[dict[str, Any]], after: list[dict[str, Any]]) -> dict[str, str]:
    before_order = order(before)
    after_order = order(after)
    before_by_node = node_map(before)
    after_by_node = node_map(after)
    before_set = set(before_by_node)
    after_set = set(after_by_node)
    shared = before_set & after_set
    changed_fields: list[str] = []
    for node in sorted(shared):
        changed = [
            field
            for field in THREAD_FIELDS
            if scalar(before_by_node[node].get(field)) != scalar(after_by_node[node].get(field))
        ]
        if changed:
            changed_fields.append(f"{node}:{'|'.join(changed)}")
    relative_order_changed = [node for node in before_order if node in shared] != [
        node for node in after_order if node in shared
    ]
    return {
        "added": "|".join(node for node in after_order if node not in before_set),
        "removed": "|".join(node for node in before_order if node not in after_set),
        "relative_order_changed": "true" if relative_order_changed else "false",
        "field_mutations": ";".join(changed_fields),
    }


def write_csv(path: Path, rows: Iterable[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({name: scalar(row.get(name)) for name in fieldnames})


def load_events(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            text = line.strip()
            if not text:
                continue
            try:
                event = json.loads(text)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
            if not isinstance(event, dict):
                raise ValueError(f"{path}:{line_number}: JSON row is not an object")
            events.append(event)
    events.sort(key=sequence)
    return events


def nearest_frames(
    frames: list[dict[str, Any]], frame_sequences: list[int], event_sequence: int
) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    index = bisect_left(frame_sequences, event_sequence)
    previous = frames[index - 1] if index > 0 else None
    next_frame = frames[index] if index < len(frames) else None
    return previous, next_frame


def prior_state_event(events: list[dict[str, Any]], event_index: int) -> dict[str, Any] | None:
    for index in range(event_index - 1, -1, -1):
        if "view_cache_control_8030A062" in events[index]:
            return events[index]
    return None


def next_event_with_id(
    events: list[dict[str, Any]], event_index: int, expected_id: str
) -> dict[str, Any] | None:
    for index in range(event_index + 1, len(events)):
        candidate = events[index]
        if checkpoint_id(candidate) == CONTROL_WATCH_ID:
            return None
        if checkpoint_id(candidate) == expected_id:
            return candidate
    return None


def build_frame_rows(frames: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for frame in frames:
        base = {
            "capture_sequence": sequence(frame),
            "frame_count": frame.get("frame_count"),
            "movie_input_count": frame.get("movie_input_count"),
            "vi_field_count": frame.get("vi_field_count"),
            "turn_phase": frame.get("turn_phase_8034733c"),
            "battle_input_state": frame.get("battle_input_state_80347338"),
            "active_actor_slot": frame.get("active_actor_slot_80347334"),
            "action_sequence": frame.get("action_sequence_80347335"),
            "view_interrupt_flags": frame.get("view_interrupt_flags_80309F10"),
            "thread_list_read_ok": frame.get("thread_list_read_ok"),
            "thread_list_truncated": frame.get("thread_list_truncated"),
            "thread_list_cycle_detected": frame.get("thread_list_cycle_detected"),
            "thread_list_node_count": frame.get("thread_list_node_count"),
            **cache_values(frame),
        }
        nodes = event_nodes(frame)
        if not nodes:
            rows.append(base)
            continue
        for index, node in enumerate(nodes):
            rows.append({
                **base,
                "thread_index": index,
                "thread_node": node.get("node"),
                **{f"thread_{field}": node.get(field) for field in THREAD_FIELDS},
            })
    return rows


def build_mutation_rows(
    events: list[dict[str, Any]], frames: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[str]]:
    frame_sequences = [sequence(frame) for frame in frames]
    mutation_rows: list[dict[str, Any]] = []
    window_rows: list[dict[str, Any]] = []
    issues: list[str] = []

    for event_index, event in enumerate(events):
        if checkpoint_id(event) != CONTROL_WATCH_ID:
            continue
        source_pc = writer_pc(event)
        expected_publication = PUBLICATION_BY_WRITER_PC.get(source_pc, "")
        publication = (
            next_event_with_id(events, event_index, expected_publication)
            if expected_publication
            else None
        )
        previous_frame, next_frame = nearest_frames(frames, frame_sequences, sequence(event))
        before_state = prior_state_event(events, event_index)
        after_state = publication
        writer_class = (
            "recognized_cache_publication"
            if expected_publication
            else "unresolved_overlapping_write"
        )
        if not expected_publication:
            issues.append(f"control write {sequence(event)} has unknown writer {source_pc or 'missing'}")
        elif publication is None:
            issues.append(
                f"control write {sequence(event)} at {source_pc} lacks {expected_publication} publication"
            )

        row = {
            "control_write_sequence": sequence(event),
            "source_pc": source_pc,
            "memwatch_source_pc": event.get("memwatch_source_pc"),
            "pc": event.get("pc"),
            "caller_source": event.get("caller_source"),
            "caller_callsite_pc": event.get("caller_callsite_pc"),
            "caller_return_pc": event.get("caller_return_pc"),
            "stack_frame_0_callsite_pc": event.get("stack_frame_0_callsite_pc"),
            "rng_draw_index_before": (
                publication.get("rng_draw_index_before") if publication else event.get("rng_draw_index_before")
            ),
            "writer_class": writer_class,
            "memwatch_addr": event.get("memwatch_addr"),
            "memwatch_size": event.get("memwatch_size"),
            "memwatch_access": event.get("memwatch_access"),
            "decoded_opcode": event.get("decoded_opcode"),
            "decoded_value_reg": event.get("decoded_value_reg"),
            "decoded_value": event.get("decoded_value"),
            "post_write_decoded_memory_value": event.get("decoded_memory_value"),
            "memwatch_confirmed_current_instruction": event.get("memwatch_confirmed_current_instruction"),
            "memwatch_unattributed_extra_hits": event.get("memwatch_unattributed_extra_hits"),
            "expected_publication_id": expected_publication,
            "publication_sequence": sequence(publication) if publication else "",
            "publication_matched": "true" if publication else "false",
            "previous_frame_sequence": sequence(previous_frame) if previous_frame else "",
            "previous_frame_count": previous_frame.get("frame_count") if previous_frame else "",
            "next_frame_sequence": sequence(next_frame) if next_frame else "",
            "next_frame_count": next_frame.get("frame_count") if next_frame else "",
        }
        for name, value in cache_values(before_state).items():
            row[f"before_{name}"] = value
        for name, value in cache_values(after_state).items():
            row[f"after_{name}"] = value
        mutation_rows.append(row)

        before_nodes = event_nodes(previous_frame) if previous_frame else []
        publication_nodes = event_nodes(publication) if publication else []
        after_nodes = event_nodes(next_frame) if next_frame else []
        before_to_publication = summarize_node_delta(before_nodes, publication_nodes)
        publication_to_after = summarize_node_delta(publication_nodes, after_nodes)
        window_rows.append({
            "control_write_sequence": sequence(event),
            "source_pc": source_pc,
            "publication_sequence": sequence(publication) if publication else "",
            "previous_frame_sequence": sequence(previous_frame) if previous_frame else "",
            "next_frame_sequence": sequence(next_frame) if next_frame else "",
            "previous_frame_order": "|".join(order(before_nodes)),
            "publication_order": "|".join(order(publication_nodes)),
            "next_frame_order": "|".join(order(after_nodes)),
            **{f"before_to_publication_{key}": value for key, value in before_to_publication.items()},
            **{f"publication_to_next_{key}": value for key, value in publication_to_after.items()},
        })

    return mutation_rows, window_rows, issues


def source_details(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    by_source: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_source.setdefault(scalar(row["source_pc"]) or "missing", []).append(row)
    details: dict[str, dict[str, Any]] = {}
    for source_pc, source_rows in sorted(by_source.items()):
        details[source_pc] = {
            "count": len(source_rows),
            "writer_class": sorted({scalar(row["writer_class"]) for row in source_rows}),
            "decoded_opcodes": sorted({scalar(row["decoded_opcode"]) for row in source_rows}),
            "decoded_values": sorted({scalar(row["decoded_value"]) for row in source_rows}),
            "post_write_decoded_memory_values": sorted({
                scalar(row["post_write_decoded_memory_value"]) for row in source_rows
            }),
            "matched_publication_count": sum(
                row["publication_matched"] == "true" for row in source_rows
            ),
        }
    return details


def write_readme(
    output_dir: Path,
    capture_path: Path,
    summary: dict[str, Any],
    writer_details: dict[str, dict[str, Any]],
) -> None:
    acceptance = summary["acceptance"]
    lines = [
        "# View-Placement Frame/Thread Capture",
        "",
        f"Raw capture: `{capture_path}`",
        "",
        "The cache-control write watchpoint is a Dolphin write watchpoint. Its captured value is post-write; source attribution uses `memwatch_source_pc` when nonzero and otherwise the watchpoint event `pc`.",
        "",
        "## Counts",
        "",
        f"- Events: {summary['event_count']}",
        f"- Case-5 frame snapshots: {summary['frame_count']}",
        f"- Cache-control writes: {summary['control_write_count']}",
        f"- Mapped publications: {summary['mapped_publication_count']}",
        f"- Timeline events without both frame boundaries: {summary['timeline_events_missing_frame_boundary']}",
        "",
        "## Acceptance",
        "",
        f"- Snapshot integrity: {'PASS' if acceptance['snapshot_integrity'] else 'FAIL'}",
        f"- Frame cap not reached: {'PASS' if acceptance['frame_cap_not_reached'] else 'FAIL'}",
        f"- Cache writers recognized and paired: {'PASS' if acceptance['cache_writers_paired'] else 'FAIL'}",
        f"- Decision/draw/publication frame placement: {'PASS' if acceptance['timeline_frame_bounded'] else 'FAIL'}",
        f"- Overall: {'PASS' if acceptance['overall'] else 'FAIL'}",
        "",
        "## Cache-Write Sources",
        "",
    ]
    if writer_details:
        for source_pc, details in writer_details.items():
            lines.append(
                f"- `{source_pc}`: {details['count']} writes; "
                f"class={','.join(details['writer_class'])}; "
                f"opcode={','.join(details['decoded_opcodes'])}; "
                f"post-write={','.join(details['post_write_decoded_memory_values'])}; "
                f"matched publications={details['matched_publication_count']}"
            )
    else:
        lines.append("- None observed.")
    lines.extend([
        "",
        "## Outputs",
        "",
        "- `frame_thread_order.csv`: one row per frame/list node.",
        "- `cache_mutations.csv`: post-write cache-control events paired to expected publishers.",
        "- `mutation_windows.csv`: thread order before publication, at publication, and at the next case-5 frame, including list deltas.",
        "- `summary.json`: machine-readable acceptance details and unresolved rows.",
        "",
    ])
    (output_dir / "README.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--strict", action="store_true", help="Return nonzero when acceptance fails.")
    args = parser.parse_args()

    events = load_events(args.capture)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    frames = [event for event in events if checkpoint_id(event) == FRAME_ID]
    list_events = [event for event in events if checkpoint_id(event) in LIST_EVENT_IDS]
    snapshot_issues = [
        f"{checkpoint_id(event)} sequence {sequence(event)}: {', '.join(list_snapshot_issues(event))}"
        for event in list_events
        if list_snapshot_issues(event)
    ]
    frame_cap_reached = len(frames) >= FRAME_CAP
    mutation_rows, window_rows, mutation_issues = build_mutation_rows(events, frames)
    source_counts = Counter(row["source_pc"] or "missing" for row in mutation_rows)
    writer_details = source_details(mutation_rows)

    frame_sequences = [sequence(frame) for frame in frames]
    timeline_missing_boundary: list[str] = []
    for event in events:
        if checkpoint_id(event) not in TIMELINE_EVENT_IDS:
            continue
        previous_frame, next_frame = nearest_frames(frames, frame_sequences, sequence(event))
        if previous_frame is None or next_frame is None:
            timeline_missing_boundary.append(
                f"{checkpoint_id(event)} sequence {sequence(event)}"
            )

    cache_writers_paired = bool(mutation_rows) and not mutation_issues
    acceptance = {
        "snapshot_integrity": not snapshot_issues and bool(list_events),
        "frame_cap_not_reached": not frame_cap_reached,
        "cache_writers_paired": cache_writers_paired,
        "timeline_frame_bounded": not timeline_missing_boundary,
    }
    acceptance["overall"] = all(acceptance.values())
    summary = {
        "capture": str(args.capture),
        "event_count": len(events),
        "frame_count": len(frames),
        "frame_cap": FRAME_CAP,
        "control_write_count": len(mutation_rows),
        "mapped_publication_count": sum(row["publication_matched"] == "true" for row in mutation_rows),
        "list_snapshot_event_count": len(list_events),
        "snapshot_issues": snapshot_issues,
        "frame_cap_reached": frame_cap_reached,
        "mutation_issues": mutation_issues,
        "timeline_events_missing_frame_boundary": len(timeline_missing_boundary),
        "timeline_missing_frame_boundary": timeline_missing_boundary,
        "cache_write_source_counts": dict(sorted(source_counts.items())),
        "cache_write_source_details": writer_details,
        "acceptance": acceptance,
    }

    write_csv(
        args.output_dir / "frame_thread_order.csv",
        build_frame_rows(frames),
        [
            "capture_sequence", "frame_count", "movie_input_count", "vi_field_count",
            "turn_phase", "battle_input_state", "active_actor_slot", "action_sequence",
            "view_interrupt_flags", "thread_list_read_ok", "thread_list_truncated",
            "thread_list_cycle_detected", "thread_list_node_count", *CACHE_FIELDS,
            "thread_index", "thread_node", *[f"thread_{field}" for field in THREAD_FIELDS],
        ],
    )
    write_csv(
        args.output_dir / "cache_mutations.csv",
        mutation_rows,
        [
            "control_write_sequence", "source_pc", "memwatch_source_pc", "pc", "caller_source",
            "caller_callsite_pc", "caller_return_pc", "stack_frame_0_callsite_pc",
            "rng_draw_index_before", "writer_class", "memwatch_addr", "memwatch_size",
            "memwatch_access", "decoded_opcode", "decoded_value_reg", "decoded_value",
            "post_write_decoded_memory_value", "memwatch_confirmed_current_instruction",
            "memwatch_unattributed_extra_hits", "expected_publication_id",
            "publication_sequence", "publication_matched", "previous_frame_sequence",
            "previous_frame_count", "next_frame_sequence", "next_frame_count",
            *[f"before_{field}" for field in CACHE_FIELDS],
            *[f"after_{field}" for field in CACHE_FIELDS],
        ],
    )
    write_csv(
        args.output_dir / "mutation_windows.csv",
        window_rows,
        [
            "control_write_sequence", "source_pc", "publication_sequence",
            "previous_frame_sequence", "next_frame_sequence", "previous_frame_order",
            "publication_order", "next_frame_order",
            "before_to_publication_added", "before_to_publication_removed",
            "before_to_publication_relative_order_changed", "before_to_publication_field_mutations",
            "publication_to_next_added", "publication_to_next_removed",
            "publication_to_next_relative_order_changed", "publication_to_next_field_mutations",
        ],
    )
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_readme(args.output_dir, args.capture, summary, writer_details)

    print(json.dumps({"output_dir": str(args.output_dir), "acceptance": acceptance}, sort_keys=True))
    return 1 if args.strict and not acceptance["overall"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
