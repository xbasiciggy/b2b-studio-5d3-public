"""Turn a .cube 3D LUT into a B2B Studio preset: PLOOKnn.LUT (33^3, red fastest, sRGB in and out) and a
catalogue record appended to FOSFILMS.BIN.

    python3 cube_to_plook.py "My LUT.cube" --index 76 --key my_lut --name "My LUT" --brand MH --word "MY LUT" \
        --out ML/DARKROOM --catalog ML/data/FOSFILMS.BIN

The cube is resampled to 33 points by trilinear interpolation (exact when it is already 33). The camera's JPEG
is treated as the LUT's input as it is: no colour-space conversion is applied. Needs numpy."""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xmp_to_plook import GRID, append_record, fnv_hash  # noqa: E402


def read_cube(path):
    size, dmin, dmax, rows = None, np.zeros(3), np.ones(3), []
    for line in Path(path).read_text().splitlines():
        t = line.split("#")[0].split()
        if not t:
            continue
        if t[0] == "LUT_3D_SIZE":
            size = int(t[1])
        elif t[0] == "DOMAIN_MIN":
            dmin = np.array(t[1:4], float)
        elif t[0] == "DOMAIN_MAX":
            dmax = np.array(t[1:4], float)
        elif t[0] in ("TITLE", "LUT_1D_SIZE", "LUT_1D_INPUT_RANGE", "LUT_3D_INPUT_RANGE"):
            if t[0] == "LUT_1D_SIZE":
                raise SystemExit("1D LUTs are not supported")
        else:
            rows.append([float(x) for x in t[:3]])
    assert size and len(rows) == size ** 3, f"cube has {len(rows)} rows for size {size}"
    return size, dmin, dmax, np.array(rows).reshape(size, size, size, 3)     # [b][g][r]


def resample(size, dmin, dmax, cube):
    g = np.arange(GRID) / (GRID - 1)
    b, gg, r = np.meshgrid(g, g, g, indexing="ij")
    pts = np.stack([r, gg, b], -1).reshape(-1, 3) * (dmax - dmin) + dmin
    pos = np.clip((pts - dmin) / (dmax - dmin), 0, 1) * (size - 1)
    i0 = np.minimum(pos.astype(int), size - 2)
    f = pos - i0
    out = 0
    for dr in (0, 1):
        for dg in (0, 1):
            for db in (0, 1):
                w = (f[:, 0] if dr else 1 - f[:, 0]) * (f[:, 1] if dg else 1 - f[:, 1]) * (f[:, 2] if db else 1 - f[:, 2])
                out = out + w[:, None] * cube[i0[:, 2] + db, i0[:, 1] + dg, i0[:, 0] + dr]
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cube")
    ap.add_argument("--index", type=int, required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--brand", default="MH")
    ap.add_argument("--word")
    ap.add_argument("--out", required=True)
    ap.add_argument("--catalog", required=True)
    a = ap.parse_args()
    table = resample(*read_cube(a.cube))
    look = append_record(a.catalog, a.index, a.key, a.brand, a.name, a.word or a.name.upper())
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    dst = out / f"PLOOK{a.index:02d}.LUT"
    dst.write_bytes(b"FOPV" + struct.pack("<I", fnv_hash(a.key, look)) + table.tobytes())
    print("wrote", dst, table.nbytes, "table bytes")


if __name__ == "__main__":
    main()
