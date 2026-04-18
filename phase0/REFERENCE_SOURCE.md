# SA3D Port Reference Source Freeze

Date: 2026-04-18
Status: Active for Phase 0

## Parser reference (pinned)

- Repository: `https://github.com/X-Hax/SA3D.Modeling`
- Release tag: `1.2.1`
- Commit: `13813e7`
- Local checkout target: `third_party/SA3D.Modeling`

### Fetch command

```bash
bash tools/sa3d_ref/fetch_ref.sh
```

### Verify pinned hash

```bash
git -C third_party/SA3D.Modeling rev-parse --short HEAD
```

Expected output:

```text
13813e7
```

---

## Reference runner source

- Repository: `https://github.com/jahorta/SA3D.Modeling`
- Branch: `DetailedIO`
- Local checkout target: `third_party/SA3D.Modeling.DetailedIO`

### Fetch command

```bash
bash tools/sa3d_ref/fetch_runner.sh
```

### Verify branch

```bash
git -C third_party/SA3D.Modeling.DetailedIO branch --show-current
```

Expected output:

```text
DetailedIO
```

---

## Notes

- The runner repo is intentionally kept as a normal clone for now (not yet tracked as a git submodule).
- Phase 0 parity harness work should target scripts, schema, and manifest flow first.
