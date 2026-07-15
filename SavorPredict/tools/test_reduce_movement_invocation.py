#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reduce_movement_destination_stop_capture as reducer


def frame(
    sequence: int,
    callback: int,
    *,
    include_payload: bool = True,
) -> dict[str, object]:
    nodes: list[dict[str, object]] = []
    if include_payload:
        nodes.append({
            "index": 3,
            "callback": f"0x{callback:08X}",
            "payload_word_read_ok": True,
            "payload_word": "0x81001000",
        })
    return {
        "capture_sequence": sequence,
        "checkpoint_id": reducer.FRAME_ID,
        "slot0_movement_worksheet_ptr": "0x81001000",
        "thread_list_read_ok": True,
        "thread_list_truncated": False,
        "thread_list_cycle_detected": False,
        "thread_list_node_count": len(nodes),
        "thread_list_nodes": nodes,
    }


def invocation_fixture(include_payload: bool = True) -> list[dict[str, object]]:
    return [
        frame(10, 0x80086308, include_payload=include_payload),
        {
            "capture_sequence": 11,
            "checkpoint_id": "movement_reachability_entry_80083728",
            "actor_slot": 0,
            "target_slot": 4,
        },
        {
            "capture_sequence": 12,
            "checkpoint_id": "movement_commit_callsite_80086480",
            "pc": "0x80086480",
            "slot": 0,
            "action_sequence_80347335": 2,
            "active_actor_slot_80347334": 0,
        },
        frame(13, 0x80086308, include_payload=include_payload),
    ]


class InvocationWindowTests(unittest.TestCase):
    def test_unique_payload_mapping_and_frame_bracketing(self) -> None:
        rows = reducer.invocation_windows(147896, invocation_fixture())
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertTrue(row["frame_bracketed"])
        self.assertTrue(row["unique_payload_to_slot_mapping"])
        self.assertEqual(row["previous_thread_index"], 3)
        self.assertEqual(row["controller_family"], "ActivePcDirect")
        self.assertEqual(row["semantic_target_slot"], 4)
        self.assertTrue(row["callback_transition_matches_family"])

    def test_missing_payload_mapping_stays_unknown(self) -> None:
        rows = reducer.invocation_windows(
            147896, invocation_fixture(include_payload=False)
        )
        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]["unique_payload_to_slot_mapping"])
        self.assertEqual(rows[0]["assignment_status"], "Unknown")


class SemanticSegmentTests(unittest.TestCase):
    def test_predictor_segments_use_invocation_metadata_and_leg(self) -> None:
        predictor = {
            "events": [
                {
                    "phase": "frame_scheduler",
                    "movement_controller_family": "ActivePcDirect",
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "frame_index": 5,
                    "detail": "step_kind=MovementCommit",
                },
                {
                    "phase": "frame_scheduler",
                    "movement_controller_family": "ActivePcDirect",
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "frame_index": 6,
                    "detail": (
                        "step_kind=MoveIncrementApply_80061340; "
                        "combatant_cur_pos_0x1c=(0,0,0)->(2,0,-1)"
                    ),
                },
            ]
        }
        segments = reducer.predictor_semantic_segments(predictor)
        key = (1, 0, "ActivePcDirect", 4, 0)
        self.assertIn(key, segments)
        self.assertEqual(segments[key][0]["delta"], (2.0, 0.0, -1.0))

    def test_matched_semantic_pair_requires_vectors_terminal_and_count(self) -> None:
        key = (0, 0, "ActivePcDirect", 4, 0)
        segment = [{
            "frame": 10,
            "position": (15.0, 0.0, -15.0),
            "delta": (2.5, 0.0, -2.5),
            "speed": 3.5355339,
        }]
        rows = reducer.compare_semantic_segment_maps(
            147896,
            {key: segment},
            {key: [dict(segment[0], frame=3)]},
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], "Matched")
        self.assertTrue(rows[0]["fully_matches_after_onset_alignment"])

    def test_unattributed_segment_is_reported_unpaired(self) -> None:
        key = (0, 1, "AmbientFormation", 4, 0)
        rows = reducer.compare_semantic_segment_maps(
            147896,
            {key: [{
                "frame": 10,
                "position": (1.0, 0.0, 1.0),
                "delta": (1.0, 0.0, 1.0),
                "speed": 1.4142,
            }]},
            {},
        )
        self.assertEqual(rows[0]["status"], "Unpaired")


class AffectedTargetReactionTests(unittest.TestCase):
    def test_reaction_lifetime_is_separate_and_action_bounded(self) -> None:
        live = frame(20, 0x8008D3B0)
        live.pop("slot0_movement_worksheet_ptr")
        live["action_sequence_80347335"] = 1
        live["slot4_movement_worksheet_ptr"] = "0x81001000"
        predictor = {
            "events": [
                {
                    "phase": "frame_scheduler",
                    "label": "passive_family_selection",
                    "movement_controller_family": "AffectedTargetReaction",
                    "movement_relation_route": "AffectedTarget1",
                    "action_ordinal": 1,
                    "actor_slot": 4,
                    "frame_index": 7,
                },
                {
                    "phase": "frame_scheduler",
                    "label": "passive_completion_deferred",
                    "movement_controller_family": "AffectedTargetReaction",
                    "movement_relation_route": "AffectedTarget1",
                    "action_ordinal": 1,
                    "actor_slot": 4,
                    "frame_index": 8,
                },
                {
                    "phase": "frame_scheduler",
                    "label": "action_complete",
                    "action_ordinal": 1,
                    "actor_slot": -1,
                    "frame_index": 9,
                },
            ]
        }

        rows = reducer.affected_target_reaction_rows(147896, [live], predictor)

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["pairing_status"], "Paired")
        self.assertTrue(rows[0]["live_action_bounded"])
        self.assertTrue(rows[0]["predictor_waited_for_result"])
        self.assertTrue(rows[0]["predictor_action_bounded"])


if __name__ == "__main__":
    unittest.main()
