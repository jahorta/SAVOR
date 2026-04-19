#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET_DIR="$ROOT_DIR/third-party/SA3D.Modeling.ref"
REMOTE_URL="https://github.com/X-Hax/SA3D.Modeling.git"
PINNED_COMMIT="13813e7"

if [[ -d "$TARGET_DIR/.git" ]]; then
  git -C "$TARGET_DIR" fetch --all --tags --prune
else
  mkdir -p "$(dirname "$TARGET_DIR")"
  git clone "$REMOTE_URL" "$TARGET_DIR"
fi

git -C "$TARGET_DIR" checkout "$PINNED_COMMIT"

echo "Checked out SA3D.Modeling at $(git -C "$TARGET_DIR" rev-parse --short HEAD)"
