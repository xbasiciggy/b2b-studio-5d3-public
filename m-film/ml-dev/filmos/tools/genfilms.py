"""The films beyond the built-in eight, for the card:

    python3 genfilms.py ML/DATA/FOSFILMS.BIN [--tables ML/DARKROOM]

FOSFILMS.BIN: every film of films/films.py that fos_film.c doesn't have
built in, with what the shelf shows (tools/catalog.py), the picture style
for LiveView and the look, packed as struct film_rec in fos_film.c.

--tables: the film tables (LOOKnn.LUT) of the films whose colour comes
from a LUT in films/luts/, baked here from the .cube, the way the lab
applies it: the camera's grid of Cineon codes decoded to light, through
the LUT. Films whose LUT isn't there are left to the camera, which mixes
their table from the look the first time (a plain print).
Needs numpy (the lab's).
"""

import argparse
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAB = HERE.parents[1] / "films"
sys.path.insert(0, str(LAB))
sys.path.insert(0, str(HERE))
from films import FILMS, short_name  # noqa: E402
from catalog import DESC, LABELS      # noqa: E402

HUES = ["red", "orange", "yellow", "green", "cyan", "blue", "magenta"]
GROUP = {"neg": 0, "slide": 1, "xpro": 1, "bw": 2, "cine": 3, "print": 3}
PS_STANDARD, PS_LANDSCAPE, PS_MONO = 0x81, 0x83, 0x86
GRID = 33
LOOK_MODEL = 1                          # as fos_dev.c
LOOK = struct.Struct("<ii ff 3f3f3f3f i" + "bbxxff" * 4 + "ff 3f ff i 3f")
REC = struct.Struct("<16s16s24s8s48s24s 10i 4B i BB10B20s14s8s18s %ds" % LOOK.size)


def builtin_keys():
    src = (HERE.parent / "fos_film.c").read_text()
    table = src[src.index("static const struct fos_film builtin[]"):]
    return re.findall(r'\{ "(\w+)", "', table[:table.index("};")])


def cstr(s, n):
    b = s.encode("ascii")
    assert len(b) < n, (s, n)
    return b


def look_bytes(f):
    hues = f.get("hues", [])
    hv = []
    for a, b, amt, ch in hues[:4]:
        hv += [HUES.index(a), HUES.index(b), amt, ch]
    hv += [0, 0, 0.0, 0.0] * (4 - len(hues[:4]))
    tri = lambda v: list(v) + [v[0]] * (3 - len(v))     # noqa: E731
    mono = f.get("mono")
    return LOOK.pack(int(f.get("balance", 5500)), int(bool(f.get("slide"))), f.get("ev", 0), f.get("sat", 1.0),
                     *tri(f["contrast"]), *tri(f["pivot"]), *tri(f["black"]), *tri(f["white"]),
                     min(len(hues), 4), *hv,
                     f.get("halation", 0), f.get("hal_thr", 0.75), *f.get("hal_tint", (1.0, 0.28, 0.06)),
                     f.get("grain", 0), f.get("grain_size", 1.0), int(bool(mono)), *tri(mono or (0, 0, 0)))


def camera_iso(iso):
    return max(100, iso)                # the EOS M starts at 100


def record(key):
    f = FILMS[key]
    kind = f["kind"]
    brand, name = f["name"].split(" ", 1)
    process = {"neg": "C-41", "slide": "E-6", "xpro": "X-PRO", "bw": "B&W", "cine": "C-41", "print": "ECP"}[kind]
    if key.startswith("kodachrome"):
        process = "K-14"
    if key in ("xp2", "bw400cn"):
        process = "C-41"
    iso = f["iso"]
    style = {"neg": (PS_STANDARD, 0, 0, 0), "slide": (PS_LANDSCAPE, 2, 2, 0), "xpro": (PS_LANDSCAPE, 3, 3, 0),
             "bw": (PS_MONO, 1, 0, 0), "cine": (PS_STANDARD, 1, 1, -1), "print": (PS_STANDARD, 1, -1, 0)}[kind]
    grain = 1 if iso <= 64 else 2 if iso <= 160 else 3 if iso <= 400 else 4 if iso <= 800 else 5
    punch = {"neg": 2, "slide": 4, "xpro": 5, "bw": 3, "cine": 3, "print": 3}[kind]
    colour = {"neg": 3, "slide": 4, "xpro": 5, "bw": 0, "cine": 3, "print": 3}[kind]
    lab = LABELS[key]
    return REC.pack(cstr(key, 16), cstr(brand.upper(), 16), cstr(name, 24), cstr(process, 8),
                    cstr(DESC[key], 48), cstr(short_name(key), 24),
                    iso, camera_iso(iso), style[0], style[1], style[2], style[3], 2, 5200, 0, 0,
                    grain, punch, colour, 3, GROUP[kind],
                    lab["style"], lab["leader"], *lab["bg"], *lab["band"], *lab["accent"], *lab["text"], *lab["num"],
                    cstr(lab["top"], 20), cstr(lab["word"], 14), cstr(lab["num_s"], 8), cstr(lab["sub"], 18),
                    look_bytes(f))


def look_hash(key, look):
    h = 2166136261 ^ (GRID * 256 + LOOK_MODEL)
    for b in key.encode("ascii") + look:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def bake(key, index, out_dir):
    """LOOKnn.LUT from the film's LUT, or None when it isn't there."""
    import numpy as np
    import lut as luts
    path = luts.find(key)
    if not path:
        return None
    table = luts.load(str(path), FILMS[key].get("lut_space") if path.name.split(".")[0] != key else None)
    g = np.arange(GRID, dtype=np.float32) / (GRID - 1)
    b, gg, r = np.meshgrid(g, g, g, indexing="ij")          # red changes fastest, as the camera's grid
    lin = luts.decode(np.stack([r, gg, b], -1).reshape(-1, 3), "cineon")
    rgb = np.clip(table.apply(lin), 0, 1)
    data = (rgb * 255 + 0.5).astype(np.uint8).tobytes()
    dst = out_dir / f"LOOK{index:02d}.LUT"
    dst.write_bytes(b"FOL1" + struct.pack("<I", look_hash(key, look_bytes(FILMS[key]))) + data)
    return dst


def main():
    ap = argparse.ArgumentParser(description="FOSFILMS.BIN and the LUT films' tables for the card.")
    ap.add_argument("out")
    ap.add_argument("--tables", help="the card's ML/DARKROOM, for LOOKnn.LUT")
    a = ap.parse_args()
    built = builtin_keys()
    more = [k for k in FILMS if k not in built]
    recs = b"".join(record(k) for k in more)
    Path(a.out).write_bytes(b"FOSM" + struct.pack("<BBHI", 1, 0, REC.size, len(more)) + recs)
    print(f"{a.out}: {len(more)} films after the {len(built)} built in")
    if a.tables:
        out = Path(a.tables)
        out.mkdir(parents=True, exist_ok=True)
        baked = [k for i, k in enumerate(more) if bake(k, len(built) + i, out)]
        missing = [k for k in more if k not in baked and FILMS[k].get("lut")]
        print(f"  {len(baked)} tables baked from LUTs" + (f"; LUT not found for {', '.join(missing)}" if missing else ""))


if __name__ == "__main__":
    main()
