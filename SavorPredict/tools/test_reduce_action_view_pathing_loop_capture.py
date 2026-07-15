import unittest

from reduce_action_view_pathing_loop_capture import (
    ACTOR_CALL,
    CANDIDATE_CALL,
    CANDIDATE_RETURN,
    FALLBACK,
    OUTER_ENTRY,
    OUTER_RETURN,
    reduce_events,
    score_geometry,
)


def bits(value: float) -> str:
    import struct

    return f"0x{struct.unpack('>I', struct.pack('>f', value))[0]:08X}"


class ReduceActionViewPathingLoopCaptureTests(unittest.TestCase):
    def test_geometry_accepts_collinear_candidate(self):
        result = score_geometry((10.0, 0.0, 0.0), (5.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        self.assertTrue(result.accepted)
        self.assertEqual(result.perpendicular_distance, 0.0)

    def test_reducer_pairs_scan_and_candidate(self):
        events = [
            {"capture_sequence": 1, "checkpoint_id": OUTER_ENTRY},
            {
                "capture_sequence": 2,
                "checkpoint_id": ACTOR_CALL,
                "yaw_iteration": "0x0",
                "actor_slot": "0x0",
                "target_slot": "0x4",
                "excluded_slot": "0x0",
                "outer_input_x": bits(10.0),
                "outer_input_y": bits(0.0),
                "outer_input_z": bits(0.0),
                "outer_path_x": bits(0.0),
                "outer_path_y": bits(0.0),
                "outer_path_z": bits(0.0),
            },
            {
                "capture_sequence": 3,
                "checkpoint_id": CANDIDATE_CALL,
                "candidate_index": "0x2",
                "candidate_slot_0x00": "0x4",
                "candidate_flags_0xec": "0x0",
                "candidate_flags_0xf0": "0x0",
                "candidate_extent_0x15c": bits(15.0),
                "scan_input_x": bits(10.0),
                "scan_input_y": bits(0.0),
                "scan_input_z": bits(0.0),
                "candidate_position_x": bits(5.0),
                "candidate_position_y": bits(0.0),
                "candidate_position_z": bits(0.0),
                "scan_path_x": bits(0.0),
                "scan_path_y": bits(0.0),
                "scan_path_z": bits(0.0),
            },
            {
                "capture_sequence": 4,
                "checkpoint_id": CANDIDATE_RETURN,
                "accepted_return": "0x1",
                "scan_score_after": bits(0.0),
            },
            {"capture_sequence": 5, "checkpoint_id": FALLBACK},
            {"capture_sequence": 6, "checkpoint_id": OUTER_RETURN},
        ]

        scans, candidates, summary = reduce_events(events, 123)
        self.assertEqual(len(scans), 1)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(summary["actor_scans"], 1)
        self.assertEqual(summary["candidate_acceptance_matches"], 1)
        self.assertFalse(scans[0]["modeled_fallback_draw"])


if __name__ == "__main__":
    unittest.main()
