#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reduce_action_view_service_lifecycle_capture as reducer


def frame(sequence: int, *threads: str) -> dict[str, object]:
    return {
        "capture_sequence": sequence,
        "checkpoint_id": reducer.FRAME_ID,
        "thread_list_read_ok": True,
        "thread_list_truncated": False,
        "thread_list_cycle_detected": False,
        "thread_list_nodes": [
            {"node": thread, "callback": "0x80051264"}
            for thread in threads
        ],
    }


class LifecycleReducerTests(unittest.TestCase):
    def test_adjacent_mode0e_checkpoint_does_not_override_unrelated_stack(self) -> None:
        events = [
            {
                "capture_sequence": 1,
                "checkpoint_id": "action_view_record_mode0e_call_800514C8",
            },
            {
                "capture_sequence": 2,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "caller_callsite_pc": "0x80042EB8",
                "stack_frame_count": 1,
                "stack_frame_0_callsite_pc": "0x80042EB8",
            },
        ]

        rows = reducer.rng_window_rows(158364, events)

        self.assertEqual(rows[0]["source"], "other")
        self.assertEqual(rows[0]["source_pc"], "0x80042EB8")

    def test_overlapping_record_publications_do_not_truncate_each_other(self) -> None:
        events = [
            frame(1),
            {
                "capture_sequence": 2,
                "checkpoint_id": "serialized_action_view_publication_8003C738",
                "record_thread": "0x81001000",
            },
            {
                "capture_sequence": 3,
                "checkpoint_id": "synthetic_action_view_publication_800540BC",
                "record_thread": "0x81002000",
            },
            {
                "capture_sequence": 4,
                "checkpoint_id": "action_view_record_normal_completion_80051698",
                "record_thread": "0x81001000",
            },
            {
                "capture_sequence": 5,
                "checkpoint_id": "action_view_record_state3_completion_800516BC",
                "record_thread": "0x81002000",
            },
            frame(6),
        ]

        rows = reducer.action_view_record_rows(149113, events)

        self.assertEqual(len(rows), 2)
        self.assertEqual([row["completion_sequence"] for row in rows], [4, 5])
        self.assertTrue(all(row["complete"] for row in rows))

    def test_reduces_record_service_and_rng_lifetimes(self) -> None:
        events = [
            frame(10),
            {
                "capture_sequence": 12,
                "checkpoint_id": "serialized_action_view_publication_8003C738",
                "record_thread": "0x81001000",
                "record_origin_thread_ptr_0x74": "0x81002000",
                "record_origin_iw_slot_0x00": "0x00000004",
                "record_payload_mode_0x22": "0x00000000",
            },
            {
                "capture_sequence": 14,
                "checkpoint_id": "action_view_record_entry_80051264",
                "record_thread": "0x81001000",
            },
            frame(15, "0x81001000"),
            {
                "capture_sequence": 16,
                "checkpoint_id": "action_view_record_mode0_draw_800513D4",
                "record_thread": "0x81001000",
                "record_effective_mode_0x112": "0x0000000E",
            },
            {
                "capture_sequence": 17,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "rng_draw_index_before": 4,
                "rng_seed_before": "0x12345678",
                "caller_callsite_pc": "0x80051320",
                "stack_frame_count": 1,
                "stack_frame_0_callsite_pc": "0x80051320",
            },
            {
                "capture_sequence": 18,
                "checkpoint_id": "action_view_record_mode0e_call_800514C8",
                "record_thread": "0x81001000",
            },
            {
                "capture_sequence": 19,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "rng_draw_index_before": 5,
                "rng_seed_before": "0x23456789",
                "caller_callsite_pc": "0x80052BEC",
                "stack_frame_count": 2,
                "stack_frame_0_callsite_pc": "0x80052BEC",
                "stack_frame_1_callsite_pc": "0x800514C8",
            },
            {
                "capture_sequence": 20,
                "checkpoint_id": "action_view_record_normal_completion_80051698",
                "record_thread": "0x81001000",
            },
            frame(25),
            {
                "capture_sequence": 30,
                "checkpoint_id": reducer.SERVICE_PUBLICATION,
                "service_thread": "0x81003000",
                "service_origin_thread_ptr_0x08": "0x81002000",
                "service_origin_iw_slot_0x00": "0x00000004",
                "service_command_mode_0x12": "0x00000002",
                "service_command_subtype_0x14": "0x00000001",
                "service_derived_mode_0x02": "0x00000003",
            },
            {
                "capture_sequence": 32,
                "checkpoint_id": "action_service_entry_8004281C",
                "service_thread": "0x81003000",
            },
            {
                "capture_sequence": 33,
                "checkpoint_id": "action_service_delay_80042938",
                "service_thread": "0x81003000",
                "service_thread_state_0x19": "0x00000001",
                "service_derived_mode_0x02": "0x00000004",
                "service_derived_subtype_0x04": "0x0000FFFF",
                "service_selected_slot_0x06": "0x00000005",
                "service_selected_target_thread_ptr_0x0c": "0x81004000",
                "service_selected_target_iw_slot_0x00": "0x00000005",
            },
            {
                "capture_sequence": 34,
                "checkpoint_id": reducer.SERVICE_NESTED_CALL,
                "service_thread": "0x81003000",
            },
            frame(35, "0x81003000"),
            {
                "capture_sequence": 36,
                "checkpoint_id": "eb4c_entry_8002EB4C",
            },
            {
                "capture_sequence": 38,
                "checkpoint_id": "eb4c_fallback_draw_8002EBDC",
                "eb4c_target_iw_slot_0x00": "0x00000005",
            },
            {
                "capture_sequence": 39,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "rng_draw_index_before": 5,
                "rng_seed_before": "0xABCDEF01",
                "caller_callsite_pc": "0x8002EBDC",
                "stack_frame_count": 2,
                "stack_frame_0_callsite_pc": "0x8002EBDC",
                "stack_frame_1_callsite_pc": "0x80042990",
            },
            {
                "capture_sequence": 42,
                "checkpoint_id": "eb4c_publication_call_8002EC2C",
                "selected_mode_r29": "0x00000003",
                "eb4c_target_iw_slot_0x00": "0x00000005",
            },
            {
                "capture_sequence": 45,
                "checkpoint_id": "action_service_cleanup_commit_80042A14",
                "service_thread": "0x81003000",
            },
            frame(50),
        ]

        records = reducer.action_view_record_rows(149113, events)
        services = reducer.action_service_rows(149113, events)
        rng = reducer.rng_window_rows(149113, events)

        self.assertEqual(len(records), 1)
        self.assertTrue(records[0]["complete"])
        self.assertEqual(records[0]["callback_entry_hits"], 1)
        self.assertEqual(records[0]["frame_presence_count"], 1)
        self.assertEqual(records[0]["mode0_draw_calls"], 1)
        self.assertEqual(records[0]["mode0e_rng_draws"], 1)
        self.assertEqual(records[0]["effective_mode"], "0x0000000E")
        self.assertEqual(records[0]["previous_frame_ordinal"], 0)
        self.assertEqual(records[0]["completion_next_frame_ordinal"], 2)

        self.assertEqual(len(services), 1)
        self.assertTrue(services[0]["complete"])
        self.assertEqual(services[0]["nested_calls"], 1)
        self.assertEqual(services[0]["frame_presence_count"], 1)
        self.assertEqual(services[0]["eb4c_entries"], 1)
        self.assertEqual(services[0]["eb4c_fallback_draw_calls"], 1)
        self.assertEqual(services[0]["eb4c_selected_mode"], "0x00000003")
        self.assertEqual(services[0]["service_command_mode_0x12"], "0x00000002")
        self.assertEqual(services[0]["service_derived_mode_0x02"], "0x00000004")
        self.assertEqual(services[0]["selected_target_slot"], 5)
        self.assertEqual(services[0]["initialized_sequence"], 33)
        self.assertEqual(services[0]["nested_call_sequence"], 34)
        self.assertEqual(services[0]["eb4c_publication_sequence"], 42)

        self.assertEqual([row["source"] for row in rng], [
            "action_view_record_mode0",
            "action_view_record_mode0e",
            "action_service_eb4c_fallback",
        ])
        self.assertEqual(rng[2]["post_write_seed"], "0xABCDEF01")

    def test_manifest_and_main_outputs_are_durable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            capture = root / "capture.jsonl"
            events = [frame(1), frame(2)]
            capture.write_text(
                "".join(json.dumps(event) + "\n" for event in events),
                encoding="utf-8",
            )
            (root / "manifest.json").write_text(json.dumps({
                "jobs": [{
                    "original_exec_job_id": 158364,
                    "stable_capture_path": str(capture),
                    "terminal_state": "SUCCEEDED",
                    "fake_attacks_this_turn": 0,
                }]
            }), encoding="utf-8")
            output = root / "analysis"
            old_argv = sys.argv
            try:
                sys.argv = ["reducer", "--run", str(root), "--output", str(output)]
                self.assertEqual(reducer.main(), 0)
            finally:
                sys.argv = old_argv

            expected = {
                "controller_selector_timeline.csv",
                "action_view_record_lifetimes.csv",
                "action_service_lifetimes.csv",
                "rng_invocation_windows.csv",
                "capture_quality.csv",
                "findings.md",
            }
            self.assertEqual({path.name for path in output.iterdir()}, expected)
            with (output / "capture_quality.csv").open(newline="", encoding="utf-8") as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(rows[0]["quality_status"], "complete")
            self.assertEqual(rows[0]["fake_attacks_this_turn"], "0")


if __name__ == "__main__":
    unittest.main()
