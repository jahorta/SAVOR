#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reduce_predictor_live_comparison as reducer


class PredictorArtifactTests(unittest.TestCase):
    def test_accepts_explicit_predictor_artifact_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = root / "job_147884.predictor.json"
            expected.write_text("{}", encoding="utf-8")

            self.assertEqual(reducer.predictor_path(root, 147884), expected)


class RngCallerAttributionTests(unittest.TestCase):
    def test_pathing_geometry_lr_normalizes_to_fallback_rand_callsite(self) -> None:
        rows = reducer.live_rng_draws([
            {
                "capture_sequence": 11,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "decoded_value": "0x12345678",
                "rng_draw_index_before": 3,
                "caller_callsite_pc": "0x80011724",
                "caller_source": "stack_frame_0",
                "stack_frame_count": 8,
            }
        ])

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["live_source_pc"], 0x80011794)
        self.assertEqual(rows[0]["live_source"], "action_view_pathing_fallback")
        self.assertTrue(rows[0]["live_source_trusted"])
        self.assertEqual(rows[0]["live_stack_source_pc"], 0x80011724)

    def test_action_service_row_lookup_lr_normalizes_to_rand_callsite(self) -> None:
        rows = reducer.live_rng_draws([
            {
                "capture_sequence": 12,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "decoded_value": "0x12345678",
                "rng_draw_index_before": 4,
                "caller_callsite_pc": "0x8002EBA4",
                "caller_source": "stack_frame_0",
                "stack_frame_count": 8,
            }
        ])

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["live_source_pc"], 0x8002EBDC)
        self.assertEqual(rows[0]["live_source"], "action_service_eb4c")
        self.assertTrue(rows[0]["live_source_trusted"])
        self.assertEqual(rows[0]["live_stack_source_pc"], 0x8002EBA4)

    def test_stale_pre_arming_checkpoint_does_not_override_stack_caller(self) -> None:
        rows = reducer.live_rng_draws([
            {
                "capture_sequence": 10,
                "checkpoint_id": "turn_order_priority_jitter_800711F8",
                "stop_kind": "pc_breakpoint",
                "pc": "0x800711F8",
                "owns_rng_draw": False,
                "rng_seed_before": "0x11111111",
            },
            {
                "capture_sequence": 11,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "decoded_value": "0x12345678",
                "rng_draw_index_before": 0,
                "caller_callsite_pc": "0x800145C8",
                "caller_source": "stack_frame_1",
                "stack_frame_count": 8,
            },
        ])

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["live_source_pc"], 0x800145C8)
        self.assertEqual(rows[0]["live_source_attribution"], "caller_stack")
        self.assertEqual(rows[0]["live_source_checkpoint_id"], "")

    def test_adjacent_checkpoint_remains_preferred_over_stack_caller(self) -> None:
        rows = reducer.live_rng_draws([
            {
                "capture_sequence": 20,
                "checkpoint_id": "attack_critical_80010C44",
                "stop_kind": "pc_breakpoint",
                "pc": "0x80010C44",
                "owns_rng_draw": False,
                "rng_seed_before": "0x12345678",
            },
            {
                "capture_sequence": 21,
                "checkpoint_id": reducer.RNG_WATCH_ID,
                "decoded_value": hex(reducer.advance_once(0x12345678)),
                "rng_draw_index_before": 2,
                "caller_callsite_pc": "0x00000000",
            },
        ])

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["live_source_pc"], 0x80010C44)
        self.assertEqual(rows[0]["live_source_attribution"], "same_draw_checkpoint")
        self.assertEqual(
            rows[0]["live_source_checkpoint_id"],
            "attack_critical_80010C44",
        )


class VisualTimelineReductionTests(unittest.TestCase):
    def test_reduces_publication_lifetime_and_rng_owner(self) -> None:
        predictor = {
            "events": [
                {
                    "sequence": 10,
                    "phase": "frame_scheduler",
                    "label": "visual_command_publish",
                    "frame_index": 4,
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "visual_command_kind": "SystemCamera",
                    "visual_resource": "ma000",
                    "visual_record_index": 12,
                    "visual_epoch": 2,
                    "visual_task_sequence": 7,
                    "visual_child_kind": "ActionViewRecord",
                    "visual_payload_mode": 0,
                    "draws_consumed": 0,
                },
                {
                    "sequence": 11,
                    "phase": "frame_scheduler",
                    "label": "visual_child_state0",
                    "frame_index": 4,
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "visual_task_sequence": 7,
                    "visual_child_kind": "ActionViewRecord",
                    "visual_payload_mode": 0,
                    "draws_consumed": 0,
                },
                {
                    "sequence": 12,
                    "phase": "frame_scheduler",
                    "label": "action_view_record_mode0",
                    "frame_index": 4,
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "visual_task_sequence": 7,
                    "visual_child_kind": "ActionViewRecord",
                    "visual_effective_mode": 14,
                    "draws_consumed": 1,
                    "rng_seed_before": 1,
                    "rng_seed_after": reducer.advance_once(1),
                },
                {
                    "sequence": 20,
                    "phase": "frame_scheduler",
                    "label": "visual_child_cleanup",
                    "frame_index": 9,
                    "action_ordinal": 1,
                    "actor_slot": 0,
                    "target_slot": 4,
                    "visual_task_sequence": 7,
                    "visual_child_kind": "ActionViewRecord",
                    "draws_consumed": 0,
                },
            ]
        }

        timeline = reducer.predictor_visual_timeline(149113, predictor)
        lifetimes = reducer.predictor_visual_child_lifetimes(timeline)
        draws = reducer.expand_predictor_draws(predictor)

        self.assertEqual(len(timeline), 4)
        self.assertEqual(len(lifetimes), 1)
        self.assertTrue(lifetimes[0]["complete"])
        self.assertEqual(lifetimes[0]["publication_frame"], 4)
        self.assertEqual(lifetimes[0]["cleanup_frame"], 9)
        self.assertEqual(lifetimes[0]["rng_draws"], 1)
        self.assertEqual(draws[0]["predictor_source"], "mode0_action_view_camera_fallback")

    def test_visual_source_comparison_does_not_align_by_seed_value(self) -> None:
        live = [
            {
                "live_draw_index": 20,
                "capture_sequence": 100,
                "live_source_pc": 0x800145C8,
                "live_source": "view_placement_direct_view",
                "live_source_attribution": "caller_stack",
                "live_seed_after": 0x11111111,
            },
            {
                "live_draw_index": 21,
                "capture_sequence": 101,
                "live_source_pc": 0x80010BDC,
                "live_source": "attack_hit_dodge",
                "live_source_attribution": "caller_stack",
                "live_seed_after": 0x22222222,
            },
        ]
        predictor = [
            {
                "predictor_draw_ordinal": 10,
                "predictor_event_sequence": 4,
                "predictor_frame": 1,
                "predictor_label": "action_view_record_mode0",
                "predictor_source_pc": 0x800513D4,
                "predictor_source": "mode0_action_view_camera_fallback",
                "predictor_seed_after": 0x11111111,
            },
        ]

        rows, summary = reducer.compare_visual_rng_sources(153108, live, predictor)

        self.assertEqual(len(rows), 1)
        self.assertFalse(rows[0]["source_match"])
        self.assertEqual(rows[0]["live_source"], "view_placement_direct_view")
        self.assertEqual(rows[0]["predictor_source"], "mode0_action_view_camera_fallback")
        self.assertEqual(summary["first_visual_rng_source_mismatch_ordinal"], 0)

    def test_visual_publication_summary_keeps_live_and_predictor_counts_separate(self) -> None:
        events = [
            {"checkpoint_id": reducer.ACTION_SERVICE_PUBLICATION_ID},
            {"checkpoint_id": reducer.SERIALIZED_ACTION_VIEW_PUBLICATION_ID},
            {"checkpoint_id": reducer.SYNTHETIC_ACTION_VIEW_PUBLICATION_ID},
        ]
        predictor = [
            {"label": "visual_command_publish", "command_kind": "SetCommand"},
            {"label": "visual_command_publish", "command_kind": "SetCommand"},
            {"label": "visual_command_publish", "command_kind": "SystemCamera"},
        ]

        summary = reducer.visual_publication_summary(events, predictor)

        self.assertEqual(summary["visual_live_set_command_publications"], 1)
        self.assertEqual(summary["visual_predictor_set_command_publications"], 2)
        self.assertEqual(summary["visual_live_system_camera_publications"], 1)
        self.assertEqual(summary["visual_predictor_system_camera_publications"], 1)
        self.assertEqual(summary["visual_live_synthetic_publications"], 1)
        self.assertEqual(summary["visual_predictor_synthetic_publications"], 0)


class FrameStateReductionTests(unittest.TestCase):
    def test_noncombatant_events_do_not_seed_zero_position_state(self) -> None:
        predictor = {
            "events": [
                {
                    "phase": "frame_scheduler",
                    "label": "visual_controller_visit",
                    "frame_index": 0,
                    "actor_slot": 0,
                    "detail": "callback=FUN_800136DC/FUN_80012F58",
                },
                {
                    "phase": "frame_scheduler",
                    "label": "movement_invocation_activate",
                    "frame_index": 1,
                    "actor_slot": 0,
                    "detail": (
                        "combatant_cur_pos_0x1c=(-15,0,15)->(-15,0,15)"
                    ),
                },
            ]
        }

        frames = reducer.predictor_frames(predictor)

        self.assertEqual(frames[0], {})
        self.assertEqual(frames[1][0], (-15.0, 0.0, 15.0))

    def test_facing_frames_keep_initial_state_and_signed_wrap_delta(self) -> None:
        predictor = {
            "events": [
                {
                    "phase": "battle_coordinator",
                    "label": "initial_facing_seeded",
                    "actor_slot": 0,
                    "facing_angle_0x2c": 0,
                },
                {
                    "phase": "frame_scheduler",
                    "label": "visual_controller_visit",
                    "frame_index": 0,
                    "actor_slot": 0,
                },
                {
                    "phase": "frame_scheduler",
                    "label": "combatant_instruction_rotation",
                    "frame_index": 1,
                    "actor_slot": 0,
                    "facing_angle_0x2c": 0xFF00,
                },
            ]
        }

        frames = reducer.predictor_facing_frames(predictor)
        steps = reducer.rotation_steps(
            (frame, frames[frame]) for frame in sorted(frames)
        )

        self.assertEqual(frames[0][0], 0)
        self.assertEqual(frames[1][0], 0xFF00)
        self.assertEqual(steps[0][0]["delta"], -0x100)
        self.assertEqual(steps[0][0]["speed"], 0x100)

    def test_rotation_rate_excludes_terminal_clamps_from_full_step_metric(self) -> None:
        live = [
            {"ordinal": 0, "facings": {0: 0}},
            {"ordinal": 1, "facings": {0: 100}},
            {"ordinal": 2, "facings": {0: 150}},
        ]
        predictor = {
            0: {0: 0},
            1: {0: 100},
            2: {0: 200},
            3: {0: 250},
        }

        _, summary = reducer.compare_rotation_rates(147884, live, predictor, 0, 0)

        self.assertEqual(summary["rotation_paired_rate_steps"], 2)
        self.assertEqual(summary["rotation_speed_matches"], 1)
        self.assertEqual(summary["rotation_full_step_pairs"], 1)
        self.assertEqual(summary["rotation_full_step_speed_matches"], 1)


if __name__ == "__main__":
    unittest.main()
