import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("reduce_mode1_pathing_lifetime_capture.py")
SPEC = importlib.util.spec_from_file_location("mode1_reducer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
REDUCER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REDUCER)


def event(sequence, checkpoint, **values):
    return {
        "capture_sequence": sequence,
        "checkpoint_id": checkpoint,
        "vi_field_count": 100 + sequence,
        **values,
    }


class Mode1ReducerTests(unittest.TestCase):
    def test_links_callback_publication_child_draw_and_pathing(self):
        thread = "0x81001000"
        iw = "0x81002000"
        record_thread = "0x81003000"
        record_worksheet = "0x81003100"
        before_list = []
        after_list = [{"index": 7, "node": record_thread, "callback": "0x80051264"}]
        common = {
            "thread": thread,
            "instruction_worksheet": iw,
            "callback_state_iw_slot_0x00": "0x00000001",
            "callback_state_iw_mode_0x06": "0x00000004",
        }
        events = [
            event(1, "callback_gate_state6_motion_return_8001B6DC", result="0x00000000", callback_state_iw_flags_0xec="0x80000040", **common),
            event(2, "callback_gate_state6_motion_return_8001B6DC", result="0x00000001", callback_state_iw_flags_0xec="0x00000040", **common),
            event(3, "basic_attack_callback_state8_8001B6F8", **common),
            event(4, "delay_descriptor_match_8001DE30", instruction_worksheet=iw, delay_payload_delay_0x10="0x0000000D"),
            event(5, "delay_gate_return_8001DE40", instruction_worksheet=iw, gate_result="0x00000000"),
            event(6, "callback_delay_store_8001B70C", delay="0x00000000", **common),
            event(7, "basic_attack_callback_state9_8001B718", **common),
            event(8, "basic_attack_callback_state10_8001B738", **common),
            event(9, REDUCER.AUX_CALL_ID, thread_list_nodes=before_list, thread_list_read_ok=True, thread_list_truncated=False, thread_list_cycle_detected=False, **common),
            event(10, REDUCER.CREATOR_ID),
            event(11, REDUCER.PUBLICATION_ID, record_thread=record_thread, record_thread_callback_0x00_read_ok=True, record_origin_thread_ptr_0x74_read_ok=True, thread_list_nodes=after_list, thread_list_read_ok=True, thread_list_truncated=False, thread_list_cycle_detected=False),
            event(12, REDUCER.AUX_RETURN_ID, **common),
            event(13, REDUCER.MOTION_ID, **common),
            event(14, REDUCER.CHILD_ID, record_thread=record_thread, record_worksheet=record_worksheet, origin_instruction=iw, record_thread_callback_0x00_read_ok=True, thread_list_nodes=after_list, thread_list_read_ok=True, thread_list_truncated=False, thread_list_cycle_detected=False),
            event(15, REDUCER.MODE1_ID, record_worksheet=record_worksheet, origin_instruction=iw, mode1_record_origin_thread_0x74=thread, mode1_origin_iw_slot_0x00="0x00000001", mode1_origin_iw_mode_0x06="0x00000004", mode1_origin_iw_slot_0x00_read_ok=True, mode1_origin_iw_mode_0x06_read_ok=True, mode1_record_origin_thread_0x74_read_ok=True, mode1_record_payload_mode_0x22_read_ok=True, thread_list_nodes=after_list, thread_list_read_ok=True, thread_list_truncated=False, thread_list_cycle_detected=False),
            event(16, REDUCER.RNG_ID, owns_rng_draw=True, caller_callsite_pc="0x80051BB0", stack_frame_0_callsite_pc="0x80051BB0", rng_seed_after="0x12345678"),
            event(17, REDUCER.PATHING_ID, thread_list_nodes=after_list, thread_list_read_ok=True, thread_list_truncated=False, thread_list_cycle_detected=False),
        ]

        chains = REDUCER.build_mode1_chains(149113, events)

        self.assertEqual(len(chains), 1)
        chain = chains[0]
        self.assertTrue(chain["complete"])
        self.assertEqual(chain["payload_delay"], 13)
        self.assertEqual(chain["delay_gate_result"], 0)
        self.assertEqual(chain["stored_delay"], 0)
        self.assertEqual(chain["delay_decrement_count"], 0)
        self.assertEqual(chain["state6_gate_results"], "0>1")
        self.assertEqual(chain["state6_flags_after"], "0x80000040>0x00000040")
        self.assertFalse(chain["record_present_before_publication"])
        self.assertTrue(chain["record_present_at_publication"])
        self.assertTrue(chain["rng_caller_contains_80051BB0"])
        self.assertTrue(chain["sequence_order_valid"])
        self.assertTrue(chain["thread_lists_valid"])


if __name__ == "__main__":
    unittest.main()
