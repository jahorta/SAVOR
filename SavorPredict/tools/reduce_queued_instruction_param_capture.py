#!/usr/bin/env python3
"""Reduce queued-instruction parameter, route, state, and mode captures."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any, Iterable


MACRO_RE = re.compile(r"macro_untrusted_slot(\d+)_instr_param_0x06$")
ROUTE_IDS = {
    "queued_rows_initialized_80071A68": "initialized",
    "setup_action_pc_handler_store_80070A54": "command_selected",
    "pc_execution_rewrite_entry_800855AC": "execution_rewrite_begin",
    "pc_execution_rewrite_return_80085708": "execution_resolved",
    "pc_final_param_consumer_80086F10": "route_consumed",
    "pc_direct_worker_selected_80086F48": "direct_worker_selected",
    "pc_fallback_worker_selected_80086F70": "fallback_worker_selected",
    "enemy_final_param_consumer_8008BD80": "route_consumed",
    "enemy_direct_worker_selected_8008BDAC": "direct_worker_selected",
    "enemy_fallback_worker_selected_8008BDDC": "fallback_worker_selected",
    "attack_critical_80010C44": "critical_gate",
    "attack_result_return_80081BE8": "attack_result",
    "attack_result_write_80081C48": "attack_result",
}
STATE_IDS = {
    "queued_state_setter_entry_80081168",
    "queued_state_write_complete_800811C8",
    "queued_state_case5_mode4_80021810",
    "queued_state_case6_mode8_80021818",
    "queued_state_case7_mode5_80021820",
    "queued_transition_mode_write_complete_8002279C",
    "instruction_thread_visit_80022850",
}
MAPPER_CASES = {
    "queued_state_case5_mode4_80021810": (5, 4),
    "queued_state_case6_mode8_80021818": (6, 8),
    "queued_state_case7_mode5_80021820": (7, 5),
}

WRITER_MATRIX = (
    ("initialization", "Battle::Run::setupBattle_80071990", "0x80071A40", "all 12 rows", "instrParam=-1", "setupBattle return", "high"),
    ("menu selection", "ActionSelectController_8007CAB0", "0x8007CB50", "selected PC row", "instruction=3 for attack", "target acceptance", "high"),
    ("target acceptance", "EnemyTargetSelectController_800794D8", "0x80079838/0x80079854", "selected PC row", "attack param=0 when movement_flags&0x40 else 1", "FUN_800855AC", "high"),
    ("menu selection", "FUN_8007B328", "0x8007B474", "selected PC row", "selected item or command ID", "command consumer", "high"),
    ("menu selection", "LaunchTargetSelectorForAction_8007B958", "0x8007B9F8", "selected PC row", "selected magic/S-Move/crew ID", "command consumer", "high"),
    ("menu selection", "DispatchAcceptedCommand_8007C600", "0x8007C668", "selected PC row", "focus/guard param=-1", "setupAction", "high"),
    ("menu selection", "DispatchAcceptedCommand_8007C600", "0x8007C6E8-0x8007C70C", "PC rows 0-3", "multi-row accepted-command publication", "setupAction", "high"),
    ("reset", "Battle::HandlePCInst_80086C68", "0x80086D18", "current PC row", "instruction and param reset before dispatch", "PC execution rewrite", "medium"),
    ("execution rewrite", "FUN_800855AC", "0x80085608/0x800856C4/0x800856F8", "current PC row", "final basic-attack param 0=direct or nonzero=fallback", "0x80086F10", "high"),
    ("execution rewrite", "Battle::HandlePCInst_80086C68", "0x80086E4C", "current PC row", "adjacency repair of final attack param", "0x80086F10", "high"),
    ("AI production/reset", "Battle::HandleECInst_8008B9E0", "0x8008BABC-0x8008BCBC", "current enemy row", "AI instruction/initial param and branch rewrites", "enemy route resolution", "medium"),
    ("execution rewrite", "Battle::HandleECInst_8008B9E0", "0x8008BCA8-0x8008BD7C", "current enemy row", "final basic-attack param 0=direct or nonzero=fallback", "0x8008BD80", "high"),
    ("queued-state publication", "SetQueuedSpecialActionState_80081168", "0x800811C4", "current actor special-state row", "direct result!=2 -> 5; direct result=2 -> 6; fallback -> 7", "MapQueuedStateToStdActionId_800217D0", "high"),
    ("mode mapping", "MapQueuedStateToStdActionId_800217D0", "0x8002180C/0x80021814/0x8002181C", "current instruction worksheet", "5->4; 6->8; 7->5", "FUN_800221FC worksheet publication", "high"),
)


def integer(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
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


def signed16(value: Any) -> int | None:
    parsed = integer(value)
    if parsed is None:
        return None
    parsed &= 0xFFFF
    return parsed - 0x10000 if parsed & 0x8000 else parsed


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id", ""))


def load_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    value = json.loads(data.decode(encoding))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: expected an object")
            rows.append(value)
    return sorted(rows, key=sequence)


def locate_captures(run_root: Path) -> dict[int, Path]:
    manifest_path = run_root / "manifest.json"
    if not manifest_path.exists():
        manifest_path = run_root / "runs" / "manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"no manifest.json under {run_root}")
    manifest = load_json(manifest_path)
    captures: dict[int, Path] = {}
    jobs = manifest.get("jobs")
    if isinstance(jobs, list):
        for row in jobs:
            if not isinstance(row, dict):
                continue
            job = integer(row.get("original_exec_job_id"))
            raw_path = row.get("stable_capture_path")
            if job is None or raw_path is None:
                continue
            path = Path(str(raw_path))
            if not path.is_absolute():
                path = manifest_path.parent / path
            if path.exists():
                captures[job] = path
    else:
        job = integer(manifest.get("original_exec_job_id"))
        path = manifest_path.parent / "capture.jsonl"
        if job is not None and path.exists():
            captures[job] = path
    return captures


def slot_snapshot(event: dict[str, Any], slot: int) -> dict[str, int | None]:
    prefix = f"slot{slot}_"
    return {
        "instruction": integer(event.get(prefix + "queued_instruction_0x00")),
        "target": integer(event.get(prefix + "queued_target_0x04")),
        "instr_param": signed16(event.get(prefix + "queued_instr_param_0x06")),
        "result": integer(event.get(prefix + "queued_result_0x08")),
        "result_copy": integer(event.get(prefix + "queued_result_copy_0x09")),
        "queued_state": integer(event.get(prefix + "special_state_0x00")),
    }


def route_name(parameter: int | None) -> str:
    if parameter is None:
        return "Unknown"
    return "DirectMelee" if parameter == 0 else "FallbackRanged"


def queued_row_slot_from_address(address: Any, field_offset: int) -> int | None:
    parsed = integer(address)
    if parsed is None:
        return None
    relative = parsed - (0x80309174 + field_offset)
    if relative < 0 or relative % 0x20 != 0:
        return None
    slot = relative // 0x20
    return slot if 0 <= slot < 12 else None


def event_slot(event: dict[str, Any], event_id: str) -> int | None:
    if event_id in {"pc_execution_rewrite_entry_800855AC", "pc_execution_rewrite_return_80085708"}:
        slot = integer(event.get("r3"))
    elif event_id == "pc_final_param_consumer_80086F10":
        slot = queued_row_slot_from_address(event.get("pc_consumer_instruction_address"), 0)
        if slot is None:
            slot = queued_row_slot_from_address(event.get("r3"), 0)
    elif event_id in {"pc_direct_worker_selected_80086F48", "pc_fallback_worker_selected_80086F70"}:
        slot = integer(event.get("r29"))
    elif event_id == "enemy_final_param_consumer_8008BD80":
        slot = queued_row_slot_from_address(event.get("enemy_consumer_instr_param_address"), 6)
        if slot is None:
            slot = queued_row_slot_from_address(event.get("r30"), 6)
    elif event_id in {"enemy_direct_worker_selected_8008BDAC", "enemy_fallback_worker_selected_8008BDDC"}:
        slot = integer(event.get("r29"))
    elif event_id == "attack_critical_80010C44":
        slot = integer(event.get("r28"))
    elif event_id in {"attack_result_return_80081BE8", "attack_result_write_80081C48"}:
        slot = integer(event.get("r31"))
    else:
        return None
    return slot if slot is not None and 0 <= slot < 12 else None


def macro_rows(job: int, events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        match = MACRO_RE.search(checkpoint_id(event))
        if match is None:
            continue
        stack = [str(event.get(f"stack_frame_{index}_callsite_pc", "")) for index in range(8)]
        rows.append({
            "job": job,
            "capture_sequence": sequence(event),
            "slot": int(match.group(1)),
            "source_pc": event.get("decoded_pc", event.get("pc", "")),
            "post_write_value": signed16(event.get("decoded_value")),
            "decoded_memory_value": signed16(event.get("decoded_memory_value")),
            "confirmed_current_instruction": event.get("memwatch_confirmed_current_instruction", ""),
            "scope": "macro_untrusted",
            "stack": " > ".join(part for part in stack if part),
        })
    return rows


def attack_param_rows(job: int, events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        if event_id not in ROUTE_IDS:
            continue
        selected_slot = event_slot(event, event_id)
        slots = range(12) if selected_slot is None else (selected_slot,)
        for slot in slots:
            snapshot = slot_snapshot(event, slot)
            if all(value is None for value in snapshot.values()):
                continue
            rows.append({
                "job": job,
                "capture_sequence": sequence(event),
                "checkpoint_id": event_id,
                "stage": ROUTE_IDS[event_id],
                "slot": slot,
                **snapshot,
                "route": route_name(snapshot["instr_param"])
                    if ROUTE_IDS[event_id] in {"execution_resolved", "route_consumed", "direct_worker_selected", "fallback_worker_selected"}
                    else "",
                "evidence_class": "reliable_normal_scope",
            })
    return rows


def state_mode_rows(job: int, events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    latest_route: dict[int, str] = {}
    latest_result: dict[int, int] = {}
    pending: dict[int, dict[str, Any]] = {}
    next_transition_ordinal = 0
    for event in events:
        event_id = checkpoint_id(event)
        selected_slot = event_slot(event, event_id)
        if event_id in {
            "pc_direct_worker_selected_80086F48",
            "enemy_direct_worker_selected_8008BDAC",
        } and selected_slot is not None:
            latest_route[selected_slot] = "DirectMelee"
        elif event_id in {
            "pc_fallback_worker_selected_80086F70",
            "enemy_fallback_worker_selected_8008BDDC",
        } and selected_slot is not None:
            latest_route[selected_slot] = "FallbackRanged"
        elif event_id == "attack_result_return_80081BE8" and selected_slot is not None:
            result_value = integer(event.get("r3"))
            if result_value is not None:
                latest_result[selected_slot] = result_value
        if event_id not in STATE_IDS:
            continue
        slot: int | None = None
        state: int | None = None
        execution_route = ""
        attack_result: int | str = ""
        transition_ordinal: int | str = ""
        publication_correlated: bool | str = ""
        if event_id == "queued_state_setter_entry_80081168":
            slot = integer(event.get("r3"))
            state = integer(event.get("r4"))
        elif event_id == "queued_state_write_complete_800811C8":
            slot = integer(event.get("r6"))
            state = integer(event.get("r4"))
            if slot is not None:
                execution_route = latest_route.get(slot, "")
                attack_result = latest_result.get(slot, "")
            if slot is not None and state in {5, 6, 7}:
                pending[slot] = {
                    "ordinal": next_transition_ordinal,
                    "state": state,
                    "route": execution_route,
                    "attack_result": attack_result,
                    "mapped_mode": None,
                }
                transition_ordinal = next_transition_ordinal
                publication_correlated = True
                next_transition_ordinal += 1
        elif event_id.startswith("instruction_thread_"):
            slot = integer(event.get("instruction_slot_0x00"))
            if slot is not None:
                state = integer(event.get(f"slot{slot}_special_state_0x00"))
        elif event_id in MAPPER_CASES:
            slot = integer(event.get("resolver_iw_slot_0x00"))
            state, _ = MAPPER_CASES[event_id]
            item = pending.get(slot) if slot is not None else None
            if item is not None and item["state"] == state and item["mapped_mode"] is None:
                item["mapped_mode"] = integer(event.get("r6"))
                if item["mapped_mode"] is None:
                    item["mapped_mode"] = MAPPER_CASES[event_id][1]
                execution_route = item["route"]
                attack_result = item["attack_result"]
                transition_ordinal = item["ordinal"]
                publication_correlated = True
            else:
                publication_correlated = False
        elif event_id == "queued_transition_mode_write_complete_8002279C":
            slot = integer(event.get("resolver_iw_slot_0x00"))
            item = pending.get(slot) if slot is not None else None
            if item is not None and item["mapped_mode"] is not None:
                state = item["state"]
                execution_route = item["route"]
                attack_result = item["attack_result"]
                transition_ordinal = item["ordinal"]
                publication_correlated = True
        mode = integer(event.get("resolver_iw_mode_0x06"))
        if mode is None:
            mode = integer(event.get("instruction_mode_0x06"))
        if event_id in MAPPER_CASES:
            mode = integer(event.get("r6"))
            if mode is None:
                _, mode = MAPPER_CASES[event_id]
        comparison_scope = ""
        if event_id in MAPPER_CASES:
            comparison_scope = "mapper_result" if publication_correlated is True else "mapper_poll"
        elif event_id == "queued_transition_mode_write_complete_8002279C" \
                and publication_correlated is True:
            comparison_scope = "post_write"
        expected_mode = {5: 4, 6: 8, 7: 5}.get(state)
        rows.append({
            "job": job,
            "capture_sequence": sequence(event),
            "checkpoint_id": event_id,
            "slot": slot,
            "queued_state": state,
            "execution_route": execution_route,
            "attack_result": attack_result,
            "transition_ordinal": transition_ordinal,
            "publication_correlated": publication_correlated,
            "mapped_mode": mode,
            "expected_mode": "" if expected_mode is None else expected_mode,
            "comparison_scope": comparison_scope,
            "mapping_matches": "" if comparison_scope == "" or mode is None or expected_mode is None
                else mode == expected_mode,
            "instruction_thread_state": integer(event.get("instruction_thread_state_0x19")),
            "rng_draw_index_before": integer(event.get("rng_draw_index_before")),
        })
        if event_id == "queued_transition_mode_write_complete_8002279C" \
                and publication_correlated is True and slot is not None:
            pending.pop(slot, None)
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def writer_rows() -> list[dict[str, Any]]:
    return [dict(zip(
        ("classification", "function", "writer_pc", "row_scope", "meaning", "final_consumer", "confidence"),
        row,
    ), evidence_date="2026-07-15") for row in WRITER_MATRIX]


def reduce(run_root: Path, output_dir: Path) -> dict[str, int]:
    captures = locate_captures(run_root)
    all_macro: list[dict[str, Any]] = []
    all_params: list[dict[str, Any]] = []
    all_states: list[dict[str, Any]] = []
    per_job: dict[int, dict[str, int]] = {}
    for job, path in sorted(captures.items()):
        events = load_jsonl(path)
        macro = macro_rows(job, events)
        params = attack_param_rows(job, events)
        states = state_mode_rows(job, events)
        all_macro.extend(macro)
        all_params.extend(params)
        all_states.extend(states)
        ids = {checkpoint_id(event) for event in events}
        per_job[job] = {
            "pre": int("queued_rows_initialized_80071A68" in ids),
            "post": int("setup_action_pc_handler_store_80070A54" in ids),
            "consumer": int(bool(ids & {"pc_final_param_consumer_80086F10", "enemy_final_param_consumer_8008BD80"})),
            "result": int(bool(ids & {"attack_result_return_80081BE8", "attack_result_write_80081C48"})),
            "state": int("queued_state_write_complete_800811C8" in ids),
            "mode": int("queued_transition_mode_write_complete_8002279C" in ids),
            "instruction_visit": int("instruction_thread_visit_80022850" in ids),
        }

    write_csv(output_dir / "writer_matrix.csv", writer_rows(), [
        "classification", "function", "writer_pc", "row_scope", "meaning",
        "final_consumer", "confidence", "evidence_date",
    ])
    write_csv(output_dir / "attack_param_timeline.csv", all_params, [
        "job", "capture_sequence", "checkpoint_id", "stage", "slot", "instruction",
        "target", "instr_param", "result", "result_copy", "queued_state", "route",
        "evidence_class",
    ])
    write_csv(output_dir / "queued_state_mode_timeline.csv", all_states, [
        "job", "capture_sequence", "checkpoint_id", "slot", "queued_state",
        "execution_route", "attack_result", "transition_ordinal",
        "publication_correlated", "mapped_mode", "expected_mode",
        "comparison_scope", "mapping_matches", "instruction_thread_state",
        "rng_draw_index_before",
    ])
    write_csv(output_dir / "input_macro_observations.csv", all_macro, [
        "job", "capture_sequence", "slot", "source_pc", "post_write_value",
        "decoded_memory_value", "confirmed_current_instruction", "scope", "stack",
    ])

    complete = sum(all(value == 1 for value in checks.values()) for checks in per_job.values())
    findings = [
        "# Queued Instruction Parameter Findings",
        "",
        f"Run root: `{run_root}`",
        "",
        f"Reduced {len(captures)} job captures; {complete} satisfy every reliable runtime boundary.",
        "",
        "Macro-time watchpoint rows are diagnostics only. The accepted command value comes from the reliable post-macro snapshot, and final route comes from the normal-scope consumer.",
        "",
        "## Runtime Acceptance",
        "",
        "| Job | Pre | Post | Consumer | Result | State | Mode | Instruction visit | Complete |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|:---:|",
    ]
    for job, checks in sorted(per_job.items()):
        is_complete = all(value == 1 for value in checks.values())
        findings.append(
            f"| {job} | {checks['pre']} | {checks['post']} | {checks['consumer']} | "
            f"{checks['result']} | {checks['state']} | {checks['mode']} | "
            f"{checks['instruction_visit']} | {'yes' if is_complete else 'no'} |"
        )
    findings.extend([
        "",
        "## Live Transition Coverage",
        "",
        "| Job | PC direct | PC fallback | Enemy direct | Enemy fallback | 5 -> 4 | 6 -> 8 | 7 -> 5 | Commit mismatches |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for job in sorted(captures):
        params = [row for row in all_params if row["job"] == job]
        states = [row for row in all_states if row["job"] == job]
        checkpoint_count = lambda event_id: sum(
            row["checkpoint_id"] == event_id for row in params)
        transition_count = lambda state: sum(
            row["comparison_scope"] == "post_write"
            and row["queued_state"] == state
            and row["mapping_matches"] is True
            for row in states)
        mismatches = sum(
            row["comparison_scope"] == "post_write"
            and row["mapping_matches"] is False
            for row in states)
        findings.append(
            f"| {job} | {checkpoint_count('pc_direct_worker_selected_80086F48')} | "
            f"{checkpoint_count('pc_fallback_worker_selected_80086F70')} | "
            f"{checkpoint_count('enemy_direct_worker_selected_8008BDAC')} | "
            f"{checkpoint_count('enemy_fallback_worker_selected_8008BDDC')} | "
            f"{transition_count(5)} | {transition_count(6)} | "
            f"{transition_count(7)} | {mismatches} |"
        )
    correlated_commits = [
        row for row in all_states
        if row["comparison_scope"] == "post_write"
        and row["mapping_matches"] is True
    ]
    direct_hits = sum(
        row["queued_state"] == 5
        and row["execution_route"] == "DirectMelee"
        and row["attack_result"] == 1
        for row in correlated_commits)
    direct_criticals = sum(
        row["queued_state"] == 6
        and row["execution_route"] == "DirectMelee"
        and row["attack_result"] == 2
        for row in correlated_commits)
    fallback_hits = sum(
        row["queued_state"] == 7
        and row["execution_route"] == "FallbackRanged"
        and row["attack_result"] == 1
        for row in correlated_commits)
    critical_label = "critical" if direct_criticals == 1 else "criticals"
    findings.extend([
        "",
        f"Action-local commit vectors: {direct_hits} direct hits reached `5 -> 4`; "
        f"{direct_criticals} direct {critical_label} reached `6 -> 8`; "
        f"{fallback_hits} fallback hits reached `7 -> 5`.",
    ])
    findings.extend([
        "",
        "## Static Contract",
        "",
        "- PC and enemy final consumers select direct when final `instrParam` is zero and fallback when it is nonzero.",
        "- Direct workers publish state 5 for miss/hit and state 6 for critical result 2.",
        "- Fallback workers publish state 7.",
        "- `MapQueuedStateToStdActionId_800217D0` maps states `5 -> 4`, `6 -> 8`, and `7 -> 5`.",
        "- No event in this pipeline owns an RNG draw; the seed watch exists only for timeline correlation.",
        "",
    ])
    (output_dir / "findings.md").write_text("\n".join(findings), encoding="utf-8")
    return {
        "jobs": len(captures),
        "complete_jobs": complete,
        "macro_rows": len(all_macro),
        "param_rows": len(all_params),
        "state_rows": len(all_states),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    result = reduce(args.run_root, args.output_dir)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
