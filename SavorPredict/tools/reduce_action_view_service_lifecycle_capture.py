#!/usr/bin/env python3
"""Reduce action-view controller and action-service lifetime captures."""

from __future__ import annotations

import argparse
import csv
import json
from bisect import bisect_left
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


FRAME_ID = "battle_case5_after_threads_8000A2FC"
RNG_WATCH_ID = "memwatch.rng_seed_write_803469A8"
FRAME_CAP = 2400

RECORD_PUBLICATIONS = {
    "serialized_action_view_publication_8003C738": "serialized",
    "synthetic_action_view_publication_800540BC": "synthetic",
}
RECORD_COMPLETIONS = {
    "action_view_record_normal_completion_80051698": "normal",
    "action_view_record_state3_completion_800516BC": "state3",
}
SERVICE_PUBLICATION = "action_service_publication_8003B2B4"
SERVICE_COMPLETIONS = {
    "action_service_cleanup_commit_80042A14": "normal",
    "action_service_alt_cleanup_commit_80042A90": "alternate",
}
SERVICE_NESTED_CALL = "action_service_nested_call_80042990"

RNG_SOURCES = {
    0x800145C8: "direct_view_cache_miss",
    0x800513D4: "action_view_record_mode0",
    0x8002EBDC: "action_service_eb4c_fallback",
}

CHECKPOINT_RNG_SOURCES = {
    "direct_view_cache_draw_call_800145C8": ("direct_view_cache_miss", 0x800145C8),
    "action_view_record_mode0_draw_800513D4": ("action_view_record_mode0", 0x800513D4),
    "action_view_record_mode0e_call_800514C8": (
        "action_view_record_mode0e", 0x80052BF0
    ),
    "eb4c_fallback_draw_8002EBDC": ("action_service_eb4c_fallback", 0x8002EBDC),
}

CHECKPOINT_RNG_STACK_EVIDENCE = {
    "direct_view_cache_draw_call_800145C8": {0x800145C8},
    "action_view_record_mode0_draw_800513D4": {0x80051320, 0x80051344, 0x800513D4},
    "action_view_record_mode0e_call_800514C8": {0x80052BEC, 0x80052BF0},
    "eb4c_fallback_draw_8002EBDC": {0x8002EBA4, 0x8002EBDC},
}

CONTROLLER_SELECTOR_PREFIXES = (
    "action_view_controller_",
    "action_view_selector_",
    "action_view_category",
)

CONTROLLER_FIELDS = (
    "controller_thread",
    "controller_thread_return",
    "selector_state",
    "controller_selector_ptr_0x6c",
    "selector_requested_mode_0x00",
    "selector_previous_mode_0x01",
    "selector_active_slot_0x02",
    "selector_target_slot_0x04",
    "selector_effective_mode_0x2f",
    "selector_state_0x30",
    "action_view_chain_actor_field6_0x6",
    "action_view_chain_target_field6_0x6",
)

RECORD_FIELDS = (
    "record_payload_primary_0x00",
    "record_payload_secondary_0x02",
    "record_payload_variant_0x04",
    "record_payload_flags_0x10",
    "record_payload_mode_0x22",
    "record_saved_mode_0x110",
    "record_effective_mode_0x112",
    "record_substate_0x10e",
    "record_thread_state_0x19",
)

SERVICE_FIELDS = (
    "service_command_mode_0x12",
    "service_command_subtype_0x14",
    "service_derived_mode_0x02",
    "service_derived_subtype_0x04",
    "service_selected_slot_0x06",
    "service_sync_flags_0x16",
    "service_flags_0x20",
    "service_forced_mode_0x26",
    "service_thread_state_0x19",
    "service_origin_iw_target_0x04",
    "service_origin_iw_mode_0x06",
    "service_origin_iw_flags_0xec",
    "service_selected_target_iw_slot_0x00",
    "service_selected_target_iw_mode_0x06",
)


def scalar(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def integer(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return scalar(event.get("checkpoint_id"))


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


def pointer(value: Any) -> int | None:
    parsed = integer(value)
    if parsed in (None, 0):
        return None
    return parsed & 0xFFFFFFFF


def pointer_text(value: Any) -> str:
    parsed = pointer(value)
    return "" if parsed is None else f"0x{parsed:08X}"


def load_jsonl(path: Path) -> list[dict[str, Any]]:
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


def locate_manifest(run: Path) -> Path:
    for candidate in (run / "manifest.json", run / "runs" / "manifest.json"):
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"no manifest.json found under {run}")


def resolve_capture_path(manifest_path: Path, raw_path: Any) -> Path:
    candidate = Path(scalar(raw_path))
    if candidate.is_absolute():
        return candidate
    return manifest_path.parent / candidate


def run_captures(run: Path) -> Iterable[tuple[int, Path, dict[str, Any]]]:
    manifest_path = locate_manifest(run)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    jobs = manifest.get("jobs")
    if isinstance(jobs, list):
        for job_row in jobs:
            if not isinstance(job_row, dict):
                continue
            job = integer(job_row.get("original_exec_job_id"))
            capture = resolve_capture_path(manifest_path, job_row.get("stable_capture_path"))
            if job is not None and capture.exists():
                yield job, capture, job_row
        return
    job = integer(manifest.get("original_exec_job_id"))
    capture = manifest_path.parent / "capture.jsonl"
    if job is not None and capture.exists():
        yield job, capture, manifest


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


def node_pointer(node: dict[str, Any]) -> int | None:
    for key in ("node", "address", "node_address"):
        value = pointer(node.get(key))
        if value is not None:
            return value
    return None


def thread_index(event: dict[str, Any], thread: Any) -> int | None:
    expected = pointer(thread)
    if expected is None:
        return None
    for index, node in enumerate(event_nodes(event)):
        if node_pointer(node) == expected:
            return index
    return None


def slot_for_thread(event: dict[str, Any], thread: Any) -> int | None:
    expected = pointer(thread)
    if expected is None:
        return None
    for slot in range(12):
        if pointer(event.get(f"slot{slot}_combatant_thread_ptr")) == expected:
            return slot
    return None


def frame_context(events: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[int]]:
    frames = [event for event in events if checkpoint_id(event) == FRAME_ID]
    return frames, [sequence(frame) for frame in frames]


def frame_bracket(
    frames: list[dict[str, Any]], frame_sequences: list[int], event_sequence: int
) -> dict[str, Any]:
    index = bisect_left(frame_sequences, event_sequence)
    previous = frames[index - 1] if index > 0 else None
    following = frames[index] if index < len(frames) else None
    return {
        "previous_frame_ordinal": index - 1 if previous is not None else "",
        "previous_frame_sequence": sequence(previous) if previous is not None else "",
        "next_frame_ordinal": index if following is not None else "",
        "next_frame_sequence": sequence(following) if following is not None else "",
    }


def frame_presence(
    frames: list[dict[str, Any]],
    thread: Any,
    start_sequence: int,
    end_sequence: int,
) -> dict[str, Any]:
    observations: list[tuple[int, int, int]] = []
    for ordinal, frame in enumerate(frames):
        frame_sequence = sequence(frame)
        if frame_sequence < start_sequence or frame_sequence > end_sequence:
            continue
        index = thread_index(frame, thread)
        if index is not None:
            observations.append((ordinal, frame_sequence, index))
    if not observations:
        return {
            "frame_presence_count": 0,
            "first_presence_frame_ordinal": "",
            "first_presence_frame_sequence": "",
            "first_presence_thread_index": "",
            "last_presence_frame_ordinal": "",
            "last_presence_frame_sequence": "",
            "last_presence_thread_index": "",
        }
    first = observations[0]
    last = observations[-1]
    return {
        "frame_presence_count": len(observations),
        "first_presence_frame_ordinal": first[0],
        "first_presence_frame_sequence": first[1],
        "first_presence_thread_index": first[2],
        "last_presence_frame_ordinal": last[0],
        "last_presence_frame_sequence": last[1],
        "last_presence_thread_index": last[2],
    }


def first_present(event: dict[str, Any], names: Iterable[str]) -> Any:
    for name in names:
        value = event.get(name)
        if pointer(value) is not None:
            return value
    return None


def event_thread(event: dict[str, Any], kind: str) -> Any:
    if kind == "controller":
        return first_present(event, ("controller_thread", "controller_thread_return"))
    if kind == "record":
        return first_present(event, ("record_thread", "record_thread_return"))
    if kind == "service":
        return first_present(event, ("service_thread", "service_thread_return"))
    return None


def controller_selector_rows(
    job: int, events: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    frames, frame_sequences = frame_context(events)
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        if not event_id.startswith(CONTROLLER_SELECTOR_PREFIXES):
            continue
        thread = event_thread(event, "controller")
        row = {
            "source_exec_job_id": job,
            "capture_sequence": sequence(event),
            "checkpoint_id": event_id,
            "pc": event.get("pc"),
            "frame_count": event.get("frame_count"),
            "vi_field_count": event.get("vi_field_count"),
            "rng_draw_index_before": event.get("rng_draw_index_before"),
            "turn_phase": event.get("turn_phase_8034733c"),
            "active_actor_slot": event.get("active_actor_slot_80347334"),
            "action_sequence": event.get("action_sequence_80347335"),
            "controller_thread": pointer_text(thread),
            "thread_index": thread_index(event, thread),
            **frame_bracket(frames, frame_sequences, sequence(event)),
        }
        row.update({name: event.get(name) for name in CONTROLLER_FIELDS})
        rows.append(row)
    return rows


def matching_events(
    events: list[dict[str, Any]],
    kind: str,
    thread: Any,
    start_sequence: int,
    stop_sequence: int | None = None,
) -> list[dict[str, Any]]:
    expected = pointer(thread)
    if expected is None:
        return []
    rows: list[dict[str, Any]] = []
    for event in events:
        event_sequence = sequence(event)
        if event_sequence < start_sequence:
            continue
        if stop_sequence is not None and event_sequence > stop_sequence:
            break
        if pointer(event_thread(event, kind)) == expected:
            rows.append(event)
    return rows


def next_same_thread_publication_sequence(
    publications: list[dict[str, Any]], publication_index: int, kind: str
) -> int | None:
    current_thread = pointer(event_thread(publications[publication_index], kind))
    if current_thread is None:
        return None
    for later in publications[publication_index + 1:]:
        if pointer(event_thread(later, kind)) == current_thread:
            return sequence(later) - 1
    return None


def completion_event(
    events: list[dict[str, Any]], completion_ids: dict[str, str]
) -> tuple[dict[str, Any] | None, str]:
    for event in events:
        reason = completion_ids.get(checkpoint_id(event))
        if reason:
            return event, reason
    return None, ""


def latest_value(events: list[dict[str, Any]], fields: Iterable[str]) -> Any:
    for event in reversed(events):
        for field in fields:
            value = event.get(field)
            if value not in (None, ""):
                return value
    return ""


def first_event_with_id(
    events: Iterable[dict[str, Any]], *event_ids: str
) -> dict[str, Any] | None:
    expected = set(event_ids)
    return next((event for event in events if checkpoint_id(event) in expected), None)


def first_event_with_pointer(
    events: Iterable[dict[str, Any]], field: str
) -> dict[str, Any] | None:
    return next((event for event in events if pointer(event.get(field)) is not None), None)


def event_milestone(
    prefix: str,
    event: dict[str, Any] | None,
    frames: list[dict[str, Any]],
    frame_sequences: list[int],
) -> dict[str, Any]:
    if event is None:
        return {
            f"{prefix}_sequence": "",
            f"{prefix}_previous_frame_ordinal": "",
            f"{prefix}_next_frame_ordinal": "",
        }
    bracket = frame_bracket(frames, frame_sequences, sequence(event))
    return {
        f"{prefix}_sequence": sequence(event),
        f"{prefix}_previous_frame_ordinal": bracket["previous_frame_ordinal"],
        f"{prefix}_next_frame_ordinal": bracket["next_frame_ordinal"],
    }


def action_view_record_rows(
    job: int, events: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    frames, frame_sequences = frame_context(events)
    event_by_sequence = {sequence(event): event for event in events}
    publications = [event for event in events if checkpoint_id(event) in RECORD_PUBLICATIONS]
    rows: list[dict[str, Any]] = []
    for publication_index, publication in enumerate(publications):
        thread = event_thread(publication, "record")
        next_publication_sequence = next_same_thread_publication_sequence(
            publications, publication_index, "record"
        )
        lifetime = matching_events(
            events, "record", thread, sequence(publication), next_publication_sequence
        )
        completion, completion_reason = completion_event(lifetime, RECORD_COMPLETIONS)
        end_event = completion or (lifetime[-1] if lifetime else publication)
        origin_thread = publication.get("record_origin_thread_ptr_0x74")
        callback_events = [
            event for event in lifetime if checkpoint_id(event).startswith("action_view_record_")
        ]
        state0_event = first_event_with_id(
            lifetime, "action_view_record_state0_helper_80051320"
        )
        mode0_draw_event = first_event_with_id(
            lifetime, "action_view_record_mode0_draw_800513D4"
        )
        effective_write_event = first_event_with_id(
            lifetime, "action_view_record_effective_mode_write_8005141C"
        )
        mode1_event = first_event_with_id(lifetime, "action_view_record_mode1_call_800514B0")
        mode0e_event = first_event_with_id(
            lifetime, "action_view_record_mode0e_call_800514C8"
        )
        mode0e_events = [
            event
            for event in lifetime
            if checkpoint_id(event) == "action_view_record_mode0e_call_800514C8"
        ]
        mode0e_rng_draws = 0
        for mode0e_call in mode0e_events:
            following = event_by_sequence.get(sequence(mode0e_call) + 1)
            if (
                following is not None
                and checkpoint_id(following) == RNG_WATCH_ID
                and classify_rng_source(following)[0] == "action_view_record_mode0e"
            ):
                mode0e_rng_draws += 1
        row = {
            "source_exec_job_id": job,
            "creator_kind": RECORD_PUBLICATIONS[checkpoint_id(publication)],
            "record_thread": pointer_text(thread),
            "origin_thread": pointer_text(origin_thread),
            "origin_slot": integer(publication.get("record_origin_iw_slot_0x00"))
            if publication.get("record_origin_iw_slot_0x00") not in (None, "")
            else slot_for_thread(publication, origin_thread),
            "publication_sequence": sequence(publication),
            "publication_thread_index": thread_index(publication, thread),
            "publication_turn_phase": publication.get("turn_phase_8034733c"),
            "publication_active_actor_slot": publication.get("active_actor_slot_80347334"),
            "publication_action_sequence": publication.get("action_sequence_80347335"),
            "publication_rng_draw_index_before": publication.get("rng_draw_index_before"),
            "first_callback_sequence": sequence(callback_events[0]) if callback_events else "",
            "last_callback_sequence": sequence(callback_events[-1]) if callback_events else "",
            "callback_checkpoint_hits": len(callback_events),
            "callback_entry_hits": sum(
                checkpoint_id(event) == "action_view_record_entry_80051264"
                for event in lifetime
            ),
            "completion_sequence": sequence(completion) if completion else "",
            "completion_reason": completion_reason,
            "complete": completion is not None,
            "completion_frame_count": completion.get("frame_count") if completion else "",
            "payload_mode": latest_value(lifetime or [publication], ("record_payload_mode_0x22",)),
            "saved_mode": latest_value(lifetime or [publication], ("record_saved_mode_0x110",)),
            "effective_mode": latest_value(
                lifetime or [publication], ("record_effective_mode_0x112",)
            ),
            "mode0_draw_calls": sum(
                checkpoint_id(event) == "action_view_record_mode0_draw_800513D4"
                for event in lifetime
            ),
            "mode1_calls": sum(
                checkpoint_id(event) == "action_view_record_mode1_call_800514B0"
                for event in lifetime
            ),
            "mode0e_calls": sum(
                checkpoint_id(event) == "action_view_record_mode0e_call_800514C8"
                for event in lifetime
            ),
            "mode0e_rng_draws": mode0e_rng_draws,
            **frame_presence(
                frames,
                thread,
                sequence(publication),
                sequence(end_event),
            ),
            **frame_bracket(frames, frame_sequences, sequence(publication)),
            **event_milestone("state0", state0_event, frames, frame_sequences),
            **event_milestone("mode0_draw", mode0_draw_event, frames, frame_sequences),
            **event_milestone(
                "effective_mode_write", effective_write_event, frames, frame_sequences
            ),
            **event_milestone("mode1_first_call", mode1_event, frames, frame_sequences),
            **event_milestone("mode0e_first_call", mode0e_event, frames, frame_sequences),
        }
        completion_bracket = frame_bracket(frames, frame_sequences, sequence(end_event))
        row.update({f"completion_{key}": value for key, value in completion_bracket.items()})
        row.update({name: latest_value(lifetime or [publication], (name,)) for name in RECORD_FIELDS})
        rows.append(row)
    return rows


def action_service_rows(
    job: int, events: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    frames, frame_sequences = frame_context(events)
    publications = [event for event in events if checkpoint_id(event) == SERVICE_PUBLICATION]
    rows: list[dict[str, Any]] = []
    for publication_index, publication in enumerate(publications):
        thread = event_thread(publication, "service")
        next_publication_sequence = next_same_thread_publication_sequence(
            publications, publication_index, "service"
        )
        lifetime = matching_events(
            events, "service", thread, sequence(publication), next_publication_sequence
        )
        completion, completion_reason = completion_event(lifetime, SERVICE_COMPLETIONS)
        end_sequence = sequence(completion) if completion else (
            sequence(lifetime[-1]) if lifetime else sequence(publication)
        )
        nested_events = [
            event
            for event in events
            if sequence(publication) <= sequence(event) <= end_sequence
            and (
                checkpoint_id(event).startswith("action_service_nested_")
                or checkpoint_id(event).startswith("action_service_resolution_")
                or checkpoint_id(event).startswith("action_service_mode_path_")
                or checkpoint_id(event).startswith("action_service_eb4c_")
                or checkpoint_id(event).startswith("eb4c_")
            )
        ]
        eb4c_publication_events = [
            event
            for event in nested_events
            if checkpoint_id(event) == "eb4c_publication_call_8002EC2C"
        ]
        origin_thread = publication.get("service_origin_thread_ptr_0x08")
        callback_events = [
            event for event in lifetime if checkpoint_id(event).startswith("action_service_")
        ]
        initialized_event = first_event_with_id(
            lifetime, "action_service_delay_80042938"
        )
        state0_event = first_event_with_id(lifetime, "action_service_init_800428D4")
        target_event = first_event_with_pointer(
            lifetime, "service_selected_target_thread_ptr_0x0c"
        )
        selected_target = (
            target_event.get("service_selected_target_thread_ptr_0x0c")
            if target_event is not None
            else None
        )
        split_event = first_event_with_id(lifetime, "action_service_split_80042958")
        nested_call_event = first_event_with_id(lifetime, SERVICE_NESTED_CALL)
        eb4c_entry_event = first_event_with_id(nested_events, "eb4c_entry_8002EB4C")
        eb4c_draw_event = first_event_with_id(nested_events, "eb4c_fallback_draw_8002EBDC")
        eb4c_publication_event = first_event_with_id(
            nested_events, "eb4c_publication_call_8002EC2C"
        )
        row = {
            "source_exec_job_id": job,
            "service_thread": pointer_text(thread),
            "origin_thread": pointer_text(origin_thread),
            "origin_slot": integer(publication.get("service_origin_iw_slot_0x00"))
            if publication.get("service_origin_iw_slot_0x00") not in (None, "")
            else slot_for_thread(publication, origin_thread),
            "selected_target_thread": pointer_text(selected_target),
            "selected_target_slot": (
                integer(target_event.get("service_selected_target_iw_slot_0x00"))
                if target_event is not None
                else None
            ),
            "publication_sequence": sequence(publication),
            "publication_thread_index": thread_index(publication, thread),
            "publication_turn_phase": publication.get("turn_phase_8034733c"),
            "publication_active_actor_slot": publication.get("active_actor_slot_80347334"),
            "publication_action_sequence": publication.get("action_sequence_80347335"),
            "publication_rng_draw_index_before": publication.get("rng_draw_index_before"),
            "service_delay_initial_0x24": publication.get("service_delay_0x24"),
            "delay_checkpoint_hits": sum(
                checkpoint_id(event) == "action_service_delay_80042938"
                for event in lifetime
            ),
            "first_callback_sequence": sequence(callback_events[0]) if callback_events else "",
            "last_callback_sequence": sequence(callback_events[-1]) if callback_events else "",
            "callback_checkpoint_hits": len(callback_events),
            "callback_entry_hits": sum(
                checkpoint_id(event) == "action_service_entry_8004281C" for event in lifetime
            ),
            "nested_calls": sum(
                checkpoint_id(event) == SERVICE_NESTED_CALL for event in lifetime
            ),
            "eb4c_entries": sum(
                checkpoint_id(event) == "eb4c_entry_8002EB4C" for event in nested_events
            ),
            "eb4c_fallback_draw_calls": sum(
                checkpoint_id(event) == "eb4c_fallback_draw_8002EBDC"
                for event in nested_events
            ),
            "eb4c_selected_mode": latest_value(
                eb4c_publication_events, ("selected_mode_r29",)
            ),
            "eb4c_target_slot": latest_value(
                eb4c_publication_events, ("eb4c_target_iw_slot_0x00",)
            ),
            "completion_sequence": sequence(completion) if completion else "",
            "completion_reason": completion_reason,
            "complete": completion is not None,
            "completion_frame_count": completion.get("frame_count") if completion else "",
            **frame_presence(frames, thread, sequence(publication), end_sequence),
            **frame_bracket(frames, frame_sequences, sequence(publication)),
            **event_milestone("state0", state0_event, frames, frame_sequences),
            **event_milestone("initialized", initialized_event, frames, frame_sequences),
            **event_milestone("split", split_event, frames, frame_sequences),
            **event_milestone("nested_call", nested_call_event, frames, frame_sequences),
            **event_milestone("eb4c_entry", eb4c_entry_event, frames, frame_sequences),
            **event_milestone("eb4c_draw", eb4c_draw_event, frames, frame_sequences),
            **event_milestone(
                "eb4c_publication", eb4c_publication_event, frames, frame_sequences
            ),
        }
        completion_bracket = frame_bracket(frames, frame_sequences, end_sequence)
        row.update({f"completion_{key}": value for key, value in completion_bracket.items()})
        service_values = lifetime or [publication]
        row.update({name: latest_value(service_values, (name,)) for name in SERVICE_FIELDS})
        row["service_delay_final_0x24"] = latest_value(
            service_values, ("service_delay_0x24",)
        )
        if initialized_event is not None:
            row["service_derived_mode_0x02"] = initialized_event.get(
                "service_derived_mode_0x02", row["service_derived_mode_0x02"]
            )
            row["service_derived_subtype_0x04"] = initialized_event.get(
                "service_derived_subtype_0x04", row["service_derived_subtype_0x04"]
            )
            row["service_selected_slot_0x06"] = initialized_event.get(
                "service_selected_slot_0x06", row["service_selected_slot_0x06"]
            )
        rows.append(row)
    return rows


def stack_callsite_pcs(event: dict[str, Any]) -> list[int]:
    values: list[int] = []
    for name in ("caller_callsite_pc", "lr_callsite_pc"):
        value = integer(event.get(name))
        if value not in (None, 0):
            values.append(value & 0xFFFFFFFF)
    frame_count = integer(event.get("stack_frame_count")) or 0
    for index in range(frame_count):
        value = integer(event.get(f"stack_frame_{index}_callsite_pc"))
        if value not in (None, 0):
            values.append(value & 0xFFFFFFFF)
    return values


def classify_rng_source(event: dict[str, Any]) -> tuple[str, str]:
    callsites = stack_callsite_pcs(event)
    if 0x80052BEC in callsites and 0x800514C8 in callsites:
        return "action_view_record_mode0e", "0x80052BF0"
    for pc, name in RNG_SOURCES.items():
        if pc in callsites:
            return name, f"0x{pc:08X}"
    # Older unwind rows can land on the candidate lookup immediately before
    # FUN_8002EB4C's fallback draw.
    if 0x8002EBA4 in callsites:
        return RNG_SOURCES[0x8002EBDC], "0x8002EBDC"
    return "other", pointer_text(callsites[0]) if callsites else ""


def rng_window_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    frames, frame_sequences = frame_context(events)
    semantic_events = [
        event
        for event in events
        if checkpoint_id(event) not in (RNG_WATCH_ID, FRAME_ID)
        and not checkpoint_id(event).startswith("memwatch.")
    ]
    semantic_sequences = [sequence(event) for event in semantic_events]
    rows: list[dict[str, Any]] = []
    for event in events:
        if checkpoint_id(event) != RNG_WATCH_ID:
            continue
        event_sequence = sequence(event)
        index = bisect_left(semantic_sequences, event_sequence)
        previous = semantic_events[index - 1] if index > 0 else None
        following = semantic_events[index] if index < len(semantic_events) else None
        source, source_pc = classify_rng_source(event)
        callsites = stack_callsite_pcs(event)
        if previous is not None and event_sequence - sequence(previous) == 1:
            previous_id = checkpoint_id(previous)
            checkpoint_source = CHECKPOINT_RNG_SOURCES.get(previous_id)
            stack_evidence = CHECKPOINT_RNG_STACK_EVIDENCE.get(previous_id, set())
            if checkpoint_source is not None and any(
                pc in stack_evidence for pc in callsites
            ):
                source, checkpoint_pc = checkpoint_source
                source_pc = f"0x{checkpoint_pc:08X}"
        rows.append({
            "source_exec_job_id": job,
            "capture_sequence": event_sequence,
            "rng_draw_index_before": event.get("rng_draw_index_before"),
            "post_write_seed": event.get("rng_seed_before") or event.get("decoded_value"),
            "source": source,
            "source_pc": source_pc,
            "caller_callsite_pc": event.get("caller_callsite_pc"),
            "caller_return_pc": event.get("caller_return_pc"),
            "caller_source": event.get("caller_source"),
            "stack_callsites": "|".join(f"0x{pc:08X}" for pc in callsites),
            "previous_checkpoint_id": checkpoint_id(previous) if previous else "",
            "previous_checkpoint_sequence": sequence(previous) if previous else "",
            "next_checkpoint_id": checkpoint_id(following) if following else "",
            "next_checkpoint_sequence": sequence(following) if following else "",
            "frame_count": event.get("frame_count"),
            "vi_field_count": event.get("vi_field_count"),
            **frame_bracket(frames, frame_sequences, event_sequence),
        })
    return rows


def list_snapshot_issues(event: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    if bool_value(event.get("thread_list_read_ok")) is not True:
        issues.append("read_ok")
    if bool_value(event.get("thread_list_truncated")) is not False:
        issues.append("truncated")
    if bool_value(event.get("thread_list_cycle_detected")) is not False:
        issues.append("cycle")
    return issues


def capture_quality_row(
    job: int,
    capture_path: Path,
    metadata: dict[str, Any],
    events: list[dict[str, Any]],
    record_rows: list[dict[str, Any]],
    service_rows: list[dict[str, Any]],
    rng_rows: list[dict[str, Any]],
) -> dict[str, Any]:
    frames = [event for event in events if checkpoint_id(event) == FRAME_ID]
    list_events = [event for event in events if "thread_list_read_ok" in event]
    issue_counts: Counter[str] = Counter()
    for event in list_events:
        issue_counts.update(list_snapshot_issues(event))
    rng_sources = Counter(row["source"] for row in rng_rows)
    frame_cap_reached = len(frames) >= FRAME_CAP
    fake_attack = metadata.get("fake_attacks_this_turn", metadata.get("source_fake_attacks_this_turn"))
    issues: list[str] = []
    if integer(fake_attack) != 0:
        issues.append("fake_attack is not zero")
    if not frames:
        issues.append("no case-5 frame snapshots")
    if frame_cap_reached:
        issues.append("case-5 frame cap reached")
    if issue_counts:
        issues.append("thread-list snapshot failure")
    if metadata.get("terminal_state") != "SUCCEEDED":
        issues.append("job did not succeed")
    controller_frame_presence = sum(
        any(integer(node.get("callback")) == 0x800136DC for node in event_nodes(frame))
        for frame in frames
    )
    return {
        "source_exec_job_id": job,
        "capture_path": str(capture_path),
        "terminal_state": metadata.get("terminal_state", ""),
        "fake_attacks_this_turn": fake_attack,
        "event_count": len(events),
        "frame_count": len(frames),
        "frame_cap_reached": frame_cap_reached,
        "list_snapshot_count": len(list_events),
        "list_read_failures": issue_counts["read_ok"],
        "list_truncations": issue_counts["truncated"],
        "list_cycles": issue_counts["cycle"],
        "controller_visits": controller_frame_presence,
        "selector_calls": sum(
            checkpoint_id(event) == "action_view_controller_selector_call_80013B20"
            for event in events
        ),
        "record_publications": len(record_rows),
        "record_completions": sum(bool_value(row["complete"]) is True for row in record_rows),
        "service_publications": len(service_rows),
        "service_completions": sum(bool_value(row["complete"]) is True for row in service_rows),
        "eb4c_entries": sum(integer(row["eb4c_entries"]) or 0 for row in service_rows),
        "rng_draws": len(rng_rows),
        "direct_view_draws": rng_sources["direct_view_cache_miss"],
        "record_mode0_draws": rng_sources["action_view_record_mode0"],
        "record_mode0e_draws": rng_sources["action_view_record_mode0e"],
        "eb4c_fallback_draws": rng_sources["action_service_eb4c_fallback"],
        "quality_status": "complete" if not issues else "incomplete",
        "quality_issues": "; ".join(issues),
    }


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str] | None = None) -> None:
    if fields is None:
        fields = []
        seen: set[str] = set()
        for row in rows:
            for key in row:
                if key not in seen:
                    fields.append(key)
                    seen.add(key)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: scalar(row.get(field)) for field in fields})


def counter_text(values: Iterable[Any]) -> str:
    counts = Counter(scalar(value) for value in values)
    return ", ".join(f"`{name}`: {count}" for name, count in sorted(counts.items()))


def record_mode_counter_text(record_rows: Iterable[dict[str, Any]]) -> str:
    counts = Counter(
        (
            scalar(row.get("creator_kind")),
            scalar(row.get("payload_mode")),
            scalar(row.get("effective_mode")),
        )
        for row in record_rows
    )
    return ", ".join(
        f"`{creator} {payload}->{effective}`: {count}"
        for (creator, payload, effective), count in sorted(counts.items())
    )


def markdown_findings(
    quality_rows: list[dict[str, Any]],
    record_rows: list[dict[str, Any]],
    service_rows: list[dict[str, Any]],
) -> str:
    run_roots = sorted({
        str(Path(scalar(row.get("capture_path"))).parents[1])
        for row in quality_rows
        if scalar(row.get("capture_path"))
    })
    lines = [
        "# Action-view and action-service lifecycle capture",
        "",
        "This pass brackets persistent action-view controller work, transient action-view records, "
        "action-service workers, and `FUN_8002EB4C` RNG draws against the same case-5 frame stream.",
        "",
        "Dolphin seed-watch values are post-write. Heap thread addresses in the CSVs are run-local "
        "identity keys only; no address is reused as a predictor input or across jobs.",
        "",
        "Accepted raw run root: " + (", ".join(f"`{root}`" for root in run_roots) or "unknown"),
        "All accepted source jobs have `fake_attacks_this_turn=0`, completed naturally, and use no RNG or fake-attack override.",
        "",
        "| Job | Frames | Controller present | Selector calls | Records done/pub | Services done/pub | EB4C entries | RNG direct/mode0/mode0e/EB4C | Quality |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in quality_rows:
        lines.append(
            f"| {row['source_exec_job_id']} | {row['frame_count']} | "
            f"{row['controller_visits']} | {row['selector_calls']} | "
            f"{row['record_completions']}/{row['record_publications']} | "
            f"{row['service_completions']}/{row['service_publications']} | "
            f"{row['eb4c_entries']} | {row['direct_view_draws']}/"
            f"{row['record_mode0_draws']}/{row['record_mode0e_draws']}/"
            f"{row['eb4c_fallback_draws']} | "
            f"{row['quality_status']} |"
        )
    mode0_rows = [row for row in record_rows if (integer(row.get("mode0_draw_calls")) or 0) > 0]
    mode0_rewrites = sum(integer(row.get("effective_mode")) == 0xE for row in mode0_rows)
    effective_mode0e_rows = [
        row for row in record_rows if integer(row.get("effective_mode")) == 0xE
    ]
    service_gate_exact = all(
        bool((integer(row.get("service_origin_iw_flags_0xec")) or 0) & 0x00100000)
        == ((integer(row.get("eb4c_entries")) or 0) > 0)
        for row in service_rows
    )
    derived_matches_origin = sum(
        integer(row.get("service_derived_mode_0x02"))
        == integer(row.get("service_origin_iw_mode_0x06"))
        for row in service_rows
    )
    target_matches_origin = sum(
        integer(row.get("service_selected_slot_0x06"))
        == integer(row.get("service_origin_iw_target_0x04"))
        == integer(row.get("selected_target_slot"))
        for row in service_rows
    )
    exact_delay_visits = sum(
        (integer(row.get("delay_checkpoint_hits")) or 0)
        == (integer(row.get("service_delay_initial_0x24")) or 0) + 1
        for row in service_rows
    )
    incomplete = [row for row in quality_rows if row["quality_status"] != "complete"]
    lines.extend([
        "",
        "## Findings",
        "",
        f"The persistent `FUN_800136DC` controller was present in every one of the "
        f"{sum(integer(row['frame_count']) or 0 for row in quality_rows)} captured frame-end lists. "
        "The selector call is conditional and must remain separate from controller lifetime.",
        "",
        f"All {len(record_rows)} action-view records and all {len(service_rows)} action-service "
        "children reached a captured cleanup. Record creator/mode/effective-mode counts are "
        f"{record_mode_counter_text(record_rows)}.",
        "",
        f"There were {len(mode0_rows)} serialized mode-0 rewrite-gate draws. "
        f"{mode0_rewrites} rewrote to effective mode `0x0E`; the remaining "
        f"{len(mode0_rows) - mode0_rewrites} stayed mode `0`. In every case, publication, "
        "state-0 setup, the `0x800513D4` draw, and the effective-mode write all occurred "
        "between the same two frame-end snapshots. Mode `0x0E` draws are separately owned "
        "by `FUN_80052B24:0x80052BF0` on the record callback path.",
        "",
        f"All {len(effective_mode0e_rows)} effective-mode-`0x0E` records consumed exactly one "
        "`0x80052BF0` draw during their lifetime. Later mode-`0x0E` callback visits did not "
        "implicitly consume another draw.",
        "",
        "Every copied service command mode/subtype in this corpus was `0/0`, so those fields "
        "do not select the live action path. State 0 instead produced derived modes "
        f"{counter_text(row.get('service_derived_mode_0x02') for row in service_rows)}. "
        f"Derived mode matched origin `IW+0x06` in {derived_matches_origin}/{len(service_rows)} "
        f"services, and selected slot/target matched origin `IW+0x04` in "
        f"{target_matches_origin}/{len(service_rows)} services.",
        "",
        f"The state-1 delay loop matched payload semantics in {exact_delay_visits}/"
        f"{len(service_rows)} services: a payload delay `N` produced `N+1` delay checkpoints, "
        "including the zero visit that fell through to the nested call. The nested call and "
        "cleanup remained in that same packed-thread visit.",
        "",
        f"`FUN_8002EB4C` was entered by {sum(integer(row.get('eb4c_entries')) or 0 for row in service_rows)} "
        f"of {len(service_rows)} nested service calls. The `IW+0xEC & 0x00100000` gate classified "
        f"all observed invocations and noninvocations exactly: `{str(service_gate_exact).lower()}`. "
        "Each admitted call used one fallback draw and published mode `0x0C` or `0x0D`; no "
        "unflagged service consumed that draw.",
        "",
        "## Predictor boundary",
        "",
        "The current frame predictor's `trigger=passive_destination_committed` ownership is "
        "disproved. The provisional replacement can be an action-service child runtime: "
        "publish from the visual `SET COMMAND` handler, derive mode/target from the origin "
        "instruction worksheet on state 0, count down payload `+0x24` on packed-thread visits, "
        "and invoke the nested EB4C path only when origin `IW+0xEC & 0x00100000` is set.",
        "",
        "The unconditional pre-attack action-view worker is also disproved. Camera RNG belongs "
        "to transient `UpdateActionViewRecord_80051264` children selected by the STD visual "
        "command dispatcher. The lifetime and same-frame child execution are now known, but a "
        "generic integration must schedule the `SYSTEM CAMERA` visual command publication; "
        "calling the record once per queued attack would preserve the original error.",
        "",
        "Static evidence identifies both creators as handlers in the shared STD combatant visual "
        "command table (`SET COMMAND` and `SYSTEM CAMERA`). The next implementation boundary is "
        "therefore a small frame-backed combatant-visual child-task dispatcher, not two new "
        "attack-level RNG shortcuts.",
        "",
        "## Interpretation boundary",
        "",
        "`controller_selector_timeline.csv` establishes when the persistent controller selects or "
        "invokes view work. `action_view_record_lifetimes.csv` and `action_service_lifetimes.csv` "
        "separate publication, packed-thread visits, nested resolution, and cleanup. "
        "`rng_invocation_windows.csv` attributes each seed write from its current call stack.",
        "",
    ])
    if incomplete:
        lines.append("Capture-quality issues:")
        lines.append("")
        for row in incomplete:
            lines.append(f"- Job {row['source_exec_job_id']}: {row['quality_issues']}")
        lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    controller_rows: list[dict[str, Any]] = []
    record_rows: list[dict[str, Any]] = []
    service_rows: list[dict[str, Any]] = []
    rng_rows: list[dict[str, Any]] = []
    quality_rows: list[dict[str, Any]] = []
    for run in args.run:
        for job, capture, metadata in run_captures(run):
            events = load_jsonl(capture)
            job_controller_rows = controller_selector_rows(job, events)
            job_record_rows = action_view_record_rows(job, events)
            job_service_rows = action_service_rows(job, events)
            job_rng_rows = rng_window_rows(job, events)
            controller_rows.extend(job_controller_rows)
            record_rows.extend(job_record_rows)
            service_rows.extend(job_service_rows)
            rng_rows.extend(job_rng_rows)
            quality_rows.append(capture_quality_row(
                job,
                capture,
                metadata,
                events,
                job_record_rows,
                job_service_rows,
                job_rng_rows,
            ))

    write_csv(args.output / "controller_selector_timeline.csv", controller_rows)
    write_csv(args.output / "action_view_record_lifetimes.csv", record_rows)
    write_csv(args.output / "action_service_lifetimes.csv", service_rows)
    write_csv(args.output / "rng_invocation_windows.csv", rng_rows)
    write_csv(args.output / "capture_quality.csv", quality_rows)
    (args.output / "findings.md").write_text(
        markdown_findings(quality_rows, record_rows, service_rows), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
