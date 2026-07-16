#!/usr/bin/env python3
"""Build a canonical predictor source bundle from structured game-data exports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compact_json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def roster_fingerprint(combatants: list[dict[str, object]]) -> str:
    present = [item for item in combatants if item.get("present") is True]
    present.sort(key=lambda item: int(item["slot"]))
    return "|".join(
        f"{int(item['slot'])}:{item['side']}:{int(item['id'])}" for item in present
    )


def signed_low_16(value: int) -> int:
    low = value & 0xFFFF
    return low if low < 0x8000 else low - 0x10000


def literal_expression_value(parameter: dict[str, object]) -> int:
    expression = parameter.get("expression")
    if not isinstance(expression, dict):
        raise ValueError("opcode 112 parameter has no decoded expression")
    ast = expression.get("ast")
    if not isinstance(ast, dict) or ast.get("kind") != "float_literal":
        raise ValueError("opcode 112 parameter is not an immutable float literal")
    raw_words = ast.get("rawWords")
    if not isinstance(raw_words, list) or len(raw_words) != 2:
        raise ValueError("opcode 112 literal has an unexpected raw-word shape")
    value = struct.unpack(">f", struct.pack(">I", int(raw_words[1])))[0]
    if not math.isfinite(value):
        raise ValueError("opcode 112 literal is not finite")
    return int(value)


def scripted_battle_request(
    document: dict[str, object],
    section_name: str,
    payload_offset: int,
) -> dict[str, int]:
    if document.get("schema") != "spice_sct_ir_v1" or document.get("parseOk") is not True:
        raise ValueError("SCT JSON is not a successful spice_sct_ir_v1 decode")
    sections = [
        section
        for section in document.get("sections", [])
        if section.get("name") == section_name
    ]
    if len(sections) != 1:
        raise ValueError(f"SCT section {section_name!r} is absent or ambiguous")
    instructions = [
        instruction
        for instruction in sections[0].get("instructions", [])
        if instruction.get("payloadOffset") == payload_offset
    ]
    if len(instructions) != 1:
        raise ValueError(f"SCT payload offset {payload_offset} is absent or ambiguous")
    instruction = instructions[0]
    if instruction.get("opcode") != 112 or instruction.get("decodeOk") is not True:
        raise ValueError("selected SCT instruction is not a decoded opcode 112")
    parameters = sorted(instruction.get("parameters", []), key=lambda item: item.get("index", -1))
    if [parameter.get("index") for parameter in parameters] != [0, 1, 2, 3]:
        raise ValueError("opcode 112 does not contain exactly four indexed parameters")
    values = [literal_expression_value(parameter) for parameter in parameters]
    return {
        "instruction_offset": int(instruction["offset"]),
        "event_mode": values[0],
        "event_or_encounter_id": values[1],
        "stage_id": values[2],
        "transition_selector": values[3],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--key", required=True)
    parser.add_argument("--alx-json", type=Path, required=True)
    parser.add_argument("--alx-csv", type=Path, required=True)
    parser.add_argument("--sst-json", type=Path, required=True)
    parser.add_argument("--sct-json", type=Path, required=True)
    parser.add_argument("--sct-source", type=Path, required=True)
    parser.add_argument("--sct-section", required=True)
    parser.add_argument("--sct-payload-offset", type=int, required=True)
    parser.add_argument("--savestate", type=Path, action="append", default=[])
    parser.add_argument("--sst-record-index", type=int, default=0)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    sct_document = json.loads(args.sct_json.read_text(encoding="utf-8"))
    request = scripted_battle_request(
        sct_document, args.sct_section, args.sct_payload_offset
    )
    if request["event_mode"] == 0:
        raise ValueError(
            "this bundle builder currently requires an opcode-112 EnemyEvent request"
        )
    decoded_source = Path(str(sct_document.get("source", ""))).name
    if decoded_source.lower() != args.sct_source.name.lower():
        raise ValueError("SCT decode source does not match --sct-source")
    event_id = signed_low_16(request["event_or_encounter_id"])
    stage_number = signed_low_16(request["stage_id"])
    stage_identity = f"s{stage_number:03d}"
    if not args.sst_json.name.lower().startswith(stage_identity.lower()):
        raise ValueError(
            f"SST export {args.sst_json.name} does not match scripted stage {stage_identity}"
        )

    alx_document = json.loads(args.alx_json.read_text(encoding="utf-8"))
    if alx_document.get("schema") != "spice_alx_enemy_events_v1":
        raise ValueError("ALX JSON is not spice_alx_enemy_events_v1")
    encounter = next(
        (event for event in alx_document["events"] if event["entryId"] == event_id),
        None,
    )
    if encounter is None:
        raise ValueError(f"event {event_id} is absent from the ALX export")

    sst_document = json.loads(args.sst_json.read_text(encoding="utf-8"))
    record = next(
        (item for item in sst_document["records"] if item["index"] == args.sst_record_index),
        None,
    )
    if record is None:
        raise ValueError(f"SST record {args.sst_record_index} is absent")
    terrain = record["sstCommandBlock"].get("battleGridTerrainSource9x9")
    if not terrain or terrain.get("inBounds") is not True:
        raise ValueError("selected SST record has no complete 9x9 terrain source")
    rows = terrain.get("source9x9Rows")
    if len(rows) != 9 or any(len(row) != 9 for row in rows):
        raise ValueError("selected SST terrain is not 9x9")

    placements = [
        {
            "slot": int(item["slot"]),
            "side": str(item["side"]),
            "id": int(item["id"]),
            "name": str(item["name"]),
            "gridX": int(item["gridX"]),
            "gridZ": int(item["gridZ"]),
            "present": bool(item["present"]),
            "status": "Exact",
            "provenance": f"spice_alx_enemy_events_v1 entryId={event_id}",
        }
        for item in encounter["combatants"]
        if item["present"] is True
    ]
    snapshot = {
        "schema": "savor_battle_source_snapshot_v1",
        "manifestKey": args.key,
        "encounter": {
            "sourceKind": "alx_enemy_event",
            "requestedEntryId": event_id,
            "entryId": event_id,
            "initiative": int(encounter["initiative"]),
            "status": "Exact",
            "provenance": "SPICE versioned ALX import",
        },
        "stage": {
            "stageId": stage_identity,
            "sstRecordIndex": args.sst_record_index,
            "status": "Exact",
            "provenance": "SPICE SST/SML command-map export",
        },
        "placements": placements,
        "terrain": {
            "source9x9Rows": rows,
            "status": "Exact",
            "provenance": (
                f"SPICE SST battleGridTerrainSource9x9 record={args.sst_record_index} "
                f"offset={terrain['sourceOffset']}"
            ),
        },
        "resourceIdentityRule": {
            "id": "combatant-side-and-id-v1",
            "status": "Exact",
            "provenance": "LoadMovementStdResourceById_80030280 naming branches",
        },
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    snapshot_path = args.output_dir / "snapshot.json"
    snapshot_path.write_bytes(compact_json_bytes(snapshot))

    manifest = {
        "schema": "savor_battle_source_manifest_v2",
        "key": args.key,
        "snapshotFile": snapshot_path.name,
        "snapshotSha256": sha256(snapshot_path),
        "encounterIdentity": f"alx-enemyevent-{event_id}",
        "stageIdentity": f"sst-{stage_identity}-record-{args.sst_record_index}",
        "scriptedBattleRequest": {
            "kind": "scpt_opcode_112",
            "scriptIdentity": args.sct_source.name,
            "sectionIdentity": args.sct_section,
            "instructionOffset": request["instruction_offset"],
            "instructionPayloadOffset": args.sct_payload_offset,
            "eventMode": request["event_mode"],
            "eventOrEncounterId": request["event_or_encounter_id"],
            "stageId": request["stage_id"],
            "transitionSelector": request["transition_selector"],
            "status": "Exact",
            "provenance": (
                f"SPICE spice_sct_ir_v1 decodes {args.sct_source.name} section "
                f"{args.sct_section} payload {args.sct_payload_offset} as opcode 112 "
                "with four immutable literal operands"
            ),
        },
        "sources": [
            {
                "kind": "alx_enemy_event_csv",
                "identity": "ALX-5.0.0/2002-12-19-gc-us-final/enemyevent.csv",
                "sha256": sha256(args.alx_csv),
            },
            {
                "kind": "spice_alx_enemy_events_v1",
                "identity": args.alx_json.name,
                "sha256": sha256(args.alx_json),
            },
            {
                "kind": "spice_sst_sml_command_map",
                "identity": args.sst_json.name,
                "sha256": sha256(args.sst_json),
            },
            {
                "kind": "spice_sct_ir_v1",
                "identity": args.sct_json.name,
                "sha256": sha256(args.sct_json),
            },
            {
                "kind": "scpt_script_source",
                "identity": (
                    "soa_parser_reference_bundle/sct_context/sct_input/"
                    f"{args.sct_source.name}"
                ),
                "sha256": sha256(args.sct_source),
            },
        ],
        "validation": {
            "rosterFingerprint": roster_fingerprint(encounter["combatants"]),
            "acceptedSavestateSha256": sorted({
                sha256(savestate) for savestate in args.savestate
            }),
        },
    }
    (args.output_dir / "manifest.json").write_bytes(compact_json_bytes(manifest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
