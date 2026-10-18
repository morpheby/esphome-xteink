#!/usr/bin/env bash
# Sync the vendored FreeInk SDK subset into components/xteink/sdk.
#   scripts/sync-sdk.sh <freeink-sdk commit sha> [path to a local freeink-sdk checkout]
set -euo pipefail
SHA=${1:?usage: sync-sdk.sh <freeink-sdk commit sha> [local checkout]}
REPO=https://github.com/Free-Ink/freeink-sdk
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST=$ROOT/components/xteink/sdk
LIBS=(display/FreeInkDisplay hardware/BoardConfig hardware/InputManager hardware/BatteryMonitor hardware/XteinkDetect hardware/FrontlightManager)

SRC=${2:-}
if [ -z "$SRC" ]; then
  SRC=$(mktemp -d); trap 'rm -rf "$SRC"' EXIT
  git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 "$REPO" "$SHA"
  git -C "$SRC" checkout -q FETCH_HEAD
fi

rm -rf "$DST/libs"
for l in "${LIBS[@]}"; do
  mkdir -p "$DST/libs/$(dirname "$l")"
  cp -R "$SRC/libs/$l" "$DST/libs/$l"
done
cp "$SRC/LICENSE" "$SRC/NOTICE" "$DST/"
echo "$SHA" > "$DST/SDK_COMMIT"

PATCH_DIR=$ROOT/components/xteink/sdk-patches
if compgen -G "$PATCH_DIR/*.patch" >/dev/null; then
  for patch in "$PATCH_DIR"/*.patch; do
    git -C "$ROOT" apply --unidiff-zero --whitespace=nowarn \
      --directory=components/xteink/sdk "$patch"
  done
fi

echo "synced freeink-sdk@$SHA -> $DST"
