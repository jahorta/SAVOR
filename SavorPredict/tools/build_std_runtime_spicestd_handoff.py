#!/usr/bin/env python3
"""Assemble the SpiceStd STD runtime capture handoff folder."""

from __future__ import annotations

import csv
import json
import shutil
import sqlite3
from collections import Counter
from pathlib import Path
from typing import Any


UNKNOWN = "unknown"
REPO_ROOT = Path(__file__).resolve().parents[2]
SUPPORT_DIR = REPO_ROOT / "Analyses" / "std_runtime" / "std_runtime_capture_response_2026-06-22_support"
HANDOFF_DIR = REPO_ROOT / "Analyses" / "std_runtime" / "std_runtime_spicestd_handoff_2026-07-09"
STD_JSON_DIR = Path(r"D:\SavorPredictDB\.std_json")
AUTHORING_DB = Path(r"D:\SavorPredictDB\authoring.db")
FIELD6_RUN = Path(r"C:\savor\field6_dynamic_iw_watch_158364_20260708_2")
REWRITE_RUN = Path(r"C:\savor\std_runtime_mode_rewrite_watch_158364_20260709_1")

PACKET_FIELDS = [
    "record_kind",
    "job_id",
    "source_job_id",
    "turn_job_id",
    "source_turn_job_id",
    "action_sequence_id",
    "checkpoint",
    "pc",
    "function",
    "rng_draw_index_before",
    "rng_seed_before",
    "actor_slot",
    "target_slot",
    "resource_stem",
    "resource_path",
    "instruction",
    "instr_param",
    "field6_before",
    "field6_source",
    "field6_after",
    "row_index",
    "row_action_id",
    "row_type",
    "row_callback_index",
    "row_callback_ordinal",
    "row_flags_hex",
    "row_secondary_key",
    "row_callback_aux_param",
    "callback_pc",
    "aux_root",
    "query_key",
    "query_result",
    "payload_ptr",
    "payload_primary_key",
    "payload_secondary_key",
    "payload_variant",
    "payload_flags",
    "payload_start_frame",
    "payload_end_frame",
    "payload_hold",
    "payload_step",
    "payload_mode",
    "payload_mode_effective",
    "mode_rewrite",
    "draw_owner",
    "payload_classification",
    "evidence_sequences",
    "raw_capture",
    "notes",
]


def int_value(value: Any) -> int | None:
    if value is None or value == UNKNOWN:
        return None
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


def clean(value: Any) -> str:
    if value is None or value == "":
        return UNKNOWN
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def first_known(*values: Any) -> str:
    for value in values:
        value = clean(value)
        if value != UNKNOWN:
            return value
    return UNKNOWN


def first_nonzero(*values: Any) -> str:
    for value in values:
        value = clean(value)
        if value != UNKNOWN and value != "0x00000000":
            return value
    return UNKNOWN


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as src:
        for line in src:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def unknown_packet(record_kind: str) -> dict[str, str]:
    row = {field: UNKNOWN for field in PACKET_FIELDS}
    row["record_kind"] = record_kind
    return row


def resource_stem_for_slot(value: Any) -> str:
    slot = int_value(value)
    if slot == 0:
        return "ma000"
    if slot == 1:
        return "MA001"
    if slot in (4, 5):
        return "MB000"
    return UNKNOWN


def std_source_path(stem: str) -> str:
    if stem == UNKNOWN:
        return UNKNOWN
    path = STD_JSON_DIR / f"{stem}.std.json"
    if not path.exists():
        return UNKNOWN
    data = load_json(path)
    return clean(data.get("source"))


def static_row_by_action(stem: str, action_id: Any) -> dict[str, Any] | None:
    action = int_value(action_id)
    if stem == UNKNOWN or action is None:
        return None
    path = STD_JSON_DIR / f"{stem}.std.json"
    if not path.exists():
        path = next((p for p in STD_JSON_DIR.glob("*.std.json") if p.name.lower() == f"{stem}.std.json".lower()), path)
    if not path.exists():
        return None
    data = load_json(path)
    rows = data.get("actionRows", {}).get("rows") or []
    matches = [row for row in rows if int_value(row.get("actionId")) == action]
    if not matches:
        return None
    return sorted(matches, key=lambda row: int_value(row.get("index")) or 0)[0]


def mode_rewrite_from_reduced(row: dict[str, Any]) -> str:
    classification = clean(row.get("payload_classification"))
    before = int_value(row.get("payload_mode_before"))
    effective = int_value(first_known(row.get("payload_mode_effective"), row.get("worksheet_effective_mode")))
    if classification == "synthetic_mode0e_spawn":
        return "synthetic_mode0e_spawn"
    if before == 0 and effective == 0x0E:
        return "mode0_to_mode0e_effective_mode_observed"
    if before == 0 and clean(row.get("mode0e_camera_sequence")) != UNKNOWN:
        return "mode0e_camera_reached_after_mode0_payload"
    if before == 0:
        return "mode0_payload_observed_no_mode0e_in_pair"
    return UNKNOWN


def packet_from_reduced(row: dict[str, Any]) -> dict[str, str]:
    packet = unknown_packet("action_view_packet")
    resource_stem = clean(row.get("resource_stem"))
    packet.update(
        {
            "job_id": clean(row.get("clone_exec_job_id")),
            "source_job_id": clean(row.get("source_exec_job_id")),
            "turn_job_id": clean(row.get("clone_turn_job_id")),
            "source_turn_job_id": clean(row.get("source_turn_job_id")),
            "checkpoint": "action_view_category2_query_packet",
            "pc": clean(row.get("pc")),
            "function": clean(row.get("function")),
            "rng_draw_index_before": clean(row.get("rng_draw_index_before")),
            "rng_seed_before": clean(row.get("rng_seed_before")),
            "actor_slot": clean(row.get("active_slot")),
            "target_slot": clean(row.get("target_slot")),
            "resource_stem": resource_stem,
            "resource_path": std_source_path(resource_stem),
            "instruction": clean(first_known(row.get("std_action_id"), row.get("query_arg0"))),
            "instr_param": UNKNOWN,
            "field6_before": UNKNOWN,
            "field6_source": "action_view_actor_field6",
            "field6_after": clean(row.get("actor_field6")),
            "row_index": clean(row.get("std_row_index")),
            "row_action_id": clean(row.get("std_action_id")),
            "row_type": clean(row.get("std_row_type")),
            "row_callback_index": clean(row.get("std_callback_index")),
            "row_callback_ordinal": clean(row.get("std_callback_ordinal")),
            "row_flags_hex": clean(row.get("std_flags_hex")),
            "row_secondary_key": clean(row.get("std_secondary_key")),
            "row_callback_aux_param": clean(row.get("std_callback_aux_param")),
            "callback_pc": clean(first_nonzero(row.get("worksheet_handler"))),
            "aux_root": clean(row.get("aux_list_root")),
            "query_key": clean(row.get("query_key_combined")),
            "query_result": clean(row.get("query_result")),
            "payload_ptr": clean(row.get("payload_ptr")),
            "payload_primary_key": clean(row.get("payload_primary_key")),
            "payload_secondary_key": clean(row.get("payload_secondary_key")),
            "payload_variant": clean(row.get("payload_variant")),
            "payload_flags": clean(row.get("payload_flags")),
            "payload_start_frame": clean(row.get("payload_start_frame")),
            "payload_end_frame": clean(row.get("payload_end_frame")),
            "payload_hold": clean(row.get("payload_hold")),
            "payload_step": clean(row.get("payload_step")),
            "payload_mode": clean(row.get("payload_mode_before")),
            "payload_mode_effective": clean(first_known(row.get("payload_mode_effective"), row.get("worksheet_effective_mode"))),
            "mode_rewrite": mode_rewrite_from_reduced(row),
            "draw_owner": clean(
                first_known(
                    "mode0_action_view_camera_fallback" if clean(row.get("mode0_fallback_sequence")) != UNKNOWN else UNKNOWN,
                    "mode0e_action_view_camera" if clean(row.get("mode0e_camera_sequence")) != UNKNOWN else UNKNOWN,
                )
            ),
            "payload_classification": clean(row.get("payload_classification")),
            "evidence_sequences": clean(row.get("evidence_sequences")),
            "raw_capture": str(Path(row.get("run_root", UNKNOWN)) / "capture.jsonl"),
            "notes": clean(row.get("warnings")),
        }
    )
    return packet


def infer_packed_slot(event: dict[str, Any]) -> tuple[str, str, str, str]:
    actor_iw = clean(event.get("actor_instruction_r28"))
    for prefix, slot in (("p0", "0"), ("p1", "1"), ("p2", "4"), ("p3", "5")):
        if actor_iw != UNKNOWN and clean(event.get(f"{prefix}_instruction_ptr")) == actor_iw:
            return (
                slot,
                clean(event.get(f"{prefix}_field6_mode")),
                clean(event.get(f"{prefix}_fielde4_row")),
                clean(event.get(f"{prefix}_fielde0_callback")),
            )
    return UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN


def key8_dynamic_packet() -> dict[str, str] | None:
    capture_path = FIELD6_RUN / "capture.jsonl"
    if not capture_path.exists():
        return None
    events = load_jsonl(capture_path)
    manifests = load_json(FIELD6_RUN / "manifest.json")
    calls = [event for event in events if clean(event.get("checkpoint_name")) == "action_view_category2_query_call"]
    results = [event for event in events if clean(event.get("checkpoint_name")) == "action_view_category2_query_result"]
    for call in calls:
        slot, field6, row_index, callback = infer_packed_slot(call)
        if int_value(field6) != 8:
            continue
        call_seq = int_value(call.get("capture_sequence")) or 0
        result = next(
            (
                event
                for event in results
                if (int_value(event.get("capture_sequence")) or 0) > call_seq
                and clean(event.get("actor_instruction_r28")) == clean(call.get("actor_instruction_r28"))
            ),
            None,
        )
        stem = resource_stem_for_slot(slot)
        row = static_row_by_action(stem, field6)
        packet = unknown_packet("key8_dynamic_field6_packet")
        packet.update(
            {
                "job_id": clean(manifests.get("cloned_exec_job_id")),
                "source_job_id": clean(manifests.get("original_exec_job_id")),
                "turn_job_id": clean(manifests.get("cloned_turn_job_id")),
                "source_turn_job_id": clean(manifests.get("original_turn_job_id")),
                "checkpoint": "action_view_category2_query_result",
                "pc": clean(result.get("pc") if result else call.get("pc")),
                "function": clean(call.get("function")),
                "rng_draw_index_before": clean(call.get("rng_draw_index_before")),
                "rng_seed_before": clean(call.get("rng_seed_before")),
                "actor_slot": slot,
                "target_slot": UNKNOWN,
                "resource_stem": stem,
                "resource_path": std_source_path(stem),
                "instruction": clean(field6),
                "field6_before": UNKNOWN,
                "field6_source": "PTR_ARRAY_80309e24_instruction_worksheet",
                "field6_after": clean(field6),
                "row_index": clean(row_index),
                "row_action_id": clean(row.get("actionId") if row else field6),
                "row_type": clean(row.get("rowType") if row else None),
                "row_callback_index": clean(row.get("callbackIndex") if row else None),
                "row_callback_ordinal": clean(row.get("callbackOrdinal") if row else None),
                "row_flags_hex": clean(row.get("flagsHex") if row else None),
                "row_secondary_key": clean(row.get("secondaryKey") if row else None),
                "row_callback_aux_param": clean(row.get("callbackAuxParam") if row else None),
                "callback_pc": clean(callback),
                "query_key": "0x0003002a",
                "query_result": clean(result.get("query_result_r3") if result else None),
                "payload_classification": "serialized_0x0003002a_key8",
                "evidence_sequences": f"query_call={call_seq};query_result={clean(result.get('capture_sequence') if result else None)}",
                "raw_capture": str(capture_path),
                "notes": "query args inferred from category-2 checkpoint; key8 live field6 and row observed from packed PTR_ARRAY_80309e24 samples",
            }
        )
        return packet
    return None


def rewrite_watch_packets() -> list[dict[str, str]]:
    capture_path = REWRITE_RUN / "capture.jsonl"
    if not capture_path.exists():
        return []
    manifest = load_json(REWRITE_RUN / "manifest.json")
    events = load_jsonl(capture_path)
    results = [event for event in events if clean(event.get("checkpoint_name")) == "action_view_category2_query_result"]
    mode0es = [event for event in events if clean(event.get("checkpoint_name")) == "mode0e_action_view_camera"]
    memchecks = [event for event in events if clean(event.get("stop_kind")) == "memcheck"]
    packets: list[dict[str, str]] = []
    for fallback in [event for event in events if clean(event.get("checkpoint_name")) == "mode0_action_view_camera_fallback"]:
        seq = int_value(fallback.get("capture_sequence")) or 0
        prior_result = next(
            (
                event
                for event in reversed(results)
                if (int_value(event.get("capture_sequence")) or 0) < seq
            ),
            None,
        )
        payload_ptr = clean(fallback.get("worksheet_payload_ptr_0x178"))
        next_mode0e = next(
            (
                event
                for event in mode0es
                if (int_value(event.get("capture_sequence")) or 0) > seq
                and clean(event.get("worksheet_payload_ptr_0x178")) == payload_ptr
            ),
            None,
        )
        slot = clean(prior_result.get("active_slot") if prior_result else None)
        stem = resource_stem_for_slot(slot)
        instruction = clean(fallback.get("payload_primary_0x00"))
        row = static_row_by_action(stem, instruction)
        packet = unknown_packet("mode_rewrite_watch_packet")
        rewrite_note = "payload_write_watchpoint_no_hit"
        if next_mode0e:
            rewrite_note = "mode0e_camera_reached_same_payload;payload_write_watchpoint_no_hit"
        if memchecks:
            rewrite_note = "payload_write_watchpoint_hit"
        packet.update(
            {
                "job_id": clean(manifest.get("cloned_exec_job_id")),
                "source_job_id": clean(manifest.get("original_exec_job_id")),
                "turn_job_id": clean(manifest.get("cloned_turn_job_id")),
                "source_turn_job_id": clean(manifest.get("original_turn_job_id")),
                "checkpoint": "mode0_action_view_camera_fallback",
                "pc": clean(fallback.get("pc")),
                "function": clean(fallback.get("function")),
                "rng_draw_index_before": clean(fallback.get("rng_draw_index_before")),
                "rng_seed_before": clean(fallback.get("rng_seed_before")),
                "actor_slot": slot,
                "target_slot": clean(prior_result.get("target_slot") if prior_result else None),
                "resource_stem": stem,
                "resource_path": std_source_path(stem),
                "instruction": instruction,
                "field6_source": "payload_primary_and_prior_query",
                "field6_after": instruction,
                "row_index": clean(row.get("index") if row else None),
                "row_action_id": clean(row.get("actionId") if row else None),
                "row_type": clean(row.get("rowType") if row else None),
                "row_callback_index": clean(row.get("callbackIndex") if row else None),
                "row_callback_ordinal": clean(row.get("callbackOrdinal") if row else None),
                "row_flags_hex": clean(row.get("flagsHex") if row else None),
                "row_secondary_key": clean(row.get("secondaryKey") if row else None),
                "row_callback_aux_param": clean(row.get("callbackAuxParam") if row else None),
                "callback_pc": clean(fallback.get("worksheet_handler_0xe0")),
                "query_key": "0x0003002a",
                "query_result": clean(prior_result.get("query_result") if prior_result else None),
                "payload_ptr": payload_ptr,
                "payload_primary_key": instruction,
                "payload_secondary_key": clean(fallback.get("payload_secondary_0x02")),
                "payload_variant": clean(fallback.get("payload_variant_0x04")),
                "payload_flags": clean(fallback.get("payload_flags_0x10")),
                "payload_start_frame": clean(fallback.get("payload_start_frame_0x18")),
                "payload_end_frame": clean(fallback.get("payload_end_frame_0x1c")),
                "payload_hold": clean(fallback.get("payload_hold_0x1e")),
                "payload_step": clean(fallback.get("payload_step_0x20")),
                "payload_mode": clean(fallback.get("payload_mode_0x22")),
                "payload_mode_effective": clean(
                    next_mode0e.get("worksheet_effective_mode_0x112") if next_mode0e else fallback.get("worksheet_effective_mode_0x112")
                ),
                "mode_rewrite": rewrite_note,
                "draw_owner": "mode0_action_view_camera_fallback",
                "payload_classification": "serialized_0x0003002a",
                "evidence_sequences": (
                    f"query_result={clean(prior_result.get('capture_sequence') if prior_result else None)};"
                    f"fallback={clean(fallback.get('capture_sequence'))};"
                    f"mode0e={clean(next_mode0e.get('capture_sequence') if next_mode0e else None)}"
                ),
                "raw_capture": str(capture_path),
                "notes": "dynamic watchpoint armed at 800513D4 r3+0x22; no memcheck hit was observed in this run",
            }
        )
        packets.append(packet)
    return packets


def write_jsonl(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as out:
        for row in rows:
            out.write(json.dumps(row, ensure_ascii=False))
            out.write("\n")


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=PACKET_FIELDS, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def write_scope_checks() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    if AUTHORING_DB.exists():
        with sqlite3.connect(AUTHORING_DB) as conn:
            conn.row_factory = sqlite3.Row
            for row in conn.execute(
                "select action_preset_id, name, macro, target_kind, item_id, target_mask_bits, "
                "target_single_slot, target_same_as_actor_slot, flags "
                "from au_battle_plan_action_preset order by action_preset_id"
            ):
                rows.append({key: clean(row[key]) for key in row.keys()})
    path = HANDOFF_DIR / "db_scope_action_presets.tsv"
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "action_preset_id",
        "name",
        "macro",
        "target_kind",
        "item_id",
        "target_mask_bits",
        "target_single_slot",
        "target_same_as_actor_slot",
        "flags",
    ]
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    return rows


def copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_raw_artifacts() -> None:
    copy_file(REPO_ROOT / "Analyses" / "std_runtime" / "std_runtime_capture_request_2026-06-22.md", HANDOFF_DIR / "source_request.md")
    copy_file(SUPPORT_DIR / "std_runtime_action_bridge_profile.ini", HANDOFF_DIR / "profiles" / "std_runtime_action_bridge_profile.ini")
    copy_file(SUPPORT_DIR / "reduced" / "std_runtime_reduced.jsonl", HANDOFF_DIR / "derived" / "std_runtime_reduced.jsonl")
    copy_file(SUPPORT_DIR / "reduced" / "std_runtime_reduced.tsv", HANDOFF_DIR / "derived" / "std_runtime_reduced.tsv")
    copy_file(SUPPORT_DIR / "reduced" / "std_runtime_reduced_summary.md", HANDOFF_DIR / "derived" / "std_runtime_reduced_summary.md")

    for run_root in sorted(path for path in SUPPORT_DIR.glob("r2_j*") if path.is_dir()):
        dest = HANDOFF_DIR / "raw" / run_root.name
        for name in ("manifest.json", "capture.jsonl", "trace_checkpoints.txt", "capture_profile.ini", "summary.txt"):
            copy_file(run_root / name, dest / name)

    for label, run_root in (
        ("field6_dynamic_iw_watch_158364_20260708_2", FIELD6_RUN),
        ("mode_rewrite_watch_158364_20260709_1", REWRITE_RUN),
    ):
        dest = HANDOFF_DIR / "raw" / label
        for name in ("manifest.json", "capture.jsonl", "trace_checkpoints.txt", "capture_profile.ini", "capture_profile_source.ini", "summary.txt"):
            copy_file(run_root / name, dest / name)


def read_trace_error_count(path: Path) -> str:
    if not path.exists():
        return UNKNOWN
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.strip().startswith("errors:"):
            return line.split(":", 1)[1].strip()
    return UNKNOWN


def write_readme(packet_rows: list[dict[str, str]], action_presets: list[dict[str, str]]) -> None:
    by_kind = Counter(row["record_kind"] for row in packet_rows)
    by_classification = Counter(row["payload_classification"] for row in packet_rows)
    query_nonzero = sum(1 for row in packet_rows if int_value(row["query_result"]) not in (None, 0))
    query_zero = sum(1 for row in packet_rows if int_value(row["query_result"]) == 0)
    trace_errors = {
        "r2_j147884": read_trace_error_count(SUPPORT_DIR / "r2_j147884" / "trace_checkpoints.txt"),
        "r2_j147896": read_trace_error_count(SUPPORT_DIR / "r2_j147896" / "trace_checkpoints.txt"),
        "r2_j148016": read_trace_error_count(SUPPORT_DIR / "r2_j148016" / "trace_checkpoints.txt"),
        "r2_j149113": read_trace_error_count(SUPPORT_DIR / "r2_j149113" / "trace_checkpoints.txt"),
        "r2_j158364": read_trace_error_count(SUPPORT_DIR / "r2_j158364" / "trace_checkpoints.txt"),
        "field6_dynamic_iw_watch_158364_20260708_2": read_trace_error_count(FIELD6_RUN / "trace_checkpoints.txt"),
        "mode_rewrite_watch_158364_20260709_1": read_trace_error_count(REWRITE_RUN / "trace_checkpoints.txt"),
    }
    trace_error_text = ", ".join(f"{name}={count}" for name, count in trace_errors.items())
    kind_text = ", ".join(f"{kind}={count}" for kind, count in by_kind.items())
    classification_text = ", ".join(f"{kind}={count}" for kind, count in by_classification.items())
    preset_text = ", ".join(row["name"] for row in action_presets) if action_presets else "unknown"

    readme = f"""# SpiceStd STD Runtime Capture Handoff

Date: 2026-07-09

This folder is the handoff packet for `std_runtime_capture_request_2026-06-22.md`.
It contains normalized STD runtime rows plus the raw capture JSONL, manifests,
trace reports, and profiles used to produce them.

## Files

- `std_runtime_capture_packet.jsonl`: grep-friendly normalized packet in the requested common-field shape.
- `std_runtime_capture_packet.tsv`: spreadsheet version of the same rows.
- `derived/`: reducer outputs from the five June 22 representative first-battle runs.
- `raw/`: copied raw `capture.jsonl`, `manifest.json`, `trace_checkpoints.txt`, and profile files.
- `profiles/`: capture profiles used for the main action-view pass and the focused mode-rewrite watch.
- `db_scope_action_presets.tsv`: authored action presets available in `D:/SavorPredictDB`.
- `source_request.md`: the original SpiceStd request.

## Coverage

- Packet rows: {len(packet_rows)} ({kind_text})
- Payload classifications: {classification_text}
- Query results in packet: nonzero={query_nonzero}, zero={query_zero}
- Trace-checkpoint errors: {trace_error_text}
- Authored action presets available in this DB: {preset_text}

## Acceptance Answers

1. PC and Soldier basic attacks: the packet confirms live action-view `field6_0x6=4` maps to STD row index `3` for `ma000`, `MA001`, and `MB000`. The older `8006778C` bridge checkpoint did not fire in the five June runs, so `FUN_8006721c` source/actor values are not claimed from that PC; the included July key-8 run closes the observed crit path through live worksheet samples and action-view query evidence.
2. `FUN_80009030(aux_list, 4, -1, 0x2a, 3)` returns nonzero for PC resources and zero for Soldier `MB000` rows in the five-job pass.
3. Nonzero queries select serialized `0x0003002a` payloads. Payload fields are present in the normalized rows. The focused mode-rewrite watch observed mode-0 fallback payloads and same-payload mode-0e dispatch for the key-8 case, but the dynamic payload `+0x22` write watchpoint did not hit, so this packet does not claim a direct payload-store rewrite.
4. Zero queries reach `FUN_80053f38(slot, 0)` and then mode-0e camera dispatch in the focused run; the normalized rows classify those as `synthetic_mode0e_spawn`.
5. The `0x18` / `0x1d` special secondary-key lookup path was not exercised. The source authoring DB only contains the attack presets listed in `db_scope_action_presets.tsv`, so this first-battle corpus does not provide magic/S-move/special commands for that path.
6. Instruction worksheet `+0x02` origin and dynamic `E67%03d%02d.MLD` filename construction were not exercised by this attack-only packet.
7. No `%s_STD` or `%s0_STD` file-format semantic rename is required from this evidence. The packet supports `%s_STD` as action rows and `%s0_STD` as action-view entry/payload tables; runtime dispatch/effective-mode behavior should remain separate from serialized payload mode naming.

## Notes

- `unknown` is used for unavailable fields instead of omitting columns.
- `job_id` and `turn_job_id` are the cloned sandbox IDs that produced the raw capture; `source_job_id` and `source_turn_job_id` preserve the original `D:/SavorPredictDB` IDs.
- Raw warning lines about missing `rng_seed_after` are expected for RNG callsite-entry checkpoints in these profiles.
"""
    (HANDOFF_DIR / "README.md").write_text(readme, encoding="utf-8")


def build_packet() -> list[dict[str, str]]:
    reduced_path = SUPPORT_DIR / "reduced" / "std_runtime_reduced.jsonl"
    rows = [packet_from_reduced(row) for row in load_jsonl(reduced_path)]
    key8_row = key8_dynamic_packet()
    if key8_row:
        rows.append(key8_row)
    rows.extend(rewrite_watch_packets())
    return rows


def main() -> int:
    HANDOFF_DIR.mkdir(parents=True, exist_ok=True)
    (HANDOFF_DIR / "derived").mkdir(parents=True, exist_ok=True)
    (HANDOFF_DIR / "raw").mkdir(parents=True, exist_ok=True)
    rows = build_packet()
    write_jsonl(HANDOFF_DIR / "std_runtime_capture_packet.jsonl", rows)
    write_tsv(HANDOFF_DIR / "std_runtime_capture_packet.tsv", rows)
    copy_raw_artifacts()
    action_presets = write_scope_checks()
    write_readme(rows, action_presets)
    print(f"handoff_dir={HANDOFF_DIR}")
    print(f"packet_rows={len(rows)}")
    print(f"jsonl={HANDOFF_DIR / 'std_runtime_capture_packet.jsonl'}")
    print(f"tsv={HANDOFF_DIR / 'std_runtime_capture_packet.tsv'}")
    print(f"readme={HANDOFF_DIR / 'README.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
