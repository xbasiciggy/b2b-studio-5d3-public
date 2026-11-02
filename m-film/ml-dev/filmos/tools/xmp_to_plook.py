"""Convert an Adobe Lightroom/Camera Raw preset (.xmp) into a B2B Studio preset:
a PLOOKnn.LUT table and, with --catalog, a record appended to FOSFILMS.BIN.

    python3 xmp_to_plook.py "My Look.xmp" --index 75 --key my_look \
        --name "Fuji Film" --brand MH --out ML/DARKROOM --catalog ML/data/FOSFILMS.BIN

This is an APPROXIMATION of Lightroom's pipeline, shaped to its documented
behaviour, not Adobe's own maths. Baked into the 33^3 table (input and output are the
camera's sRGB JPEG code values, red changing fastest, the layout of fos_lut_pixel):
  white balance (as a small shift from as-shot), exposure, contrast, highlights,
  shadows, whites, blacks, parametric curve, master and R/G/B point curves, vibrance,
  saturation, per-colour HSL, split toning / colour grading.
Not baked (spatial, or they need the RAW): grain, texture, clarity, dehaze,
sharpening, noise reduction, lens correction, camera profile.
Needs numpy.
"""

import argparse
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

GRID = 33
NS = {"rdf": "http://www.w3.org/1999/02/22-rdf-syntax-ns#", "crs": "http://ns.adobe.com/camera-raw-settings/1.0/"}
CRS = "{%s}" % NS["crs"]
LOOK_MODEL = 1
REC_SIZE = 416
LUMA = (0.2126, 0.7152, 0.0722)


def luma(c):
    return c[..., 0] * LUMA[0] + c[..., 1] * LUMA[1] + c[..., 2] * LUMA[2]
# record layout (genfilms.REC): strings, 10 ints, 4 bytes, group, label, look
OFF_KEY, OFF_BRAND, OFF_NAME, OFF_DESC, OFF_SHORT = 0, 16, 32, 64, 112
OFF_LABEL_WORD = 216                    # label.top[20] ends at 216, word[14] follows
OFF_LOOK = 256


def read_xmp(path):
    root = ET.parse(path).getroot()
    desc = root.find(".//rdf:Description", NS)
    p = {k[len(CRS):]: v for k, v in desc.attrib.items() if k.startswith(CRS)}
    for k in ("ToneCurvePV2012", "ToneCurvePV2012Red", "ToneCurvePV2012Green", "ToneCurvePV2012Blue"):
        el = desc.find("crs:" + k, NS)
        if el is not None:
            p[k] = [tuple(float(x) for x in li.text.split(",")) for li in el.findall(".//rdf:li", NS)]
    return p


def num(p, key, default=0.0):
    try:
        return float(p.get(key, default))
    except (TypeError, ValueError):
        return default


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, None)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def pchip(points):
    """Monotone cubic curve through points given in 0..255 -> function on 0..1."""
    pts = sorted(points)
    x = np.array([a for a, _ in pts]) / 255.0
    y = np.array([b for _, b in pts]) / 255.0
    h = np.diff(x)
    d = np.diff(y) / h
    m = np.zeros_like(x)
    for i in range(1, len(x) - 1):
        if d[i - 1] * d[i] > 0:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
    m[0], m[-1] = d[0], d[-1]

    def f(t):
        t = np.clip(t, 0, 1)
        i = np.clip(np.searchsorted(x, t, side="right") - 1, 0, len(x) - 2)
        s = (t - x[i]) / h[i]
        return ((2 * s ** 3 - 3 * s ** 2 + 1) * y[i] + (s ** 3 - 2 * s ** 2 + s) * h[i] * m[i]
                + (-2 * s ** 3 + 3 * s ** 2) * y[i + 1] + (s ** 3 - s ** 2) * h[i] * m[i + 1])

    return f


def rgb_to_hsv(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx, mn = rgb.max(-1), rgb.min(-1)
    d = mx - mn
    h = np.zeros_like(mx)
    nz = d > 1e-9
    rc = nz & (mx == r)
    gc = nz & (mx == g) & ~rc
    bc = nz & ~rc & ~gc
    h[rc] = ((g - b)[rc] / d[rc]) % 6
    h[gc] = (b - r)[gc] / d[gc] + 2
    h[bc] = (r - g)[bc] / d[bc] + 4
    return h * 60.0, np.where(mx > 1e-9, d / np.maximum(mx, 1e-9), 0), mx


def hsv_to_rgb(h, s, v):
    h = (h % 360) / 60.0
    i = np.floor(h).astype(int) % 6
    f = h - np.floor(h)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    return np.stack([np.choose(i, [v, q, p, p, t, v]), np.choose(i, [t, v, v, q, p, p]),
                     np.choose(i, [p, p, t, v, v, q])], -1)


CENTRES = {"Red": 0, "Orange": 30, "Yellow": 60, "Green": 120, "Aqua": 180, "Blue": 225, "Purple": 275, "Magenta": 320}


def hue_weights(h):
    """Triangular weights for LR's eight colour ranges; they sum to 1 around the wheel."""
    names = list(CENTRES)
    cs = [CENTRES[n] for n in names]
    out = {}
    for i, n in enumerate(names):
        c, a, b = cs[i], cs[i - 1], cs[(i + 1) % 8]
        d1, d2 = (c - a) % 360, (b - c) % 360
        rel = (h - c + 180) % 360 - 180                 # signed hue distance from the centre
        out[n] = np.clip(np.where(rel < 0, 1 + rel / d1, 1 - rel / d2), 0, 1)
    return out


def bump(y, lo, hi):
    t = np.clip((y - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)


def grade(rgb_s, p):
    """(N,3) sRGB 0..1 -> (N,3)."""
    asT, asTint = num(p, "AsShotTemperature", 6400), num(p, "AsShotTint", 0)
    mired = 1e6 / asT - 1e6 / max(num(p, "Temperature", asT), 1)       # >0: warmer than as shot
    dTint = num(p, "Tint", asTint) - asTint
    lin = srgb_to_lin(rgb_s) * np.array([1 + mired * 0.0027, 1 - dTint * 0.0012, 1 - mired * 0.0027])
    lin = lin * 2.0 ** num(p, "Exposure2012")
    lin = np.where(lin > 0.75, 0.75 + 0.25 * (1 - np.exp(-(lin - 0.75) / 0.25)), lin)    # soft shoulder, as LR keeps highlight detail
    rgb = np.clip(lin_to_srgb(lin), 0, 1)

    # tone, on luminance; the colour follows by ratio so hue holds
    y = luma(rgb)
    y2 = y.copy()
    y2 += num(p, "Highlights2012") / 100 * 0.20 * bump(y, 0.35, 0.9) * (1 - 0.75 * bump(y, 0.9, 1.0))
    y2 += num(p, "Shadows2012") / 100 * (0.20 * (1 - bump(y, 0.0, 0.55)) * bump(y, 0.0, 0.12) * 1.4
                                         + 0.12 * (1 - bump(y, 0.1, 0.6)) * bump(y, 0.05, 0.3))
    y2 += num(p, "Whites2012") / 100 * 0.12 * bump(y, 0.65, 1.0)
    y2 += num(p, "Blacks2012") / 100 * 0.12 * (1 - bump(y, 0.0, 0.35))
    s1 = max(num(p, "ParametricShadowSplit", 25) / 100, 0.05)
    s2 = max(num(p, "ParametricMidtoneSplit", 50) / 100, s1 + 0.05)
    s3 = max(num(p, "ParametricHighlightSplit", 75) / 100, s2 + 0.05)
    for key, c, wid in (("ParametricShadows", s1 / 2, s1 * 0.9 + 0.1), ("ParametricDarks", (s1 + s2) / 2, (s2 - s1) * 0.9 + 0.1),
                        ("ParametricLights", (s2 + s3) / 2, (s3 - s2) * 0.9 + 0.1),
                        ("ParametricHighlights", (s3 + 1) / 2, (1 - s3) * 0.9 + 0.1)):
        y2 += num(p, key) / 100 * 0.12 * np.exp(-((y - c) / wid) ** 2 * 1.6)
    y2 = 0.5 + (y2 - 0.5) * (1 + num(p, "Contrast2012") / 100 * 0.6)
    y2 = np.clip(y2, 0, 1)
    ratio = y2 / np.maximum(y, 1e-4)
    rgb = np.where((y > 1e-4)[:, None], rgb * ratio[:, None], y2[:, None])
    rgb = np.clip(rgb, 0, 1)

    # point curves on gamma codes: master, then each channel
    if p.get("ToneCurvePV2012"):
        rgb = pchip(p["ToneCurvePV2012"])(rgb)
    for i, key in enumerate(("ToneCurvePV2012Red", "ToneCurvePV2012Green", "ToneCurvePV2012Blue")):
        if p.get(key):
            rgb[:, i] = pchip(p[key])(rgb[:, i])
    rgb = np.clip(rgb, 0, 1)

    # vibrance (spares saturated colours and skin) and saturation
    h, s, v = rgb_to_hsv(rgb)
    skin = np.exp(-(((h - 25 + 180) % 360 - 180) / 28) ** 2) * 0.5
    s = np.clip(s * (1 + num(p, "Vibrance") / 100 * 0.8 * (1 - s) * (1 - skin)), 0, 1)
    s = np.clip(s * (1 + num(p, "Saturation") / 100), 0, 1)

    # HSL panel
    w = hue_weights(h)
    dh = sum(w[n] * num(p, "HueAdjustment" + n) / 100 * 30.0 for n in CENTRES)
    ds = sum(w[n] * num(p, "SaturationAdjustment" + n) / 100 for n in CENTRES)
    dl = sum(w[n] * num(p, "LuminanceAdjustment" + n) / 100 for n in CENTRES)
    chroma = np.clip(s * 3, 0, 1)                                       # greys have no hue to move
    rgb = hsv_to_rgb(h + dh * chroma, np.clip(s * (1 + ds), 0, 1), np.clip(v * (1 + dl * 0.35 * chroma), 0, 1))

    # split toning and the midtone colour grade
    y = luma(rgb)
    mid_edge = 0.5 + num(p, "SplitToningBalance") / 100 * 0.25
    for hue_k, sat_k, mask in (("SplitToningShadowHue", "SplitToningShadowSaturation", 1 - bump(y, 0.0, mid_edge + 0.2)),
                               ("SplitToningHighlightHue", "SplitToningHighlightSaturation", bump(y, mid_edge - 0.2, 1.0)),
                               ("ColorGradeMidtoneHue", "ColorGradeMidtoneSat", np.exp(-((y - 0.5) / 0.3) ** 2))):
        amt = num(p, sat_k) / 100
        if amt <= 0:
            continue
        tint = hsv_to_rgb(np.full_like(y, num(p, hue_k)), np.ones_like(y), np.ones_like(y))
        tint = tint - luma(tint)[:, None]                            # a colour cast, not a lift
        rgb = rgb + tint * (amt * 0.35 * mask)[:, None]
    return np.clip(rgb, 0, 1)


def build_table(p):
    g = np.arange(GRID, dtype=np.float64) / (GRID - 1)
    b, gg, r = np.meshgrid(g, g, g, indexing="ij")                      # red changes fastest
    grid = np.stack([r, gg, b], -1).reshape(-1, 3)
    return (np.clip(grade(grid, p), 0, 1) * 255 + 0.5).astype(np.uint8)


def fnv_hash(key, look):
    h = 2166136261 ^ (GRID * 256 + LOOK_MODEL)
    for c in key.encode("ascii") + look:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def cstr(s, n):
    b = s.encode("ascii")
    assert len(b) < n, (s, n)
    return b.ljust(n, b"\0")


def append_record(path, index, key, brand, name, word):
    blob = bytearray(Path(path).read_bytes())
    assert blob[:4] == b"FOSM" and struct.unpack("<H", blob[6:8])[0] == REC_SIZE
    n = struct.unpack("<I", blob[8:12])[0]
    assert n + 8 == index, f"index {index} must be catalog size {n} + 8 built in"
    r = bytearray(blob[12 + (n - 1) * REC_SIZE: 12 + n * REC_SIZE])    # clone the last custom preset
    for off, size, text in ((OFF_KEY, 16, key), (OFF_BRAND, 16, brand), (OFF_NAME, 24, name), (OFF_DESC, 48, "Custom LUT - assumes sRGB input"),
                            (OFF_SHORT, 24, key), (OFF_LABEL_WORD, 14, word)):
        r[off:off + size] = cstr(text, size)
    blob += bytes(r)
    blob[8:12] = struct.pack("<I", n + 1)
    Path(path).write_bytes(bytes(blob))
    return bytes(r[OFF_LOOK:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xmp")
    ap.add_argument("--index", type=int, required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--brand", default="MH")
    ap.add_argument("--word", help="canister label word (13 characters at most)")
    ap.add_argument("--out", required=True, help="folder for PLOOKnn.LUT")
    ap.add_argument("--catalog", help="FOSFILMS.BIN to append the record to")
    a = ap.parse_args()
    table = build_table(read_xmp(a.xmp))
    look = append_record(a.catalog, a.index, a.key, a.brand, a.name, a.word or a.name.upper()) if a.catalog else bytes(160)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    dst = out / f"PLOOK{a.index:02d}.LUT"
    dst.write_bytes(b"FOPV" + struct.pack("<I", fnv_hash(a.key, look)) + table.tobytes())
    print("wrote", dst, table.nbytes, "table bytes")


if __name__ == "__main__":
    main()
