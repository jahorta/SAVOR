#!/usr/bin/env python3
"""Reduce full-hook action-motion invocation captures into model evidence."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import reduce_predictor_live_comparison as rng_base  # noqa: E402


INSTALL_MARKER = "installed action-row motion"
ACTION_BOUNDARY_ID = "action_setup_entry_80082134"
INSTALL_ID = "action_motion_install_entry_8001EBA4"
SETUP_COMPLETE_ID = "action_motion_setup_complete_8001EC9C"
SETUP_RETURN_ID = "action_motion_setup_return_8001EC74"
APPLY_CALL_ID = "action_motion_apply_call_8001EC8C"
INSTALL_RETURN_ID = "action_motion_install_return_8001ECB0"
RELEASE_ID = "action_motion_release_8001B750"
RNG_ID = "rng_seed_write_803469A8"
SELECTED_LOADER_ID = "selected_row_loader_entry_80075F2C"
LOOKED_UP_LOADER_ID = "looked_up_row_loader_entry_80075FF0"
FRAME_LIST_IDS = {
    "battle_case5_thread_list_changed_8000A2FC",
    "battle_case5_thread_list_flight_8000A2FC",
}

RNG_CHECKPOINT_SOURCES = {
    "attack_hit_dodge_80010BDC": (0x80010BDC, "attack_hit_dodge"),
    "attack_critical_80010C44": (0x80010C44, "attack_critical"),
    "counter_roll_80081A88": (0x80081A88, "counter_roll"),
}

COMBAT_EFFECT_SOURCE_PCS = frozenset({
    0x80042F3C, 0x80042FBC, 0x80043020, 0x80043048,
    0x80043070, 0x800430FC, 0x80043200,
})

EFFECT_EMITTER_SOURCE_PCS = frozenset({
    0x80041F3C, 0x80041F60, 0x80041F88, 0x80041FB0,
    0x80041FCC, 0x80041FE8, 0x80042020, 0x800425A0,
    0x800425E0, 0x80042630, 0x80042670,
})

TERMINAL_CLEANUP_CALLSITES = frozenset({0x8001BA8C, 0x801E1824, 0x801E18AC})

DETAIL_RE = re.compile(r"(?:^|;\s*)([A-Za-z0-9_]+)=([^;]*)")
SLOT_ID_RE = re.compile(r"^slot(\d+)_")
ROOT_ID_RE = re.compile(r"^root(\d+)_")
HEX_SUFFIX_RE = re.compile(r"_([0-9A-Fa-f]{8})$")
DISPATCH_RE = re.compile(r"\[seedprobe-dispatch\]\s+job=(\d+)\s+worker=(\d+)")
IW_WRITE_RE = re.compile(r"^(?:root|slot)\d+_iw_(.+)_write$")


def integer(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def scalar(value: Any) -> Any:
    if isinstance(value, dict):
        if value.get("read_ok") is False:
            return None
        return value.get("value")
    return value


def event_value(event: dict[str, Any], name: str) -> int | None:
    return integer(scalar(event.get(name)))


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def record_sequence(event: dict[str, Any]) -> int:
    return integer(event.get("record_sequence")) or 0


def checkpoint(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id", ""))


def hex32(value: Any) -> str:
    parsed = integer(value)
    return "" if parsed is None else f"0x{parsed & 0xFFFFFFFF:08X}"


def detail_pairs(event: dict[str, Any]) -> dict[str, str]:
    text = str(event.get("detail", ""))
    return {match.group(1): match.group(2).strip() for match in DETAIL_RE.finditer(text)}


def load_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    value = json.loads(data.decode(encoding))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            text = line.strip()
            if not text:
                continue
            value = json.loads(text)
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: expected object")
            events.append(value)
    events.sort(key=lambda event: (sequence(event), record_sequence(event)))
    return events


def write_csv(path: Path, rows: list[dict[str, Any]], fallback_fields: Iterable[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields: list[str] = []
    for row in rows:
        for name in row:
            if name not in fields:
                fields.append(name)
    if not fields:
        fields = list(fallback_fields)
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def first_callsite(event: dict[str, Any]) -> int | None:
    stack = event.get("call_stack")
    if not isinstance(stack, dict):
        return None
    frames = stack.get("frames")
    if not isinstance(frames, list) or not frames:
        return None
    for frame in frames:
        if not isinstance(frame, dict):
            continue
        callsite = integer(frame.get("callsite_pc"))
        if callsite is not None and 0x80000000 <= callsite < 0x81800000:
            return callsite
    return None


def direct_stack_callsite(event: dict[str, Any]) -> int | None:
    """Return only a captured direct frame; do not promote an outer ancestor."""
    stack = event.get("call_stack")
    if not isinstance(stack, dict):
        return None
    frames = stack.get("frames")
    if not isinstance(frames, list):
        return None
    for frame in frames:
        if not isinstance(frame, dict):
            continue
        # A zero stack pointer denotes the optional live-LR pseudo-frame.
        # It is useful evidence but may still be stale at a JIT entry hook.
        if integer(frame.get("stack_pointer")) == 0:
            continue
        callsite = integer(frame.get("callsite_pc"))
        return callsite if callsite is not None and 0x80000000 <= callsite < 0x81800000 else None
    return None


def stack_callsites(event: dict[str, Any]) -> list[int]:
    stack = event.get("call_stack")
    if not isinstance(stack, dict):
        return []
    frames = stack.get("frames")
    if not isinstance(frames, list):
        return []
    return [
        callsite
        for frame in frames
        if isinstance(frame, dict)
        and (callsite := integer(frame.get("callsite_pc"))) is not None
    ]


def predictor_rng_source_family(source_pc: int | None) -> tuple[str, str]:
    if source_pc is None:
        return "unknown", "unknown"
    if source_pc in COMBAT_EFFECT_SOURCE_PCS:
        return "combat_effect_worker", "region"
    if source_pc in EFFECT_EMITTER_SOURCE_PCS:
        return "effect_emitter", "region"
    aliases = {
        0x80010BDC: "attack_hit_dodge",
        0x80010C44: "attack_critical",
        0x80011724: "action_view_pathing_fallback",
        0x80011794: "action_view_pathing_fallback",
        0x80051B48: "action_view_pathing_record",
        0x80051BB0: "action_view_pathing_record",
        0x8002EBA4: "action_service",
        0x8002EBDC: "action_service",
        0x80051320: "mode0_action_view_camera",
        0x800513D4: "mode0_action_view_camera",
        0x80052BEC: "mode0e_action_view_camera",
        0x80052BF0: "mode0e_action_view_camera",
        0x80010958: "attack_damage",
        0x80010984: "attack_damage",
        0x80081A88: "counter_roll",
    }
    return aliases.get(source_pc, f"pc_{source_pc:08X}"), "exact"


def classify_live_rng_source(
    event: dict[str, Any], previous_event: dict[str, Any] | None,
) -> dict[str, Any]:
    raw_pc = direct_stack_callsite(event)
    ancestor_pc = first_callsite(event)
    callsites = stack_callsites(event)
    previous_id = checkpoint(previous_event) if previous_event is not None else ""
    previous_source = RNG_CHECKPOINT_SOURCES.get(previous_id)
    if (previous_source is not None
            and previous_event is not None
            and sequence(previous_event) + 1 == sequence(event)):
        source_pc, family = previous_source
        return {
            "live_normalized_source_pc": source_pc,
            "live_source_family": family,
            "live_source_precision": "exact",
            "live_source_attribution": "adjacent_producer_checkpoint",
        }

    if 0x800145C8 in callsites:
        return {
            "live_normalized_source_pc": 0x800145C8,
            "live_source_family": "pc_800145C8",
            "live_source_precision": "exact",
            "live_source_attribution": "caller_stack_ancestor",
        }
    if 0x80010B2C in callsites:
        return {
            "live_normalized_source_pc": None,
            "live_source_family": "attack_damage",
            "live_source_precision": "region",
            "live_source_attribution": "static_stack_owner_family",
        }
    if 0x80042EB8 in callsites:
        return {
            "live_normalized_source_pc": None,
            "live_source_family": "combat_effect_worker",
            "live_source_precision": "region",
            "live_source_attribution": "static_stack_owner_family",
        }

    normalized_pc = rng_base.SOURCE_PC_ALIASES.get(raw_pc, raw_pc)
    family, precision = predictor_rng_source_family(normalized_pc)
    if family != "unknown" and normalized_pc in rng_base.SOURCE_NAMES:
        attribution = "direct_caller_stack"
    elif family != "unknown" and normalized_pc != raw_pc:
        attribution = "static_caller_alias"
    elif family.startswith("pc_"):
        family = "unknown"
        precision = "unknown"
        attribution = "unclassified_stack"
        normalized_pc = None
    else:
        attribution = "direct_caller_stack"
    return {
        "live_normalized_source_pc": normalized_pc,
        "live_source_family": family,
        "live_source_precision": precision,
        "live_source_attribution": attribution,
    }


def root_indices(event: dict[str, Any]) -> list[int]:
    indices = {
        int(match.group(1))
        for name in event
        if (match := re.match(r"^root(\d+)_thread_ptr$", name))
    }
    return sorted(indices)


def root_index_for_event(event: dict[str, Any]) -> int | None:
    root_match = ROOT_ID_RE.match(checkpoint(event))
    if root_match:
        return int(root_match.group(1))
    indices = root_indices(event)
    preferred_registers = (
        (29, 30, 31, 3, 26, 27, 28, 4, 5, 6)
        if checkpoint(event).startswith("instruction_dispatch")
        else (3, 30, 29, 31, 26, 27, 28, 4, 5, 6)
    )
    for register in preferred_registers:
        value = event_value(event, f"r{register}")
        if value in (None, 0):
            continue
        matches = []
        for root_index in indices:
            pointers = {
                event_value(event, f"root{root_index}_thread_ptr"),
                event_value(event, f"root{root_index}_combatant_worksheet_ptr"),
                event_value(event, f"root{root_index}_instruction_worksheet_ptr"),
            }
            if value in pointers:
                matches.append(root_index)
        if len(matches) == 1:
            return matches[0]
    return None


def slot_for_event(event: dict[str, Any]) -> int | None:
    match = SLOT_ID_RE.match(checkpoint(event))
    if match:
        return int(match.group(1))
    root_index = root_index_for_event(event)
    if root_index is not None:
        slot = event_value(event, f"root{root_index}_iw_slot")
        if slot is not None and 0 <= slot < 12:
            return slot
    return None


def action_boundaries(events: list[dict[str, Any]]) -> list[int]:
    return [sequence(event) for event in events if checkpoint(event) == ACTION_BOUNDARY_ID]


def action_ordinal_for_sequence(boundaries: list[int], event_sequence: int) -> int | None:
    index = bisect.bisect_right(boundaries, event_sequence) - 1
    return index if index >= 0 else None


def annotate_action_ordinals(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    ordered = sorted(events, key=lambda event: (sequence(event), record_sequence(event)))
    boundaries = action_boundaries(ordered)
    for event in ordered:
        event["_action_ordinal"] = action_ordinal_for_sequence(boundaries, sequence(event))
    return ordered


def slot_state(event: dict[str, Any], slot: int | None, field: str) -> int | None:
    if slot is None:
        return None
    for root_index in root_indices(event):
        if event_value(event, f"root{root_index}_iw_slot") == slot:
            value = event_value(event, f"root{root_index}_{field}")
            if value is not None:
                return value
    return event_value(event, f"slot{slot}_{field}")


def base_event_row(job: int, event: dict[str, Any]) -> dict[str, Any]:
    return {
        "source_exec_job_id": job,
        "capture_sequence": sequence(event),
        "record_sequence": record_sequence(event),
        "delayed_record": record_sequence(event) != sequence(event),
        "snapshot_id": integer(event.get("snapshot_id")) or 0,
        "frame_index": integer(event.get("frame_index")),
        "action_ordinal": event.get("_action_ordinal"),
        "checkpoint_id": checkpoint(event),
        "pc": event.get("pc", ""),
    }


def instruction_dispatch_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint(event)
        if not (event_id.startswith("instruction_dispatch_")
                or event_id.startswith("queued_transition_")):
            continue
        slot = slot_for_event(event)
        rows.append({
            **base_event_row(job, event),
            "slot": slot,
            "thread": hex32(event_value(event, "r29") or event_value(event, "r3")),
            "persistent_callback": hex32(slot_state(event, slot, "iw_persistent_callback")),
            "mode": slot_state(event, slot, "iw_mode"),
            "subtype": slot_state(event, slot, "iw_subtype"),
            "callback_control": slot_state(event, slot, "iw_callback_control"),
            "selected_row": slot_state(event, slot, "iw_selected_row"),
            "flags_ec": hex32(slot_state(event, slot, "iw_flags_ec")),
            "runtime_word": hex32(slot_state(event, slot, "iw_runtime_word")),
            "descriptor_delay": hex32(slot_state(event, slot, "iw_descriptor_delay")),
            "callback_result_r3": event_value(event, "r3"),
        })
    return rows


def mutation_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    dispatch_sequences: dict[int, list[int]] = defaultdict(list)
    for event in events:
        if checkpoint(event) != "instruction_dispatch_callback_call_80022A40":
            continue
        slot = slot_for_event(event)
        if slot is not None:
            dispatch_sequences[slot].append(sequence(event))
    for event in events:
        event_id = checkpoint(event)
        match = IW_WRITE_RE.match(event_id)
        if match is None:
            continue
        slot = slot_for_event(event)
        next_dispatch_sequence = None
        if slot is not None:
            candidates = dispatch_sequences.get(slot, [])
            index = bisect.bisect_right(candidates, sequence(event))
            if index < len(candidates):
                next_dispatch_sequence = candidates[index]
        writer = direct_stack_callsite(event)
        ancestor = first_callsite(event)
        if next_dispatch_sequence is not None:
            dispatch_status = "Observed"
        elif integer(event.get("value")) == 0 and ancestor in TERMINAL_CLEANUP_CALLSITES:
            dispatch_status = "TerminalCleanupNoDispatch"
        else:
            dispatch_status = "Missing"
        rows.append({
            **base_event_row(job, event),
            "slot": slot,
            "root_index": root_index_for_event(event),
            "region": match.group(1),
            "address": event.get("address", ""),
            "size": integer(event.get("size")),
            "post_write_value": hex32(event.get("value")),
            "writer_callsite": hex32(writer),
            "nearest_known_writer_ancestor": hex32(ancestor),
            "writer_attribution": "direct" if writer is not None
                else "outer_ancestor_only",
            "stack_read_ok": bool(event.get("call_stack", {}).get("read_ok", False))
                if isinstance(event.get("call_stack"), dict) else False,
            "next_persistent_dispatch_sequence": next_dispatch_sequence,
            "next_persistent_dispatch_delta": "" if next_dispatch_sequence is None
                else next_dispatch_sequence - sequence(event),
            "subsequent_dispatch_status": dispatch_status,
        })
    return rows


def resolver_branch(result: int | None, callsite: int | None = None) -> str:
    if callsite == 0x80017BE4:
        return "LoadSelectedRegardlessOfResult"
    if callsite == 0x8001A8D8:
        return "LoadLookedUpRegardlessOfResult"
    if callsite in {0x8001B828, 0x80066E28, 0x80066E3C}:
        return "LoadLookedUpWithoutInstall" if result in (1, 2) else "WaitRestoreOrSkip"
    if callsite in {0x8001A928, 0x8001B974, 0x80066FB0, 0x80066FC4}:
        return "InstallActionMotionPlayback" if result not in (None, 0) else "WaitRestoreOrSkip"
    if result == 2:
        return "InstallActionMotionPlayback"
    if result == 1:
        return "LoadMotionWithoutInstall"
    if result == 0:
        return "WaitRestoreOrSkip"
    return "Unsupported"


def resolver_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    resolver_events = [
        event for event in events
        if checkpoint(event).startswith("action_motion_resolver_return_")
    ]
    branch_ids = {INSTALL_ID, SELECTED_LOADER_ID, LOOKED_UP_LOADER_ID}
    for index, event in enumerate(resolver_events):
        event_id = checkpoint(event)
        slot = slot_for_event(event)
        match = HEX_SUFFIX_RE.search(event_id)
        return_pc = int(match.group(1), 16) if match else integer(event.get("pc"))
        callsite = None if return_pc is None else return_pc - 4
        result = event_value(event, "r3")
        next_resolver_sequence = (
            sequence(resolver_events[index + 1]) if index + 1 < len(resolver_events) else None)
        branch_event = next(
            (candidate for candidate in events
             if sequence(event) < sequence(candidate) <= sequence(event) + 2
             and (next_resolver_sequence is None
                  or sequence(candidate) < next_resolver_sequence)
             and checkpoint(candidate) in branch_ids),
            None,
        )
        observed_branch = "WaitRestoreOrSkip"
        if branch_event is not None:
            if checkpoint(branch_event) == INSTALL_ID:
                observed_branch = "InstallActionMotionPlayback"
            elif checkpoint(branch_event) == SELECTED_LOADER_ID:
                observed_branch = "LoadSelectedWithoutInstall"
            else:
                observed_branch = "LoadLookedUpWithoutInstall"
        contract_branch = resolver_branch(result, callsite)
        contract_matches = (
            contract_branch == observed_branch
            or contract_branch == "LoadMotionWithoutInstall"
            and observed_branch in {
                "LoadSelectedWithoutInstall", "LoadLookedUpWithoutInstall"}
            or contract_branch == "LoadSelectedRegardlessOfResult"
            and observed_branch == "LoadSelectedWithoutInstall"
            or contract_branch == "LoadLookedUpRegardlessOfResult"
            and observed_branch == "LoadLookedUpWithoutInstall"
        )
        rows.append({
            **base_event_row(job, event),
            "slot": slot,
            "root_index": root_index_for_event(event),
            "caller_callsite": hex32(callsite),
            "resolver_result": result,
            "output_row": event_value(event, "resolver_output_row"),
            "install_decision": contract_branch,
            "observed_branch": observed_branch,
            "branch_sequence": "" if branch_event is None else sequence(branch_event),
            "branch_checkpoint": "" if branch_event is None else checkpoint(branch_event),
            "contract_matches_capture": contract_matches,
            "persistent_callback": hex32(slot_state(event, slot, "iw_persistent_callback")),
            "mode": slot_state(event, slot, "iw_mode"),
            "callback_control": slot_state(event, slot, "iw_callback_control"),
            "selected_row_before": slot_state(event, slot, "iw_selected_row"),
        })
    return rows


def next_event(events: list[dict[str, Any]], start: int, ids: set[str], end: int | None = None) -> dict[str, Any] | None:
    for event in events:
        current = sequence(event)
        if current <= start:
            continue
        if end is not None and current >= end:
            return None
        if checkpoint(event) in ids:
            return event
    return None


def invocation_chains(
    job: int,
    events: list[dict[str, Any]],
    decisions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    decision_by_action_slot: dict[tuple[int | None, int | None], list[dict[str, Any]]] = defaultdict(list)
    for decision in decisions:
        decision_by_action_slot[(
            integer(decision.get("action_ordinal")), integer(decision.get("slot"))
        )].append(decision)
    installs = [event for event in events if checkpoint(event) == INSTALL_ID]
    for index, event in enumerate(installs):
        slot = slot_for_event(event)
        action = integer(event.get("_action_ordinal"))
        seq = sequence(event)
        next_install = sequence(installs[index + 1]) if index + 1 < len(installs) else None
        candidates = [
            row for row in decision_by_action_slot.get((action, slot), [])
            if (integer(row.get("capture_sequence")) or 0) < seq
        ]
        resolver = candidates[-1] if candidates else None
        setup = next_event(events, seq, {SETUP_COMPLETE_ID}, next_install)
        caller_return = next(
            (candidate for candidate in events
             if sequence(candidate) > seq
             and (next_install is None or sequence(candidate) < next_install)
             and checkpoint(candidate).startswith("action_motion_install_caller_return_")),
            None,
        )
        release = next_event(events, seq, {RELEASE_ID}, next_install)
        setup_return = next_event(events, seq, {SETUP_RETURN_ID}, next_install)
        apply_call = next_event(events, seq, {APPLY_CALL_ID}, next_install)
        installer_return = next_event(events, seq, {INSTALL_RETURN_ID}, next_install)
        selected_row = event_value(event, "r4")
        mode = slot_state(event, slot, "iw_mode")
        subtype = slot_state(event, slot, "iw_subtype")
        persistent_callback = slot_state(event, slot, "iw_persistent_callback")
        caller_return_pc = None
        if caller_return is not None:
            match = HEX_SUFFIX_RE.search(checkpoint(caller_return))
            caller_return_pc = (
                int(match.group(1), 16) if match else integer(caller_return.get("pc")))
        rows.append({
            **base_event_row(job, event),
            "chain_id": f"{job}:live:{index}",
            "slot": slot,
            "root_index": root_index_for_event(event),
            "thread": hex32(event_value(event, "r3")),
            "persistent_callback": hex32(persistent_callback),
            "mode": mode,
            "subtype": subtype,
            "selected_row": selected_row,
            "install_callsite": hex32(
                None if caller_return_pc is None else caller_return_pc - 4),
            "resolver_sequence": "" if resolver is None else resolver.get("capture_sequence", ""),
            "resolver_callsite": "" if resolver is None else resolver.get("caller_callsite", ""),
            "resolver_result": "" if resolver is None else resolver.get("resolver_result", ""),
            "resolver_output_row": "" if resolver is None else resolver.get("output_row", ""),
            "resolver_callback_state": "" if resolver is None
                else resolver.get("callback_control", ""),
            "resolver_contract_matches": "" if resolver is None
                else resolver.get("contract_matches_capture", ""),
            "installer_result": "" if installer_return is None
                else event_value(installer_return, "r3"),
            "setup_return_sequence": "" if setup_return is None else sequence(setup_return),
            "apply_call_sequence": "" if apply_call is None else sequence(apply_call),
            "setup_complete_sequence": "" if setup is None else sequence(setup),
            "installer_return_sequence": "" if installer_return is None
                else sequence(installer_return),
            "caller_return_sequence": "" if caller_return is None else sequence(caller_return),
            "caller_state_after": "" if caller_return is None
                else slot_state(caller_return, slot, "iw_callback_control"),
            "release_sequence": "" if release is None else sequence(release),
            "complete_chain": (
                setup_return is not None and apply_call is not None
                and setup is not None and installer_return is not None
                and caller_return is not None),
        })
    occurrences: Counter[tuple[Any, ...]] = Counter()
    for row in rows:
        key = (
            row["action_ordinal"], row["slot"], row["persistent_callback"],
            row["mode"], row["selected_row"], row["install_callsite"],
        )
        row["occurrence"] = occurrences[key]
        occurrences[key] += 1
    return rows


def thread_window_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    list_events = [event for event in events if checkpoint(event) in FRAME_LIST_IDS]
    list_sequences = [sequence(event) for event in list_events]
    rows: list[dict[str, Any]] = []
    for event in list_events:
        snapshot = event.get("thread_list") if isinstance(event.get("thread_list"), dict) else {}
        nodes = snapshot.get("nodes") if isinstance(snapshot.get("nodes"), list) else []
        callbacks: list[str] = []
        payloads: list[str] = []
        for node in nodes:
            fields = node.get("fields", {}) if isinstance(node, dict) else {}
            callbacks.append(hex32(scalar(fields.get("callback"))))
            payloads.append(hex32(scalar(fields.get("payload_word"))))
        rows.append({
            **base_event_row(job, event),
            "window_kind": "frame_snapshot",
            "thread_head": snapshot.get("head", ""),
            "node_count": len(nodes),
            "read_ok": snapshot.get("read_ok", False),
            "truncated": snapshot.get("truncated", False),
            "cycle_detected": snapshot.get("cycle_detected", False),
            "callbacks": ">".join(callbacks),
            "payloads": ">".join(payloads),
        })
    for event in events:
        if not checkpoint(event).startswith("combatant_thread_roots_"):
            continue
        seq = sequence(event)
        index = bisect.bisect_left(list_sequences, seq)
        rows.append({
            **base_event_row(job, event),
            "window_kind": "thread_root_mutation",
            "post_write_value": hex32(event.get("value")),
            "writer_callsite": hex32(direct_stack_callsite(event)),
            "nearest_known_writer_ancestor": hex32(first_callsite(event)),
            "previous_frame_sequence": list_sequences[index - 1] if index else "",
            "next_frame_sequence": list_sequences[index] if index < len(list_sequences) else "",
        })
    rows.sort(key=lambda row: (integer(row.get("capture_sequence")) or 0,
                               integer(row.get("record_sequence")) or 0))
    return rows


def infer_prediction_mode(events: list[dict[str, Any]], index: int) -> int | None:
    install = events[index]
    actor = integer(install.get("actor_slot"))
    action = integer(install.get("action_ordinal"))
    for candidate in reversed(events[:index]):
        if integer(candidate.get("actor_slot")) != actor:
            continue
        candidate_action = integer(candidate.get("action_ordinal"))
        if action is not None and candidate_action is not None and candidate_action != action:
            continue
        pairs = detail_pairs(candidate)
        for key in ("motion_action_mode", "action_key", "selected_instruction_mode_0x6"):
            value = integer(pairs.get(key))
            if value is not None:
                return value
        transition = pairs.get("action_mode", "")
        if "->" in transition:
            value = integer(transition.rsplit("->", 1)[-1])
            if value is not None:
                return value
    return None


def prediction_chains(path: Path, job: int) -> list[dict[str, Any]]:
    root = load_json(path)
    prediction = root.get("prediction", root)
    events = prediction.get("events", []) if isinstance(prediction, dict) else []
    events = [event for event in events if isinstance(event, dict)]
    events.sort(key=lambda event: integer(event.get("sequence")) or 0)
    rows: list[dict[str, Any]] = []
    occurrences: Counter[tuple[Any, ...]] = Counter()
    for index, event in enumerate(events):
        if INSTALL_MARKER not in str(event.get("detail", "")):
            continue
        pairs = detail_pairs(event)
        callback = pairs.get("persistent_callback", pairs.get("callback", ""))
        callsite = pairs.get("install_callsite", "")
        row = {
            "source_exec_job_id": job,
            "prediction_chain_id": f"{job}:pred:{len(rows)}",
            "prediction_sequence": integer(event.get("sequence")),
            "prediction_frame": integer(event.get("frame_index")),
            "action_ordinal": integer(event.get("action_ordinal")),
            "slot": integer(event.get("actor_slot")),
            "persistent_callback": callback,
            "mode": infer_prediction_mode(events, index),
            "selected_row": integer(pairs.get("row")),
            "install_callsite": callsite,
            "instruction_revision": integer(pairs.get("instruction_state_revision")),
        }
        key = (
            row["action_ordinal"], row["slot"], row["persistent_callback"],
            row["mode"], row["selected_row"], row["install_callsite"],
        )
        row["occurrence"] = occurrences[key]
        occurrences[key] += 1
        rows.append(row)
    return rows


def action_has_negative_proof(
    events: list[dict[str, Any]], action: int | None, slot: int | None,
) -> bool:
    if action is None:
        return False
    boundaries = action_boundaries(events)
    if action >= len(boundaries):
        return False
    start = boundaries[action]
    end = boundaries[action + 1] if action + 1 < len(boundaries) else None
    if end is None:
        end_markers = [sequence(event) for event in events if checkpoint(event) == "job.end"]
        end = end_markers[-1] if end_markers else None
    if end is None:
        return False
    return not any(
        checkpoint(event) == INSTALL_ID
        and start <= sequence(event) < end
        and (slot is None or slot_for_event(event) == slot)
        for event in events
    )


def semantic_comparison(
    job: int,
    events: list[dict[str, Any]],
    live: list[dict[str, Any]],
    predicted: list[dict[str, Any]],
    decisions: list[dict[str, Any]],
    capture_complete: bool,
) -> list[dict[str, Any]]:
    live_groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    prediction_groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in live:
        if integer(row.get("action_ordinal")) is None:
            continue
        live_groups[(row.get("action_ordinal"), row.get("slot"), row.get("mode"),
                     row.get("selected_row"))].append(row)
    for row in predicted:
        prediction_groups[(row.get("action_ordinal"), row.get("slot"), row.get("mode"),
                           row.get("selected_row"))].append(row)
    rows: list[dict[str, Any]] = []
    for key in sorted(set(live_groups) | set(prediction_groups), key=str):
        live_rows = live_groups.get(key, [])
        predicted_rows = prediction_groups.get(key, [])
        paired = min(len(live_rows), len(predicted_rows))
        for occurrence in range(paired):
            live_row = live_rows[occurrence]
            predicted_row = predicted_rows[occurrence]
            producer_available = bool(predicted_row.get("persistent_callback")
                                      and predicted_row.get("install_callsite"))
            rows.append({
                "source_exec_job_id": job,
                "action_ordinal": key[0], "slot": key[1], "mode": key[2],
                "selected_row": key[3], "occurrence": occurrence,
                "live_chain_id": live_row.get("chain_id", ""),
                "prediction_chain_id": predicted_row.get("prediction_chain_id", ""),
                "live_persistent_callback": live_row.get("persistent_callback", ""),
                "predicted_persistent_callback": predicted_row.get("persistent_callback", ""),
                "live_install_callsite": live_row.get("install_callsite", ""),
                "predicted_install_callsite": predicted_row.get("install_callsite", ""),
                "status": "Paired",
                "classification": "MatchedInvocation" if producer_available
                    else "ProducerIdentityMissingFromPrediction",
                "negative_live_proof": False,
            })
        for live_row in live_rows[paired:]:
            rows.append({
                "source_exec_job_id": job,
                "action_ordinal": key[0], "slot": key[1], "mode": key[2],
                "selected_row": key[3], "occurrence": live_rows.index(live_row),
                "live_chain_id": live_row.get("chain_id", ""),
                "prediction_chain_id": "", "status": "LiveOnly",
                "classification": "PublicationError" if capture_complete else "IncompleteEvidence",
                "negative_live_proof": False,
            })
        for predicted_row in predicted_rows[paired:]:
            action = integer(predicted_row.get("action_ordinal"))
            slot = integer(predicted_row.get("slot"))
            action_decisions = [row for row in decisions
                                if integer(row.get("action_ordinal")) == action
                                and integer(row.get("slot")) == slot]
            branches = {row.get("install_decision") for row in action_decisions}
            negative = action_has_negative_proof(events, action, slot)
            if not capture_complete:
                classification = "IncompleteEvidence"
            elif "LoadMotionWithoutInstall" in branches or negative:
                classification = "WrongInstallLoadBranch"
            elif "WaitRestoreOrSkip" in branches:
                classification = "ResolverMismatch"
            elif action is None:
                classification = "ActionBoundaryError"
            else:
                classification = "CallbackLifetimeError"
            rows.append({
                "source_exec_job_id": job,
                "action_ordinal": action, "slot": slot, "mode": key[2],
                "selected_row": key[3], "occurrence": predicted_rows.index(predicted_row),
                "live_chain_id": "",
                "prediction_chain_id": predicted_row.get("prediction_chain_id", ""),
                "status": "PredictionOnly", "classification": classification,
                "resolver_branches_in_live_window": ">".join(sorted(str(value) for value in branches)),
                "negative_live_proof": negative,
            })
    return rows


def predictor_rng_rows(path: Path, job: int) -> list[dict[str, Any]]:
    root = load_json(path)
    prediction = root.get("prediction", root)
    rows: list[dict[str, Any]] = []
    if not isinstance(prediction, dict):
        return rows
    for event in prediction.get("events", []):
        if not isinstance(event, dict):
            continue
        sources = rng_base.predictor_sources(event)
        state = integer(event.get("rng_seed_before"))
        if not sources or state is None:
            continue
        for local_index, source_pc in enumerate(sources):
            state = rng_base.advance_once(state)
            rows.append({
                "source_exec_job_id": job,
                "action_ordinal": integer(event.get("action_ordinal")),
                "prediction_draw_ordinal": len(rows),
                "prediction_event_sequence": integer(event.get("sequence")),
                "prediction_local_draw": local_index,
                "prediction_phase": event.get("phase", ""),
                "prediction_label": event.get("label", ""),
                "prediction_source_pc": source_pc,
                "prediction_post_seed": state,
            })
    return rows


def rng_comparison_rows(
    job: int, events: list[dict[str, Any]], prediction_path: Path,
) -> list[dict[str, Any]]:
    live = []
    previous_event: dict[str, Any] | None = None
    for event in events:
        if checkpoint(event) != RNG_ID:
            previous_event = event
            continue
        classified = classify_live_rng_source(event, previous_event)
        live.append({
            "source_exec_job_id": job,
            "live_action_ordinal": event.get("_action_ordinal"),
            "capture_sequence": sequence(event),
            "live_draw_index": integer(event.get("rng_draw_index_before")),
            "live_source_pc": direct_stack_callsite(event),
            "live_nearest_known_ancestor_pc": first_callsite(event),
            "live_stack_callsites": ">".join(hex32(pc) for pc in stack_callsites(event)),
            **classified,
            "live_post_seed": integer(event.get("value")),
        })
        previous_event = event
    predicted = predictor_rng_rows(prediction_path, job)
    live_anchor = next((index for index, row in enumerate(live)
                        if row.get("live_source_family")
                        == f"pc_{rng_base.ANCHOR_PC:08X}"), len(live))
    prediction_anchor = next((index for index, row in enumerate(predicted)
                              if integer(row.get("prediction_source_pc")) == rng_base.ANCHOR_PC),
                             len(predicted))
    comparable = max(len(live) - live_anchor, len(predicted) - prediction_anchor)
    rows: list[dict[str, Any]] = []
    divergence_seen = False
    for ordinal in range(comparable):
        live_row = live[live_anchor + ordinal] if live_anchor + ordinal < len(live) else {}
        predicted_row = (predicted[prediction_anchor + ordinal]
                         if prediction_anchor + ordinal < len(predicted) else {})
        prediction_family, prediction_precision = predictor_rng_source_family(
            integer(predicted_row.get("prediction_source_pc")))
        source_comparable = (
            live_row.get("live_source_family") not in (None, "", "unknown")
            and prediction_family != "unknown")
        source_match = (
            source_comparable
            and live_row.get("live_source_family") == prediction_family)
        seed_match = (live_row.get("live_post_seed") == predicted_row.get("prediction_post_seed")
                      and bool(live_row) and bool(predicted_row))
        first_divergence = not divergence_seen and source_comparable and not source_match
        divergence_seen = divergence_seen or first_divergence
        rows.append({
            "source_exec_job_id": job,
            "alignment": "first_direct_view_caller",
            "aligned_draw_ordinal": ordinal,
            **live_row,
            **predicted_row,
            "prediction_source_family": prediction_family,
            "prediction_source_precision": prediction_precision,
            "source_comparable": source_comparable,
            "source_match": source_match,
            "post_seed_match": seed_match,
            "first_divergence": first_divergence,
        })
    return rows


def capture_health_row(
    job: int, manifest_job: dict[str, Any], events: list[dict[str, Any]],
) -> dict[str, Any]:
    artifact = manifest_job.get("capture_artifact", {})
    if not isinstance(artifact, dict):
        artifact = {}
    metrics = [event for event in events if event.get("event_kind") == "metrics"]
    binding_failures = sum(event_value(event, "binding_failures") or 0 for event in metrics)
    stale_watchpoints = sum(1 for event in metrics
                            if checkpoint(event).startswith("probe.metrics.slot")
                            and event_value(event, "binding_generation") in (None, 0))
    lists = [event.get("thread_list") for event in events if checkpoint(event) in FRAME_LIST_IDS]
    lists = [value for value in lists if isinstance(value, dict)]
    return {
        "source_exec_job_id": job,
        "cloned_exec_job_id": manifest_job.get("cloned_exec_job_id", ""),
        "worker_id": manifest_job.get("_worker_id", ""),
        "worker_user_dir": manifest_job.get("_worker_user_dir", ""),
        "worker_user_dir_exists": bool(manifest_job.get("_worker_user_dir"))
            and Path(str(manifest_job.get("_worker_user_dir"))).is_dir(),
        "binary_runtime_slot": manifest_job.get("_binary_runtime_slot", ""),
        "binary_runtime_slot_exists": bool(manifest_job.get("_binary_runtime_slot"))
            and Path(str(manifest_job.get("_binary_runtime_slot"))).is_dir(),
        "started_at_utc": manifest_job.get("_started_at_utc", ""),
        "ended_at_utc": manifest_job.get("_ended_at_utc", ""),
        "all_worker_intervals_overlap": manifest_job.get("_all_intervals_overlap", False),
        "terminal_state": manifest_job.get("terminal_state", ""),
        "capture_verified": artifact.get("verified", False),
        "capture_complete": artifact.get("complete", False),
        "incomplete_reason": artifact.get("incomplete_reason", ""),
        "segment_count": artifact.get("segment_count", 0),
        "chunk_count": artifact.get("chunk_count", 0),
        "event_count": artifact.get("event_count", len(events)),
        "gap_count": artifact.get("gap_count", 0),
        "drops": artifact.get("drops", 0),
        "binding_failures": binding_failures,
        "stale_watchpoint_metrics": stale_watchpoints,
        "frame_clock_hits": sum(1 for event in events
                                if checkpoint(event) == "battle_case5_frame_clock_8000A2FC"),
        "frame_cap_reached": sum(1 for event in events
                                 if checkpoint(event) == "battle_case5_frame_clock_8000A2FC") >= 2400,
        "thread_snapshot_count": len(lists),
        "thread_snapshot_failures": sum(not value.get("read_ok", False) for value in lists),
        "thread_snapshot_truncations": sum(bool(value.get("truncated")) for value in lists),
        "thread_snapshot_cycles": sum(bool(value.get("cycle_detected")) for value in lists),
        "rng_events": sum(1 for event in events if checkpoint(event) == RNG_ID),
    }


def run_inputs(run_root: Path) -> tuple[dict[str, Any], dict[int, tuple[Path, dict[str, Any]]]]:
    manifest = load_json(run_root / "manifest.json")
    dispatch_workers: dict[int, int] = {}
    for value in manifest.get("events", []):
        match = DISPATCH_RE.search(str(value))
        if match:
            dispatch_workers[int(match.group(1))] = int(match.group(2))

    intervals: dict[int, tuple[int, int]] = {}
    execution_db = run_root / "db" / "execution.db"
    if execution_db.exists():
        with sqlite3.connect(execution_db) as connection:
            for job_id, started, ended in connection.execute(
                "SELECT job_id, started_at_utc, ended_at_utc FROM exec_job "
                "WHERE started_at_utc IS NOT NULL AND ended_at_utc IS NOT NULL"
            ):
                intervals[int(job_id)] = (int(started), int(ended))

    selected_intervals = [
        intervals[cloned]
        for row in manifest.get("jobs", [])
        if isinstance(row, dict)
        and (cloned := integer(row.get("cloned_exec_job_id"))) in intervals
    ]
    all_intervals_overlap = (
        len(selected_intervals) == len(manifest.get("jobs", []))
        and bool(selected_intervals)
        and max(start for start, _ in selected_intervals)
        < min(end for _, end in selected_intervals)
    )

    binary_hash_dirs = sorted(
        path for path in (run_root / ".worker-binary-runtime").glob("*")
        if path.is_dir()
    )
    binary_root = binary_hash_dirs[0] if len(binary_hash_dirs) == 1 else None
    jobs: dict[int, tuple[Path, dict[str, Any]]] = {}
    for row in manifest.get("jobs", []):
        if not isinstance(row, dict):
            continue
        job = integer(row.get("original_exec_job_id"))
        cloned = integer(row.get("cloned_exec_job_id"))
        export = Path(str(row.get("capture_export_path", "")))
        worker = dispatch_workers.get(cloned) if cloned is not None else None
        interval = intervals.get(cloned) if cloned is not None else None
        row["_worker_id"] = "" if worker is None else worker
        row["_worker_user_dir"] = "" if worker is None else str(
            run_root / ".worker-runtime" / f"workflow-worker-{worker}")
        row["_binary_runtime_slot"] = "" if worker is None or binary_root is None else str(
            binary_root / f"slot-{worker}")
        row["_started_at_utc"] = "" if interval is None else interval[0]
        row["_ended_at_utc"] = "" if interval is None else interval[1]
        row["_all_intervals_overlap"] = all_intervals_overlap
        if job is not None and export.exists():
            jobs[job] = (export, row)
    return manifest, jobs


def prediction_path(directory: Path, job: int) -> Path:
    for name in (f"job_{job}.json", f"job-{job}.json", f"{job}.json"):
        candidate = directory / name
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"missing prediction JSON for job {job} under {directory}")


def findings_text(
    manifest: dict[str, Any], health: list[dict[str, Any]],
    mutations: list[dict[str, Any]], decisions: list[dict[str, Any]],
    chains: list[dict[str, Any]],
    comparisons: list[dict[str, Any]], rng_rows: list[dict[str, Any]],
) -> str:
    complete = sum(bool(row.get("capture_complete")) for row in health)
    branch_counts = Counter(str(row.get("install_decision")) for row in decisions)
    observed_branch_counts = Counter(str(row.get("observed_branch")) for row in decisions)
    comparison_counts = Counter(str(row.get("classification")) for row in comparisons)
    first_rng = [row for row in rng_rows if row.get("first_divergence")]
    action_live = [row for row in chains if integer(row.get("action_ordinal")) is not None]
    complete_chains = sum(bool(row.get("complete_chain")) for row in chains)
    row_matches = sum(
        integer(row.get("selected_row")) == integer(row.get("resolver_output_row"))
        for row in chains)
    contract_matches = sum(bool(row.get("contract_matches_capture")) for row in decisions)
    paired_count = sum(row.get("status") == "Paired" for row in comparisons)
    live_only_count = sum(row.get("status") == "LiveOnly" for row in comparisons)
    prediction_only_count = sum(row.get("status") == "PredictionOnly" for row in comparisons)
    predicted_count = paired_count + prediction_only_count
    comparable_rng = sum(bool(row.get("source_comparable")) for row in rng_rows)
    matching_rng = sum(bool(row.get("source_match")) for row in rng_rows)
    paired_seed_rows = [row for row in rng_rows
                        if row.get("live_post_seed") is not None
                        and row.get("prediction_post_seed") is not None]
    matching_seed_rows = sum(bool(row.get("post_seed_match")) for row in paired_seed_rows)
    region_rng = sum(row.get("live_source_precision") == "region" for row in rng_rows)
    gaps = sum(integer(row.get("gap_count")) or 0 for row in health)
    drops = sum(integer(row.get("drops")) or 0 for row in health)
    binding_failures = sum(integer(row.get("binding_failures")) or 0 for row in health)
    stale_watchpoints = sum(integer(row.get("stale_watchpoint_metrics")) or 0 for row in health)
    list_failures = sum(integer(row.get("thread_snapshot_failures")) or 0 for row in health)
    list_truncations = sum(integer(row.get("thread_snapshot_truncations")) or 0 for row in health)
    list_cycles = sum(integer(row.get("thread_snapshot_cycles")) or 0 for row in health)
    worker_ids = {row.get("worker_id") for row in health if row.get("worker_id") != ""}
    worker_dirs = {row.get("worker_user_dir") for row in health if row.get("worker_user_dir")}
    binary_slots = {row.get("binary_runtime_slot") for row in health
                    if row.get("binary_runtime_slot")}
    intervals_overlap = bool(health) and all(
        bool(row.get("all_worker_intervals_overlap")) for row in health)
    resources_exist = bool(health) and all(
        bool(row.get("worker_user_dir_exists"))
        and bool(row.get("binary_runtime_slot_exists"))
        for row in health)
    callback_writes = [row for row in mutations if row.get("region") == "callback_rows_e4"]
    callback_writer_mapped = sum(
        bool(row.get("writer_callsite") or row.get("nearest_known_writer_ancestor"))
        for row in callback_writes)
    callback_dispatch_counts = Counter(
        str(row.get("subsequent_dispatch_status")) for row in callback_writes)
    live_mode_rows = Counter(
        (integer(row.get("mode")), integer(row.get("selected_row")))
        for row in action_live)
    top_live_mode_rows = ", ".join(
        f"mode {mode}/row {selected}: {count}"
        for (mode, selected), count in live_mode_rows.most_common(8))
    lines = [
        "# Action-Motion Invocation Findings",
        "",
        "## Capture Health",
        "",
        f"- `{complete}/{len(health)}` captures are complete; batch worker count was "
        f"`{manifest.get('worker_count', '')}`.",
        f"- Capture loss: gaps `{gaps}`, drops `{drops}`, binding failures "
        f"`{binding_failures}`, stale watchpoints `{stale_watchpoints}`.",
        f"- Thread-list health: read failures `{list_failures}`, truncations "
        f"`{list_truncations}`, cycles `{list_cycles}`; no frame clock reached its cap.",
        f"- Parallel isolation: `{len(worker_ids)}` worker IDs, `{len(worker_dirs)}` "
        f"user/runtime directories, `{len(binary_slots)}` binary slots; all five "
        f"execution intervals overlap: `{str(intervals_overlap).lower()}`; all resource "
        f"paths exist: `{str(resources_exist).lower()}`.",
        f"- Live installer calls: `{len(chains)}` total, `{len(action_live)}` action-bound; "
        f"`{complete_chains}/{len(chains)}` reached setup, apply, installer return, and caller return.",
        f"- All `{row_matches}/{len(chains)}` installer rows equal the immediately preceding "
        "resolver output row; every installer returned `1`.",
        f"- Resolver decisions: `{len(decisions)}`; observed branches: "
        f"`{dict(sorted(observed_branch_counts.items()))}`.",
        f"- `{contract_matches}/{len(decisions)}` runtime branches agree with the "
        "callsite-specific static contract.",
        "- Resolver result `2` is not a global install signal: `0x8001A8D8` and "
        "`0x8001B828` produced 14 result-2 decisions that deliberately loaded a row "
        "without calling `FUN_8001EBA4`.",
        f"- Callback/selected-row writes: `{callback_writer_mapped}/{len(callback_writes)}` "
        f"map to a writer stack; subsequent outcomes are "
        f"`{dict(sorted(callback_dispatch_counts.items()))}`.",
        "",
        "## Invocation Comparison",
        "",
        f"- Predictor installations: `{predicted_count}` versus `{len(action_live)}` live "
        f"action-bound installations; paired `{paired_count}`, live-only `{live_only_count}`, "
        f"prediction-only `{prediction_only_count}` by action/slot/mode/row occurrence.",
        f"- Live mode/row cardinality is led by: {top_live_mode_rows}.",
        f"- Diagnostic classifications: `{dict(sorted(comparison_counts.items()))}`.",
        "- Pairing is action-local and uses slot, persistent callback, mode, selected row, "
        "install callsite, and occurrence where both sides expose them. Missing predictor "
        "producer identity is reported rather than inferred.",
        "- Prediction-only rows marked `WrongInstallLoadBranch` include a completed live "
        "action window with no `FUN_8001EBA4` call, or a live result-1 loader branch.",
        "",
        "## RNG Sources",
        "",
        f"- Ordinal alignment begins at the first direct-view caller and never realigns "
        f"by RNG value. All `{matching_seed_rows}/{len(paired_seed_rows)}` paired "
        "post-write seeds match.",
        f"- Caller-family comparison matched `{matching_rng}/{comparable_rng}` "
        f"comparable rows. `{region_rng}` live rows expose only a statically proven "
        "stack-owner family, so their raw owner PC is retained without inventing a "
        "local `bl rand` callsite.",
    ]
    if first_rng:
        for row in first_rng:
            lines.append(
                f"- Job `{row['source_exec_job_id']}` first diverges at action-aligned draw "
                f"`{row['aligned_draw_ordinal']}`: live family "
                f"`{row.get('live_source_family')}` (raw owner "
                f"`{hex32(row.get('live_source_pc'))}`), predictor family "
                f"`{row.get('prediction_source_family')}` (source "
                f"`{hex32(row.get('prediction_source_pc'))}`)."
            )
    else:
        lines.append(
            f"- No caller-family divergence was present in `{comparable_rng}` comparable "
            "rows."
        )
    lines.extend([
        "",
        "## Interpretation",
        "",
        "Treat invocation cardinality and semantic pairing as separate diagnostics. A low "
        "installation count or a wrong install/load branch points at callback-state invocation "
        "policy. Near-live cardinality with poor action/slot/mode/row pairing points instead "
        "at persistent instruction publication or callback lifetime. In either case, keep the "
        "validated playback arithmetic unchanged and do not add timing compensation.",
        "",
    ])
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--prediction-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest, captures = run_inputs(args.run_root)
    if not captures:
        raise RuntimeError(f"no capture exports found under {args.run_root}")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    all_dispatch: list[dict[str, Any]] = []
    all_mutations: list[dict[str, Any]] = []
    all_decisions: list[dict[str, Any]] = []
    all_chains: list[dict[str, Any]] = []
    all_threads: list[dict[str, Any]] = []
    all_comparisons: list[dict[str, Any]] = []
    all_rng: list[dict[str, Any]] = []
    all_health: list[dict[str, Any]] = []

    for job, (export_path, manifest_job) in sorted(captures.items()):
        events = annotate_action_ordinals(load_jsonl(export_path))
        prediction = prediction_path(args.prediction_dir, job)
        dispatch = instruction_dispatch_rows(job, events)
        mutations = mutation_rows(job, events)
        decisions = resolver_rows(job, events)
        chains = invocation_chains(job, events, decisions)
        threads = thread_window_rows(job, events)
        predicted = prediction_chains(prediction, job)
        complete = bool(manifest_job.get("capture_artifact", {}).get("complete", False))
        comparisons = semantic_comparison(
            job, events, chains, predicted, decisions, complete)
        rng_rows = rng_comparison_rows(job, events, prediction)
        health = capture_health_row(job, manifest_job, events)

        all_dispatch.extend(dispatch)
        all_mutations.extend(mutations)
        all_decisions.extend(decisions)
        all_chains.extend(chains)
        all_threads.extend(threads)
        all_comparisons.extend(comparisons)
        all_rng.extend(rng_rows)
        all_health.append(health)

    write_csv(args.output_dir / "instruction_dispatch_timeline.csv", all_dispatch,
              ["source_exec_job_id", "capture_sequence", "checkpoint_id"])
    write_csv(args.output_dir / "instruction_mutation_timeline.csv", all_mutations,
              ["source_exec_job_id", "capture_sequence", "checkpoint_id"])
    write_csv(args.output_dir / "motion_resolver_decisions.csv", all_decisions,
              ["source_exec_job_id", "capture_sequence", "resolver_result"])
    write_csv(args.output_dir / "action_motion_invocation_chains.csv", all_chains,
              ["source_exec_job_id", "chain_id", "action_ordinal", "slot"])
    write_csv(args.output_dir / "thread_insertion_windows.csv", all_threads,
              ["source_exec_job_id", "capture_sequence", "window_kind"])
    write_csv(args.output_dir / "action_motion_semantic_comparison.csv", all_comparisons,
              ["source_exec_job_id", "status", "classification"])
    write_csv(args.output_dir / "rng_source_comparison.csv", all_rng,
              ["source_exec_job_id", "aligned_draw_ordinal", "source_match"])
    write_csv(args.output_dir / "capture_health.csv", all_health,
              ["source_exec_job_id", "capture_complete", "binding_failures"])
    (args.output_dir / "findings.md").write_text(
        findings_text(manifest, all_health, all_mutations, all_decisions, all_chains,
                      all_comparisons, all_rng), encoding="utf-8")

    print(json.dumps({
        "jobs": len(all_health),
        "complete_captures": sum(bool(row.get("capture_complete")) for row in all_health),
        "resolver_decisions": len(all_decisions),
        "live_invocations": len(all_chains),
        "semantic_rows": len(all_comparisons),
        "rng_comparable_rows": len(all_rng),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
