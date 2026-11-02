"""Fetch the Film-Luts LUTs the films in filmluts.py use, into luts/filmluts/.

    python getluts.py               # once; again only to update them
    python getluts.py --from ~/Film-Luts      # from a clone you already have

Downloads the collection (about 40 MB) and keeps only the LUTs a film
names. They are not part of this repository: the collection is shared
as-is and some LUTs may carry their makers' rights.
"""

import argparse
import io
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from filmluts import DIR, REPO, STOCKS, ZIP

HERE = Path(__file__).resolve().parent


def download(url):
    # curl first: Python from python.org on a Mac often can't check
    # certificates until "Install Certificates.command" has been run
    if shutil.which("curl"):
        r = subprocess.run(["curl", "-fsSL", url], capture_output=True)
        if r.returncode == 0:
            return r.stdout
    with urllib.request.urlopen(url) as resp:
        return resp.read()


def from_zip(want):
    """{path in the repository: bytes} for the wanted LUTs, from GitHub's zip."""
    data = zipfile.ZipFile(io.BytesIO(download(ZIP)))
    out = {}
    for name in data.namelist():
        rel = name.split("/", 1)[1] if "/" in name else name     # drop "Film-Luts-main/"
        if rel in want:
            out[rel] = data.read(name)
    return out


def from_dir(root, want):
    root = Path(root).expanduser()
    return {rel: (root / rel).read_bytes() for rel in want if (root / rel).is_file()}


def from_git(want):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "clone", "--depth", "1", "-q", REPO, tmp], check=True)
        return from_dir(tmp, want)


def main():
    ap = argparse.ArgumentParser(description="Fetch the Film-Luts LUTs the lab's films use.")
    ap.add_argument("--from", dest="src", help="a Film-Luts clone already on this computer")
    a = ap.parse_args()
    dest = HERE / "luts" / DIR
    want = {f"luts/{path}.cube" for _, path, *_ in STOCKS}
    if a.src:
        files = from_dir(a.src, want)
    else:
        print(f"downloading {REPO} ...")
        try:
            files = from_zip(want)
        except Exception as e:                   # no zip: try git
            print(f"  zip download failed ({e}); trying git clone")
            files = from_git(want)
    for rel, data in files.items():
        dst = dest / rel[len("luts/"):]
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    print(f"{len(files)} of {len(want)} LUTs -> {dest}")
    if len(files) < len(want):
        print("missing:", ", ".join(sorted(want - set(files))))
        sys.exit(1)


if __name__ == "__main__":
    main()
