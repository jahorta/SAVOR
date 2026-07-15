import unittest

from reduce_battle_thread_producer_capture import reduce_events


def node(index: int, address: int, callback: int, payload: int) -> dict:
    return {
        "index": index,
        "node": address,
        "callback": callback,
        "next": 0,
        "parent": 0,
        "flags": 0,
        "state": 0,
        "payload_word": payload,
    }


class BattleThreadProducerReducerTests(unittest.TestCase):
    def test_reconstructs_producer_orders_without_seed_vector(self) -> None:
        events = [
            {
                "capture_sequence": 1,
                "checkpoint_id": "setup_combatant_mkchild_return_80084304",
                "thread_list_read_ok": True,
                "thread_list_nodes": [node(0, 0x81000000, 0x800804B8, 0x82000000)],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
            },
            {
                "capture_sequence": 2,
                "checkpoint_id": "create_std_root_publication_8001FFB8",
                "thread_list_read_ok": True,
                "thread_list_nodes": [
                    node(0, 0x81000000, 0x800804B8, 0x82000000),
                    node(1, 0x81000100, 0x80022850, 0x82000100),
                ],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
                "instruction_root0": 0x81000100,
                "instruction_root0_owner_slot": 0,
            },
            {
                "capture_sequence": 3,
                "checkpoint_id": "create_std_root_publication_8001FFB8",
                "thread_list_read_ok": True,
                "thread_list_nodes": [
                    node(0, 0x81000000, 0x800804B8, 0x82000000),
                    node(1, 0x81000100, 0x80022850, 0x82000100),
                    node(2, 0x81000200, 0x80022850, 0x82000200),
                ],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
                "instruction_root0": 0x81000100,
                "instruction_root0_owner_slot": 0,
                "instruction_root1": 0x81000200,
                "instruction_root1_owner_slot": 0,
            },
            {
                "capture_sequence": 4,
                "checkpoint_id": "create_std_root_publication_8001FFB8",
                "thread_list_read_ok": True,
                "thread_list_nodes": [
                    node(0, 0x81000000, 0x800804B8, 0x82000000),
                    node(1, 0x81000300, 0x80022850, 0x82000300),
                    node(2, 0x81000200, 0x80022850, 0x82000200),
                    node(3, 0x81000100, 0x80022850, 0x82000100),
                ],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
                "instruction_root0": 0x81000100,
                "instruction_root0_owner_slot": 0,
                "instruction_root1": 0x81000200,
                "instruction_root1_owner_slot": 1,
                "instruction_root2": 0x81000300,
                "instruction_root2_owner_slot": 0,
            },
            {
                "capture_sequence": 5,
                "checkpoint_id": "create_std_root_publication_8001FFB8",
                "thread_list_read_ok": True,
                "thread_list_nodes": [
                    node(0, 0x81000000, 0x800804B8, 0x82000000),
                    node(1, 0x81000300, 0x80022850, 0x82000300),
                    node(2, 0x81000400, 0x80022850, 0x82000400),
                    node(3, 0x81000200, 0x80022850, 0x82000200),
                    node(4, 0x81000100, 0x80022850, 0x82000100),
                ],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
                "instruction_root0": 0x81000100,
                "instruction_root0_owner_slot": 0,
                "instruction_root1": 0x81000200,
                "instruction_root1_owner_slot": 1,
                "instruction_root2": 0x81000300,
                "instruction_root2_owner_slot": 4,
                "instruction_root3": 0x81000400,
                "instruction_root3_owner_slot": 0,
            },
            {
                "capture_sequence": 6,
                "checkpoint_id": "battle_case5_after_threads_8000A2FC",
                "thread_list_read_ok": True,
                "thread_list_nodes": [
                    node(0, 0x81000000, 0x800804B8, 0x82000000),
                    node(1, 0x81000300, 0x80022850, 0x82000300),
                    node(2, 0x81000400, 0x80022850, 0x82000400),
                    node(3, 0x81000200, 0x80022850, 0x82000200),
                    node(4, 0x81000100, 0x80022850, 0x82000100),
                ],
                "movement_root0": 0x81000000,
                "movement_root0_owner_slot": 0,
                "instruction_root0": 0x81000100,
                "instruction_root0_owner_slot": 0,
                "instruction_root1": 0x81000200,
                "instruction_root1_owner_slot": 1,
                "instruction_root2": 0x81000300,
                "instruction_root2_owner_slot": 4,
                "instruction_root3": 0x81000400,
                "instruction_root3_owner_slot": 5,
            },
        ]

        reduced = reduce_events(events, 147896)

        self.assertEqual(reduced["summary"]["movement_publication_order"], [0])
        self.assertEqual(
            reduced["summary"]["instruction_creation_order"], [0, 1, 4, 5]
        )
        self.assertEqual(
            reduced["summary"]["instruction_traversal_order"], [4, 5, 1, 0]
        )
        instruction_first = [
            row
            for row in reduced["thread_rows"]
            if row["family"] == "instruction" and row["first_seen"]
        ]
        self.assertEqual(
            [row["owner_slot"] for row in instruction_first], [0, 1, 4, 5]
        )

    def test_reports_invalid_or_truncated_list_snapshot(self) -> None:
        reduced = reduce_events(
            [
                {
                    "capture_sequence": 9,
                    "checkpoint_id": "battle_case5_after_threads_8000A2FC",
                    "thread_list_read_ok": True,
                    "thread_list_truncated": True,
                    "thread_list_cycle_detected": False,
                    "thread_list_nodes": [],
                }
            ],
            147896,
        )
        self.assertEqual(reduced["summary"]["thread_list_failure_count"], 1)
        self.assertEqual(reduced["summary"]["frame_count"], 1)


if __name__ == "__main__":
    unittest.main()
