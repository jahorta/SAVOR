#!/usr/bin/env python3

from __future__ import annotations

import unittest

import reduce_action_motion_invocation as base
import reduce_action_motion_publication_lifetime as reducer


def event(sequence: int, checkpoint: str, **values):
    return {
        "capture_sequence": sequence,
        "record_sequence": values.pop("record_sequence", sequence),
        "checkpoint_id": checkpoint,
        **values,
    }


def rooted_event(sequence: int, checkpoint: str, **values):
    defaults = {
        "root0_thread_ptr": 0x81000000,
        "root0_combatant_worksheet_ptr": 0x81000100,
        "root0_instruction_worksheet_ptr": 0x81000200,
        "root0_iw_slot": 4,
        "root0_iw_persistent_callback": 0x8001B1B0,
        "root0_iw_selected_row": 3,
        "root0_iw_alt_row": 2,
        "root0_iw_previous_row": 1,
        "root0_iw_mode": 4,
        "root0_iw_subtype": 0xFFFF,
        "root0_iw_callback_control": 3,
    }
    defaults.update(values)
    return event(sequence, checkpoint, **defaults)


class ActionMotionPublicationLifetimeReducerTests(unittest.TestCase):
    def test_exact_offset_distinguishes_callback_from_neighboring_rows(self):
        callback = rooted_event(
            1, "root0_iw_action_table_dc_write",
            address=0x810002E0, size=4, value=0x8001B1B0)
        row_runtime = rooted_event(
            2, "root0_iw_callback_rows_e4_write",
            address=0x810002EA, size=2, value=0)
        self.assertEqual(reducer.exact_iw_write(callback)["field"], "persistent_callback")
        self.assertEqual(reducer.exact_iw_write(row_runtime)["field"], "row_runtime_ea")

    def test_failed_root_derivation_never_guesses_an_offset(self):
        row = event(
            1, "root0_iw_action_table_dc_write",
            address=0x810002E0, size=4, value=0x8001B1B0)
        info = reducer.exact_iw_write(row)
        self.assertEqual(info["field"], "unresolved_address")
        self.assertIsNone(info["offset"])

    def test_store_pc_probe_is_an_exact_callback_publication(self):
        row = event(
            1, "callback_store_execute_800201C0",
            pc=0x800201C0, r3=0x8001B1B0, r30=0x81000200,
            current_iw_slot=4, current_iw_callback=0)
        info = reducer.exact_iw_write(row)
        self.assertEqual(info["slot"], 4)
        self.assertEqual(info["offset"], 0xE0)
        self.assertEqual(info["field"], "persistent_callback")
        self.assertEqual(info["writer_pc"], 0x800201C0)
        self.assertEqual(info["post_write_value"], 0x8001B1B0)

    def test_probe_call_cannot_publish_callback(self):
        events = base.annotate_action_ordinals([
            rooted_event(
                1, reducer.HELPER_ENTRY_ID,
                r3=0x81000100, r4=4, r5=0xFFFFFFFF, r6=1,
                call_stack={"frames": [
                    {"callsite_pc": 0x8001AEAC},
                    {"callsite_pc": 0x80022A40},
                ]}),
            rooted_event(2, reducer.HELPER_RETURN_ID, r3=3),
        ])
        row = reducer.helper_call_rows(7, events)[0]
        self.assertEqual(row["operation"], "Probe")
        self.assertEqual(row["callback_write_count"], 0)
        self.assertTrue(row["contract_matches"])

    def test_publisher_call_links_exact_e0_write_and_callsite(self):
        events = base.annotate_action_ordinals([
            rooted_event(
                1, reducer.HELPER_ENTRY_ID,
                r3=0x81000100, r4=8, r5=0xFFFFFFFF, r6=0,
                call_stack={"frames": [
                    {"callsite_pc": 0x800224CC},
                    {"callsite_pc": 0x80022A30},
                ]}),
            rooted_event(
                2, "root0_iw_action_table_dc_write",
                pc=0x800201C0, address=0x810002E0, size=4,
                value=0x8001A4F0,
                root0_iw_persistent_callback=0x8001A4F0,
                call_stack={"frames": [{"callsite_pc": 0x800224E0}]}),
            rooted_event(
                3, reducer.HELPER_RETURN_ID, r3=5,
                root0_iw_persistent_callback=0x8001A4F0),
        ])
        row = reducer.helper_call_rows(7, events)[0]
        self.assertEqual(row["operation"], "Publish")
        self.assertEqual(row["owner"], "queued_std_action_transition")
        self.assertEqual(row["publisher_callsite"], "0x800224E0")
        self.assertEqual(row["callback_write_count"], 1)
        self.assertTrue(row["contract_matches"])

    def test_publisher_call_links_store_pc_probe(self):
        events = base.annotate_action_ordinals([
            event(
                1, reducer.HELPER_ENTRY_ID,
                r3=0x81000100, r4=4, r5=0, r6=0,
                current_iw_slot=4, current_iw_callback=0,
                call_stack={"frames": [{"callsite_pc": 0x800229F8}]}),
            event(
                2, "callback_store_execute_800201C0",
                pc=0x800201C0, r3=0x8001B1B0, r30=0x81000200,
                current_iw_slot=4, current_iw_callback=0,
                call_stack={"frames": [{"callsite_pc": 0x800229F8}]}),
            event(3, reducer.HELPER_RETURN_ID, r3=5),
        ])
        row = reducer.helper_call_rows(7, events)[0]
        self.assertEqual(row["slot"], 4)
        self.assertEqual(row["owner"], "persistent_instruction_dispatch")
        self.assertEqual(row["publisher_callsite"], "0x800229F8")
        self.assertEqual(row["callback_store_pc"], "0x800201C0")
        self.assertEqual(row["callback_after"], "0x8001B1B0")
        self.assertTrue(row["contract_matches"])

    def test_write_callsite_overrides_outer_action_service_ancestor(self):
        events = base.annotate_action_ordinals([
            rooted_event(
                1, reducer.HELPER_ENTRY_ID,
                r3=0x81000100, r4=13, r5=0xFFFFFFFF, r6=0,
                call_stack={"frames": [{"callsite_pc": 0x8002EC2C}]}),
            rooted_event(
                2, "root0_iw_action_table_dc_write",
                pc=0x800201C0, address=0x810002E0, size=4,
                value=0x8001A4F0,
                call_stack={"frames": [
                    {"callsite_pc": 0x800202B4},
                    {"callsite_pc": 0x8002EC2C},
                ]}),
            rooted_event(3, reducer.HELPER_RETURN_ID, r3=4),
        ])
        row = reducer.helper_call_rows(7, events)[0]
        self.assertEqual(row["owner"], "generic_instruction_transition")
        self.assertEqual(row["publisher_callsite"], "0x800202B4")

    def test_same_value_republication_creates_a_new_revision(self):
        events = base.annotate_action_ordinals([
            rooted_event(1, reducer.WINDOW_OPEN_ID),
            rooted_event(
                2, "root0_iw_action_table_dc_write",
                pc=0x800201C0, address=0x810002E0, size=4,
                value=0x8001B1B0),
            rooted_event(3, reducer.DISPATCH_CALL_ID, r29=0x81000000),
        ])
        fields = reducer.instruction_field_rows(7, events)
        revisions = reducer.callback_revision_rows(7, events, fields, [])
        self.assertEqual(len(revisions), 2)
        self.assertTrue(revisions[1]["same_value_republication"])
        self.assertEqual(revisions[1]["dispatch_count"], 1)

    def test_release_does_not_end_persistent_callback_revision(self):
        events = base.annotate_action_ordinals([
            rooted_event(1, reducer.WINDOW_OPEN_ID),
            rooted_event(2, reducer.INSTALL_ID, r3=0x81000000),
            rooted_event(3, reducer.RELEASE_ID, r3=0x81000000),
            rooted_event(4, reducer.DISPATCH_CALL_ID, r29=0x81000000),
        ])
        revisions = reducer.callback_revision_rows(7, events, [], [])
        self.assertTrue(revisions[0]["release_does_not_end_callback_revision"])
        self.assertEqual(revisions[0]["dispatch_count"], 1)

    def test_dispatch_requires_a_matching_callback_revision_snapshot(self):
        events = base.annotate_action_ordinals([
            rooted_event(1, reducer.WINDOW_OPEN_ID),
            rooted_event(2, reducer.DISPATCH_CALL_ID, r29=0x81000000),
        ])
        revisions = reducer.callback_revision_rows(7, events, [], [])
        rows = reducer.dispatch_revision_rows(7, events, revisions)
        self.assertEqual(rows[0]["callback_revision"], 0)
        self.assertTrue(rows[0]["revision_matches_snapshot"])


if __name__ == "__main__":
    unittest.main()
