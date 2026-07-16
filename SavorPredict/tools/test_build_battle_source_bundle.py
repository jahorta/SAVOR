#!/usr/bin/env python3

import unittest

from build_battle_source_bundle import scripted_battle_request, signed_low_16


def literal_parameter(index: int, bits: int) -> dict[str, object]:
    return {
        "index": index,
        "expression": {
            "ast": {
                "kind": "float_literal",
                "rawWords": [0x04000000, bits],
            }
        },
    }


def opcode112_document() -> dict[str, object]:
    return {
        "schema": "spice_sct_ir_v1",
        "parseOk": True,
        "sections": [
            {
                "name": "loop",
                "instructions": [
                    {
                        "offset": 244,
                        "payloadOffset": 396,
                        "opcode": 112,
                        "decodeOk": True,
                        "parameters": [
                            literal_parameter(0, 0x3F800000),
                            literal_parameter(1, 0x00000000),
                            literal_parameter(2, 0x3F800000),
                            literal_parameter(3, 0x40400000),
                        ],
                    }
                ],
            }
        ],
    }


class BuildBattleSourceBundleTests(unittest.TestCase):
    def test_extracts_first_battle_opcode112_request(self) -> None:
        request = scripted_battle_request(opcode112_document(), "loop", 396)

        self.assertEqual(
            request,
            {
                "instruction_offset": 244,
                "event_mode": 1,
                "event_or_encounter_id": 0,
                "stage_id": 1,
                "transition_selector": 3,
            },
        )

    def test_rejects_nonliteral_stage_expression(self) -> None:
        document = opcode112_document()
        instruction = document["sections"][0]["instructions"][0]
        instruction["parameters"][2]["expression"]["ast"]["kind"] = "int_variable"

        with self.assertRaisesRegex(ValueError, "immutable float literal"):
            scripted_battle_request(document, "loop", 396)

    def test_signed_low_16_matches_step_counter_storage(self) -> None:
        self.assertEqual(signed_low_16(0x00010001), 1)
        self.assertEqual(signed_low_16(0x0000FFFF), -1)


if __name__ == "__main__":
    unittest.main()
