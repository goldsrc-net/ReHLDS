#!/usr/bin/env bash
#
# prepare-hlsdk.sh — stage the per-arch publish/ tree with the hlsdk
# header source tree (rehlds-bin's `hlsdk/` directory).
#
# Layout reproduces upstream's release-bin convention:
#   publish/hlsdk/{common,dlls,engine,pm_shared,public}
# with rehlds/public/rehlds/  → publish/hlsdk/engine/
#  and rehlds/public/{others} → publish/hlsdk/public/
#
# Called from each per-arch build job before the arch-specific
# binaries are moved into publish/bin/<arch>/.  Cross-platform —
# uses `cp` only, so works on github's windows-2025 runner's Git
# Bash (no rsync needed).
#
# Usage:  bash rehlds/version/prepare-hlsdk.sh [publish_dir]
#         (default: publish/)

set -euo pipefail

# Resolve $PUB to an absolute path so the `cd` below works regardless
# of whether the caller passed a relative or absolute publish dir.
PUB="${1:-publish}"
mkdir -p "$PUB"
PUB=$(cd "$PUB" && pwd)

mkdir -p "$PUB/hlsdk/common" "$PUB/hlsdk/dlls" "$PUB/hlsdk/pm_shared" \
         "$PUB/hlsdk/public" "$PUB/hlsdk/engine"

# 1. Straight copies — rehlds/<dir>/ → publish/hlsdk/<dir>/
cp -R rehlds/common/.    "$PUB/hlsdk/common/"
cp -R rehlds/dlls/.      "$PUB/hlsdk/dlls/"
cp -R rehlds/pm_shared/. "$PUB/hlsdk/pm_shared/"

# 2. rehlds/public/* → publish/hlsdk/public/, EXCLUDING the `rehlds/` subdir.
( cd rehlds/public && \
  for entry in *; do
    [ "$entry" = "rehlds" ] && continue
    cp -R "$entry" "$PUB/hlsdk/public/"
  done
)

# 3. rehlds/public/rehlds/ → publish/hlsdk/engine/  (renamed)
cp -R rehlds/public/rehlds/. "$PUB/hlsdk/engine/"
