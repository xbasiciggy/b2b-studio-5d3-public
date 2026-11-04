#!/usr/bin/env bash
set -euo pipefail
ROOT=/src; export ML_SRC=$ROOT/magiclantern_simplified; export LOG=$ROOT/arm-$1.log; mkdir -p "$HOME/ml"
for repo in "$ROOT/m-film" "$ML_SRC"; do
 if [ ! -d "$repo/.git" ]; then git -C "$repo" init -q; git -C "$repo" add .; git -C "$repo" -c user.name=x -c user.email=x@x commit -qm snap; fi
done
bash "$ROOT/m-film/ml-dev/build_fork.sh" "$1" --no-darkroom
