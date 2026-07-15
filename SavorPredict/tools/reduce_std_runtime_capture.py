#!/usr/bin/env python3
"""Reduce STD runtime captures into SpiceStd-facing evidence packets."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


UNKNOWN = "unknown"
DEFAULT_SUPPORT_DIR = (
    Path("Analyses")
    / "std_runtime"
    / "std_runtime_capture_response_2026-06-22_support"
)
DEFAULT_OUT_DIR = DEFAULT_SUPPORT_DIR / "reduced"
DEFAULT_STD_JSON_DIR = Path(r"D:\SavorPredictDB\.std_json")

SLOT_RESOURCE_STEMS = {
    0: "ma000",
    1: "MA001",
    4: "MB000",
    5: "MB000",
}

OUTPUT_FIELDS = [
    "run_root",
    "source_exec_job_id",
    "source_turn_job_id",
    "clone_exec_job_id",
    "clone_turn_job_id",
    "original_job_set_id",
    "battle_set_id",
    "wave_id",
    "turn_index",
    "fake_attacks_this_turn",
    "planned_commands",
    "prediction_source",
    "capture_sequence",
    "checkpoint_name",
    "checkpoint",
    "pc",
    "function",
    "movie_input_count",
    "vi_field_count",
    "frame_count",
    "rng_draw_index_before",
    "rng_seed_before",
    "owns_rng_draw",
    "active_slot",
    "target_slot",
    "resource_slot",
    "resource_stem",
    "std_filename",
    "std0_filename",
    "actor_field6",
    "actor_subtype",
    "instruction_flags",
    "gate_category",
    "gate_state",
    "gate_active_slot",
    "gate_target_slot",
    "aux_list_root",
    "query_arg0",
    "query_arg1",
    "query_arg2",
    "query_arg3",
    "query_key_combined",
    "query_result",
    "query_call_sequence",
    "query_result_sequence",
    "spawn_sequence",
    "mode0_fallback_sequence",
    "mode0_handler_sequence",
    "mode0e_camera_sequence",
    "payload_ptr",
    "payload_primary_key",
    "payload_secondary_key",
    "payload_variant",
    "payload_low_flags",
    "payload_flags",
    "payload_scalar",
    "payload_start_frame",
    "payload_end_frame",
    "payload_hold",
    "payload_step",
    "payload_mode_before",
    "payload_mode_direct",
    "payload_mode_chased",
    "payload_mode_effective",
    "worksheet_saved_mode",
    "worksheet_effective_mode",
    "worksheet_action_id",
    "worksheet_handler",
    "worksheet_selected_row",
    "std_row_index",
    "std_action_id",
    "std_row_type",
    "std_callback_index",
    "std_callback_ordinal",
    "std_flags_hex",
    "std_secondary_key",
    "std_callback_aux_param",
    "std_transition_gate_divisor_hex",
    "std_motion_progress_step_hex",
    "std0_entry_index",
    "std0_combined_type_hex",
    "std0_payload_size",
    "std0_payload_offset",
    "std0_payload_match",
    "payload_classification",
    "evidence_sequences",
    "warnings",
]


def unknown_row() -> dict[str, Any]:
    return {field: UNKNOWN for field in OUTPUT_FIELDS}


def clean_value(value: Any) -> Any:
    if value is None:
        return UNKNOWN
    if value == "":
        return UNKNOWN
    return value


def int_value(value: Any) -> int | None:
    if value is None or value == UNKNOWN:
        return None
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        if not text or text == UNKNOWN:
            return None
        try:
            if text.lower().startswith("0x"):
                return int(text, 16)
            return int(text, 10)
        except ValueError:
            return None
    return None


def hex32(value: Any) -> str:
    number = int_value(value)
    if number is None:
        return UNKNOWN
    return f"0x{number & 0xFFFFFFFF:08x}"


def signed32_value(value: Any) -> int | None:
    number = int_value(value)
    if number is None:
        return None
    number &= 0xFFFFFFFF
    if number & 0x80000000:
        return number - 0x100000000
    return number


def same_known(left: Any, right: Any) -> bool:
    left_int = int_value(left)
    right_int = int_value(right)
    if left_int is None or right_int is None:
        return True
    return left_int == right_int


def checkpoint_is(event: dict[str, Any], name: str) -> bool:
    checkpoint = event.get("checkpoint")
    checkpoint_name = event.get("checkpoint_name")
    return checkpoint == name or str(checkpoint_name or "").startswith(name)


def evidence_seq(event: dict[str, Any] | None) -> Any:
    if not event:
        return UNKNOWN
    return clean_value(event.get("capture_sequence"))


def first_known(*values: Any) -> Any:
    for value in values:
        value = clean_value(value)
        if value != UNKNOWN:
            return value
    return UNKNOWN


def field(event: dict[str, Any] | None, *names: str) -> Any:
    if not event:
        return UNKNOWN
    for name in names:
        if name in event:
            return clean_value(event.get(name))
    return UNKNOWN


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as src:
        return json.load(src)


def load_capture_jsonl(path: Path, warnings: list[str]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    if not path.exists():
        warnings.append(f"missing_capture:{path}")
        return events
    with path.open("r", encoding="utf-8") as src:
        for line_number, line in enumerate(src, 1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                warnings.append(f"bad_jsonl:{path}:{line_number}:{exc.msg}")
                continue
            events.append(event)
    events.sort(key=lambda event: int_value(event.get("capture_sequence")) or 0)
    return events


class StdJsonCache:
    def __init__(self, root: Path) -> None:
        self.root = root
        self._case_map: dict[str, Path] | None = None
        self._action_rows: dict[str, dict[int, dict[str, Any]]] = {}
        self._entry_records: dict[str, list[dict[str, Any]]] = {}
        self._missing: set[str] = set()

    def _build_case_map(self) -> dict[str, Path]:
        if self._case_map is None:
            self._case_map = {}
            if self.root.exists():
                for path in self.root.iterdir():
                    if path.is_file():
                        self._case_map[path.name.lower()] = path
        return self._case_map

    def _find(self, filename: str) -> Path | None:
        path = self.root / filename
        if path.exists():
            return path
        return self._build_case_map().get(filename.lower())

    def action_rows(self, stem: str, warnings: list[str]) -> dict[int, dict[str, Any]]:
        if stem in self._action_rows:
            return self._action_rows[stem]
        filename = f"{stem}.std.json"
        path = self._find(filename)
        if not path:
            self._missing.add(filename)
            warnings.append(f"std_json_missing:{filename}")
            self._action_rows[stem] = {}
            return {}
        data = load_json(path)
        rows = data.get("actionRows", {}).get("rows") or []
        indexed = {
            int(row["index"]): row
            for row in rows
            if isinstance(row, dict) and int_value(row.get("index")) is not None
        }
        self._action_rows[stem] = indexed
        return indexed

    def entry_records(self, stem: str, warnings: list[str]) -> list[dict[str, Any]]:
        if stem in self._entry_records:
            return self._entry_records[stem]
        filename = f"{stem.lower()}0.std.json"
        path = self._find(filename)
        if not path:
            self._missing.add(filename)
            warnings.append(f"std0_json_missing:{filename}")
            self._entry_records[stem] = []
            return []
        data = load_json(path)
        records = data.get("entryTable", {}).get("records") or []
        self._entry_records[stem] = [
            record for record in records if isinstance(record, dict) and not record.get("isSentinel")
        ]
        return self._entry_records[stem]


def decode_payload_bytes(record: dict[str, Any]) -> dict[str, int]:
    payload_hex = record.get("payloadBytesHex")
    if not isinstance(payload_hex, str):
        return {}
    try:
        data = bytes.fromhex(payload_hex)
    except ValueError:
        return {}

    def u16(offset: int) -> int | None:
        if offset + 2 > len(data):
            return None
        return int.from_bytes(data[offset : offset + 2], "big")

    def u32(offset: int) -> int | None:
        if offset + 4 > len(data):
            return None
        return int.from_bytes(data[offset : offset + 4], "big")

    decoded = {
        "payload_primary_key": u16(0x00),
        "payload_secondary_key": u16(0x02),
        "payload_variant": u16(0x04),
        "payload_low_flags": u16(0x06),
        "payload_flags": u32(0x10),
        "payload_scalar": u32(0x14),
        "payload_start_frame": u32(0x18),
        "payload_end_frame": u16(0x1C),
        "payload_hold": u16(0x1E),
        "payload_step": u16(0x20),
        "payload_mode_before": u16(0x22),
    }
    return {key: value for key, value in decoded.items() if value is not None}


def payload_matches(row: dict[str, Any], decoded: dict[str, int]) -> bool:
    compared = 0
    for key, expected in decoded.items():
        actual = int_value(row.get(key))
        if actual is None:
            continue
        compared += 1
        if actual != expected:
            return False
    return compared > 0


def resource_stem_for_slot(slot_value: Any) -> str:
    slot = int_value(slot_value)
    if slot is None:
        return UNKNOWN
    return SLOT_RESOURCE_STEMS.get(slot, UNKNOWN)


def companion_std0_filename(stem: str) -> str:
    if stem == UNKNOWN:
        return UNKNOWN
    return f"{stem.lower()}0.std"


def parse_planned_commands(manifest: dict[str, Any]) -> str:
    commands: list[str] = []
    for event in manifest.get("events") or []:
        marker = " text="
        if not isinstance(event, str) or marker not in event:
            continue
        text = event.split(marker, 1)[1]
        if "Attack" in text or "Defend" in text or "Guard" in text:
            commands.append(text.replace("\r\n", "\\n").replace("\n", "\\n"))
            break
    return "; ".join(commands) if commands else UNKNOWN


def prediction_path_for_run(run_root: Path, manifest: dict[str, Any], provided: dict[int, Path]) -> str:
    exec_id = int_value(manifest.get("original_exec_job_id"))
    if exec_id is not None and exec_id in provided:
        return str(provided[exec_id])
    if exec_id is None:
        return UNKNOWN
    candidate = run_root.parent / f"predict_j{exec_id}.json"
    return str(candidate) if candidate.exists() else UNKNOWN


def match_event(
    candidates: Iterable[dict[str, Any]],
    start_seq: int,
    end_seq: int | None,
    warnings: list[str],
    warning_name: str,
    *,
    after_seq: int | None = None,
    payload_ptr: Any = UNKNOWN,
    active_slot: Any = UNKNOWN,
    target_slot: Any = UNKNOWN,
    actor_field6: Any = UNKNOWN,
) -> dict[str, Any] | None:
    matches: list[dict[str, Any]] = []
    lower_bound = after_seq if after_seq is not None else start_seq
    for event in candidates:
        seq = int_value(event.get("capture_sequence"))
        if seq is None or seq < lower_bound:
            continue
        if end_seq is not None and seq >= end_seq:
            continue
        if payload_ptr != UNKNOWN:
            event_ptr = first_known(
                event.get("worksheet_payload_ptr_0x178"),
                event.get("worksheet_payload_ptr_addrprog"),
                event.get("mode0_entry_payload_ptr_addrprog"),
                event.get("mode0e_payload_ptr_addrprog"),
                event.get("r3_payload_or_return"),
            )
            if not same_known(payload_ptr, event_ptr):
                continue
        if active_slot != UNKNOWN and "active_slot" in event and not same_known(active_slot, event.get("active_slot")):
            continue
        if target_slot != UNKNOWN and "target_slot" in event and not same_known(target_slot, event.get("target_slot")):
            continue
        if actor_field6 != UNKNOWN:
            event_field6 = first_known(event.get("actor_field6_0x6"), event.get("actor_field6_addrprog"))
            if event_field6 != UNKNOWN and not same_known(actor_field6, event_field6):
                continue
        matches.append(event)

    if not matches:
        return None
    matches.sort(key=lambda event: int_value(event.get("capture_sequence")) or 0)
    if len(matches) > 1:
        warnings.append(f"{warning_name}:{len(matches)}")
    return matches[0]


def pair_query_result(
    call: dict[str, Any],
    result_events: list[dict[str, Any]],
    end_seq: int | None,
    warnings: list[str],
) -> dict[str, Any] | None:
    call_seq = int_value(call.get("capture_sequence")) or 0
    matches = []
    for event in result_events:
        seq = int_value(event.get("capture_sequence"))
        if seq is None or seq < call_seq:
            continue
        if end_seq is not None and seq >= end_seq:
            continue
        if not same_known(call.get("active_slot"), event.get("active_slot")):
            continue
        if not same_known(call.get("target_slot"), event.get("target_slot")):
            continue
        if not same_known(call.get("r29_action_view_state"), event.get("r29_action_view_state")):
            continue
        if not same_known(call.get("r30_action_view_payload"), event.get("r30_action_view_payload")):
            continue
        matches.append(event)

    if not matches:
        return None
    matches.sort(key=lambda event: int_value(event.get("capture_sequence")) or 0)
    if len(matches) > 1:
        warnings.append(f"ambiguous_query_result:{len(matches)}")
    return matches[0]


def add_std_enrichment(row: dict[str, Any], std_cache: StdJsonCache, warnings: list[str]) -> None:
    stem = row["resource_stem"]
    if stem == UNKNOWN:
        warnings.append("unknown_resource_stem")
        return

    row["std_filename"] = f"{stem}.std"
    row["std0_filename"] = companion_std0_filename(stem)

    rows = std_cache.action_rows(stem, warnings)
    action_row: dict[str, Any] | None = None
    action_id = int_value(first_known(row["actor_field6"], row["query_arg0"]))
    if action_id is not None:
        candidates = [
            candidate
            for candidate in rows.values()
            if int_value(candidate.get("actionId")) == action_id
        ]
        secondary_key = signed32_value(row["query_arg1"])
        if secondary_key is not None and len(candidates) > 1:
            keyed_candidates = [
                candidate
                for candidate in candidates
                if int_value(candidate.get("secondaryKey")) == secondary_key
            ]
            if keyed_candidates:
                candidates = keyed_candidates
        if candidates:
            candidates.sort(key=lambda candidate: int_value(candidate.get("index")) or 0)
            if len(candidates) > 1:
                warnings.append(f"std_action_ambiguous:{stem}:{action_id}:{len(candidates)}")
            action_row = candidates[0]

    row_index = int_value(row["std_row_index"])
    if action_row is None and row_index is not None:
        action_row = rows.get(row_index)
        if not action_row:
            warnings.append(f"std_row_missing:{stem}:{row_index}")
    if action_row is None:
        row["std_row_index"] = UNKNOWN
        if action_id is not None:
            warnings.append(f"std_action_missing:{stem}:{action_id}")
    else:
        row["std_row_index"] = clean_value(action_row.get("index"))
        row["std_action_id"] = clean_value(action_row.get("actionId"))
        row["std_row_type"] = clean_value(action_row.get("rowType"))
        row["std_callback_index"] = clean_value(action_row.get("callbackIndex"))
        row["std_callback_ordinal"] = clean_value(action_row.get("callbackOrdinal"))
        row["std_flags_hex"] = clean_value(action_row.get("flagsHex"))
        row["std_secondary_key"] = clean_value(action_row.get("secondaryKey"))
        row["std_callback_aux_param"] = clean_value(action_row.get("callbackAuxParam"))
        row["std_transition_gate_divisor_hex"] = clean_value(
            action_row.get("transitionGateDivisorHex")
        )
        row["std_motion_progress_step_hex"] = clean_value(
            action_row.get("motionProgressStepHex")
        )

    query_key = row["query_key_combined"]
    records = std_cache.entry_records(stem, warnings)
    if not records or query_key == UNKNOWN:
        return

    candidates = [
        record
        for record in records
        if str(record.get("combinedTypeHex", "")).lower() == str(query_key).lower()
    ]
    if not candidates:
        warnings.append(f"std0_no_key_match:{stem}:{query_key}")
        return

    decoded_candidates = [
        (record, decode_payload_bytes(record)) for record in candidates
    ]
    exact_matches = [
        (record, decoded)
        for record, decoded in decoded_candidates
        if payload_matches(row, decoded)
    ]
    selected: tuple[dict[str, Any], dict[str, int]]
    if exact_matches:
        if len(exact_matches) > 1:
            warnings.append(f"std0_ambiguous_payload_match:{len(exact_matches)}")
        selected = exact_matches[0]
        row["std0_payload_match"] = "exact_payload"
    else:
        if len(candidates) > 1:
            warnings.append(f"std0_ambiguous_key_match:{len(candidates)}")
        else:
            warnings.append("std0_no_payload_match")
        selected = decoded_candidates[0]
        row["std0_payload_match"] = "key_only"

    record, _decoded = selected
    row["std0_entry_index"] = clean_value(record.get("index"))
    row["std0_combined_type_hex"] = clean_value(record.get("combinedTypeHex"))
    row["std0_payload_size"] = clean_value(record.get("payloadSize"))
    row["std0_payload_offset"] = clean_value(record.get("payloadOffsetOrPtrHex"))


def classify(row: dict[str, Any], spawn: dict[str, Any] | None, fallback: dict[str, Any] | None, mode0e: dict[str, Any] | None, warnings: list[str]) -> str:
    query_result = int_value(row["query_result"])
    has_payload = row["payload_primary_key"] != UNKNOWN or row["payload_ptr"] != UNKNOWN
    if query_result is not None and query_result != 0 and has_payload:
        return "serialized_0x0003002a"
    if query_result == 0 and (spawn is not None or mode0e is not None):
        return "synthetic_mode0e_spawn"
    if fallback is not None:
        return "mode0_payload_observed"
    warnings.append("unknown_payload_classification")
    return UNKNOWN


def reduce_run(
    run_root: Path,
    std_cache: StdJsonCache,
    prediction_by_exec_id: dict[int, Path],
) -> tuple[list[dict[str, Any]], list[str]]:
    run_warnings: list[str] = []
    manifest_path = run_root / "manifest.json"
    if not manifest_path.exists():
        return [], [f"missing_manifest:{run_root}"]
    manifest = load_json(manifest_path)
    capture_path = run_root / "capture.jsonl"
    events = load_capture_jsonl(capture_path, run_warnings)

    call_events = [event for event in events if checkpoint_is(event, "action_view_category2_query_call")]
    result_events = [event for event in events if checkpoint_is(event, "action_view_category2_query_result")]
    spawn_events = [event for event in events if checkpoint_is(event, "action_view_category2_spawn")]
    fallback_events = [event for event in events if checkpoint_is(event, "mode0_action_view_camera_fallback")]
    handler_events = [event for event in events if checkpoint_is(event, "mode0_action_view_handler_entry")]
    mode0e_events = [event for event in events if checkpoint_is(event, "mode0e_action_view_camera")]

    rows: list[dict[str, Any]] = []
    planned_commands = parse_planned_commands(manifest)
    prediction_source = prediction_path_for_run(run_root, manifest, prediction_by_exec_id)

    for index, call in enumerate(call_events):
        warnings = list(run_warnings)
        call_seq = int_value(call.get("capture_sequence")) or 0
        next_call_seq = (
            int_value(call_events[index + 1].get("capture_sequence"))
            if index + 1 < len(call_events)
            else None
        )
        result = pair_query_result(call, result_events, next_call_seq, warnings)
        after_result_seq = int_value(result.get("capture_sequence")) if result else call_seq
        active_slot = first_known(call.get("active_slot"), field(result, "active_slot"))
        target_slot = first_known(call.get("target_slot"), field(result, "target_slot"))
        actor_field6 = first_known(
            call.get("actor_field6_0x6"),
            call.get("actor_field6_addrprog"),
            field(result, "actor_field6_0x6", "actor_field6_addrprog"),
        )

        fallback = match_event(
            fallback_events,
            call_seq,
            next_call_seq,
            warnings,
            "ambiguous_mode0_fallback",
            after_seq=after_result_seq,
        )
        payload_ptr = first_known(
            field(fallback, "worksheet_payload_ptr_0x178", "worksheet_payload_ptr_addrprog", "r3_payload_or_return"),
            call.get("r30_action_view_payload"),
            field(result, "r30_action_view_payload"),
        )
        handler = match_event(
            handler_events,
            call_seq,
            next_call_seq,
            warnings,
            "ambiguous_mode0_handler",
            after_seq=int_value(fallback.get("capture_sequence")) if fallback else after_result_seq,
            payload_ptr=payload_ptr,
        )
        mode0e = match_event(
            mode0e_events,
            call_seq,
            next_call_seq,
            warnings,
            "ambiguous_mode0e_camera",
            after_seq=after_result_seq,
            payload_ptr=payload_ptr,
        )
        spawn = match_event(
            spawn_events,
            call_seq,
            next_call_seq,
            warnings,
            "ambiguous_spawn",
            after_seq=after_result_seq,
            active_slot=active_slot,
            target_slot=target_slot,
            actor_field6=actor_field6,
        )

        row = unknown_row()
        row.update(
            {
                "run_root": str(run_root),
                "source_exec_job_id": clean_value(manifest.get("original_exec_job_id")),
                "source_turn_job_id": clean_value(manifest.get("original_turn_job_id")),
                "clone_exec_job_id": clean_value(manifest.get("cloned_exec_job_id")),
                "clone_turn_job_id": clean_value(manifest.get("cloned_turn_job_id")),
                "original_job_set_id": clean_value(manifest.get("original_job_set_id")),
                "battle_set_id": clean_value(manifest.get("battle_set_id")),
                "wave_id": clean_value(manifest.get("wave_id")),
                "turn_index": clean_value(manifest.get("turn_index")),
                "fake_attacks_this_turn": clean_value(manifest.get("fake_attacks_this_turn")),
                "planned_commands": planned_commands,
                "prediction_source": prediction_source,
                "capture_sequence": clean_value(call.get("capture_sequence")),
                "checkpoint_name": clean_value(call.get("checkpoint_name")),
                "checkpoint": clean_value(call.get("checkpoint")),
                "pc": clean_value(call.get("pc")),
                "function": clean_value(call.get("function")),
                "movie_input_count": clean_value(call.get("movie_input_count")),
                "vi_field_count": clean_value(call.get("vi_field_count")),
                "frame_count": clean_value(call.get("frame_count")),
                "rng_draw_index_before": clean_value(call.get("rng_draw_index_before")),
                "rng_seed_before": clean_value(call.get("rng_seed_before")),
                "owns_rng_draw": clean_value(call.get("owns_rng_draw")),
                "active_slot": clean_value(active_slot),
                "target_slot": clean_value(target_slot),
                "resource_slot": clean_value(active_slot),
                "resource_stem": resource_stem_for_slot(active_slot),
                "actor_field6": clean_value(actor_field6),
                "actor_subtype": first_known(
                    call.get("actor_subtype_0x8"),
                    call.get("actor_subtype_addrprog"),
                    field(result, "actor_subtype_0x8", "actor_subtype_addrprog"),
                ),
                "instruction_flags": first_known(
                    call.get("instruction_flags_0xf0"),
                    field(result, "instruction_flags_0xf0"),
                    field(fallback, "instruction_flags_0xf0"),
                ),
                "gate_category": first_known(call.get("gate_category_0x2f"), field(result, "gate_category_0x2f")),
                "gate_state": first_known(call.get("gate_state_0x30"), field(result, "gate_state_0x30")),
                "gate_active_slot": first_known(call.get("gate_active_slot_0x02"), field(result, "gate_active_slot_0x02")),
                "gate_target_slot": first_known(call.get("gate_target_slot_0x04"), field(result, "gate_target_slot_0x04")),
                "aux_list_root": clean_value(call.get("aux_list_root")),
                "query_arg0": clean_value(call.get("query_arg0")),
                "query_arg1": clean_value(call.get("query_arg1")),
                "query_arg2": clean_value(call.get("query_arg2")),
                "query_arg3": clean_value(call.get("query_arg3")),
                "query_result": field(result, "query_result"),
                "query_call_sequence": clean_value(call.get("capture_sequence")),
                "query_result_sequence": evidence_seq(result),
                "spawn_sequence": evidence_seq(spawn),
                "mode0_fallback_sequence": evidence_seq(fallback),
                "mode0_handler_sequence": evidence_seq(handler),
                "mode0e_camera_sequence": evidence_seq(mode0e),
                "payload_ptr": payload_ptr,
                "payload_primary_key": field(fallback, "payload_primary_0x00"),
                "payload_secondary_key": field(fallback, "payload_secondary_0x02"),
                "payload_variant": field(fallback, "payload_variant_0x04"),
                "payload_low_flags": field(fallback, "payload_low_flags_0x06"),
                "payload_flags": field(fallback, "payload_flags_0x10"),
                "payload_scalar": field(fallback, "payload_scalar_0x14"),
                "payload_start_frame": field(fallback, "payload_start_frame_0x18"),
                "payload_end_frame": field(fallback, "payload_end_frame_0x1c"),
                "payload_hold": field(fallback, "payload_hold_0x1e"),
                "payload_step": field(fallback, "payload_step_0x20"),
                "payload_mode_before": field(fallback, "payload_mode_0x22"),
                "payload_mode_direct": first_known(
                    field(fallback, "payload_mode_direct_addrprog"),
                    field(mode0e, "mode0e_payload_mode_direct_addrprog"),
                ),
                "payload_mode_chased": first_known(
                    field(fallback, "payload_mode_chased_addrprog"),
                    field(mode0e, "mode0e_payload_mode_chased_addrprog"),
                ),
                "payload_mode_effective": first_known(
                    field(handler, "mode0_entry_effective_mode_addrprog"),
                    field(mode0e, "mode0e_effective_mode_addrprog"),
                ),
                "worksheet_saved_mode": first_known(
                    field(fallback, "worksheet_saved_mode_0x110"),
                    field(handler, "mode0_entry_saved_mode_addrprog"),
                ),
                "worksheet_effective_mode": first_known(
                    field(fallback, "worksheet_effective_mode_0x112"),
                    field(handler, "mode0_entry_effective_mode_addrprog"),
                    field(mode0e, "mode0e_effective_mode_addrprog"),
                ),
                "worksheet_action_id": first_known(
                    field(fallback, "worksheet_action_id_0x6", "worksheet_action_id_addrprog"),
                    field(mode0e, "mode0e_action_id_addrprog"),
                ),
                "worksheet_handler": first_known(
                    field(fallback, "worksheet_handler_0xe0", "worksheet_handler_addrprog"),
                    field(mode0e, "mode0e_handler_addrprog"),
                ),
                "worksheet_selected_row": clean_value(field(fallback, "worksheet_selected_row_0xe4")),
            }
        )

        query_arg2 = int_value(row["query_arg2"])
        query_arg3 = int_value(row["query_arg3"])
        if query_arg2 is not None and query_arg3 is not None:
            row["query_key_combined"] = hex32((query_arg3 << 16) | query_arg2)
        else:
            warnings.append("query_key_unavailable")

        row["std_row_index"] = clean_value(row["worksheet_selected_row"])
        add_std_enrichment(row, std_cache, warnings)
        row["payload_classification"] = classify(row, spawn, fallback, mode0e, warnings)
        seqs = {
            "query_call": row["query_call_sequence"],
            "query_result": row["query_result_sequence"],
            "spawn": row["spawn_sequence"],
            "mode0_fallback": row["mode0_fallback_sequence"],
            "mode0_handler": row["mode0_handler_sequence"],
            "mode0e_camera": row["mode0e_camera_sequence"],
        }
        row["evidence_sequences"] = ";".join(
            f"{key}={value}" for key, value in seqs.items() if value != UNKNOWN
        ) or UNKNOWN
        row["warnings"] = ";".join(dict.fromkeys(warnings)) if warnings else UNKNOWN
        rows.append(row)

    return rows, run_warnings


def parse_prediction_args(paths: list[Path]) -> dict[int, Path]:
    by_exec_id: dict[int, Path] = {}
    for path in paths:
        if not path.exists():
            continue
        try:
            data = load_json(path)
        except json.JSONDecodeError:
            continue
        input_data = data.get("input", {}) if isinstance(data, dict) else {}
        exec_id = int_value(input_data.get("requested_exec_job_id"))
        if exec_id is None:
            exec_id = int_value(input_data.get("exec_job_id"))
        if exec_id is not None:
            by_exec_id[exec_id] = path
    return by_exec_id


def default_run_roots() -> list[Path]:
    if not DEFAULT_SUPPORT_DIR.exists():
        return []
    return sorted(
        path for path in DEFAULT_SUPPORT_DIR.glob("r2_j*") if path.is_dir()
    )


def write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as out:
        for row in rows:
            out.write(json.dumps(row, ensure_ascii=False, sort_keys=False))
            out.write("\n")


def write_tsv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=OUTPUT_FIELDS, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_summary(path: Path, rows: list[dict[str, Any]], run_roots: list[Path], run_warnings: dict[str, list[str]]) -> None:
    by_job = Counter(row["source_exec_job_id"] for row in rows)
    by_classification = Counter(row["payload_classification"] for row in rows)
    by_resource = Counter(row["resource_stem"] for row in rows)
    warning_counts: Counter[str] = Counter()
    for row in rows:
        warnings = row.get("warnings")
        if warnings and warnings != UNKNOWN:
            for warning in str(warnings).split(";"):
                warning_counts[warning] += 1
    enriched_rows = sum(1 for row in rows if row["std_action_id"] != UNKNOWN)
    exact_std0_rows = sum(1 for row in rows if row["std0_payload_match"] == "exact_payload")

    lines = [
        "# STD Runtime Reduced Summary",
        "",
        f"- run roots: {len(run_roots)}",
        f"- reduced rows: {len(rows)}",
        f"- rows with STD action-row enrichment: {enriched_rows}",
        f"- rows with exact STD0 payload match: {exact_std0_rows}",
        "",
        "## Rows By Source Exec Job",
        "",
    ]
    for key in sorted(by_job, key=lambda item: str(item)):
        lines.append(f"- {key}: {by_job[key]}")
    lines.extend(["", "## Payload Classifications", ""])
    for key, count in by_classification.most_common():
        lines.append(f"- {key}: {count}")
    lines.extend(["", "## Resource Stems", ""])
    for key, count in by_resource.most_common():
        lines.append(f"- {key}: {count}")
    lines.extend(["", "## Warnings", ""])
    if warning_counts:
        for key, count in warning_counts.most_common():
            lines.append(f"- {key}: {count}")
    else:
        lines.append("- none")

    root_warning_lines = [
        f"- {run_root}: {';'.join(warnings)}"
        for run_root, warnings in run_warnings.items()
        if warnings
    ]
    if root_warning_lines:
        lines.extend(["", "## Run-Level Warnings", ""])
        lines.extend(root_warning_lines)

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Reduce STD runtime action-view capture JSONL into stable evidence packets."
    )
    parser.add_argument(
        "--run-root",
        action="append",
        type=Path,
        default=[],
        help="Run root containing manifest.json and capture.jsonl. May be repeated.",
    )
    parser.add_argument(
        "--std-json-dir",
        type=Path,
        default=DEFAULT_STD_JSON_DIR,
        help="Directory containing SPICE std_json exports.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help="Output directory for reduced JSONL, TSV, and summary.",
    )
    parser.add_argument(
        "--prediction-json",
        action="append",
        type=Path,
        default=[],
        help="Optional prediction JSON to associate with matching exec IDs. May be repeated.",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    run_roots = args.run_root or default_run_roots()
    if not run_roots:
        raise SystemExit("No run roots found. Pass --run-root or restore the June 22 support runs.")

    std_cache = StdJsonCache(args.std_json_dir)
    prediction_by_exec_id = parse_prediction_args(args.prediction_json)

    all_rows: list[dict[str, Any]] = []
    run_warnings: dict[str, list[str]] = defaultdict(list)
    for run_root in run_roots:
        rows, warnings = reduce_run(run_root, std_cache, prediction_by_exec_id)
        all_rows.extend(rows)
        if warnings:
            run_warnings[str(run_root)].extend(warnings)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    jsonl_path = args.out_dir / "std_runtime_reduced.jsonl"
    tsv_path = args.out_dir / "std_runtime_reduced.tsv"
    summary_path = args.out_dir / "std_runtime_reduced_summary.md"
    write_jsonl(jsonl_path, all_rows)
    write_tsv(tsv_path, all_rows)
    write_summary(summary_path, all_rows, run_roots, run_warnings)

    print(f"reduced_rows={len(all_rows)}")
    print(f"jsonl={jsonl_path}")
    print(f"tsv={tsv_path}")
    print(f"summary={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
