#!/usr/bin/env bash
# Copy the filmroll module into the ML tree in WSL, build it, and copy the
# .mo back to ml-dev/out/ so it can go on the card (ML/MODULES/).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HOME/ml/magiclantern_simplified"

mkdir -p "$SRC/modules/filmroll" "$HERE/out"
cp "$HERE"/filmroll/{filmroll.c,Makefile,README.rst} "$SRC/modules/filmroll/"
sed -i 's/\r$//' "$SRC"/modules/filmroll/{filmroll.c,Makefile,README.rst}

cd "$SRC/modules/filmroll"
make clean >/dev/null
make FATAL_WARNINGS=y
cp build/filmroll.mo "$HERE/out/"

echo
echo "Undefined symbols (must all exist in ML/MODULES/EOSM_202.sym on the card):"
arm-none-eabi-nm -u build/filmroll.mo | awk '{print $2}' | while read -r s; do
    grep -q " $s\$" "$SRC/platform/EOSM.202/build/EOSM_202.sym" && echo "  ok      $s" || echo "  MISSING $s"
done
ls -l "$HERE/out/filmroll.mo"
