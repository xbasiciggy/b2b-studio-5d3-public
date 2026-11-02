"""The donation links as QR codes for the camera's Support panel:

    python3 genqr.py ../../donate.txt ML/DATA/FOSQR.BIN

donate.txt: up to two lines "Label URL" (lines starting with # are
ignored). With no links, no file: the panel just says thanks.

FOSQR.BIN: "FOSQ", count, then per code: label[48], url[80], its size in
modules, and the modules row by row, 8 to a byte (most significant first).
Needs segno (pip install segno).
"""

import struct
import sys
from pathlib import Path


def main(src, dst):
    links = []
    for line in Path(src).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            label, _, url = line.rpartition(" ")
            links.append((label.strip() or "Donate", url))
    links = links[:2]
    out = Path(dst)
    if not links:
        out.unlink(missing_ok=True)
        print(f"{src}: no links, no {out.name}")
        return
    import segno
    blob = b"FOSQ" + struct.pack("<I", len(links))
    for label, url in links:
        qr = segno.make(url, error="m", micro=False)
        rows = [list(r) for r in qr.matrix]
        n = len(rows)
        bits = [b for r in rows for b in r]
        data = bytes(sum(bits[i + k] << (7 - k) for k in range(8) if i + k < len(bits)) for i in range(0, len(bits), 8))
        blob += label.encode("ascii")[:47].ljust(48, b"\0") + url.encode("ascii")[:79].ljust(80, b"\0")
        blob += struct.pack("<I", n) + data
        print(f"  {label}: {url} ({n} x {n})")
    out.write_bytes(blob)


if __name__ == "__main__":
    main(*sys.argv[1:3])
