#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET_DIR="$ROOT_DIR/third-party/SA3D.Modeling"
REMOTE_URL="https://github.com/jahorta/SA3D.Modeling.git"
TARGET_BRANCH="DetailedIO"

if [[ -d "$TARGET_DIR/.git" ]]; then
  git -C "$TARGET_DIR" fetch --all --tags --prune
else
  mkdir -p "$(dirname "$TARGET_DIR")"
  git clone "$REMOTE_URL" "$TARGET_DIR"
fi

git -C "$TARGET_DIR" checkout "$TARGET_BRANCH"
git -C "$TARGET_DIR" pull --ff-only origin "$TARGET_BRANCH"

echo "Checked out runner branch $(git -C "$TARGET_DIR" branch --show-current)"
