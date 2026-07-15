import unittest

from reduce_thread_pathing_timing_capture import reduce_event_stream


def snapshot(sequence, vi_field, x_bits, z_bits, checkpoint_id, actor=None, target=None):
    event = {
        "capture_sequence": sequence,
        "vi_field_count": vi_field,
        "checkpoint_id": checkpoint_id,
        "slot0_iw_slot_0x00": "0x00000005",
        "slot0_cw_cur_x_0x1c": x_bits,
        "slot0_cw_cur_x_0x1c_address": "0x8123001C",
        "slot0_cw_cur_z_0x24": z_bits,
        "slot0_cw_cur_z_0x24_address": "0x81230024",
        "slot0_thread_callback_0x00": "0x80022850",
        "thread_list_read_ok": True,
        "thread_list_truncated": False,
        "thread_list_cycle_detected": False,
        "thread_list_node_count": 2,
        "thread_runner_previous_80311A78": "0x81200000",
        "thread_runner_current_80311A7C": "0x81201000",
        "thread_list_nodes": [
            {"index": 0, "node": "0x81200000", "callback": "0x80022850", "state": 1, "flags": 8, "payload_word": "0x81230000"},
            {"index": 1, "node": "0x81201000", "callback": "0x80051264", "state": 2, "flags": 8, "payload_word": "0x81240000"},
        ],
    }
    if actor is not None:
        event["turn_actor_slot_0x00"] = actor
        event["turn_target_slot_0x04"] = target
        event["turn_yaw_0xd8"] = "0x42B40000"
    return event


class ThreadPathingTimingReducerTest(unittest.TestCase):
    def test_maps_packed_watch_to_logical_slot_and_brackets_pathing(self):
        events = [
            snapshot(10, 100, "0x41700000", "0xC2340000", "battle_case5_after_threads_8000A2FC"),
            {
                "capture_sequence": 11,
                "vi_field_count": 101,
                "checkpoint_id": "memwatch.packed0_cw_cur_z_write",
                "pc": "0x80061358",
                "memwatch_addr": "0x81230024",
                "decoded_memory_value": "0x00000000C20F0000",
                "memwatch_confirmed_current_instruction": True,
                "stack_frame_count": 6,
                "stack_frame_0_callsite_pc": "0x80014D50",
                "stack_frame_1_callsite_pc": "0x8001B774",
                "stack_frame_2_callsite_pc": "0x80022A40",
                "stack_frame_3_callsite_pc": "0x802265BC",
                "stack_frame_4_callsite_pc": "0x80030A4C",
                "stack_frame_5_callsite_pc": "0x8000A2F4",
            },
            {
                "capture_sequence": 12,
                "vi_field_count": 101,
                "checkpoint_id": "memwatch_delta.packed0_cw_cur_z_write",
                "pc": "0x8001FADC",
                "memwatch_label": "packed0_cw_cur_z_write",
                "memwatch_addr": "0x81230024",
                "memwatch_hits_before": 1,
                "memwatch_hits_after": 3,
            },
            snapshot(13, 101, "0x41700000", "0xC20F0000", "pathing_outer_loop_entry_800526EC", 0, 5),
            snapshot(14, 102, "0x41700000", "0xC20F0000", "battle_case5_after_threads_8000A2FC"),
        ]

        result = reduce_event_stream(events, 123)

        self.assertEqual(result["summary"]["frames"], 2)
        self.assertEqual(result["summary"]["pathing_invocations"], 1)
        self.assertEqual(result["summary"]["unresolved_exact_position_writes"], 0)
        exact = next(row for row in result["write_rows"] if row["kind"] == "exact")
        self.assertEqual(exact["logical_slot"], 5)
        self.assertEqual(exact["component"], "z")
        self.assertEqual(exact["owner_callsite"], "0x80022A40")
        delta = next(row for row in result["write_rows"] if row["kind"] == "delta")
        self.assertEqual(delta["source_pc"], "")
        self.assertEqual(delta["hit_delta"], 2)
        invocation = result["invocation_rows"][0]
        self.assertEqual(invocation["previous_frame_index"], 0)
        self.assertEqual(invocation["next_frame_index"], 1)
        self.assertEqual(invocation["pathing_action_view_thread_indices"], "1")
        self.assertEqual(invocation["pathing_thread_runner_current_index"], 1)
        self.assertEqual(invocation["pathing_combatant_thread_order"], "0:5")
        visible = next(row for row in result["visibility_rows"] if row["logical_slot"] == 5)
        self.assertEqual(visible["previous_z_bits"], "0xC2340000")
        self.assertEqual(visible["pathing_z_bits"], "0xC20F0000")
        self.assertEqual(visible["last_exact_z_source_pc"], "0x80061358")


if __name__ == "__main__":
    unittest.main()
