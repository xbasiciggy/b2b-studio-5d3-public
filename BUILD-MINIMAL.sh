#!/usr/bin/env bash
# Run in Linux with gcc-arm-none-eabi, build-essential, zip, Python 3.10,
# numpy, scipy, pillow, segno and docutils installed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HOME/ml"
export ML_SRC="$ROOT/magiclantern_simplified"
export LOG="$ROOT/arm-build.log"
for repo in "$ROOT/m-film" "$ML_SRC"; do
 if [ ! -d "$repo/.git" ]; then
  git -C "$repo" init -q
  git -C "$repo" add .
  git -C "$repo" -c user.name='Source snapshot' -c user.email='source@local' commit -qm 'B2B minimal editable source snapshot'
 fi
done
bash "$ROOT/m-film/ml-dev/build_fork.sh" 5D3.123
OUT="$ROOT/m-film/ml-dev/out/fork-5D3.123"
cp -R "$ROOT/resources/ML/data/." "$OUT/ML/data/"
rm -f "$OUT/ML/DARKROOM/"LOOK*.LUT
cp "$ROOT/resources/ML/DARKROOM/"PLOOK*.LUT "$OUT/ML/DARKROOM/"
cp "$ROOT/START-HERE.txt" "$OUT/"
cp "$ROOT/m-film/LICENSE" "$ROOT/m-film/THIRD_PARTY.md" "$OUT/"
cp -R "$ROOT/resources/LICENSES" "$OUT/"
rm -f "$OUT/ML-SETUP.FIR"
(cd "$OUT" && zip -qr "$ROOT/B2B-Studio-Minimal-5D3.123-rebuilt.zip" .)
