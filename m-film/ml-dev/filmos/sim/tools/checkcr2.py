"""Check test CR2 files from mkcr2 with LibRaw (rawpy): the camera is
recognised, the raw samples are exactly the ones written, and the picture
develops to the expected colours.

    python3 checkcr2.py file.CR2 file.u16 [out.png]
"""
import sys

import numpy as np
import rawpy

cr2, u16 = sys.argv[1], sys.argv[2]
with rawpy.imread(cr2) as r:
    s = r.sizes
    ref = np.fromfile(u16, dtype="<u2").reshape(s.raw_height, s.raw_width)
    got = r.raw_image
    same = np.array_equal(got, ref)
    print(f"{cr2}: raw {s.raw_width}x{s.raw_height} margins {s.left_margin},{s.top_margin} "
          f"image {s.width}x{s.height} flip {s.flip} pattern {r.raw_pattern.tolist()} "
          f"colours {r.color_desc.decode()} black {r.black_level_per_channel} white {r.white_level}")
    print(f"  raw samples identical: {same}" + ("" if same else f"  ({np.count_nonzero(got != ref)} differ)"))
    if len(sys.argv) > 3:
        d = list(r.daylight_whitebalance)
        rgb = r.postprocess(user_wb=d, output_bps=8, no_auto_bright=True, bright=4.0)
        from PIL import Image
        Image.fromarray(rgb).save(sys.argv[3])
        print("  developed ->", sys.argv[3])
