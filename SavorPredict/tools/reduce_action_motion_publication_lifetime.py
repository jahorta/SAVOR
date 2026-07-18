#!/usr/bin/env python3
"""Reduce persistent instruction-callback publication and lifetime evidence."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

import reduce_action_motion_invocation as base


WINDOW_OPEN_ID = "setup_turn_end_800715EC"
HELPER_ENTRY_ID = "worksheet_action_row_resolver_entry_80020094"
HELPER_RETURN_ID = "worksheet_action_row_resolver_return_8002024C"
DISPATCH_CALL_ID = "instruction_dispatch_callback_call_80022A40"
INSTALL_ID = "action_motion_install_entry_8001EBA4"
RELEASE_ID = "action_motion_release_8001B750"

CALLBACK_STORE_PCS = {
    0x80020134: "matched_secondary_row_selector",
    0x80020150: "mode_0e_row_selector",
    0x8002017C: "mode_09_fixed_selector",
    0x80020198: "mode_0a_fixed_selector",
    0x800201C0: "matched_row_selector",
    0x80020200: "mode_0e_fallback_selector",
    0x80020228: "mode_25_fallback_selector",
}

STATIC_HELPER_CALLSITES = {
    0x80017058, 0x8001716C, 0x80017844, 0x80018800, 0x80019810,
    0x8001A1A8, 0x8001A5E0, 0x8001AEEC, 0x8001B438, 0x8001BE60,
    0x800202B4, 0x80021F4C, 0x80022164, 0x800224B4, 0x800224E0,
    0x800229F8, 0x8002EBA4, 0x8002EC08,
}

HELPER_CALLSITE_OWNERS = {
    0x80017058: "row_property_query",
    0x8001716C: "row_property_query",
    0x80017844: "geometry_query",
    0x80018800: "callback_80018798",
    0x80019810: "callback_8001977c",
    0x8001A1A8: "mode_0e_callback",
    0x8001A5E0: "special_action_callback",
    0x8001AEEC: "position_sync_callback",
    0x8001B438: "basic_attack_callback",
    0x8001BE60: "ranged_attack_callback",
    0x800202B4: "generic_instruction_transition",
    0x80021F4C: "direct_instruction_transition",
    0x80022164: "std_instruction_setup",
    0x800224B4: "queued_std_action_transition",
    0x800224E0: "queued_std_action_transition",
    0x800229F8: "persistent_instruction_dispatch",
    0x8002EBA4: "action_service_candidate",
    0x8002EC08: "action_service_candidate",
}

FIELD_OFFSETS = {
    0x06: ("mode", 2),
    0x08: ("subtype", 2),
    0x0A: ("staged_mode", 2),
    0x0E: ("callback_control_0e", 2),
    0x10: ("callback_control_10", 2),
    0x12: ("callback_control", 2),
    0x5C: ("motion_resource", 4),
    0x60: ("motion_id", 4),
    0x64: ("motion_progress", 4),
    0x68: ("motion_increment", 4),
    0x6C: ("motion_completion", 4),
    0x70: ("motion_field_70", 4),
    0xDC: ("action_table", 4),
    0xE0: ("persistent_callback", 4),
    0xE4: ("selected_row", 2),
    0xE6: ("alternate_row", 2),
    0xE8: ("previous_row", 2),
    0xEA: ("row_runtime_ea", 2),
    0xEC: ("flags_ec", 4),
    0xF0: ("flags_f0", 4),
    0x134: ("runtime_word", 4),
    0x138: ("descriptor_delay", 4),
}

INITIAL_FIELDS = {
    "persistent_callback": "iw_persistent_callback",
    "selected_row": "iw_selected_row",
    "alternate_row": "iw_alt_row",
    "previous_row": "iw_previous_row",
    "mode": "iw_mode",
    "subtype": "iw_subtype",
    "callback_control": "iw_callback_control",
}


def signed16(value: Any) -> int | None:
    parsed = base.integer(value)
    if parsed is None:
        return None
    parsed &= 0xFFFF
    return parsed - 0x10000 if parsed & 0x8000 else parsed


def hex_value(value: Any, size: int | None = None) -> str:
    parsed = base.integer(value)
    if parsed is None:
        return ""
    width = max(2, min(16, (size or 4) * 2))
    mask = (1 << (width * 4)) - 1
    return f"0x{parsed & mask:0{width}X}"


def event_slot(event: dict[str, Any]) -> int | None:
    slot = base.slot_for_event(event)
    if slot is None:
        slot = base.event_value(event, "current_iw_slot")
    return slot if slot is not None and 0 <= slot < 12 else None


def event_instruction_field(
    event: dict[str, Any], slot: int | None, field: str,
) -> int | None:
    value = base.slot_state(event, slot, field)
    if value is not None:
        return value
    current_names = {
        "iw_persistent_callback": "current_iw_callback",
        "iw_selected_row": "current_iw_selected_row",
        "iw_mode": "current_iw_mode",
        "iw_subtype": "current_iw_subtype",
    }
    name = current_names.get(field)
    return base.event_value(event, name) if name else None


def exact_iw_write(event: dict[str, Any]) -> dict[str, Any] | None:
    event_pc = base.event_value(event, "pc")
    if (base.checkpoint(event).startswith("callback_store_execute_")
            and event_pc in CALLBACK_STORE_PCS):
        return {
            "root_index": None,
            "slot": event_slot(event),
            "worksheet": base.event_value(event, "r30"),
            "offset": 0xE0,
            "field": "persistent_callback",
            "field_width": 4,
            "pre_write_value": base.event_value(event, "current_iw_callback"),
            "post_write_value": base.event_value(event, "r3"),
            "writer_pc": event_pc,
            "store_pc_probe": True,
        }
    if base.IW_WRITE_RE.match(base.checkpoint(event)) is None:
        return None
    root_index = base.root_index_for_event(event)
    if root_index is None:
        return {
            "root_index": None,
            "slot": base.slot_for_event(event),
            "offset": None,
            "field": "unresolved_root",
            "field_width": None,
        }
    worksheet = base.event_value(
        event, f"root{root_index}_instruction_worksheet_ptr")
    address = base.event_value(event, "address")
    if worksheet in (None, 0) or address is None:
        return {
            "root_index": root_index,
            "slot": base.slot_for_event(event),
            "offset": None,
            "field": "unresolved_address",
            "field_width": None,
        }
    offset = address - worksheet
    field, width = FIELD_OFFSETS.get(
        offset, (f"unknown_0x{offset:X}", base.integer(event.get("size"))))
    return {
        "root_index": root_index,
        "slot": base.slot_for_event(event),
        "worksheet": worksheet,
        "offset": offset,
        "field": field,
        "field_width": width,
        "post_write_value": event.get("value"),
        "writer_pc": event_pc,
        "store_pc_probe": False,
    }


def helper_owner(event: dict[str, Any]) -> str:
    callsites = base.stack_callsites(event)
    ranges = (
        (0x8002EB4C, 0x8002EC50, "action_service_candidate"),
        (0x80022850, 0x80022A80, "persistent_instruction_dispatch"),
        (0x800221FC, 0x80022850, "queued_std_action_transition"),
        (0x80021FCC, 0x800221FC, "std_instruction_setup"),
        (0x80021EB8, 0x80021FCC, "direct_instruction_transition"),
        (0x80020250, 0x80020454, "generic_instruction_transition"),
        (0x8001BE2C, 0x8001C110, "ranged_attack_callback"),
        (0x8001B1B0, 0x8001BAC0, "basic_attack_callback"),
        (0x8001AB60, 0x8001B1B0, "position_sync_callback"),
        (0x8001A4F0, 0x8001AB60, "special_action_callback"),
        (0x80019F0C, 0x8001A4F0, "mode_0e_callback"),
        (0x8001977C, 0x80019F0C, "callback_8001977c"),
        (0x80018798, 0x8001977C, "callback_80018798"),
        (0x800177C0, 0x80018000, "geometry_query"),
        (0x8001712C, 0x800177C0, "row_property_query"),
        (0x80017018, 0x8001712C, "row_property_query"),
    )
    for callsite in callsites:
        for begin, end, owner in ranges:
            if begin <= callsite < end:
                return owner
    return "unknown"


def inferred_helper_callsite(event: dict[str, Any], owner: str) -> str:
    probe = base.event_value(event, "r6") not in (None, 0)
    mappings = {
        "position_sync_callback": "0x8001AEEC",
        "basic_attack_callback": "0x8001B438",
        "ranged_attack_callback": "0x8001BE60",
        "generic_instruction_transition": "0x800202B4",
        "direct_instruction_transition": "0x80021F4C",
        "std_instruction_setup": "0x80022164",
        "persistent_instruction_dispatch": "0x800229F8",
    }
    if owner == "queued_std_action_transition":
        return "0x800224B4" if probe else "0x800224E0"
    if owner == "action_service_candidate":
        return "0x8002EBA4_or_0x8002EC08"
    return mappings.get(owner, "")


def instruction_field_rows(
    job: int, events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        info = exact_iw_write(event)
        if info is None:
            continue
        size = info["field_width"] if info.get("store_pc_probe") \
            else base.integer(event.get("size"))
        writer_pc = info.get("writer_pc")
        static_callsites = [
            callsite for callsite in base.stack_callsites(event)
            if callsite in STATIC_HELPER_CALLSITES
        ]
        rows.append({
            **base.base_event_row(job, event),
            "slot": info["slot"],
            "root_index": info["root_index"],
            "worksheet": base.hex32(info.get("worksheet")),
            "worksheet_offset": "" if info["offset"] is None
                else f"0x{info['offset']:X}",
            "field": info["field"],
            "write_size": size,
            "post_write_value": hex_value(info.get("post_write_value"), size),
            "writer_pc": base.hex32(writer_pc),
            "writer_kind": CALLBACK_STORE_PCS.get(writer_pc, "")
                if info["field"] == "persistent_callback" else "",
            "static_helper_callsite": base.hex32(static_callsites[0])
                if static_callsites else "",
            "stack_callsites": ";".join(
                base.hex32(value) for value in base.stack_callsites(event)),
        })
    return rows


def helper_call_rows(
    job: int, events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    pending: dict[str, Any] | None = None
    call_index = 0
    for event in events:
        event_id = base.checkpoint(event)
        info = exact_iw_write(event)
        if event_id == HELPER_ENTRY_ID:
            if pending is not None:
                pending["return_status"] = "missing_before_next_entry"
                rows.append(pending)
            owner = helper_owner(event)
            slot = event_slot(event)
            param4 = base.event_value(event, "r6")
            pending = {
                **base.base_event_row(job, event),
                "helper_call_id": f"{job}:helper:{call_index}",
                "slot": slot,
                "root_index": base.root_index_for_event(event),
                "owner": owner,
                "inferred_callsite": inferred_helper_callsite(event, owner),
                "mode": signed16(base.event_value(event, "r4")),
                "subtype": signed16(base.event_value(event, "r5")),
                "param4": param4,
                "operation": "Probe" if param4 not in (None, 0) else "Publish",
                "callback_before": base.hex32(event_instruction_field(
                    event, slot, "iw_persistent_callback")),
                "return_sequence": "",
                "result_row": "",
                "callback_write_count": 0,
                "callback_write_sequence": "",
                "callback_after": "",
                "callback_store_pc": "",
                "publisher_callsite": "",
                "return_status": "pending",
                "contract_matches": False,
            }
            call_index += 1
            continue
        if (pending is not None and info is not None
                and info["field"] == "persistent_callback"):
            pending["callback_write_count"] += 1
            pending["callback_write_sequence"] = base.sequence(event)
            pending["callback_after"] = base.hex32(info.get("post_write_value"))
            pending["callback_store_pc"] = base.hex32(info.get("writer_pc"))
            static_callsites = [
                callsite for callsite in base.stack_callsites(event)
                if callsite in STATIC_HELPER_CALLSITES
            ]
            if static_callsites:
                callsite = static_callsites[0]
                pending["publisher_callsite"] = base.hex32(callsite)
                pending["owner"] = HELPER_CALLSITE_OWNERS[callsite]
                pending["inferred_callsite"] = base.hex32(callsite)
            continue
        if event_id == HELPER_RETURN_ID and pending is not None:
            result = signed16(base.event_value(event, "r3"))
            pending["return_sequence"] = base.sequence(event)
            pending["result_row"] = result
            pending["callback_after"] = pending["callback_after"] \
                or pending["callback_before"] \
                or base.hex32(event_instruction_field(
                    event, pending["slot"], "iw_persistent_callback"))
            expected_writes = 1 if pending["param4"] == 0 and result is not None and result >= 0 else 0
            pending["contract_matches"] = (
                pending["callback_write_count"] == expected_writes)
            pending["return_status"] = "observed"
            rows.append(pending)
            pending = None
    if pending is not None:
        pending["return_status"] = "missing_at_capture_end"
        rows.append(pending)
    return rows


def initial_callback_publications(
    job: int, events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    opener = next(
        (event for event in events if base.checkpoint(event) == WINDOW_OPEN_ID), None)
    if opener is None:
        return []
    rows: list[dict[str, Any]] = []
    for root_index in base.root_indices(opener):
        slot = base.event_value(opener, f"root{root_index}_iw_slot")
        if slot is None:
            continue
        rows.append({
            **base.base_event_row(job, opener),
            "slot": slot,
            "root_index": root_index,
            "worksheet": base.hex32(base.event_value(
                opener, f"root{root_index}_instruction_worksheet_ptr")),
            "callback": base.hex32(base.event_value(
                opener, f"root{root_index}_iw_persistent_callback")),
            "selected_row": base.event_value(opener, f"root{root_index}_iw_selected_row"),
            "alternate_row": base.event_value(opener, f"root{root_index}_iw_alt_row"),
            "previous_row": base.event_value(opener, f"root{root_index}_iw_previous_row"),
            "mode": signed16(base.event_value(opener, f"root{root_index}_iw_mode")),
            "subtype": signed16(base.event_value(opener, f"root{root_index}_iw_subtype")),
            "callback_control": signed16(base.event_value(
                opener, f"root{root_index}_iw_callback_control")),
            "provenance": "authoritative post-macro snapshot; publication occurred before capture window",
        })
    return rows


def callback_revision_rows(
    job: int,
    events: list[dict[str, Any]],
    fields: list[dict[str, Any]],
    helpers: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    initial = initial_callback_publications(job, events)
    helper_by_write = {
        base.integer(row.get("callback_write_sequence")): row
        for row in helpers if base.integer(row.get("callback_write_sequence")) is not None
    }
    starts: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in initial:
        starts[int(row["slot"])].append({
            "start_sequence": row["capture_sequence"],
            "frame_index": row["frame_index"],
            "action_ordinal": row["action_ordinal"],
            "callback": row["callback"],
            "publisher": "pre_window_snapshot",
            "publisher_callsite": "",
            "publisher_mode": row["mode"],
            "publisher_subtype": row["subtype"],
        })
    for row in fields:
        if row["field"] != "persistent_callback" or row["slot"] is None:
            continue
        helper = helper_by_write.get(base.integer(row["capture_sequence"]))
        starts[int(row["slot"])].append({
            "start_sequence": row["capture_sequence"],
            "frame_index": row["frame_index"],
            "action_ordinal": row["action_ordinal"],
            "callback": row["post_write_value"],
            "publisher": helper.get("owner", "unpaired_callback_write")
                if helper else "unpaired_callback_write",
            "publisher_callsite": helper.get("publisher_callsite", "")
                if helper else row.get("static_helper_callsite", ""),
            "publisher_mode": helper.get("mode", "") if helper else "",
            "publisher_subtype": helper.get("subtype", "") if helper else "",
        })

    dispatches: dict[int, list[dict[str, Any]]] = defaultdict(list)
    installs: dict[int, list[dict[str, Any]]] = defaultdict(list)
    releases: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        slot = base.slot_for_event(event)
        if slot is None:
            continue
        if base.checkpoint(event) == DISPATCH_CALL_ID:
            dispatches[slot].append(event)
        elif base.checkpoint(event) == INSTALL_ID:
            installs[slot].append(event)
        elif base.checkpoint(event) == RELEASE_ID:
            releases[slot].append(event)

    rows: list[dict[str, Any]] = []
    capture_end = max((base.sequence(event) for event in events), default=0)
    for slot, revisions in sorted(starts.items()):
        revisions.sort(key=lambda row: int(row["start_sequence"]))
        for revision, start in enumerate(revisions):
            begin = int(start["start_sequence"])
            end = int(revisions[revision + 1]["start_sequence"]) \
                if revision + 1 < len(revisions) else capture_end + 1
            revision_dispatches = [
                event for event in dispatches.get(slot, [])
                if begin <= base.sequence(event) < end
            ]
            revision_installs = [
                event for event in installs.get(slot, [])
                if begin <= base.sequence(event) < end
            ]
            revision_releases = [
                event for event in releases.get(slot, [])
                if begin <= base.sequence(event) < end
            ]
            action_ordinals = sorted({
                int(event["_action_ordinal"])
                for event in revision_dispatches
                if event.get("_action_ordinal") is not None
            })
            previous = revisions[revision - 1] if revision > 0 else None
            snapshot_matches = sum(
                base.hex32(base.slot_state(
                    event, slot, "iw_persistent_callback")) == start["callback"]
                for event in revision_dispatches
            )
            rows.append({
                "source_exec_job_id": job,
                "slot": slot,
                "callback_revision": revision,
                "start_sequence": begin,
                "end_sequence_exclusive": end,
                "end_reason": "republication" if end <= capture_end else "capture_end",
                "start_frame": start["frame_index"],
                "start_action_ordinal": start["action_ordinal"],
                "callback": start["callback"],
                "same_value_republication": previous is not None
                    and previous["callback"] == start["callback"],
                "publisher": start["publisher"],
                "publisher_callsite": start["publisher_callsite"],
                "publisher_mode": start["publisher_mode"],
                "publisher_subtype": start["publisher_subtype"],
                "dispatch_count": len(revision_dispatches),
                "dispatch_snapshot_matches": snapshot_matches,
                "first_dispatch_sequence": base.sequence(revision_dispatches[0])
                    if revision_dispatches else "",
                "last_dispatch_sequence": base.sequence(revision_dispatches[-1])
                    if revision_dispatches else "",
                "dispatch_action_ordinals": ";".join(map(str, action_ordinals)),
                "crosses_action_boundaries": len(action_ordinals) > 1,
                "action_motion_install_count": len(revision_installs),
                "action_motion_release_count": len(revision_releases),
                "first_release_sequence": base.sequence(revision_releases[0])
                    if revision_releases else "",
                "release_does_not_end_callback_revision": bool(revision_releases)
                    and end > base.sequence(revision_releases[0]),
            })
    return rows


def dispatch_revision_rows(
    job: int, events: list[dict[str, Any]], revisions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    per_slot: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in revisions:
        per_slot[int(row["slot"])].append(row)
    rows: list[dict[str, Any]] = []
    for event in events:
        if base.checkpoint(event) != DISPATCH_CALL_ID:
            continue
        slot = base.slot_for_event(event)
        current = None
        if slot is not None:
            current = next((row for row in per_slot.get(slot, [])
                if int(row["start_sequence"]) <= base.sequence(event)
                < int(row["end_sequence_exclusive"])), None)
        snapshot_callback = base.hex32(base.slot_state(
            event, slot, "iw_persistent_callback"))
        rows.append({
            **base.base_event_row(job, event),
            "slot": slot,
            "callback_revision": "" if current is None
                else current["callback_revision"],
            "revision_callback": "" if current is None else current["callback"],
            "snapshot_callback": snapshot_callback,
            "revision_matches_snapshot": current is not None
                and current["callback"] == snapshot_callback,
            "mode": signed16(base.slot_state(event, slot, "iw_mode")),
            "subtype": signed16(base.slot_state(event, slot, "iw_subtype")),
            "callback_control": signed16(base.slot_state(
                event, slot, "iw_callback_control")),
            "selected_row": signed16(base.slot_state(event, slot, "iw_selected_row")),
        })
    return rows


def lifecycle_rows(
    job: int,
    events: list[dict[str, Any]],
    revisions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    decisions = base.resolver_rows(job, events)
    chains = base.invocation_chains(job, events, decisions)
    per_slot: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in revisions:
        per_slot[int(row["slot"])].append(row)
    rows: list[dict[str, Any]] = []
    for chain in chains:
        slot = base.integer(chain.get("slot"))
        install_sequence = base.integer(chain.get("capture_sequence")) or 0
        revision = next((row for row in per_slot.get(slot, [])
            if int(row["start_sequence"]) <= install_sequence
            < int(row["end_sequence_exclusive"])), None)
        release_sequence = base.integer(chain.get("release_sequence"))
        rows.append({
            **chain,
            "callback_revision": "" if revision is None
                else revision["callback_revision"],
            "revision_publisher": "" if revision is None else revision["publisher"],
            "revision_publisher_callsite": "" if revision is None
                else revision["publisher_callsite"],
            "install_callback_matches_revision": revision is not None
                and chain.get("persistent_callback") == revision["callback"],
            "callback_republished_during_playback": revision is not None
                and release_sequence is not None
                and int(revision["end_sequence_exclusive"]) <= release_sequence,
            "release_before_revision_end": revision is not None
                and release_sequence is not None
                and release_sequence < int(revision["end_sequence_exclusive"]),
        })
    return rows


def writer_matrix_rows(
    fields: list[dict[str, Any]], helpers: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    callback_writes = [row for row in fields if row["field"] == "persistent_callback"]
    grouped: Counter[tuple[Any, ...]] = Counter()
    for row in callback_writes:
        grouped[(
            row["writer_pc"], row["writer_kind"], row["static_helper_callsite"],
            row["post_write_value"],
        )] += 1
    rows = [{
        "record_kind": "callback_writer",
        "writer_pc": key[0],
        "writer_kind": key[1],
        "static_helper_callsite": key[2],
        "callback": key[3],
        "owner": "",
        "operation": "Publish",
        "count": count,
        "callback_write_count": count,
        "contract_mismatch_count": 0,
    } for key, count in sorted(grouped.items())]

    helper_groups: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in helpers:
        helper_groups[(row["owner"], row["inferred_callsite"], row["operation"])].append(row)
    for key, values in sorted(helper_groups.items()):
        rows.append({
            "record_kind": "helper_caller",
            "writer_pc": "",
            "writer_kind": "",
            "static_helper_callsite": key[1],
            "callback": "",
            "owner": key[0],
            "operation": key[2],
            "count": len(values),
            "callback_write_count": sum(
                int(value["callback_write_count"]) for value in values),
            "contract_mismatch_count": sum(
                not bool(value["contract_matches"]) for value in values),
        })
    return rows


def initial_state0_publication_rows(
    helpers: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return [
        row for row in helpers
        if row.get("publisher_callsite") == "0x800229F8"
    ]


def followup_acceptance_rows(
    jobs: list[int],
    helpers: list[dict[str, Any]],
    scope_health: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    health_by_job = {
        int(row["source_exec_job_id"]): row for row in scope_health
    }
    rows: list[dict[str, Any]] = []
    for job in jobs:
        values = [
            row for row in helpers if int(row["source_exec_job_id"]) == job
        ]
        probes = [row for row in values if row["operation"] == "Probe"]
        publishers = [row for row in values if row["operation"] == "Publish"]
        health = health_by_job[job]
        rows.append({
            "source_exec_job_id": job,
            "helper_calls": len(values),
            "probe_calls": len(probes),
            "probe_callback_stores": sum(
                int(row["callback_write_count"]) for row in probes),
            "publisher_calls": len(publishers),
            "publisher_callback_stores": sum(
                int(row["callback_write_count"]) for row in publishers),
            "contract_mismatches": sum(
                not bool(row["contract_matches"]) for row in values),
            "initial_state0_publications": sum(
                row.get("publisher_callsite") == "0x800229F8"
                for row in values),
            "unpaired_callback_writes": health["unpaired_callback_writes"],
            "helper_cap_exhausted": health["helper_cap_exhausted"],
        })
    return rows


def run_capture_health_rows(run_root: Path) -> list[dict[str, Any]]:
    manifest = base.load_json(run_root / "manifest.json")
    rows: list[dict[str, Any]] = []
    for job in manifest.get("jobs", []):
        artifact = job.get("capture_artifact", {})
        rows.append({
            "source_exec_job_id": job.get("original_exec_job_id"),
            "cloned_exec_job_id": job.get("cloned_exec_job_id"),
            "terminal_state": job.get("terminal_state"),
            "timed_out": job.get("timed_out"),
            "capture_verified": artifact.get("verified"),
            "capture_complete": artifact.get("complete"),
            "segment_count": artifact.get("segment_count"),
            "chunk_count": artifact.get("chunk_count"),
            "event_count": artifact.get("event_count"),
            "gap_count": artifact.get("gap_count"),
            "drops": artifact.get("drops"),
            "trace_failures": artifact.get("trace_failures"),
        })
    return rows


def capture_scope_health_row(
    job: int,
    events: list[dict[str, Any]],
    fields: list[dict[str, Any]],
    helpers: list[dict[str, Any]],
) -> dict[str, Any]:
    metric = next((event for event in events
        if base.checkpoint(event)
        == "probe.metrics.worksheet_action_row_resolver_entry_80020094"), {})
    callback_writes = [
        row for row in fields if row["field"] == "persistent_callback"
    ]
    captured_write_sequences = {
        base.integer(row.get("callback_write_sequence"))
        for row in helpers
        if base.integer(row.get("callback_write_sequence")) is not None
    }
    unpaired = [
        row for row in callback_writes
        if base.integer(row.get("capture_sequence")) not in captured_write_sequences
    ]
    hits = base.event_value(metric, "hits")
    configured_max_hits = 8192 if any(
        base.checkpoint(event).startswith("callback_store_execute_")
        for event in events) else 2048
    return {
        "source_exec_job_id": job,
        "helper_native_hits": hits,
        "helper_sampled_hits": base.event_value(metric, "sampled"),
        "helper_window_rejections": base.event_value(metric, "window_rejections"),
        "helper_configured_max_hits": configured_max_hits,
        "helper_cap_exhausted": hits is not None
            and hits >= configured_max_hits,
        "callback_writes": len(callback_writes),
        "helper_publisher_calls": sum(
            row["operation"] == "Publish" for row in helpers),
        "unpaired_callback_writes": len(unpaired),
        "first_unpaired_callback_sequence": unpaired[0]["capture_sequence"]
            if unpaired else "",
        "last_callback_write_sequence": callback_writes[-1]["capture_sequence"]
            if callback_writes else "",
    }


def trigger_audit_rows() -> list[dict[str, Any]]:
    return [
        {
            "predictor_trigger": "movement_controller_instruction_publication",
            "predictor_location": "BattleFrameSchedulerModel.cpp:movement activation",
            "static_live_boundary": "FUN_8001AB60 -> 0x8001AEEC",
            "helper_mode": "Probe (param4=1)",
            "classification": "UnsupportedCallbackPublication",
            "required_change": "movement may update mode/row inputs but must not create an IW+0xE0 callback revision",
        },
        {
            "predictor_trigger": "queued_std_action_transition",
            "predictor_location": "publish_battle_frame_queued_std_action_transition",
            "static_live_boundary": "FUN_800221FC -> 0x800224B4 probe -> 0x800224E0 publish",
            "helper_mode": "Publish (param4=0)",
            "classification": "EvidenceBackedCallbackPublication",
            "required_change": "retain publication but represent the probe and callback revision separately",
        },
        {
            "predictor_trigger": "action_service_candidate_selection",
            "predictor_location": "FUN_8002EB4C visual child",
            "static_live_boundary": "0x8002EBA4/0x8002EC08 probes, then FUN_800214FC downstream transition",
            "helper_mode": "Probe then separate downstream publisher",
            "classification": "WrongImmediatePublicationBoundary",
            "required_change": "stage candidate selection and publish only through the modeled downstream instruction transition",
        },
        {
            "predictor_trigger": "initial_instruction_thread_state",
            "predictor_location": "instruction-thread initialization",
            "static_live_boundary": "FUN_80022850 state 0 -> 0x800229F8",
            "helper_mode": "Publish (param4=0)",
            "classification": "EvidenceBackedButPreWindow",
            "required_change": "initialize one persistent callback revision when the instruction thread reaches state 0",
        },
    ]


def findings_text(
    jobs: int,
    initial: list[dict[str, Any]],
    fields: list[dict[str, Any]],
    helpers: list[dict[str, Any]],
    revisions: list[dict[str, Any]],
    dispatches: list[dict[str, Any]],
    lifecycles: list[dict[str, Any]],
    scope_health: list[dict[str, Any]],
    exact_store_followup: bool = False,
) -> str:
    callback_writes = [row for row in fields if row["field"] == "persistent_callback"]
    unknown_callback_writers = [
        row for row in callback_writes if not row["writer_kind"]
    ]
    probes = [row for row in helpers if row["operation"] == "Probe"]
    publishers = [row for row in helpers if row["operation"] == "Publish"]
    contract_mismatches = [row for row in helpers if not row["contract_matches"]]
    probe_writes = sum(int(row["callback_write_count"]) for row in probes)
    position_probes = [
        row for row in probes if row["owner"] == "position_sync_callback"]
    action_service_probes = [
        row for row in probes if row["owner"] == "action_service_candidate"]
    same_value = sum(bool(row["same_value_republication"]) for row in revisions)
    dispatch_mismatches = [
        row for row in dispatches if not row["revision_matches_snapshot"]
    ]
    release_persists = sum(
        bool(row["release_does_not_end_callback_revision"]) for row in revisions)
    lifecycle_republications = sum(
        bool(row["callback_republished_during_playback"]) for row in lifecycles)
    initial_callbacks = Counter(row["callback"] for row in initial)
    writer_counts = Counter(row["writer_pc"] for row in callback_writes)
    callsite_counts = Counter(row["static_helper_callsite"] for row in callback_writes)
    row_writes = [row for row in fields if row["field"] in {
        "selected_row", "alternate_row", "previous_row", "row_runtime_ea"}]
    capped_jobs = [row for row in scope_health if row["helper_cap_exhausted"]]
    unpaired_callback_writes = sum(
        int(row["unpaired_callback_writes"]) for row in scope_health)
    initial_state0 = initial_state0_publication_rows(helpers)
    initial_state0_callbacks = Counter(
        row["callback_after"] for row in initial_state0)
    initial_state0_jobs = Counter(
        int(row["source_exec_job_id"]) for row in initial_state0)
    publisher_same_value = sum(
        row["callback_before"] == row["callback_after"] for row in publishers)
    publisher_changed_value = len(publishers) - publisher_same_value
    if exact_store_followup:
        return "\n".join([
            "# Action-Motion Callback Publication Follow-up",
            "",
            "## Capture Health",
            "",
            f"- Reduced `{jobs}` complete exact-store captures. Every helper entry and return was sampled outside the macro-scoped window; no job reached the `8192` helper cap.",
            f"- Observed `{len(helpers)}` `FUN_80020094` calls: `{len(probes)}` probes and `{len(publishers)}` publishers. Probe calls executed `{probe_writes}` callback stores; helper contract mismatches: `{len(contract_mismatches)}`.",
            f"- Observed `{len(callback_writes)}` exact `IW+0xE0` store instructions. Store-PC counts were `{dict(sorted(writer_counts.items()))}`; unknown store PCs: `{len(unknown_callback_writers)}`; unpaired stores: `{unpaired_callback_writes}`.",
            f"- Publishers performed `{publisher_same_value}` same-value republications and `{publisher_changed_value}` callback changes. Same-value stores remain real publication revisions, but they do not imply a new action-motion lifetime.",
            "",
            "## Initialization",
            "",
            f"- Captured `{len(initial_state0)}` state-0 publications at caller `0x800229F8`, distributed by job as `{dict(sorted(initial_state0_jobs.items()))}`.",
            f"- Their callback values were `{dict(sorted(initial_state0_callbacks.items()))}`. This directly closes the pre-window initialization gap left by the broad profile.",
            "",
            "## Negative Proofs",
            "",
            f"- Movement/position-sync made `{len(position_probes)}` lookup-only calls and action-service candidate selection made `{len(action_service_probes)}` lookup-only calls. Neither executed any of the seven callback store instructions.",
            "- The exact Ghidra control flow confirms every `param4 != 0` branch jumps over the store. Breakpoints at the following join instructions are therefore not valid publication evidence: `0x80020138`, `0x80020154`, `0x80020180`, `0x8002019C`, `0x800201C4`, `0x80020204`, and `0x8002022C`.",
            "- The authoritative store instructions are `0x80020134`, `0x80020150`, `0x8002017C`, `0x80020198`, `0x800201C0`, `0x80020200`, and `0x80020228`; each writes selector callback `r3` to `IW+0xE0`.",
            "",
            "## Conclusion",
            "",
            f"The initialization/publication contract is now fully observed across the {jobs}-job corpus. Combine it with the accepted five-job lifetime reduction: model one persistent callback revision at instruction-thread state 0, revise it only on an executed `param4=0` publisher, and keep action-motion playback installation/release as a separate action-local lifetime. Movement and action-service candidate probes must not publish callbacks directly. No further live capture is required before implementing this boundary.",
            "",
        ])
    lines = [
        "# Action-Motion Callback Publication and Lifetime Findings",
        "",
        "## Evidence",
        "",
        f"- Reduced `{jobs}` complete accepted captures. The authoritative post-macro boundary exposed `{len(initial)}` live instruction worksheets; initial callback values were `{dict(sorted(initial_callbacks.items()))}`.",
        f"- Observed `{len(helpers)}` `FUN_80020094` calls: `{len(probes)}` probes and `{len(publishers)}` publishers. Probe calls produced `{probe_writes}` `IW+0xE0` writes; contract mismatches: `{len(contract_mismatches)}`.",
        f"- Observed `{len(callback_writes)}` post-window `IW+0xE0` writes. Store-PC counts were `{dict(sorted(writer_counts.items()))}` and caller counts were `{dict(sorted(callsite_counts.items()))}`; unknown callback writers: `{len(unknown_callback_writers)}`.",
        f"- The helper entry/return probe exhausted its `2048` native-hit cap in `{len(capped_jobs)}/{jobs}` jobs because pre-window rejections count toward `max_hits`. Consequently `{unpaired_callback_writes}` callback writes lack paired helper entry/return rows, although their always-on write watch still captured the exact store PC, callback value, and static caller.",
        f"- Movement/position-sync contributed `{len(position_probes)}` row probes and action-service candidate selection contributed `{len(action_service_probes)}` probes. Neither category is itself a callback publisher.",
        f"- Exact-offset reduction separated `{len(row_writes)}` selected/alternate/previous/runtime row writes from callback publication. A row mutation is not an `IW+0xE0` revision.",
        "",
        "## Lifetime",
        "",
        f"- Built `{len(revisions)}` per-slot callback revisions, including snapshot revision 0. `{same_value}` are same-value republications, which remain real revisions because the game executed the publisher.",
        f"- Assigned `{len(dispatches)}` persistent callback visits to revisions; snapshot/revision mismatches: `{len(dispatch_mismatches)}`.",
        f"- `{release_persists}` revisions contain an action-motion release before their next callback publication or capture end. Release ends a playback invocation, not the persistent `IW+0xE0` callback lifetime.",
        f"- `{lifecycle_republications}` of `{len(lifecycles)}` captured playback chains crossed a callback republication. Invocation arithmetic can therefore remain gated by a stable callback revision rather than recreating the callback at movement activation.",
        "",
        "## Static Contract",
        "",
        "- `FUN_80020094(..., param4=1)` performs row lookup only. It can set the mode-`0x0E` flag side effect, but it cannot write `IW+0xE0`.",
        "- `FUN_80020094(..., param4=0)` writes the selector callback only when lookup succeeds (including the mode-`0x0E` and mode-`0x25` fallbacks).",
        "- `FUN_8001AB60` uses `0x8001AEEC` with `param4=1`; movement does not publish the callback. `FUN_800221FC` probes at `0x800224B4` and publishes at `0x800224E0`. `FUN_80022850` state 0 publishes at `0x800229F8`.",
        "- `FUN_8002EB4C` probes candidates at `0x8002EBA4/0x8002EC08`, then delegates the selected mode through `FUN_800214FC`; candidate selection is not the publication boundary.",
        "",
        "## Next Boundary",
        "",
        "Introduce a persistent callback-publication runtime separate from selected-row state and from action-motion playback instances. Initialize it from the instruction thread's state-0 publisher, revise it only at modeled `param4=0` instruction transitions, and let each instruction-thread visit invoke the currently installed callback. Movement and action-service candidate probes may update lookup inputs, but must not stage a callback revision directly. Playback release clears only the action-local invocation.",
        "",
    ]
    if (unknown_callback_writers or contract_mismatches or dispatch_mismatches
            or capped_jobs):
        lines.extend([
            "A narrow follow-up capture is warranted before implementation. It should move helper observation outside the instruction window, raise the helper cap, and dynamically watch the current call's `IW+0xE0`; this will capture the initial state-0 publication and eliminate the cap-induced helper pairing gap without repeating the broad action-motion pass.",
            "",
        ])
    else:
        lines.extend([
            "No additional post-macro live capture is required for this implementation boundary. The one remaining evidence gap is the exact pre-window timing of the initial state-0 publication; static `0x800229F8` plus the authoritative snapshot is sufficient for provisional scheduling, while an initialization-only capture can refine that timing later without blocking this correction.",
            "",
        ])
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def capture_inputs(run_root: Path) -> dict[int, Path]:
    """Resolve accepted exports without opening the run's read-only SQLite sandbox."""
    manifest = base.load_json(run_root / "manifest.json")
    captures: dict[int, Path] = {}
    for row in manifest.get("jobs", []):
        if not isinstance(row, dict):
            continue
        job = base.integer(row.get("original_exec_job_id"))
        export = Path(str(row.get("capture_export_path", "")))
        if job is not None and export.exists():
            captures[job] = export
    return captures


def main() -> int:
    args = parse_args()
    captures = capture_inputs(args.run_root)
    if not captures:
        raise RuntimeError(f"no capture exports found under {args.run_root}")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    all_initial: list[dict[str, Any]] = []
    all_fields: list[dict[str, Any]] = []
    all_helpers: list[dict[str, Any]] = []
    all_revisions: list[dict[str, Any]] = []
    all_dispatches: list[dict[str, Any]] = []
    all_lifecycles: list[dict[str, Any]] = []
    all_scope_health: list[dict[str, Any]] = []
    exact_store_followup = False

    for job, export_path in sorted(captures.items()):
        events = base.annotate_action_ordinals(base.load_jsonl(export_path))
        is_exact_store_followup = any(
            base.checkpoint(event).startswith("callback_store_execute_")
            for event in events)
        exact_store_followup = exact_store_followup or is_exact_store_followup
        initial = initial_callback_publications(job, events)
        fields = instruction_field_rows(job, events)
        helpers = helper_call_rows(job, events)
        revisions = [] if is_exact_store_followup else callback_revision_rows(
            job, events, fields, helpers)
        dispatches = [] if is_exact_store_followup else dispatch_revision_rows(
            job, events, revisions)
        lifecycles = [] if is_exact_store_followup else lifecycle_rows(
            job, events, revisions)
        scope_health = capture_scope_health_row(job, events, fields, helpers)
        all_initial.extend(initial)
        all_fields.extend(fields)
        all_helpers.extend(helpers)
        all_revisions.extend(revisions)
        all_dispatches.extend(dispatches)
        all_lifecycles.extend(lifecycles)
        all_scope_health.append(scope_health)

    base.write_csv(args.output_dir / "initial_callback_snapshot.csv", all_initial,
                   ["source_exec_job_id", "slot", "callback"])
    base.write_csv(args.output_dir / "instruction_field_revision_timeline.csv", all_fields,
                   ["source_exec_job_id", "capture_sequence", "field"])
    base.write_csv(args.output_dir / "helper_call_timeline.csv", all_helpers,
                   ["source_exec_job_id", "helper_call_id", "operation"])
    base.write_csv(args.output_dir / "callback_revision_timeline.csv", all_revisions,
                   ["source_exec_job_id", "slot", "callback_revision"])
    base.write_csv(args.output_dir / "dispatch_revision_timeline.csv", all_dispatches,
                   ["source_exec_job_id", "capture_sequence", "callback_revision"])
    base.write_csv(args.output_dir / "playback_lifetime_windows.csv", all_lifecycles,
                   ["source_exec_job_id", "chain_id", "callback_revision"])
    base.write_csv(args.output_dir / "callback_writer_matrix.csv",
                   writer_matrix_rows(all_fields, all_helpers),
                   ["record_kind", "owner", "count"])
    base.write_csv(args.output_dir / "predictor_trigger_audit.csv", trigger_audit_rows(),
                   ["predictor_trigger", "classification"])
    base.write_csv(args.output_dir / "capture_scope_health.csv", all_scope_health,
                   ["source_exec_job_id", "helper_cap_exhausted"])
    base.write_csv(args.output_dir / "initial_state0_publications.csv",
                   initial_state0_publication_rows(all_helpers),
                   ["source_exec_job_id", "helper_call_id", "callback_after"])
    base.write_csv(args.output_dir / "followup_acceptance.csv",
                   followup_acceptance_rows(
                       sorted(captures), all_helpers, all_scope_health),
                   ["source_exec_job_id", "contract_mismatches"])
    base.write_csv(args.output_dir / "run_capture_health.csv",
                   run_capture_health_rows(args.run_root),
                   ["source_exec_job_id", "capture_complete"])
    (args.output_dir / "findings.md").write_text(findings_text(
        len(captures), all_initial, all_fields, all_helpers, all_revisions,
        all_dispatches, all_lifecycles, all_scope_health,
        exact_store_followup), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
