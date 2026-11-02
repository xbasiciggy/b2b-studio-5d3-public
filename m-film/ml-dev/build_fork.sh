#!/usr/bin/env bash
# Build the film-mode ML fork inside WSL (or any Linux) and collect the card
# files (autoexec.bin + ML/ folder, ready to copy to the card).
#
#   build_fork.sh                 EOS M          -> ml-dev/out/fork/
#   build_fork.sh 650D.104        another camera -> ml-dev/out/fork-650D.104/
#   build_fork.sh 6D.116 --no-darkroom
#                                 without the in-camera darkroom (~60 KB less at boot, for cameras
#                                 that can't spare it) -> ml-dev/out/fork-6D.116-nodark/
#
# The cameras are the folders under platform/ in magiclantern_simplified; the
# ones M-Film knows are listed in CAMERAS (ml-dev/cameras.txt).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${ML_SRC:-$HOME/ml/magiclantern_simplified}"
PLAT="${1:-EOSM.202}"
NODARK=""
[ "${2:-}" = "--no-darkroom" ] && NODARK="--no-darkroom"
if [ "$PLAT" = "EOSM.202" ] && [ -z "$NODARK" ]; then OUT="$HERE/out/fork"; else OUT="$HERE/out/fork-$PLAT${NODARK:+-nodark}"; fi
LOG="${LOG:-$HOME/ml/build_fork_$PLAT${NODARK:+-nodark}.log}"

cp "$HERE"/fork/src/filmroll.{c,h} "$HERE"/filmos/fos*.{c,h} "$HERE"/filmos/tjpgd*.{c,h} "$SRC/src/"
sed -i 's/\r$//' "$SRC"/src/filmroll.{c,h} "$SRC"/src/fos*.{c,h}
# M-Film's version, one for the camera and the program (VERSION at the top of the repository)
VERSION=$(tr -d ' \r\n' < "$HERE/../VERSION")
printf '#define MFILM_VERSION "%s"\n' "$VERSION" > "$SRC/src/fos_version.h"
echo "M-Film $VERSION"
python3 "$HERE/fork/apply_fork.py" "$SRC" "$PLAT" $NODARK

cd "$SRC/platform/$PLAT"
make clean >/dev/null
make -j"$(nproc)" FATAL_WARNINGS=y > "$LOG" 2>&1 || true
grep -E "error:|warning:" "$LOG" | grep -v "#warning FIXME" || true

ZIP=$(ls -t build/*.zip 2>/dev/null | head -1 || true)
if [ -z "$ZIP" ]; then
    echo "no zip produced, see $LOG; its end:"
    tail -60 "$LOG"
    exit 1
fi

# What autoexec.bin takes at boot: the camera gives ML everything from
# RESTARTSTART to the end of its bss out of Canon's own memory, and with too
# much it hangs at power on (card LED stays lit). The limit is known only
# where someone has found it on the camera (cameras.txt): on the EOS M,
# 727072 bytes is the largest build known to start (2026-10).
LIMIT=$(awk -v p="$PLAT" '$1 == p { print $3 }' "$HERE/cameras.txt")
RESTART=$(sed -n 's/^RESTARTSTART *= *//p' Makefile)
BSS_END=0x$(arm-none-eabi-nm build/magiclantern | awk '$3 == "_bss_end" { print $1 }')
USED=$((BSS_END - RESTART))
if [ -n "$LIMIT" ] && [ "$LIMIT" != "-" ]; then
    echo "ML takes $USED bytes at boot (limit $LIMIT)"
    if [ "$USED" -gt "$LIMIT" ]; then
        echo "TOO BIG: the camera may not start. Move code or data out of autoexec.bin."
        exit 1
    fi
else
    echo "ML takes $USED bytes at boot (no limit known for $PLAT yet: if the camera hangs at power on, it is too big)"
fi
mkdir -p "$HERE/out" && echo "$USED" > "$HERE/out/.size-$PLAT${NODARK:+-nodark}"

rm -rf "$OUT" && mkdir -p "$OUT"
(cd "$OUT" && unzip -q "$SRC/platform/$PLAT/$ZIP")

# the film OS fonts, loaded from the card after boot
gcc -O2 -I"$HERE/filmos" -o "$HOME/ml/fontblob" "$HERE/filmos/tools/fontblob.c"
mkdir -p "$OUT/ML/data"
"$HOME/ml/fontblob" "$OUT/ML/data/FOSFONT.BIN"

# the films beyond the built-in eight (films/films.py, tools/catalog.py)
F="$HERE/filmos"
python3 "$F/tools/genfilms.py" "$OUT/ML/data/FOSFILMS.BIN"

# the donation links in donate.txt as QR codes (needs segno: pip install segno)
python3 -c "import segno" 2>/dev/null || pip install -q segno
python3 "$F/tools/genqr.py" "$HERE/donate.txt" "$OUT/ML/data/FOSQR.BIN"

# with no darkroom, nothing below is used: the card is done
if [ -n "$NODARK" ]; then
    echo "== card files in $OUT (no darkroom) =="
    (cd "$OUT" && find . -maxdepth 3 | sort | head -60)
    exit 0
fi

# every film's colour table, so the darkroom never spends its first
# develop of a film mixing one (the camera checks they match its looks):
# mixed from the look, then baked from the film's LUT where it has one
# (films/getluts.py fetches the Film-Luts ones; your own go in films/luts/)
[ -d "$HERE/films/luts/filmluts" ] || python3 "$HERE/films/getluts.py"
gcc -O2 -std=gnu99 -DFOS_DEV_TEST -I"$F" -o "$HOME/ml/devtest" "$F"/sim/devtest.c "$F"/sim/port_files.c \
    "$F"/sim/films.c "$F"/fos_dev.c "$F"/fos_looks.c "$F"/fos_film.c "$F"/fos_gfx.c "$F"/fos_jpeg.c -lm
mkdir -p "$OUT/ML/DARKROOM"
FOS_FILMS="$OUT/ML/data/FOSFILMS.BIN" "$HOME/ml/devtest" -T "$OUT/ML/DARKROOM"
python3 "$F/tools/genfilms.py" "$OUT/ML/data/FOSFILMS.BIN" --tables "$OUT/ML/DARKROOM"

# the shelf's samples (PLAY on the shelf): one photo developed with every
# film, with the tables above, as the camera's darkroom would. EOS M only for now.
if [ "$PLAT" = "EOSM.202" ]; then
    mkdir -p "$OUT/ML/SAMPLES"
    FOS_FILMS="$OUT/ML/data/FOSFILMS.BIN" FOS_LUT_DIR="$OUT/ML/DARKROOM" \
        "$HOME/ml/devtest" -S "$HERE/films/sample/SAMPLE.CR2" "$OUT/ML/SAMPLES"
fi
echo "== card files in $OUT =="
(cd "$OUT" && find . -maxdepth 3 | sort | head -60)

# Editable source is distributed alongside each custom build.
