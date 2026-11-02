"""Film stock definitions for the lab.

Every number here is an artistic starting point, not measured data.
Tune each stock by comparing against real scans of that film.

Keys
  balance     colour temperature the film is balanced for (K). Daylight
              film = 5500, tungsten film = 3200.
  slide       reversal film: no lab scanner exposure correction.
  ev          exposure offset in stops (negative film likes extra light).
  sat         saturation in linear light, 1.0 = unchanged.
  contrast    curve slope per channel, in display units per stop.
  pivot       per-channel shift of the curve centre, in stops. Small
              differences between channels give colour crossovers
              (e.g. warm highlights with cool shadows).
  black/white per-channel base fog and maximum density of the print.
  hues        selective hue tweaks: (from, toward, amount, chroma_mult).
  halation    strength of the red glow around bright highlights.
  grain       grain amplitude in display units at mid-tones.
  grain_size  grain clump size in full-resolution pixels.
  soft        optical softness (gaussian sigma, full-res pixels).
  mono        channel mix for black and white stocks (acts like a filter).
  kind        neg, slide, xpro, bw, cine or print: the shelf it stands on in
              the camera.
  short       the film in folder names ("Sep0906 Portra400").
  lut         a .cube under luts/ for the colour (lut_space: its input),
              used when luts/ has no <key>.*.cube of its own. The rest of
              the entry still gives halation, grain and softness, and the
              colour when the file is missing.

The stocks in filmluts.py (colour from the Film-Luts collection) are
added at the end.
"""

import re

from filmluts import FILMS as FILM_LUTS

FILMS = {
    "portra400": dict(
        name="Kodak Portra 400", iso=400, kind="neg", short="Portra400", balance=5500, ev=0.3, sat=0.85,
        contrast=(0.165, 0.17, 0.158), pivot=(-0.15, 0.0, 0.1),
        black=(0.03, 0.026, 0.028), white=(0.97, 0.955, 0.92),
        hues=[("orange", "red", 0.06, 1.05), ("green", "yellow", 0.12, 0.85),
              ("blue", "cyan", 0.08, 0.85)],
        halation=1.0, grain=0.018, grain_size=1.1, soft=0.7,
    ),
    "cinestill800t": dict(
        name="CineStill 800T", iso=800, kind="cine", short="CineStill800T", balance=3200, ev=0.0, sat=0.95,
        contrast=(0.18, 0.18, 0.172), pivot=(-0.05, 0.0, -0.05),
        black=(0.02, 0.032, 0.045), white=(0.97, 0.96, 0.95),
        hues=[("cyan", "blue", 0.1, 1.0), ("red", "orange", 0.08, 1.05)],
        halation=6.0, hal_thr=0.6, grain=0.026, grain_size=1.4, soft=0.8,
    ),
    "ektar100": dict(
        name="Kodak Ektar 100", iso=100, kind="neg", short="Ektar100", balance=5500, ev=0.0, sat=1.25,
        contrast=(0.19, 0.19, 0.186), pivot=(-0.05, 0.0, 0.05),
        black=(0.025, 0.02, 0.025), white=(0.98, 0.97, 0.95),
        hues=[("red", "magenta", 0.05, 1.1), ("blue", "blue", 0.0, 1.15)],
        halation=0.6, grain=0.009, grain_size=0.8, soft=0.5,
    ),
    "gold200": dict(
        name="Kodak Gold 200", iso=200, kind="neg", short="Gold200", balance=5500, ev=0.2, sat=1.08,
        contrast=(0.185, 0.18, 0.168), pivot=(-0.25, -0.05, 0.2),
        black=(0.035, 0.028, 0.022), white=(0.98, 0.95, 0.86),
        hues=[("yellow", "orange", 0.1, 1.1), ("green", "yellow", 0.15, 0.9)],
        halation=1.0, grain=0.016, grain_size=1.1, soft=0.7,
    ),
    "superia400": dict(
        name="Fujicolor Superia 400", iso=400, kind="neg", short="Superia400", balance=5500, ev=0.1, sat=1.05,
        contrast=(0.18, 0.183, 0.178), pivot=(0.05, -0.1, 0.0),
        black=(0.022, 0.036, 0.032), white=(0.96, 0.97, 0.95),
        hues=[("green", "cyan", 0.18, 1.05), ("red", "magenta", 0.06, 1.0),
              ("yellow", "green", 0.06, 0.95)],
        halation=1.0, grain=0.02, grain_size=1.2, soft=0.7,
    ),
    "velvia50": dict(
        name="Fujichrome Velvia 50", iso=50, kind="slide", short="Velvia50", balance=5500, slide=True, ev=-0.3, sat=1.45,
        contrast=(0.25, 0.25, 0.25), pivot=(0.0, 0.0, -0.05),
        black=(0.01, 0.01, 0.015), white=(0.98, 0.98, 0.98),
        hues=[("green", "green", 0.0, 1.15), ("blue", "magenta", 0.06, 1.15),
              ("red", "magenta", 0.06, 1.1)],
        halation=0.3, grain=0.007, grain_size=0.7, soft=0.5,
    ),
    "hp5": dict(
        name="Ilford HP5 Plus 400", iso=400, kind="bw", short="HP5", balance=5500, ev=0.2,
        mono=(0.30, 0.60, 0.10),
        contrast=(0.175,), pivot=(0.0,), black=(0.03,), white=(0.96,),
        halation=0.4, grain=0.028, grain_size=1.3, soft=0.7,
    ),
    "trix400": dict(
        name="Kodak Tri-X 400", iso=400, kind="bw", short="TriX400", balance=5500, ev=0.0,
        mono=(0.42, 0.48, 0.10),
        contrast=(0.205,), pivot=(0.1,), black=(0.02,), white=(0.97,),
        halation=0.4, grain=0.036, grain_size=1.3, soft=0.6,
    ),
}

# CineStill 400T: the colour from your own LUT, luts/cinestill400t.slog3.cube
# (a Resolve LUT for S-Log3 footage, so the lab hands it S-Log3). Daylight
# balance, so the LUT gets the white balance it was graded for; 3200 gives
# the tungsten film's blue in daylight. Without the file, a softer 800T.
FILMS["cinestill400t"] = dict(
    name="CineStill 400T", iso=400, kind="cine", short="CineStill400T", balance=5500, ev=0.0, sat=0.95,
    contrast=(0.175, 0.175, 0.168), pivot=(-0.05, 0.0, -0.05),
    black=(0.02, 0.03, 0.04), white=(0.97, 0.96, 0.95),
    hues=[("cyan", "blue", 0.1, 1.0), ("red", "orange", 0.08, 1.05)],
    halation=3.0, hal_thr=0.65, grain=0.022, grain_size=1.3, soft=0.75,
    lut="cinestill400t.slog3.cube", lut_space="slog3",
)

FILMS.update(FILM_LUTS)


def short_name(key):
    """'Portra400': the film's name without brand, spaces or punctuation."""
    f = FILMS[key]
    if "short" in f:
        return f["short"]
    name = f["name"].split(" ", 1)[1] if " " in f["name"] else f["name"]
    return re.sub(r"[^A-Za-z0-9]", "", name)

