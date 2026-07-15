#!/usr/bin/env python3
"""Build a canonical predictor source bundle from structured game-data exports."""

from __future__ import annotations

import argparse
import hashlib
import json
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--key", required=True)
    parser.add_argument("--alx-json", type=Path, required=True)
    parser.add_argument("--alx-csv", type=Path, required=True)
    parser.add_argument("--sst-json", type=Path, required=True)
    parser.add_argument("--savestate", type=Path, action="append", required=True)
    parser.add_argument("--event-id", type=int, required=True)
    parser.add_argument("--stage-id", required=True)
    parser.add_argument("--sst-record-index", type=int, default=0)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    alx_document = json.loads(args.alx_json.read_text(encoding="utf-8"))
    if alx_document.get("schema") != "spice_alx_enemy_events_v1":
        raise ValueError("ALX JSON is not spice_alx_enemy_events_v1")
    encounter = next(
        (event for event in alx_document["events"] if event["entryId"] == args.event_id),
        None,
    )
    if encounter is None:
        raise ValueError(f"event {args.event_id} is absent from the ALX export")

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
            "provenance": f"spice_alx_enemy_events_v1 entryId={args.event_id}",
        }
        for item in encounter["combatants"]
        if item["present"] is True
    ]
    snapshot = {
        "schema": "savor_battle_source_snapshot_v1",
        "manifestKey": args.key,
        "encounter": {
            "sourceKind": "alx_enemy_event",
            "entryId": args.event_id,
            "initiative": int(encounter["initiative"]),
            "status": "Exact",
            "provenance": "SPICE versioned ALX import",
        },
        "stage": {
            "stageId": args.stage_id,
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
        "schema": "savor_battle_source_manifest_v1",
        "key": args.key,
        "snapshotFile": snapshot_path.name,
        "snapshotSha256": sha256(snapshot_path),
        "encounterIdentity": f"alx-enemyevent-{args.event_id}",
        "stageIdentity": f"sst-{args.stage_id}-record-{args.sst_record_index}",
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
