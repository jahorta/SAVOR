# SA3D Port Reference Source Freeze

Date: 2026-04-18
Status: Active for Phase 0

## Parser reference (pinned)

- Repository: `https://github.com/X-Hax/SA3D.Modeling`
- Release tag: `1.2.1`
- Commit: `13813e7`
- Local checkout target: `third-party/SA3D.Modeling.ref`

### Fetch command

```bash
bash tools/sa3d_ref/fetch_ref.sh
```

### Verify pinned hash

```bash
git -C third-party/SA3D.Modeling.ref rev-parse --short HEAD
```

Expected output:

```text
13813e7
```

---

## Reference runner source

- Repository: `https://github.com/jahorta/SA3D.Modeling`
- Branch: `DetailedIO`
- Local checkout target: `third-party/SA3D.Modeling` (git submodule)

### Fetch command

```bash
bash tools/sa3d_ref/fetch_runner.sh
```

### Verify branch

```bash
git -C third-party/SA3D.Modeling branch --show-current
```

Expected output:

```text
DetailedIO
```

---

## Notes

- The runner source is integrated as the `third-party/SA3D.Modeling` git submodule on branch `DetailedIO`.
- Phase 0 parity harness work should target scripts, schema, and manifest flow first.
