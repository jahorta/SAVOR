#!/usr/bin/env python3
import json
import sys
from pathlib import Path
from fnmatch import fnmatch


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: validate_manifest.py <path-to-FIXTURE_MANIFEST.json>")
        return 2

    manifest_path = Path(sys.argv[1]).resolve()
    if not manifest_path.is_file():
        print(f"ERROR: manifest file does not exist: {manifest_path}")
        return 1

    with manifest_path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    if data.get("schema") != "soasim_fixture_manifest_v1":
        print("ERROR: manifest schema must be soasim_fixture_manifest_v1")
        return 1

    fixtures = data.get("fixtures")
    if not isinstance(fixtures, list):
        print("ERROR: fixtures must be an array")
        return 1

    root = manifest_path.parent.parent
    fixture_policy = data.get("fixture_policy")
    if fixture_policy is not None:
        if not isinstance(fixture_policy, dict):
            print("ERROR: fixture_policy must be an object when provided")
            return 1
        if fixture_policy.get("mode") == "auto_discover":
            policy_root = fixture_policy.get("root")
            policy_glob = fixture_policy.get("glob", "*.mld")
            if not isinstance(policy_root, str) or not policy_root:
                print("ERROR: fixture_policy.root must be a non-empty string for auto_discover mode")
                return 1
            scan_root = (root / policy_root).resolve()
            if not scan_root.is_dir():
                print(f"ERROR: fixture_policy root does not exist or is not a directory: {scan_root}")
                return 1

            discovered = []
            for child in sorted(scan_root.iterdir()):
                if child.is_file() and fnmatch(child.name.lower(), policy_glob.lower()):
                    discovered.append(child)
            fixtures = [
                {
                    "id": discovered_path.stem,
                    "mld_path": f"{policy_root}/{discovered_path.name}",
                }
                for discovered_path in discovered
            ]
            print(f"Auto-discovered {len(fixtures)} fixture(s) from {scan_root}")

    failures = 0
    for index, fixture in enumerate(fixtures):
        fixture_id = fixture.get("id", f"<index:{index}>")
        mld_path = fixture.get("mld_path")
        if not isinstance(mld_path, str) or not mld_path:
            print(f"ERROR: fixture {fixture_id} has invalid mld_path")
            failures += 1
            continue
        resolved = (root / mld_path).resolve()
        if not resolved.is_file():
            print(f"ERROR: fixture {fixture_id} path missing: {resolved}")
            failures += 1

    if failures:
        print(f"Manifest validation failed with {failures} error(s).")
        return 1

    print(f"Manifest OK: {len(fixtures)} fixture(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
