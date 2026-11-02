"""Convert simulator frames (8-bit indices + their .pal, else out/pal.bin) to PNG / GIF.

    python3 topng.py out/gfxtest.idx            # -> out/gfxtest.png
    python3 topng.py --gif out/anim.gif out/frames/*.idx
    python3 topng.py --sheet out/sheet.png out/frames/*.idx
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 720, 480
OUT = Path(__file__).resolve().parent / "out"


def palette():
    p = np.fromfile(OUT / "pal.bin", dtype="<u4")
    rgb = np.stack([(p >> 16) & 255, (p >> 8) & 255, p & 255], axis=1).astype(np.uint8)
    rgb[0] = (40, 70, 60)       # index 0 is "LiveView shows through": a dull green stands in
    return rgb


def load(path, pal):
    idx = np.fromfile(path, dtype=np.uint8)[: W * H].reshape(H, W)
    own = Path(path).with_suffix(".pal")
    if own.exists():
        # the palette the frame was shown with (photographs bring their own)
        p = np.fromfile(own, dtype="<u4")
        pal = np.stack([(p >> 16) & 255, (p >> 8) & 255, p & 255], axis=1).astype(np.uint8)
        pal[0] = (40, 70, 60)
    return Image.fromarray(pal[idx], "RGB")


def main(argv):
    pal = palette()
    if argv[0] == "--gif":
        out, frames = argv[1], argv[2:]
        imgs = [load(f, pal) for f in frames]
        # frame names end in _<ms>.idx: keep the real timing
        times = [int(Path(f).stem.split("_")[-1]) for f in frames]
        durs = [max(20, b - a) for a, b in zip(times, times[1:])] + [800]
        imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=durs, loop=0, optimize=False)
        print(out, len(imgs), "frames")
    elif argv[0] == "--sheet":
        out, frames = argv[1], argv[2:]
        cols = 3
        imgs = [load(f, pal) for f in frames]
        rows = (len(imgs) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * W // 2, rows * H // 2))
        for i, im in enumerate(imgs):
            sheet.paste(im.resize((W // 2, H // 2), Image.LANCZOS), (i % cols * W // 2, i // cols * H // 2))
        sheet.save(out)
        print(out)
    else:
        for f in argv:
            out = Path(f).with_suffix(".png")
            load(f, pal).save(out)
            print(out)


if __name__ == "__main__":
    main(sys.argv[1:])
