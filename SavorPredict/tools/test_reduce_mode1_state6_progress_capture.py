import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("reduce_mode1_state6_progress_capture.py")
SPEC = importlib.util.spec_from_file_location("mode1_state6_reducer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
REDUCER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REDUCER)


IW = 0x81002000


def event(sequence, checkpoint, frame_window, **values):
    return {
        "capture_sequence": sequence,
        "checkpoint_id": checkpoint,
        "vi_field_count": 100 + frame_window * 2,
        "_frame_window": frame_window,
        "instruction_worksheet": f"0x{IW:08X}",
        **values,
    }


def iw_values(progress, increment=0.2, flags=0x80000000):
    return {
        "motion_iw_iw_slot_0x00": "0x00000001",
        "motion_iw_iw_slot_0x00_address": f"0x{IW:08X}",
        "motion_iw_iw_mode_0x06": "0x00000005",
        "motion_iw_iw_control_0x12": "0x00000006",
        "motion_iw_iw_motion_id_0x64": "0x00000002",
        "motion_iw_iw_motion_progress_0x68": REDUCER.hex32(REDUCER.float_bits(progress)),
        "motion_iw_iw_motion_increment_0x6c": REDUCER.hex32(REDUCER.float_bits(increment)),
        "motion_iw_iw_action_row_0xe4": "0x00000006",
        "motion_iw_iw_flags_0xec": REDUCER.hex32(flags),
        "motion_iw_iw_flags_0xf0": "0x00000000",
    }


def callback_values(progress, result):
    return {
        "callback_state_iw_slot_0x00": "0x00000001",
        "callback_state_iw_slot_0x00_address": f"0x{IW:08X}",
        "callback_state_iw_mode_0x06": "0x00000005",
        "callback_state_iw_control_0x12": "0x00000006",
        "callback_state_iw_motion_id_0x64": "0x00000002",
        "callback_state_iw_motion_progress_0x68": REDUCER.hex32(REDUCER.float_bits(progress)),
        "callback_state_iw_motion_increment_0x6c": REDUCER.hex32(REDUCER.float_bits(0.2)),
        "callback_state_iw_action_row_0xe4": "0x00000006",
        "callback_state_iw_flags_0xec": "0x00000000" if result else "0x80000000",
        "result": REDUCER.hex32(result),
    }


def progress_watch(sequence, frame_window, progress, pc=0x80018F98):
    return event(
        sequence,
        "memwatch.slot1_iw_motion_progress_write",
        frame_window,
        memwatch_confirmed_current_instruction=True,
        memwatch_addr=REDUCER.hex32(IW + 0x68),
        decoded_pc=REDUCER.hex32(pc),
        decoded_memory_value=REDUCER.hex32(REDUCER.float_bits(progress)),
    )


def renderer_complete(sequence, frame_window, progress):
    values = {
        key.replace("motion_iw_", "renderer_iw_", 1): value
        for key, value in iw_values(progress).items()
    }
    return event(
        sequence,
        "motion_renderer_increment_complete_80018F9C",
        frame_window,
        **values,
    )


class Mode1State6ReducerTests(unittest.TestCase):
    def test_reconstructs_exact_five_frame_progress_chain(self):
        events = [
            event(
                1,
                REDUCER.DURATION_ID,
                0,
                selected_action_row_duration_0x10="0x40A00000",
                selected_action_row_frame_step_0x14="0x3F800000",
                **iw_values(12.0, 1.0, 0),
            ),
            event(2, "motion_setup_progress_reset_complete_80076270", 0, **iw_values(0.0)),
            event(3, REDUCER.SET_ID, 0, **iw_values(0.0)),
        ]
        progress = REDUCER.f32_add(0.0, 0.2)
        sequence = 4
        events.append(progress_watch(sequence, 0, progress))
        sequence += 1
        events.append(renderer_complete(sequence, 0, progress))
        sequence += 1
        for frame in range(1, 5):
            events.append(event(sequence, REDUCER.STATE6_ID, frame, **callback_values(progress, 0)))
            sequence += 1
            progress = REDUCER.f32_add(progress, 0.2)
            events.append(progress_watch(sequence, frame, progress))
            sequence += 1
            events.append(renderer_complete(sequence, frame, progress))
            sequence += 1
        events.append(event(sequence, REDUCER.STATE6_ID, 5, **callback_values(progress, 1)))

        writers = REDUCER.writer_rows(149113, events)
        chains, cadence = REDUCER.build_progress_chains(149113, events, writers)

        self.assertEqual(len(chains), 1)
        chain = chains[0]
        self.assertEqual(chain["false_poll_count"], 4)
        self.assertEqual(chain["gate_results"], "0>0>0>0>1")
        self.assertEqual(chain["updates_per_false_poll"], "1>1>1>1")
        self.assertTrue(chain["expected_increment_matches"])
        self.assertTrue(chain["progression_exact"])
        self.assertTrue(chain["false_flags_preserved"])
        self.assertTrue(chain["true_flag_cleared"])
        self.assertTrue(chain["consecutive_gate_frames"])
        self.assertTrue(chain["complete"])
        self.assertEqual(len(cadence), 11)

    def test_marks_unknown_progress_writer(self):
        watch = progress_watch(1, 0, 0.25, pc=0x80001234)

        rows = REDUCER.writer_rows(149113, [watch])

        self.assertEqual(rows[0]["classification"], "unresolved_progress_writer")
        self.assertEqual(rows[0]["instruction_worksheet"], REDUCER.hex32(IW))

        root_watch = dict(watch)
        root_watch["checkpoint_id"] = "memwatch.root2_iw_motion_progress_write"
        self.assertTrue(REDUCER.is_relevant(root_watch["checkpoint_id"]))
        self.assertEqual(REDUCER.writer_rows(149113, [root_watch])[0]["root_index"], 2)

    def test_preserves_queued_state_publication_context(self):
        raw = event(
            12,
            REDUCER.QUEUED_STATE_ID,
            7,
            r4="0x00000006",
            r5="0x80309730",
            r28="0x81001000",
            r29="0x00000000",
            r30="0x00000000",
            r31="0x81002000",
            active_actor_slot_80347334="0x00000000",
        )
        projected = REDUCER.project_event(raw, 7)

        row = REDUCER.queued_state_row(158364, projected)

        self.assertEqual(row["queued_state"], 6)
        self.assertEqual(row["active_actor_slot"], 0)
        self.assertEqual(row["state_array"], "0x80309730")
        self.assertEqual(row["r28"], "0x81001000")

    def test_preserves_serialized_publication_origin_and_payload(self):
        raw = event(
            20,
            "serialized_action_view_publication_8003C738",
            9,
            record_origin_iw_slot_0x00="0x00000000",
            record_origin_iw_slot_0x00_address=f"0x{IW:08X}",
            record_origin_iw_target_0x04="0x00000005",
            record_origin_iw_mode_0x06="0x00000008",
            record_origin_iw_staged_mode_0x0a="0x00000002",
            record_origin_iw_previous_mode_0x1c="0x00000006",
            record_payload_primary_0x00="0x00000008",
            record_payload_flags_0x10="0x80000000",
            record_payload_mode_0x22="0x00000000",
        )
        projected = REDUCER.project_event(raw, 9)

        row = REDUCER.normalized_event(158364, projected)

        self.assertEqual(row["slot"], 0)
        self.assertEqual(row["target_slot"], 5)
        self.assertEqual(row["mode"], 8)
        self.assertEqual(row["previous_mode"], 6)
        self.assertEqual(row["payload_primary"], 8)
        self.assertEqual(row["payload_mode"], 0)
        self.assertEqual(row["payload_flags"], "0x80000000")


if __name__ == "__main__":
    unittest.main()
