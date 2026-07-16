#!/usr/bin/env python3

from __future__ import annotations

import json
import sqlite3
import sys
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reduce_visual_publication_order_capture as reducer


def list_event(
    sequence: int,
    checkpoint: str,
    nodes: tuple[str, ...],
    **values: object,
) -> dict[str, object]:
    return {
        "capture_sequence": sequence,
        "checkpoint_hit_count": values.pop("checkpoint_hit_count", 0),
        "checkpoint_id": checkpoint,
        "thread_list_read_ok": True,
        "thread_list_truncated": False,
        "thread_list_cycle_detected": False,
        "thread_list_nodes": [
            {"node": node, "callback": "0x80051264"}
            for node in nodes
        ],
        **values,
    }


def rng_event(
    sequence: int,
    seed: str,
    caller: str,
    stack: tuple[str, ...],
    hit: int = 0,
) -> dict[str, object]:
    event: dict[str, object] = {
        "capture_sequence": sequence,
        "checkpoint_hit_count": hit,
        "checkpoint_id": reducer.RNG_WATCH_ID,
        "owns_rng_draw": True,
        "rng_seed_after": seed,
        "caller_callsite_pc": caller,
        "stack_frame_count": len(stack),
    }
    for index, callsite in enumerate(stack):
        event[f"stack_frame_{index}_callsite_pc"] = callsite
    return event


def write_jsonl(path: Path, events: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(event) + "\n" for event in events),
        encoding="utf-8",
    )


def producer_chain_events() -> list[dict[str, object]]:
    old_nodes = ("0x81000000", "0x81000080")
    new_nodes = old_nodes + ("0x81001000",)
    return [
        list_event(0, reducer.FRAME_ID, old_nodes),
        {
            "capture_sequence": 1,
            "checkpoint_hit_count": 0,
            "checkpoint_id": "visual_probe_callsite_8001AE98",
            "origin_thread": "0x81000080",
        },
        list_event(
            2,
            "visual_probe_entry_800086BC",
            old_nodes,
            origin_thread="0x81000080",
        ),
        list_event(
            3,
            "aux_dispatch_call_800084C8",
            old_nodes,
            command_id="0x0000000A",
            origin_thread="0x81000080",
        ),
        list_event(
            4,
            "command_dispatch_entry_800367E8",
            old_nodes,
            command_id="0x0000000A",
            origin_thread="0x81000080",
        ),
        list_event(
            5,
            "command_handler_call_80036864",
            old_nodes,
            handler="0x8003C690",
            origin_thread="0x81000080",
        ),
        list_event(
            6,
            "serialized_action_view_creator_entry_8003C690",
            old_nodes,
            origin_thread="0x81000080",
        ),
        list_event(
            7,
            reducer.PUBLICATION_ID,
            new_nodes,
            record_thread="0x81001000",
            origin_thread="0x81000080",
            record_origin_iw_slot_0x00="0x00000004",
            record_origin_iw_mode_0x06="0x00000008",
            record_payload_mode_0x22="0x00000001",
        ),
        list_event(
            8,
            reducer.CHILD_STATE0_ID,
            new_nodes,
            record_thread="0x81001000",
        ),
        list_event(
            9,
            reducer.MODE1_ID,
            new_nodes,
            record_thread="0x81001000",
        ),
        list_event(10, reducer.MODE1_GEOMETRY_ID, new_nodes),
    ]


class VisualPublicationReducerTests(unittest.TestCase):
    def test_generation_delta_reports_first_changed_visual_event(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            left = root / "left"
            right = root / "right"
            left.mkdir()
            right.mkdir()
            common = {
                "phase": "setup",
                "label": "runtime_initialized",
                "frame_index": 0,
                "draws_consumed": 0,
            }
            left_visual = {
                "phase": "movement",
                "label": "visual_instruction_installed",
                "frame_index": 10,
                "actor_slot": 0,
                "action_ordinal": 0,
                "draws_consumed": 0,
                "detail": "instruction_revision=1; visual_command_kind=system_camera",
            }
            right_visual = {**left_visual, "frame_index": 11}
            for directory, visual, encoding in (
                (left, left_visual, "utf-8"),
                (right, right_visual, "utf-16"),
            ):
                (directory / "job_149113.json").write_text(json.dumps({
                    "prediction": {
                        "outcome": "Provisional",
                        "starting_rng_seed": 123,
                        "final_rng_seed": 456,
                        "total_draws_consumed": 0,
                        "initial_turn_type": 0,
                        "start_boundary": "captured_turn_start",
                        "events": [common, visual],
                    }
                }), encoding=encoding)

            rows = reducer.predictor_delta_rows(
                [149113], (("left", left), ("right", right))
            )

            visual = next(row for row in rows if row["scope"] == "visual_and_rng")
            self.assertEqual(visual["difference_kind"], "changed")
            self.assertEqual(visual["lhs_frame"], 10)
            self.assertEqual(visual["rhs_frame"], 11)
            self.assertTrue(visual["initialization_fingerprint_equal"])
            self.assertTrue(visual["terminal_draw_count_equal"])
            self.assertTrue(visual["terminal_seed_equal"])
            self.assertEqual(visual["coordinator_state"], "equal")
            self.assertEqual(visual["source_snapshot"], "not_recorded")
            self.assertEqual(visual["positions"], "not_recorded")
            rng = next(row for row in rows if row["scope"] == "rng_only")
            self.assertEqual(rng["difference_kind"], "equal")

    def test_publication_chain_and_thread_insertion_are_unique(self) -> None:
        events = producer_chain_events()

        chains = reducer.publication_chains(149113, events)
        insertions = reducer.thread_insertion_rows(149113, events, chains)

        self.assertEqual(len(chains), 1)
        self.assertTrue(chains[0]["producer_unique"])
        self.assertTrue(chains[0]["child_lifetime_identified"])
        self.assertTrue(chains[0]["mode1_lifetime_identified"])
        self.assertTrue(chains[0]["mode1_geometry_identified"])
        self.assertEqual(
            chains[0]["producer_callsite"], "visual_probe_callsite_8001AE98"
        )
        self.assertEqual(chains[0]["handler"], "0x8003C690")
        self.assertTrue(insertions[0]["record_added"])
        self.assertEqual(insertions[0]["publication_thread_index"], 2)
        self.assertEqual(insertions[0]["child_thread_index"], 2)

    def test_rng_alignment_uses_seed_and_complete_caller_stack(self) -> None:
        accepted = [
            rng_event(0, "0x11111111", "0x800145C8", ("0x800145C8", "0x800139F8")),
            rng_event(1, "0x22222222", "0x80010BDC", ("0x80010BDC", "0x80081BE4"), 1),
            rng_event(2, "0x33333333", "0x80010C44", ("0x80010C44", "0x80081BE4"), 2),
        ]
        captured = [
            rng_event(0, "0x22222222", "0x80010BDC", ("0x80010BDC", "0x80081BE4")),
            rng_event(1, "0x33333333", "0x80010C44", ("0x80010C44", "0x80081BE4"), 1),
        ]

        self.assertEqual(reducer.match_rng_trajectory(captured, accepted), (True, 1))
        captured[1]["stack_frame_1_callsite_pc"] = "0x80000000"
        self.assertEqual(reducer.match_rng_trajectory(captured, accepted), (False, None))

    def test_parallel_acceptance_checks_batch_isolation_and_overlap(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "db").mkdir()
            with closing(sqlite3.connect(root / "db" / "execution.db")) as connection:
                connection.execute(
                    "CREATE TABLE exec_job (job_id INTEGER PRIMARY KEY, started_at_utc INTEGER, ended_at_utc INTEGER)"
                )
                connection.executemany(
                    "INSERT INTO exec_job VALUES (?, ?, ?)",
                    ((9001, 100, 300), (9002, 200, 400)),
                )
                connection.commit()
            for worker in (0, 1):
                (root / ".worker-runtime" / f"workflow-worker-{worker}" / "User").mkdir(parents=True)
                (root / ".worker-binary-runtime" / "build" / f"slot-{worker}").mkdir(parents=True)

            captures: dict[int, tuple[Path, dict[str, object]]] = {}
            accepted: dict[int, tuple[Path, dict[str, object]]] = {}
            capture_events: dict[int, list[dict[str, object]]] = {}
            jobs: list[dict[str, object]] = []
            manifest_events: list[str] = []
            for worker, (job, clone) in enumerate(((149113, 9001), (453748, 9002))):
                stack = ("0x80010BDC", "0x80081BE4", "0x80082200")
                events = [
                    list_event(0, reducer.FRAME_ID, (f"0x8100{worker}000",)),
                    rng_event(1, f"0x{job:08X}", "0x80010BDC", stack),
                ]
                capture = root / "captures" / f"job-{clone}.jsonl"
                trace = root / "traces" / f"job-{clone}.txt"
                accepted_capture = root / "accepted" / f"job-{job}.jsonl"
                write_jsonl(capture, events)
                write_jsonl(
                    accepted_capture,
                    [rng_event(0, "0x11111111", "0x800145C8", ("0x800145C8",))]
                    + [events[1]],
                )
                trace.parent.mkdir(parents=True, exist_ok=True)
                trace.write_text("trace\n", encoding="utf-8")
                row = {
                    "original_exec_job_id": job,
                    "cloned_exec_job_id": clone,
                    "stable_capture_path": str(capture),
                    "trace_report_path": str(trace),
                    "terminal_state": "SUCCEEDED",
                    "timed_out": False,
                    "source_fake_attacks_this_turn": 0,
                    "fake_attacks_this_turn": 0,
                }
                captures[job] = (capture, row)
                accepted[job] = (accepted_capture, row)
                capture_events[job] = events
                jobs.append(row)
                manifest_events.extend((
                    f"[seedprobe-dispatch] job={clone} worker={worker}",
                    f"[battle-single-turn-result-diagnostics] job={clone} battle_outcome=Reached Next Turn(6)",
                ))

            manifest = {
                "command": "run-battle-jobs",
                "sandbox_mode": "minimal",
                "worker_count": 2,
                "override_start_rng_seed": None,
                "override_fake_attacks_this_turn": None,
                "requested_runs": [
                    {
                        "exec_job_id": row["original_exec_job_id"],
                        "override_start_rng_seed": None,
                        "override_fake_attacks_this_turn": None,
                    }
                    for row in jobs
                ],
                "std_json_cache": {
                    "resolved_std_json_dir": r"D:\SavorPredictDB\.std_json",
                    "used_explicit_dir": True,
                    "available": True,
                    "generation_attempted": False,
                },
                "jobs": jobs,
                "events": manifest_events,
            }
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            acceptance = reducer.parallel_acceptance(
                manifest_path, manifest, captures, accepted, capture_events
            )

            boolean_results = {
                key: value for key, value in acceptance.items() if isinstance(value, bool)
            }
            self.assertTrue(all(boolean_results.values()), boolean_results)
            self.assertEqual(acceptance["rng_accepted_offsets"], {149113: 1, 453748: 1})


if __name__ == "__main__":
    unittest.main()
