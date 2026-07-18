#!/usr/bin/env python3

from __future__ import annotations

import unittest

import reduce_action_motion_invocation as reducer


def event(sequence: int, checkpoint: str, **values):
    return {
        "capture_sequence": sequence,
        "record_sequence": values.pop("record_sequence", sequence),
        "checkpoint_id": checkpoint,
        **values,
    }


class ActionMotionInvocationReducerTests(unittest.TestCase):
    def test_stack_source_uses_live_lr_frame_and_skips_sentinels(self):
        row = event(1, reducer.RNG_ID, call_stack={
            "frames": [
                {"callsite_pc": "0x7FFFFFFC"},
                {"callsite_pc": "0x80010BDC"},
            ],
        })
        self.assertEqual(reducer.first_callsite(row), 0x80010BDC)

    def test_direct_stack_source_does_not_promote_an_outer_ancestor(self):
        row = event(1, reducer.RNG_ID, call_stack={
            "frames": [
                {"stack_pointer": "0x80350000", "callsite_pc": "0x7FFFFFFC"},
                {"stack_pointer": "0x80350020", "callsite_pc": "0x80081BE4"},
            ],
        })
        self.assertIsNone(reducer.direct_stack_callsite(row))
        self.assertEqual(reducer.first_callsite(row), 0x80081BE4)

    def test_slot_attribution_separates_packed_root_from_semantic_slot(self):
        row = event(
            1,
            reducer.INSTALL_ID,
            r3=0x81002000,
            root2_thread_ptr=0x81002000,
            root2_combatant_worksheet_ptr=0x81002100,
            root2_instruction_worksheet_ptr=0x81002200,
            root2_iw_slot=4,
        )
        self.assertEqual(reducer.root_index_for_event(row), 2)
        self.assertEqual(reducer.slot_for_event(row), 4)

    def test_action_ordinals_use_capture_order_not_delayed_record_order(self):
        events = [
            event(20, reducer.ACTION_BOUNDARY_ID, record_sequence=5),
            event(30, reducer.INSTALL_ID, record_sequence=4),
            event(10, reducer.ACTION_BOUNDARY_ID, record_sequence=6),
        ]
        ordered = reducer.annotate_action_ordinals(events)
        install = next(row for row in ordered if reducer.checkpoint(row) == reducer.INSTALL_ID)
        self.assertEqual(install["_action_ordinal"], 1)

    def test_memory_value_and_stack_are_post_write_and_caller_attributed(self):
        row = event(
            4,
            "root0_iw_callback_rows_e4_write",
            value=0x1122334455667788,
            size=8,
            address="0x810000E4",
            root0_thread_ptr=0x81000000,
            root0_iw_slot=0,
            call_stack={
                "read_ok": True,
                "frames": [{"callsite_pc": "0x8001B50C"}],
            },
        )
        reduced = reducer.mutation_rows(123, [row])[0]
        self.assertEqual(reduced["post_write_value"], "0x55667788")
        self.assertEqual(reduced["writer_callsite"], "0x8001B50C")

    def test_instruction_mutation_links_next_slot_dispatch_and_excludes_rng(self):
        events = [
            event(
                4,
                "root0_iw_callback_rows_e4_write",
                value=1,
                root0_iw_slot=0,
                call_stack={"read_ok": True, "frames": [
                    {"callsite_pc": "0x8001B50C"},
                ]},
            ),
            event(5, reducer.RNG_ID, value=2),
            event(
                8,
                "instruction_dispatch_callback_call_80022A40",
                r29=0x81000000,
                root0_thread_ptr=0x81000000,
                root0_iw_slot=0,
            ),
        ]
        rows = reducer.mutation_rows(123, events)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["next_persistent_dispatch_sequence"], 8)
        self.assertEqual(rows[0]["next_persistent_dispatch_delta"], 4)

    def test_terminal_cleanup_zero_write_does_not_require_another_dispatch(self):
        row = event(
            4,
            "root0_iw_callback_rows_e4_write",
            value=0,
            root0_iw_slot=0,
            call_stack={"read_ok": True, "frames": [
                {"stack_pointer": "0x80350000", "callsite_pc": "0x7FFFFFFC"},
                {"stack_pointer": "0x80350020", "callsite_pc": "0x8001BA8C"},
            ]},
        )
        reduced = reducer.mutation_rows(123, [row])[0]
        self.assertEqual(
            reduced["subsequent_dispatch_status"],
            "TerminalCleanupNoDispatch",
        )

    def test_installer_callsite_comes_from_caller_return_not_stale_entry_lr(self):
        events = reducer.annotate_action_ordinals([
            event(10, reducer.ACTION_BOUNDARY_ID),
            event(
                20,
                reducer.INSTALL_ID,
                r3=0x81000000,
                r4=6,
                root0_thread_ptr=0x81000000,
                root0_iw_slot=0,
                call_stack={"frames": [{"callsite_pc": "0x8001B58C"}]},
            ),
            event(21, reducer.SETUP_COMPLETE_ID),
            event(22, "action_motion_install_caller_return_8001B5C0"),
        ])
        row = reducer.invocation_chains(1, events, [])[0]
        self.assertEqual(row["install_callsite"], "0x8001B5BC")

    def test_resolver_results_classify_install_load_and_wait(self):
        events = []
        for sequence, result in enumerate((2, 1, 0), start=1):
            events.append(event(
                sequence,
                f"action_motion_resolver_return_8001B{sequence:03X}",
                r3=result,
                resolver_output_row=7,
            ))
        rows = reducer.resolver_rows(1, events)
        self.assertEqual(
            [row["install_decision"] for row in rows],
            ["InstallActionMotionPlayback", "LoadMotionWithoutInstall", "WaitRestoreOrSkip"],
        )

    def test_result_two_can_be_a_callsite_specific_looked_up_loader(self):
        events = reducer.annotate_action_ordinals([
            event(
                10,
                "action_motion_resolver_return_8001A8DC",
                r3=2,
                resolver_output_row=0,
            ),
            event(11, reducer.LOOKED_UP_LOADER_ID),
        ])
        row = reducer.resolver_rows(1, events)[0]
        self.assertEqual(row["install_decision"], "LoadLookedUpRegardlessOfResult")
        self.assertEqual(row["observed_branch"], "LoadLookedUpWithoutInstall")
        self.assertTrue(row["contract_matches_capture"])

    def test_resolver_branch_accepts_one_intervening_mutation_event(self):
        events = reducer.annotate_action_ordinals([
            event(
                10,
                "action_motion_resolver_return_8001A8DC",
                r3=2,
                resolver_output_row=0,
            ),
            event(11, "root0_iw_callback_rows_e4_write", value=1),
            event(12, reducer.LOOKED_UP_LOADER_ID),
        ])
        row = reducer.resolver_rows(1, events)[0]
        self.assertEqual(row["observed_branch"], "LoadLookedUpWithoutInstall")
        self.assertTrue(row["contract_matches_capture"])

    def test_rng_source_uses_adjacent_producer_checkpoint(self):
        previous = event(10, "attack_hit_dodge_80010BDC")
        current = event(11, reducer.RNG_ID, call_stack={
            "frames": [
                {"stack_pointer": "0x80350000", "callsite_pc": "0x7FFFFFFC"},
                {"stack_pointer": "0x80350020", "callsite_pc": "0x80081BE4"},
            ],
        })
        source = reducer.classify_live_rng_source(current, previous)
        self.assertEqual(source["live_normalized_source_pc"], 0x80010BDC)
        self.assertEqual(source["live_source_family"], "attack_hit_dodge")
        self.assertEqual(source["live_source_attribution"], "adjacent_producer_checkpoint")

    def test_rng_source_groups_stack_owner_regions_without_guessing_local_pc(self):
        damage = event(11, reducer.RNG_ID, call_stack={
            "frames": [
                {"stack_pointer": "0x80350000", "callsite_pc": "0x80287BFC"},
                {"stack_pointer": "0x80350020", "callsite_pc": "0x80010B2C"},
            ],
        })
        effect = event(12, reducer.RNG_ID, call_stack={
            "frames": [
                {"stack_pointer": "0x80350000", "callsite_pc": "0x80042EB8"},
            ],
        })
        damage_source = reducer.classify_live_rng_source(damage, None)
        effect_source = reducer.classify_live_rng_source(effect, damage)
        self.assertEqual(damage_source["live_source_family"], "attack_damage")
        self.assertIsNone(damage_source["live_normalized_source_pc"])
        self.assertEqual(effect_source["live_source_family"], "combat_effect_worker")
        self.assertIsNone(effect_source["live_normalized_source_pc"])

    def test_rng_source_family_maps_effect_local_pcs_to_stack_owner(self):
        family, precision = reducer.predictor_rng_source_family(0x80043048)
        self.assertEqual(family, "combat_effect_worker")
        self.assertEqual(precision, "region")

    def test_nonzero_and_loader_only_callsites_override_generic_result_policy(self):
        self.assertEqual(
            reducer.resolver_branch(1, 0x8001A928),
            "InstallActionMotionPlayback",
        )
        self.assertEqual(
            reducer.resolver_branch(2, 0x80066E28),
            "LoadLookedUpWithoutInstall",
        )

    def test_prediction_only_chain_gets_negative_live_proof(self):
        events = reducer.annotate_action_ordinals([
            event(10, reducer.ACTION_BOUNDARY_ID),
            event(20, "job.end"),
        ])
        predicted = [{
            "prediction_chain_id": "p0", "action_ordinal": 0, "slot": 0,
            "mode": 4, "selected_row": 2,
        }]
        rows = reducer.semantic_comparison(1, events, [], predicted, [], True)
        self.assertEqual(rows[0]["classification"], "WrongInstallLoadBranch")
        self.assertTrue(rows[0]["negative_live_proof"])

    def test_thread_window_retains_delayed_record_sequences(self):
        snapshot = {
            "read_ok": True,
            "truncated": False,
            "cycle_detected": False,
            "head": "0x81000000",
            "nodes": [],
        }
        rows = reducer.thread_window_rows(1, [event(
            9,
            "battle_case5_thread_list_flight_8000A2FC",
            record_sequence=22,
            thread_list=snapshot,
        )])
        self.assertEqual(rows[0]["capture_sequence"], 9)
        self.assertEqual(rows[0]["record_sequence"], 22)
        self.assertTrue(rows[0]["delayed_record"])


if __name__ == "__main__":
    unittest.main()
