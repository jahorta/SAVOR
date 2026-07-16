#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reduce_queued_instruction_param_capture as reducer


def write_jsonl(path: Path, events: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(event) + "\n" for event in events),
        encoding="utf-8",
    )


def checkpoint(sequence: int, checkpoint_id: str, **values: object) -> dict[str, object]:
    return {
        "capture_sequence": sequence,
        "checkpoint_id": checkpoint_id,
        **values,
    }


class QueuedInstructionParamReducerTests(unittest.TestCase):
    def test_macro_observations_are_explicitly_untrusted_post_writes(self) -> None:
        rows = reducer.macro_rows(149113, [checkpoint(
            3,
            "memwatch.macro_untrusted_slot4_instr_param_0x06",
            decoded_pc="0x80079838",
            decoded_value="0x000000000000FFFF",
            decoded_memory_value="0x000000000000FFFF",
            memwatch_confirmed_current_instruction=True,
            stack_frame_0_callsite_pc="0x80079838",
            stack_frame_1_callsite_pc="0x800794D8",
        )])

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["slot"], 4)
        self.assertEqual(rows[0]["post_write_value"], -1)
        self.assertEqual(rows[0]["scope"], "macro_untrusted")
        self.assertIn("0x800794D8", rows[0]["stack"])

    def test_reliable_snapshot_and_normal_consumer_define_route(self) -> None:
        events = [
            checkpoint(
                0,
                "setup_action_pc_handler_store_80070A54",
                slot0_queued_instruction_0x00="0x00000003",
                slot0_queued_target_0x04="0x00000004",
                slot0_queued_instr_param_0x06="0x00000001",
                slot0_queued_result_0x08="0x00000000",
                slot0_queued_result_copy_0x09="0x00000000",
                slot0_special_state_0x00="0x00000000",
            ),
            checkpoint(
                1,
                "pc_final_param_consumer_80086F10",
                slot0_queued_instruction_0x00="0x00000003",
                slot0_queued_target_0x04="0x00000004",
                slot0_queued_instr_param_0x06="0x00000000",
                slot0_queued_result_0x08="0x00000001",
                slot0_queued_result_copy_0x09="0x00000001",
                slot0_special_state_0x00="0x00000000",
            ),
        ]

        rows = reducer.attack_param_rows(149113, events)

        self.assertEqual(rows[0]["stage"], "command_selected")
        self.assertEqual(rows[0]["instr_param"], 1)
        self.assertEqual(rows[0]["route"], "")
        self.assertEqual(rows[1]["stage"], "route_consumed")
        self.assertEqual(rows[1]["route"], "DirectMelee")

    def test_state_to_mode_timeline_tracks_setter_and_instruction_publication(self) -> None:
        events = [
            checkpoint(0, "queued_state_setter_entry_80081168", r3="0x00000004", r4="0x00000006"),
            checkpoint(1, "queued_state_write_complete_800811C8", r6="0x00000004", r4="0x00000006"),
            checkpoint(
                2,
                "queued_state_case6_mode8_80021818",
                r6="0x00000008",
                resolver_iw_slot_0x00="0x00000004",
            ),
            checkpoint(
                3,
                "queued_transition_mode_write_complete_8002279C",
                resolver_iw_slot_0x00="0x00000004",
                resolver_iw_mode_0x06="0x00000008",
            ),
        ]

        rows = reducer.state_mode_rows(158364, events)

        self.assertEqual(rows[-1]["slot"], 4)
        self.assertEqual(rows[-1]["queued_state"], 6)
        self.assertEqual(rows[-1]["mapped_mode"], 8)
        self.assertTrue(rows[-1]["mapping_matches"])

    def test_batch_reduction_does_not_require_macro_capture_for_acceptance(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run_root = root / "run"
            output = root / "reduced"
            capture = run_root / "captures" / "job-149113.jsonl"
            events = [
                checkpoint(0, "queued_rows_initialized_80071A68"),
                checkpoint(1, "setup_action_pc_handler_store_80070A54"),
                checkpoint(2, "pc_final_param_consumer_80086F10"),
                checkpoint(3, "attack_result_write_80081C48"),
                checkpoint(4, "queued_state_write_complete_800811C8", r6=0, r4=5),
                checkpoint(5, "queued_transition_mode_write_complete_8002279C", resolver_iw_slot_0x00=0, resolver_iw_mode_0x06=4),
                checkpoint(6, "instruction_thread_visit_80022850"),
            ]
            write_jsonl(capture, events)
            run_root.mkdir(parents=True, exist_ok=True)
            (run_root / "manifest.json").write_text(json.dumps({
                "jobs": [{
                    "original_exec_job_id": 149113,
                    "stable_capture_path": "captures/job-149113.jsonl",
                }]
            }), encoding="utf-8")

            result = reducer.reduce(run_root, output)

            self.assertEqual(result["jobs"], 1)
            self.assertEqual(result["complete_jobs"], 1)
            self.assertEqual(result["macro_rows"], 0)
            findings = (output / "findings.md").read_text(encoding="utf-8")
            self.assertIn("| 149113 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | yes |", findings)
            with (output / "writer_matrix.csv").open(encoding="utf-8", newline="") as source:
                writers = list(csv.DictReader(source))
            self.assertTrue(any(row["writer_pc"] == "0x800811C4" for row in writers))


if __name__ == "__main__":
    unittest.main()
