"""Write fos_looks.c, the film looks the camera develops with, from
ml-dev/films/films.py, so the camera and the desktop darkroom can't drift apart.

    python3 genlooks.py            # -> ../fos_looks.c

Run it again after changing a film in films/films.py.
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "films"))
from films import FILMS  # noqa: E402

HUES = ["red", "orange", "yellow", "green", "cyan", "blue", "magenta"]


def f(v):
    s = f"{float(v):.6g}"
    if not any(c in s for c in ".e"):
        s += ".0"
    return s + "f"


def triple(v, n=3):
    v = list(v) + [v[0]] * (n - len(v))
    return "{ " + ", ".join(f(x) for x in v[:n]) + " }"


def look(key, d):
    hues = d.get("hues", [])
    assert len(hues) <= 4, key
    hue_c = ", ".join(f"{{ {HUES.index(a)}, {HUES.index(b)}, {f(amt)}, {f(ch)} }}" for a, b, amt, ch in hues) or "{ 0 }"
    mono = d.get("mono")
    return (f'    {{ "{key}", {int(d.get("balance", 5500))}, {int(bool(d.get("slide")))}, {f(d.get("ev", 0))}, '
            f'{f(d.get("sat", 1.0))},\n'
            f'      {triple(d["contrast"])}, {triple(d["pivot"])}, {triple(d["black"])}, {triple(d["white"])},\n'
            f'      {len(hues)}, {{ {hue_c} }},\n'
            f'      {f(d.get("halation", 0))}, {f(d.get("hal_thr", 0.75))}, {triple(d.get("hal_tint", (1.0, 0.28, 0.06)))}, '
            f'{f(d.get("grain", 0))}, {f(d.get("grain_size", 1.0))},\n'
            f'      {int(bool(mono))}, {triple(mono or (0, 0, 0))} }},')


def main():
    out = ['/* fos_looks: the film looks the camera develops with.',
           ' * Generated from films/films.py by tools/genlooks.py: edit those, not this. */',
           '',
           '#include "fos_dev.h"',
           '',
           'const struct fos_look fos_looks[] =',
           '{']
    out += [look(k, d) for k, d in FILMS.items()]
    out += ['};', 'const int fos_nlooks = sizeof(fos_looks) / sizeof(fos_looks[0]);', '']
    dst = HERE.parent / "fos_looks.c"
    dst.write_text("\n".join(out))
    print("->", dst)


if __name__ == "__main__":
    main()
