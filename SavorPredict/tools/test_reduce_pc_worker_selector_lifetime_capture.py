#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name(
    "reduce_pc_worker_selector_lifetime_capture.py"
)
SPEC = importlib.util.spec_from_file_location("pc_worker_reducer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PcWorkerSelectorReducerTests(unittest.TestCase):
    def test_selector_truth_table_classifies_distance_fallback(self) -> None:
        events = [
            {
                "capture_sequence": 1,
                "checkpoint_id": "pc_selector_entry_800855AC",
                "actor_slot": "0x0",
                "slot0_queued_target_0x04": "0x4",
                "slot0_queued_instr_param_0x06": "0x0",
                "slot0_movement_path_node0_x": "0x4",
                "slot0_movement_path_node0_z": "0x5",
                "slot0_movement_path_node1_x": "0xff",
                "slot0_movement_path_node1_z": "0xff",
            },
            {
                "capture_sequence": 2,
                "checkpoint_id": "pc_selector_reachability_return_80085670",
                "target_slot": "0x4",
                "result": "0x4",
            },
            {
                "capture_sequence": 3,
                "checkpoint_id": "pc_selector_path_shape_return_80085680",
                "result": "0x0",
            },
            {
                "capture_sequence": 4,
                "checkpoint_id": "pc_selector_distance_loaded_8008569C",
                "distance": "0x5",
            },
            {
                "capture_sequence": 5,
                "checkpoint_id": "pc_selector_fallback_branch_800856B0",
            },
            {
                "capture_sequence": 6,
                "checkpoint_id": "pc_selector_return_80085708",
                "result": "0x1",
                "slot0_queued_instr_param_0x06": "0x1",
            },
            {
                "capture_sequence": 7,
                "checkpoint_id": "handle_pc_fallback_publish_complete_80086F70",
            },
        ]
        rows = MODULE.selector_rows(events, 153108)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["decision"], "fallback")
        self.assertEqual(rows[0]["expected_decision"], "fallback")
        self.assertEqual(rows[0]["fallback_reasons"], "distance_gt_4")
        self.assertEqual(rows[0]["selector_path"], "4:5|FF")
        self.assertTrue(rows[0]["truth_table_match"])
        self.assertEqual(rows[0]["published_callback"], "0x80085CE0")

    def test_frame_slot_mapping_uses_current_root_pointer(self) -> None:
        frame = {
            "slot0_movement_thread_ptr": "0x81000000",
            "thread_list_nodes": [
                {"node": "0x80000000", "callback": "0x8000A2FC", "state": "0"},
                {"node": "0x81000000", "callback": "0x80085CE0", "state": "0xB"},
            ],
        }
        index, node = MODULE.frame_slot_node(frame, 0)
        self.assertEqual(index, 1)
        self.assertIsNotNone(node)
        self.assertEqual(node["callback"], "0x80085CE0")

    def test_active_lifetime_uses_register_rooted_slot(self) -> None:
        events = [
            {
                "capture_sequence": 10,
                "checkpoint_id": "pc_fallback_worker_entry_80085CE0",
                "thread": "0x81000100",
                "slot0_movement_thread_ptr": "0x81000000",
                "slot1_movement_thread_ptr": "0x81000100",
                "worker_iw_slot_0x00": "0x0",
                "worker_thread_state_0x19": "0xF",
                "worker_thread_callback_0x00": "0x80085CE0",
            }
        ]
        rows = MODULE.active_lifetime_rows(events, 1)
        self.assertEqual(rows[0]["slot"], 1)
        self.assertEqual(rows[0]["thread_state"], 0xF)


if __name__ == "__main__":
    unittest.main()
