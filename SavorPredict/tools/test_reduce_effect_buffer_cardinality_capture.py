import unittest

from reduce_effect_buffer_cardinality_capture import (
    AXIS,
    COPY,
    LOOP_GATE,
    POSITION_BINARY,
    POSITION_FOUR_WAY,
    SCALE_X,
    SCALE_Y,
    SCALE_Z,
    VARIANT,
    reduce_events,
)


def copied_buffer(sequence, buffer, key, loops, flags=0, variants=1, axis=0):
    return {
        "capture_sequence": sequence,
        "checkpoint_name": COPY,
        "rng_draw_index_before": 10,
        "r6_effect_buffer": buffer,
        "effect_parent_action_thread_0x04": "0x81230000",
        "effect_source_key_0x28": key,
        "effect_loop_count_0x5c": loops,
        "effect_flags_0x38": flags,
        "effect_variant_count_0x5e": variants,
        "effect_axis_mode_0x60": axis,
    }


def buffer_events(sequence, buffer, loops, position=POSITION_BINARY, variants=True, axis=False):
    events = []
    for index in range(loops + 1):
        events.append({
            "capture_sequence": sequence,
            "checkpoint_name": LOOP_GATE,
            "r29_effect_buffer": buffer,
            "r28_loop_index": index,
        })
        sequence += 1
        if index == loops:
            continue
        for checkpoint in (position, SCALE_X, SCALE_Y, SCALE_Z):
            events.append({
                "capture_sequence": sequence,
                "checkpoint_name": checkpoint,
                "r29_effect_buffer": buffer,
                "rng_draw_index_before": sequence,
            })
            sequence += 1
        if variants:
            events.append({
                "capture_sequence": sequence,
                "checkpoint_name": VARIANT,
                "r29_effect_buffer": buffer,
                "rng_draw_index_before": sequence,
            })
            sequence += 1
        if axis:
            events.append({
                "capture_sequence": sequence,
                "checkpoint_name": AXIS,
                "r29_effect_buffer": buffer,
                "rng_draw_index_before": sequence,
            })
            sequence += 1
    return events


class ReduceEffectBufferCardinalityCaptureTests(unittest.TestCase):
    def test_reduces_exact_binary_variant_pair(self):
        events = [
            copied_buffer(1, "0x1000", 5, 16),
            copied_buffer(2, "0x2000", 5, 6),
            *buffer_events(10, "0x1000", 16),
            *buffer_events(200, "0x2000", 6),
        ]

        buffers, pairs, summary = reduce_events(events, 147896)

        self.assertEqual(len(buffers), 2)
        self.assertTrue(all(row["cardinality_exact"] for row in buffers))
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0]["loop_counts"], "16+6")
        self.assertEqual(pairs[0]["observed_draws"], 110)
        self.assertEqual(summary["exact_effect_pairs"], 1)

    def test_supports_four_way_variant_and_axis_formula(self):
        flags = 0x80000 | 0x200
        events = [
            copied_buffer(1, "0x3000", 9, 2, flags=flags, variants=1, axis=3),
            *buffer_events(
                10,
                "0x3000",
                2,
                position=POSITION_FOUR_WAY,
                variants=True,
                axis=True,
            ),
        ]

        buffers, _, summary = reduce_events(events)

        self.assertEqual(buffers[0]["expected_draws"], 12)
        self.assertEqual(buffers[0]["observed_draws"], 12)
        self.assertEqual(buffers[0]["cardinality_exact"], 1)
        self.assertEqual(summary["exact_buffers"], 1)

    def test_flags_missing_variant_draw(self):
        events = [
            copied_buffer(1, "0x4000", 4, 1, variants=1),
            *buffer_events(10, "0x4000", 1, variants=False),
        ]

        buffers, _, summary = reduce_events(events)

        self.assertEqual(buffers[0]["expected_draws"], 5)
        self.assertEqual(buffers[0]["observed_draws"], 4)
        self.assertEqual(buffers[0]["cardinality_exact"], 0)
        self.assertEqual(summary["exact_buffers"], 0)


if __name__ == "__main__":
    unittest.main()
