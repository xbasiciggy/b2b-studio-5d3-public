"""LUTs for the films: read and apply .cube files (the camera build bakes
each film's colour table from its LUT with these).

    python lut.py info luts/MyPortra.logc3.cube     # what is in a LUT

A 3D LUT expects its input in one colour encoding, usually the log curve
of the camera it was made for. The lab starts from RAW, so it can give it
exactly that. Name the encoding in the file name, <name>.<space>.cube, or
pass --lut-space:

    logc3   ARRI LogC3 (EI 800) in ARRI Wide Gamut 3. Most film emulation
            LUTs are made for it ("LogC to Rec709", "Alexa")
    slog3   Sony S-Log3 in S-Gamut3.Cine, what most LUTs for Sony footage
            expect ("Slog3", "SGamut3Cine")
    slog3sg3  Sony S-Log3 in the wider S-Gamut3
    cineon  Cineon log, the density of a film scan, Rec.709 primaries
    rec709  the Rec.709 camera curve, highlights rolled off
    srgb    the sRGB display curve, highlights rolled off (LUTs made on
            ordinary photos, e.g. exported from Lightroom or Photoshop)
    linear  scene linear, only for LUTs that bring their own shaper

The output of every LUT is taken as an sRGB / Rec.709 display image.
"""

import argparse
import functools
from pathlib import Path

import numpy as np

LUT_DIR = Path(__file__).parent / "luts"
SPACES = ("logc3", "slog3", "slog3sg3", "cineon", "rec709", "srgb", "linear")

# ---------------------------------------------------------------- encodings

# linear Rec.709 / sRGB primaries (D65) <-> ARRI Wide Gamut 3, through XYZ
_709_TO_XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                        [0.2126729, 0.7151522, 0.0721750],
                        [0.0193339, 0.1191920, 0.9503041]])
_AWG3_TO_XYZ = np.array([[0.638008, 0.214704, 0.097744],
                         [0.291954, 0.823841, -0.115795],
                         [0.002798, -0.067034, 1.153294]])
M_709_TO_AWG3 = (np.linalg.inv(_AWG3_TO_XYZ) @ _709_TO_XYZ).astype(np.float32)
M_AWG3_TO_709 = np.linalg.inv(M_709_TO_AWG3).astype(np.float32)

# Sony S-Gamut3.Cine and S-Gamut3 (Sony's published primaries, D65)
_SG3C_TO_XYZ = np.array([[0.5990839208, 0.2489255161, 0.1024464902],
                         [0.2150758201, 0.8850685017, -0.1001443219],
                         [-0.0320658495, -0.0276583907, 1.1487819910]])
_SG3_TO_XYZ = np.array([[0.7064827132, 0.1288010498, 0.1151721641],
                        [0.2709796708, 0.7866064112, -0.0575860820],
                        [-0.0096778454, 0.0046000375, 1.0941355587]])
M_709_TO_SG3C = (np.linalg.inv(_SG3C_TO_XYZ) @ _709_TO_XYZ).astype(np.float32)
M_709_TO_SG3 = (np.linalg.inv(_SG3_TO_XYZ) @ _709_TO_XYZ).astype(np.float32)
M_SG3C_TO_709 = np.linalg.inv(M_709_TO_SG3C).astype(np.float32)
M_SG3_TO_709 = np.linalg.inv(M_709_TO_SG3).astype(np.float32)

# ARRI LogC3 at EI 800
LOGC_CUT, LOGC_A, LOGC_B, LOGC_C = 0.010591, 5.555556, 0.052272, 0.247190
LOGC_D, LOGC_E, LOGC_F = 0.385537, 5.367655, 0.092809

# Cineon: 0.002 density per code value, negative gamma 0.6, white at 685, black at 95
CIN_BLACK = 10 ** ((95 - 685) * 0.002 / 0.6)

KNEE = 0.8      # display curves: highlights above this roll off towards 1


def _rolloff(x):
    x = np.maximum(x, 0)
    over = x - KNEE
    soft = KNEE + (1 - KNEE) * (1 - np.exp(-np.maximum(over, 0) / (1 - KNEE)))
    return np.where(over > 0, soft, x)


def _rolloff_inv(y):
    y = np.clip(y, 0, 1 - 1e-6)
    over = y - KNEE
    hard = KNEE - (1 - KNEE) * np.log(1 - np.maximum(over, 0) / (1 - KNEE))
    return np.where(over > 0, hard, y)


def _slog3(x):
    """Sony S-Log3: scene linear (0.18 = mid grey) -> code 0..1."""
    return np.where(x >= 0.01125,
                    (420 + np.log10(np.maximum(x + 0.01, 1e-6) / 0.19) * 261.5) / 1023,
                    (x * (171.2102946929 - 95) / 0.01125 + 95) / 1023)


def _slog3_inv(v):
    return np.where(v >= 171.2102946929 / 1023,
                    np.power(10, (v * 1023 - 420) / 261.5) * 0.19 - 0.01,
                    (v * 1023 - 95) * 0.01125 / (171.2102946929 - 95))


def encode(lin, space):
    """Scene-linear Rec.709 (0.18 = mid grey) -> what a LUT for `space` expects."""
    lin = np.asarray(lin, np.float32)
    if space in ("slog3", "slog3sg3"):
        return _slog3(lin @ (M_709_TO_SG3C if space == "slog3" else M_709_TO_SG3).T)
    if space == "logc3":
        x = lin @ M_709_TO_AWG3.T
        return np.where(x > LOGC_CUT,
                        LOGC_C * np.log10(np.maximum(LOGC_A * x + LOGC_B, 1e-6)) + LOGC_D,
                        LOGC_E * x + LOGC_F)
    if space == "cineon":
        x = np.maximum(lin, 0) * (1 - CIN_BLACK) + CIN_BLACK
        return (685 + 300 * np.log10(x)) / 1023
    if space == "rec709":
        x = _rolloff(lin)
        return np.where(x < 0.018, 4.5 * x, 1.099 * np.power(x, 0.45) - 0.099)
    if space == "srgb":
        x = _rolloff(lin)
        return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)
    if space == "linear":
        return lin
    raise ValueError(f"unknown LUT input space '{space}' (one of {', '.join(SPACES)})")


def decode(v, space):
    """The inverse of encode(): back to scene-linear Rec.709."""
    v = np.asarray(v, np.float32)
    if space in ("slog3", "slog3sg3"):
        return _slog3_inv(v) @ (M_SG3C_TO_709 if space == "slog3" else M_SG3_TO_709).T
    if space == "logc3":
        x = np.where(v > LOGC_E * LOGC_CUT + LOGC_F,
                     (np.power(10, (v - LOGC_D) / LOGC_C) - LOGC_B) / LOGC_A,
                     (v - LOGC_F) / LOGC_E)
        return x @ M_AWG3_TO_709.T
    if space == "cineon":
        x = np.power(10, (v * 1023 - 685) / 300)
        return (x - CIN_BLACK) / (1 - CIN_BLACK)
    if space == "rec709":
        x = np.where(v < 0.081, v / 4.5, np.power(np.maximum(v + 0.099, 0) / 1.099, 1 / 0.45))
        return _rolloff_inv(x)
    if space == "srgb":
        x = np.where(v <= 0.04045, v / 12.92, np.power(np.maximum(v + 0.055, 0) / 1.055, 2.4))
        return _rolloff_inv(x)
    if space == "linear":
        return v
    raise ValueError(f"unknown LUT input space '{space}' (one of {', '.join(SPACES)})")


# ---------------------------------------------------------------- .cube files

def space_from_name(path):
    """'Kodak 2383.logc3.cube' -> 'logc3'; None when the name says nothing."""
    suffixes = [s.lower().lstrip(".") for s in Path(path).suffixes[:-1]]
    return suffixes[-1] if suffixes and suffixes[-1] in SPACES else None


class Lut:
    """A .cube file: an optional 1D shaper, then a 3D table (Resolve's layout)."""

    def __init__(self, path, space=None):
        self.path = Path(path)
        self.title = self.path.stem
        self.space = space or space_from_name(path) or "srgb"
        encode(np.zeros(3, np.float32), self.space)          # a bad space fails here
        size1 = size3 = 0
        dom = {"1": [np.zeros(3), np.ones(3)], "3": [np.zeros(3), np.ones(3)]}
        rows = []
        for line in self.path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            key, _, rest = line.partition(" ")
            key = key.upper()
            if key == "TITLE":
                self.title = rest.strip().strip('"') or self.title
            elif key == "LUT_1D_SIZE":
                size1 = int(rest)
            elif key == "LUT_3D_SIZE":
                size3 = int(rest)
            elif key in ("DOMAIN_MIN", "DOMAIN_MAX"):
                v = np.array(rest.split(), float)
                for d in ("1", "3"):
                    dom[d][key == "DOMAIN_MAX"] = v
            elif key in ("LUT_1D_INPUT_RANGE", "LUT_3D_INPUT_RANGE"):
                lo, hi = (float(t) for t in rest.split())
                dom[key[4]] = [np.full(3, lo), np.full(3, hi)]
            elif key[0].isdigit() or key[0] in "-+.":
                rows.append(line.split())
        data = np.array(rows, np.float32)
        if data.ndim != 2 or data.shape[1] != 3 or len(data) != size1 + size3 ** 3:
            raise ValueError(f"{self.path.name}: expected {size1} + {size3}^3 rows of 3 numbers, "
                             f"found {len(data)}")
        self.table1 = data[:size1] if size1 else None
        self.table3 = data[size1:].reshape(size3, size3, size3, 3) if size3 else None   # [b][g][r]
        self.dom1 = [np.float32(v) for v in dom["1"]]
        self.dom3 = [np.float32(v) for v in dom["3"]]

    def describe(self):
        parts = [f"'{self.title}'"]
        if self.table1 is not None:
            parts.append(f"1D shaper {len(self.table1)}")
        if self.table3 is not None:
            parts.append(f"3D {self.table3.shape[0]}^3")
        return ", ".join(parts) + f", input {self.space}"

    def apply_encoded(self, v):
        """v: values already in the LUT's input encoding, (..., 3)."""
        out = np.asarray(v, np.float32)
        if self.table1 is not None:
            n = len(self.table1)
            x = (out - self.dom1[0]) / (self.dom1[1] - self.dom1[0]) * (n - 1)
            grid = np.arange(n, dtype=np.float32)
            out = np.stack([np.interp(x[..., c], grid, self.table1[:, c]) for c in range(3)], -1)
        if self.table3 is not None:
            n = self.table3.shape[0]
            x = np.clip((out - self.dom3[0]) / (self.dom3[1] - self.dom3[0]), 0, 1) * (n - 1)
            out = _trilinear(self.table3, x)
        return out.astype(np.float32)

    def apply(self, lin, chunk=1 << 20):
        """Scene-linear Rec.709 in, display sRGB out, a slice at a time so a
        full-size frame doesn't need gigabytes."""
        flat = lin.reshape(-1, 3)
        out = np.empty_like(flat, dtype=np.float32)
        for i in range(0, len(flat), chunk):
            out[i:i + chunk] = self.apply_encoded(encode(flat[i:i + chunk], self.space))
        return out.reshape(lin.shape)


def _trilinear(table, x):
    n = table.shape[0]
    flat = table.reshape(-1, 3)
    i = np.clip(np.floor(x).astype(np.int32), 0, n - 2)
    f = x - i
    fr, fg, fb = f[..., 0:1], f[..., 1:2], f[..., 2:3]
    base = (i[..., 2] * n + i[..., 1]) * n + i[..., 0]
    def at(db, dg, dr):
        return flat[base + (db * n + dg) * n + dr]
    c00 = at(0, 0, 0) + (at(0, 0, 1) - at(0, 0, 0)) * fr
    c01 = at(0, 1, 0) + (at(0, 1, 1) - at(0, 1, 0)) * fr
    c10 = at(1, 0, 0) + (at(1, 0, 1) - at(1, 0, 0)) * fr
    c11 = at(1, 1, 0) + (at(1, 1, 1) - at(1, 1, 0)) * fr
    c0 = c00 + (c01 - c00) * fg
    c1 = c10 + (c11 - c10) * fg
    return c0 + (c1 - c0) * fb


@functools.lru_cache(maxsize=8)
def load(path, space=None):
    return Lut(path, space)


def find(film):
    """The LUT for a film key: luts/<film>.cube or luts/<film>.<space>.cube,
    else the one the film names in films.py (its "lut") if it is there."""
    if not LUT_DIR.is_dir():
        return None
    hits = sorted(p for p in LUT_DIR.glob(f"{film}*.cube")
                  if p.name.split(".")[0].lower() == film.lower())
    if hits:
        return hits[0]
    from films import FILMS
    named = FILMS.get(film, {}).get("lut")
    return LUT_DIR / named if named and (LUT_DIR / named).is_file() else None


# ---------------------------------------------------------------- command line

def main():
    ap = argparse.ArgumentParser(description="Film LUTs: inspect .cube files.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info", help="what is in a .cube file")
    i.add_argument("cube")
    i.add_argument("--lut-space", choices=SPACES)
    a = ap.parse_args()
    if a.cmd == "info":
        print(Lut(a.cube, a.lut_space).describe())


if __name__ == "__main__":
    main()
