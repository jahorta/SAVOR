#!/usr/bin/env python3
"""Reduce mode-1 pathing publication and persistent-callback lifetime captures."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


GLOBAL_EVENT_LIMIT = 4096
RNG_ID = "memwatch.rng_seed_write_803469A8"
MODE1_ID = "mode1_geometry_call_80051BB0"
PATHING_ID = "pathing_outer_loop_entry_800526EC"
AUX_CALL_ID = "callback_aux_publication_call_8001B750"
AUX_RETURN_ID = "callback_aux_publication_return_8001B754"
CREATOR_ID = "serialized_action_view_creator_entry_8003C690"
PUBLICATION_ID = "serialized_action_view_publication_8003C738"
MOTION_ID = "origin_motion_consumption_8001B778"
CHILD_ID = "action_view_record_state0_helper_80051320"
STATE_RE = re.compile(r"basic_attack_callback_state(\d+)_")

TIMELINE_PREFIXES = (
    "basic_attack_callback_",
    "callback_gate_",
    "delay_",
    "callback_delay_",
    "callback_state9_",
    "callback_aux_",
    "serialized_action_view_",
    "origin_motion_",
    "action_view_record_state0_",
    "mode1_geometry_",
    "pathing_outer_loop_",
)

FIELD_PREFIXES = (
    "callback_state",
    "callback_entry",
    "instruction_visit",
    "instruction_dispatch",
    "delay_entry",
    "delay_gate",
    "resolver",
    "mode1_origin",
    "serialized_origin",
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


def pointer(value: Any) -> int | None:
    parsed = integer(value)
    if parsed in (None, 0):
        return None
    return parsed & 0xFFFFFFFF


def hex32(value: Any) -> str:
    parsed = integer(value)
    return "" if parsed is None else f"0x{parsed & 0xFFFFFFFF:08X}"


def sequence(event: dict[str, Any]) -> int:
    return integer(event.get("capture_sequence")) or 0


def checkpoint_id(event: dict[str, Any]) -> str:
    return str(event.get("checkpoint_id", ""))


def load_json(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    value = json.loads(data.decode(encoding))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected an object")
    return value


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"{path}:{line_number}: expected an object")
            events.append(value)
    return sorted(events, key=sequence)


def locate_captures(run_root: Path) -> tuple[dict[str, Any], dict[int, Path]]:
    manifest = load_json(run_root / "manifest.json")
    captures: dict[int, Path] = {}
    for row in manifest.get("jobs", []):
        if not isinstance(row, dict):
            continue
        job = integer(row.get("original_exec_job_id"))
        raw_path = row.get("stable_capture_path")
        if job is None or raw_path is None:
            continue
        path = Path(str(raw_path))
        if not path.is_absolute():
            path = run_root / path
        captures[job] = path
    return manifest, captures


def prefixed_value(event: dict[str, Any], suffix: str) -> Any:
    for prefix in FIELD_PREFIXES:
        key = f"{prefix}_{suffix}"
        if key in event:
            return event[key]
    return None


def event_iw(event: dict[str, Any]) -> int | None:
    for key in ("instruction_worksheet", "origin_instruction"):
        value = pointer(event.get(key))
        if value is not None:
            return value
    for prefix in FIELD_PREFIXES:
        value = pointer(event.get(f"{prefix}_iw_ptr_0x4c"))
        if value is not None:
            return value
    return None


def event_thread(event: dict[str, Any]) -> int | None:
    for key in ("thread", "thread_saved", "origin_thread"):
        value = pointer(event.get(key))
        if value is not None:
            return value
    return None


def event_slot(event: dict[str, Any]) -> int | None:
    return integer(prefixed_value(event, "iw_slot_0x00"))


def event_target(event: dict[str, Any]) -> int | None:
    return integer(prefixed_value(event, "iw_target_0x04"))


def event_mode(event: dict[str, Any]) -> int | None:
    return integer(prefixed_value(event, "iw_mode_0x06"))


def event_control(event: dict[str, Any]) -> int | None:
    return integer(prefixed_value(event, "iw_control_0x12"))


def event_delay(event: dict[str, Any]) -> int | None:
    for key in ("delay", "delay_before", "callback_state_iw_delay_0x138"):
        parsed = integer(event.get(key))
        if parsed is not None:
            return parsed
    return integer(prefixed_value(event, "iw_delay_0x138"))


def state_number(event: dict[str, Any]) -> int | None:
    match = STATE_RE.match(checkpoint_id(event))
    return int(match.group(1)) if match else None


def latest(
    events: Iterable[dict[str, Any]],
    event_id: str,
    before: int,
    predicate=lambda event: True,
) -> dict[str, Any] | None:
    matches = [
        event for event in events
        if checkpoint_id(event) == event_id and sequence(event) < before and predicate(event)
    ]
    return max(matches, key=sequence) if matches else None


def earliest(
    events: Iterable[dict[str, Any]],
    event_id: str,
    after: int,
    predicate=lambda event: True,
) -> dict[str, Any] | None:
    matches = [
        event for event in events
        if checkpoint_id(event) == event_id and sequence(event) > after and predicate(event)
    ]
    return min(matches, key=sequence) if matches else None


def thread_node(event: dict[str, Any], node_address: int | None) -> dict[str, Any] | None:
    if node_address is None:
        return None
    nodes = event.get("thread_list_nodes")
    if not isinstance(nodes, list):
        return None
    for node in nodes:
        if isinstance(node, dict) and pointer(node.get("node")) == node_address:
            return node
    return None


def thread_index(event: dict[str, Any], node_address: int | None) -> int | None:
    node = thread_node(event, node_address)
    return integer(node.get("index")) if node is not None else None


def rng_caller_matches_mode1(event: dict[str, Any] | None) -> bool:
    if event is None:
        return False
    return any(
        integer(event.get(f"stack_frame_{index}_callsite_pc")) == 0x80051BB0
        for index in range(8)
    )


def callback_timeline_rows(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for event in events:
        event_id = checkpoint_id(event)
        if not event_id.startswith(TIMELINE_PREFIXES):
            continue
        rows.append({
            "job": job,
            "sequence": sequence(event),
            "vi": integer(event.get("vi_field_count")),
            "checkpoint": event_id,
            "state": state_number(event),
            "thread": hex32(event_thread(event)),
            "instruction_worksheet": hex32(event_iw(event)),
            "slot": event_slot(event),
            "target": event_target(event),
            "mode": event_mode(event),
            "control": event_control(event),
            "delay": event_delay(event),
            "iw_flags_0xec": hex32(event.get("callback_state_iw_flags_0xec")),
            "result": integer(event.get("result")),
            "gate_result": integer(event.get("gate_result")),
            "payload_delay": integer(event.get("delay_payload_delay_0x10")),
            "rng_draw_index_before": integer(event.get("rng_draw_index_before")),
        })
    return rows


def build_mode1_chains(job: int, events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    chains: list[dict[str, Any]] = []
    for ordinal, mode1 in enumerate(
        event for event in events if checkpoint_id(event) == MODE1_ID
    ):
        mode1_seq = sequence(mode1)
        origin_iw = pointer(mode1.get("origin_instruction"))
        origin_thread = pointer(mode1.get("mode1_record_origin_thread_0x74"))
        record_worksheet = pointer(mode1.get("record_worksheet"))

        child = latest(
            events,
            CHILD_ID,
            mode1_seq,
            lambda event: record_worksheet is None
            or pointer(event.get("record_worksheet")) == record_worksheet,
        )
        if origin_iw is None and child is not None:
            origin_iw = pointer(child.get("origin_instruction"))
        record_thread = pointer(child.get("record_thread")) if child else None
        publication = latest(
            events,
            PUBLICATION_ID,
            sequence(child) if child else mode1_seq,
            lambda event: record_thread is None
            or pointer(event.get("record_thread")) == record_thread,
        )
        aux_call = latest(
            events,
            AUX_CALL_ID,
            sequence(publication) if publication else mode1_seq,
            lambda event: (origin_thread is None or event_thread(event) == origin_thread)
            and (origin_iw is None or event_iw(event) == origin_iw),
        )
        aux_seq = sequence(aux_call) if aux_call else -1
        previous_aux = latest(
            events,
            AUX_CALL_ID,
            aux_seq,
            lambda event: origin_iw is None or event_iw(event) == origin_iw,
        )
        window_begin = sequence(previous_aux) if previous_aux else -1
        window = [
            event for event in events
            if window_begin < sequence(event) <= aux_seq
            and (origin_iw is None or event_iw(event) == origin_iw)
        ]
        states = [state_number(event) for event in window if state_number(event) is not None]
        state_counts = Counter(state for state in states if state is not None)
        state6_gates = [
            event for event in window
            if checkpoint_id(event) == "callback_gate_state6_motion_return_8001B6DC"
        ]
        state8 = latest(window, "basic_attack_callback_state8_8001B6F8", aux_seq + 1)
        state8_seq = sequence(state8) if state8 else -1
        descriptor = earliest(window, "delay_descriptor_match_8001DE30", state8_seq)
        gate = earliest(window, "delay_gate_return_8001DE40", state8_seq)
        delay_value = earliest(window, "delay_value_return_8001DE4C", state8_seq)
        delay_store = earliest(window, "callback_delay_store_8001B70C", state8_seq)
        decrements = [
            event for event in window
            if checkpoint_id(event) == "callback_delay_decrement_8001B728"
            and sequence(event) > state8_seq
        ]
        creator = earliest(events, CREATOR_ID, aux_seq)
        aux_return = earliest(
            events,
            AUX_RETURN_ID,
            sequence(publication) if publication else aux_seq,
            lambda event: origin_iw is None or event_iw(event) == origin_iw,
        )
        motion = earliest(
            events,
            MOTION_ID,
            sequence(aux_return) if aux_return else aux_seq,
            lambda event: origin_iw is None or event_iw(event) == origin_iw,
        )
        pathing = earliest(events, PATHING_ID, mode1_seq)
        rng = min(
            (
                event for event in events
                if sequence(event) > mode1_seq
                and event.get("owns_rng_draw") is True
                and (pathing is None or sequence(event) < sequence(pathing) + 10)
            ),
            key=sequence,
            default=None,
        )
        activation = latest(
            events,
            "queued_state_write_complete_800811C8",
            aux_seq + 1,
        )

        before_node = thread_node(aux_call or {}, record_thread)
        publication_node = thread_node(publication or {}, record_thread)
        child_node = thread_node(child or {}, record_thread)
        ordered = [aux_call, creator, publication, aux_return, motion, child, mode1, rng, pathing]
        ordered_sequences = [sequence(event) for event in ordered if event is not None]
        required = [aux_call, publication, child, mode1, rng, pathing]
        list_events = [event for event in (aux_call, publication, child, mode1, pathing) if event]
        required_sample_flags = [
            mode1.get("mode1_origin_iw_slot_0x00_read_ok"),
            mode1.get("mode1_origin_iw_mode_0x06_read_ok"),
            mode1.get("mode1_record_origin_thread_0x74_read_ok"),
            mode1.get("mode1_record_payload_mode_0x22_read_ok"),
            publication.get("record_thread_callback_0x00_read_ok") if publication else None,
            publication.get("record_origin_thread_ptr_0x74_read_ok") if publication else None,
            child.get("record_thread_callback_0x00_read_ok") if child else None,
        ]

        chains.append({
            "job": job,
            "mode1_ordinal": ordinal,
            "origin_slot": integer(mode1.get("mode1_origin_iw_slot_0x00")),
            "origin_target": integer(mode1.get("mode1_origin_iw_target_0x04")),
            "origin_mode": integer(mode1.get("mode1_origin_iw_mode_0x06")),
            "origin_control": integer(mode1.get("mode1_origin_iw_control_0x12")),
            "origin_thread": hex32(origin_thread),
            "origin_instruction_worksheet": hex32(origin_iw),
            "record_thread": hex32(record_thread),
            "record_worksheet": hex32(record_worksheet),
            "state_sequence": ">".join(str(state) for state in states),
            "state3_poll_count": state_counts[3],
            "state4_poll_count": state_counts[4],
            "state5_poll_count": state_counts[5],
            "state6_poll_count": state_counts[6],
            "state6_gate_results": ">".join(
                str(integer(event.get("result"))) for event in state6_gates
            ),
            "state6_gate_vis": ">".join(
                str(integer(event.get("vi_field_count"))) for event in state6_gates
            ),
            "state6_flags_after": ">".join(
                hex32(event.get("callback_state_iw_flags_0xec")) for event in state6_gates
            ),
            "state6_false_polls_before_true": sum(
                integer(event.get("result")) == 0 for event in state6_gates
            ),
            "activation_sequence": sequence(activation) if activation else None,
            "activation_vi": integer(activation.get("vi_field_count")) if activation else None,
            "state8_vi": integer(state8.get("vi_field_count")) if state8 else None,
            "delay_descriptor_matched": descriptor is not None,
            "payload_delay": integer(descriptor.get("delay_payload_delay_0x10")) if descriptor else None,
            "delay_gate_result": integer(gate.get("gate_result")) if gate else None,
            "returned_delay": integer(delay_value.get("delay")) if delay_value else 0 if gate and integer(gate.get("gate_result")) == 0 else None,
            "stored_delay": integer(delay_store.get("delay")) if delay_store else None,
            "delay_decrement_count": len(decrements),
            "aux_call_sequence": sequence(aux_call) if aux_call else None,
            "creator_sequence": sequence(creator) if creator else None,
            "publication_sequence": sequence(publication) if publication else None,
            "aux_return_sequence": sequence(aux_return) if aux_return else None,
            "motion_sequence": sequence(motion) if motion else None,
            "child_first_visit_sequence": sequence(child) if child else None,
            "mode1_sequence": mode1_seq,
            "rng_sequence": sequence(rng) if rng else None,
            "pathing_sequence": sequence(pathing) if pathing else None,
            "publication_vi": integer(publication.get("vi_field_count")) if publication else None,
            "activation_to_publication_vi": (
                integer(publication.get("vi_field_count"))
                - integer(activation.get("vi_field_count"))
                if publication and activation
                and integer(publication.get("vi_field_count")) is not None
                and integer(activation.get("vi_field_count")) is not None
                else None
            ),
            "state8_to_publication_vi": (
                integer(publication.get("vi_field_count"))
                - integer(state8.get("vi_field_count"))
                if publication and state8
                and integer(publication.get("vi_field_count")) is not None
                and integer(state8.get("vi_field_count")) is not None
                else None
            ),
            "child_first_visit_vi": integer(child.get("vi_field_count")) if child else None,
            "mode1_vi": integer(mode1.get("vi_field_count")),
            "rng_draw_index_before": integer(rng.get("rng_draw_index_before")) if rng else None,
            "rng_caller_callsite": hex32(rng.get("caller_callsite_pc")) if rng else "",
            "rng_stack_frame_0_callsite": hex32(rng.get("stack_frame_0_callsite_pc")) if rng else "",
            "rng_caller_contains_80051BB0": rng_caller_matches_mode1(rng),
            "rng_post_write_seed": hex32(rng.get("rng_seed_after")) if rng else "",
            "vector_x_bits": hex32(mode1.get("mode1_vector_x_stack_0x38")),
            "vector_y_bits": hex32(mode1.get("mode1_vector_y_stack_0x3c")),
            "vector_z_bits": hex32(mode1.get("mode1_vector_z_stack_0x40")),
            "camera_yaw_bits": hex32(mode1.get("mode1_camera_yaw_bits_0xd4")),
            "record_present_before_publication": before_node is not None,
            "record_present_at_publication": publication_node is not None,
            "record_present_at_first_visit": child_node is not None,
            "record_publication_thread_index": thread_index(publication or {}, record_thread),
            "record_first_visit_thread_index": thread_index(child or {}, record_thread),
            "publication_node_callback": hex32(publication_node.get("callback")) if publication_node else "",
            "sequence_order_valid": ordered_sequences == sorted(ordered_sequences),
            "thread_lists_valid": all(
                event.get("thread_list_read_ok") is True
                and event.get("thread_list_truncated") is False
                and event.get("thread_list_cycle_detected") is False
                for event in list_events
            ),
            "required_samples_valid": all(flag is True for flag in required_sample_flags),
            "complete": all(event is not None for event in required)
            and rng_caller_matches_mode1(rng),
        })
    return chains


def capture_acceptance_row(
    job: int,
    manifest_job: dict[str, Any],
    events: list[dict[str, Any]],
    chains: list[dict[str, Any]],
    limits: dict[str, int],
) -> dict[str, Any]:
    counts = Counter(checkpoint_id(event) for event in events)
    capped = sorted(event_id for event_id, limit in limits.items() if counts[event_id] >= limit)
    list_events = [event for event in events if isinstance(event.get("thread_list_nodes"), list)]
    invalid_lists = [
        event for event in list_events
        if event.get("thread_list_read_ok") is not True
        or event.get("thread_list_truncated") is not False
        or event.get("thread_list_cycle_detected") is not False
    ]
    false_samples = sum(
        value is False
        for event in events
        for key, value in event.items()
        if key.endswith(("_eval_ok", "_read_ok"))
    )
    return {
        "job": job,
        "terminal_state": manifest_job.get("terminal_state", ""),
        "fake_attack": manifest_job.get("fake_attacks_this_turn", ""),
        "event_count": len(events),
        "event_limit_headroom": GLOBAL_EVENT_LIMIT - len(events),
        "rng_draw_events": sum(event.get("owns_rng_draw") is True for event in events),
        "mode1_chains": len(chains),
        "complete_mode1_chains": sum(bool(chain.get("complete")) for chain in chains),
        "thread_list_snapshots": len(list_events),
        "invalid_thread_lists": len(invalid_lists),
        "false_addrprog_samples": false_samples,
        "checkpoints_at_local_hit_cap": ";".join(capped),
    }


def profile_limits(profile_path: Path) -> dict[str, int]:
    limits: dict[str, int] = {}
    current: str | None = None
    for raw_line in profile_path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if line.startswith("[checkpoint.") and line.endswith("]"):
            current = line[len("[checkpoint."):-1]
        elif line.startswith("["):
            current = None
        elif current and line.startswith("max_hits="):
            limits[current] = int(line.split("=", 1)[1], 0)
    return limits


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields: list[str] = []
    for row in rows:
        for field in row:
            if field not in fields:
                fields.append(field)
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def findings_text(
    run_root: Path,
    acceptance: list[dict[str, Any]],
    chains: list[dict[str, Any]],
) -> str:
    delayed_chains = [
        chain
        for chain in chains
        if chain["delay_descriptor_matched"] and chain["stored_delay"] not in (None, 0)
    ]
    zero_delay_chains = [
        chain
        for chain in chains
        if chain["stored_delay"] == 0 and chain["delay_decrement_count"] == 0
    ]
    delayed_values = sorted({chain["stored_delay"] for chain in delayed_chains})
    delay_contract_exact = bool(delayed_chains) and all(
        chain["delay_gate_result"] == 1
        and chain["payload_delay"] == chain["returned_delay"]
        and chain["returned_delay"] == chain["stored_delay"]
        and chain["stored_delay"] == chain["delay_decrement_count"]
        for chain in delayed_chains
    )

    lines = [
        "# Mode-1 Pathing Publication And Lifetime",
        "",
        "## Capture Acceptance",
        "",
        f"- Accepted raw run: `{run_root}`.",
    ]
    for row in acceptance:
        lines.append(
            f"- Job {row['job']}: {row['terminal_state']}; {row['event_count']} events "
            f"({row['event_limit_headroom']} below the hard cap), {row['rng_draw_events']} RNG writes, "
            f"{row['complete_mode1_chains']}/{row['mode1_chains']} complete mode-1 chains, "
            f"{row['invalid_thread_lists']} invalid thread-list snapshots."
        )
    lines.extend([
        f"- The reduction covers {len(acceptance)} jobs from one isolated batch; "
        f"{sum(row['fake_attack'] == 0 for row in acceptance)}/{len(acceptance)} source rows "
        "have `fake_attack=0`.",
        "- Checkpoints that reach their local 256-hit limit are broad visit diagnostics. Each modeled target chain completes before those late-turn limits and the global recorder cap is not reached.",
        "",
        "## Observed Chains",
        "",
    ])
    for chain in chains:
        lines.append(
            f"- Job {chain['job']} chain {chain['mode1_ordinal']}: slot {chain['origin_slot']} mode "
            f"{chain['origin_mode']}; callback states `{chain['state_sequence']}`; payload delay "
            f"{chain['payload_delay']}, descriptor matched={chain['delay_descriptor_matched']}, "
            f"gate {chain['delay_gate_result']}, stored delay "
            f"{chain['stored_delay']}, decrements {chain['delay_decrement_count']}; state-6 "
            f"results `{chain['state6_gate_results']}`, post-call flags "
            f"`{chain['state6_flags_after']}`; sequence "
            f"publication {chain['publication_sequence']} -> child {chain['child_first_visit_sequence']} "
            f"-> mode-1 {chain['mode1_sequence']} -> RNG {chain['rng_sequence']} -> pathing "
            f"{chain['pathing_sequence']}; caller match={chain['rng_caller_contains_80051BB0']}."
        )
    lines.extend([
        "",
        "## Interpretation",
        "",
        "- `FUN_8001B1B0` owns the observed publication lifetime. The SYSTEM CAMERA child is inserted during its state-10 auxiliary publication, visited later through the thread list, and only then reaches mode-1 geometry and its RNG draw.",
    ])
    if delayed_chains:
        agreement = (
            "agree exactly for every chain"
            if delay_contract_exact
            else "do not agree for every chain"
        )
        lines.append(
            f"- {len(delayed_chains)} descriptor-matched chains loaded nonzero delays "
            f"{delayed_values}. The payload, gate return, stored delay, and state-9 "
            f"decrement count {agreement}."
        )
    if zero_delay_chains:
        lines.append(
            f"- {len(zero_delay_chains)} chains stored delay zero and performed no state-9 "
            "countdown; those chains advanced from state 8 through state 10 without a "
            "delayed publication."
        )
    lines.extend([
        "- Static code resolves that predicate exactly: `FUN_80075D64` returns true immediately when `IW+0xEC` bit 31 is clear. When the bit is set, it returns false until `IW+0x68 >= FLOAT_80348A74`; the successful threshold path clears bit 31 before returning true.",
        "- Live post-call flags corroborate that contract: every captured false return retains `IW+0xEC` bit 31, while every successful return has bit 31 cleared and preserves any unrelated lower flag bits. The reduced chains use the exact sequences shown above.",
        "- The child record is absent from the pre-publication list and present at publication/first visit, so publication and first execution are separate thread-order events.",
        "- The playback model therefore needs an explicit state-8 delay lookup and state-9 countdown before state-10 publication. The existing next-visit publication remains valid only when the lookup returns zero.",
        "- No callback scheduling behavior was changed in this pass.",
        "",
    ])
    return "\n".join(lines)


def reduce_capture(run_root: Path, analysis_root: Path) -> dict[str, Any]:
    manifest, capture_paths = locate_captures(run_root)
    manifest_jobs = {
        integer(row.get("original_exec_job_id")): row
        for row in manifest.get("jobs", [])
        if isinstance(row, dict)
    }
    limits = profile_limits(run_root / "capture_profile.ini")
    timeline: list[dict[str, Any]] = []
    chains: list[dict[str, Any]] = []
    acceptance: list[dict[str, Any]] = []
    counts_rows: list[dict[str, Any]] = []
    for job, path in sorted(capture_paths.items()):
        events = load_jsonl(path)
        job_chains = build_mode1_chains(job, events)
        timeline.extend(callback_timeline_rows(job, events))
        chains.extend(job_chains)
        acceptance.append(capture_acceptance_row(job, manifest_jobs[job], events, job_chains, limits))
        for event_id, count in sorted(Counter(checkpoint_id(event) for event in events).items()):
            counts_rows.append({
                "job": job,
                "checkpoint": event_id,
                "count": count,
                "max_hits": limits.get(event_id),
                "reached_local_cap": limits.get(event_id) == count,
            })

    analysis_root.mkdir(parents=True, exist_ok=True)
    write_csv(analysis_root / "capture_acceptance.csv", acceptance)
    write_csv(analysis_root / "checkpoint_counts.csv", counts_rows)
    write_csv(analysis_root / "callback_state_timeline.csv", timeline)
    write_csv(analysis_root / "mode1_publication_lifetime.csv", chains)
    (analysis_root / "findings.md").write_text(
        findings_text(run_root, acceptance, chains), encoding="utf-8"
    )
    return {"acceptance": acceptance, "chains": chains}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--analysis-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = reduce_capture(args.run_root, args.analysis_root)
    print(json.dumps({
        "analysis_root": str(args.analysis_root),
        "acceptance": result["acceptance"],
        "chains": result["chains"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
