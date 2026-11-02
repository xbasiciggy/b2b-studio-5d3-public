"""Apply the film-mode fork changes to a magiclantern_simplified checkout.

    python3 apply_fork.py ~/ml/magiclantern_simplified [PLATFORM ...]

PLATFORM is a folder under platform/ (EOSM.202, 100D.101, 650D.104, 700D.115 ...);
the default is EOSM.202. The source changes are shared, the platform ones are per camera.

Idempotent: running it twice changes nothing. src/filmroll.c and .h, and
the film OS sources (ml-dev/filmos/fos*.c, .h), are copied separately by
build_fork.sh.
"""

import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
# --no-darkroom: the film OS without its in-camera darkroom (fos_nodark.c in its place), ~60 KB less at boot
NO_DARKROOM = "--no-darkroom" in sys.argv[2:]
PLATFORMS = [a for a in sys.argv[2:] if not a.startswith("--")] or ["EOSM.202"]


def edit(rel, old, new, marker):
    p = root / rel
    text = p.read_text()
    if marker in text:
        print(f"  {rel}: already applied")
        return
    if old not in text:
        sys.exit(f"  {rel}: anchor not found, ML source changed?")
    p.write_text(text.replace(old, new, 1))
    print(f"  {rel}: patched")


# key hook, ahead of modules so the picker owns the buttons while it is up
edit("src/gui-common.c",
     "    if (handle_module_keys(event) == 0) return 0;\n",
     "#ifdef FEATURE_FILMROLL\n"
     "    if (handle_filmroll_keys(event) == 0) return 0;\n"
     "#endif\n"
     "    if (handle_module_keys(event) == 0) return 0;\n",
     "handle_filmroll_keys")
edit("src/gui-common.c",
     "#include <zebra.h>\n",
     "#include <zebra.h>\n#include <filmroll.h>\n",
     "#include <filmroll.h>")

# ML's LiveView overlays step aside while the picker or the HUD is up
edit("src/zebra.c",
     "int get_global_draw() // menu setting, or off if \n{\n",
     "int get_global_draw() // menu setting, or off if \n{\n"
     "#ifdef FEATURE_FILMROLL\n"
     "    if (filmroll_overlays_active()) return 0;\n"
     "#endif\n",
     "filmroll_overlays_active")
edit("src/zebra.c",
     '#include "zebra.h"\n',
     '#include "zebra.h"\n#include "filmroll.h"\n',
     '#include "filmroll.h"')

# the Magic Lantern menu never opens: whatever asks for it (buttons,
# modules, scripts) gets the film OS home instead
edit("src/menu.c",
     "static void menu_open() \n{ \n    if (menu_shown)\n        return;\n",
     "static void menu_open() \n{ \n#ifdef FEATURE_FILMROLL\n"
     "    filmroll_menu_requested();\n"
     "    return;\n"
     "#endif\n"
     "    if (menu_shown)\n        return;\n",
     "filmroll_menu_requested")
edit("src/menu.c",
     '#include "powersave.h"\n',
     '#include "powersave.h"\n#include "filmroll.h"\n',
     '#include "filmroll.h"')

# lua doesn't build with this toolchain (newlib header clash) and isn't needed
edit("modules/Makefile",
     "    img_name \\\n    lua \\\n",
     "    img_name \\\n",
     "lua dropped by film-mode fork")
p = root / "modules/Makefile"
if "lua dropped by film-mode fork" not in p.read_text():
    p.write_text(p.read_text().replace(
        "ML_MODULES = \\\n", "# lua dropped by film-mode fork (newlib header clash with gcc 14)\nML_MODULES = \\\n", 1))

# 6D only: a bigger share of Canon's AllocateMemory pool for autoexec.bin.
# ML takes 0x44C000-0x4E0000 (592K) of the pool's 9152K (0x44C000-0xD3C000,
# checked in the 1.1.6 ROM); the fork needs ~722K at boot, and with more
# boot.h refuses to start (card LED: 3 long, 3 short, forever).
# 0x520000 gives 848K. A 6D running ML with 592K had 2192K of the pool
# free (1503K while recording RAW), so ~1.9M stays free for Canon.
# 100D and 70D keep 0x4E0000.
edit("src/boot-d45-am.c",
     "    #if defined(CONFIG_6D) || defined(CONFIG_100D) || defined(CONFIG_70D)\n",
     "    #if defined(CONFIG_6D)\n"
     "    /* film-mode fork: R0 0x44C000 -> 0x520000 => reserve 848K for ML (MOV R0, #0x520000) */\n"
     "    addr_AllocMem_end[1] = 0xE3A00852;\n"
     "    ml_reserved_mem = 0x520000 - RESTARTSTART;\n"
     "    #elif defined(CONFIG_100D) || defined(CONFIG_70D)\n",
     "film-mode fork: R0 0x44C000 -> 0x520000")

def platform(name):
    """FEATURE_FILMROLL, our objects, and no lua, for one camera."""
    # build it in
    p = root / f"platform/{name}/features.h"
    if "FEATURE_FILMROLL" not in p.read_text():
        p.write_text(p.read_text().rstrip("\n") + "\n\n/* film camera mode (src/filmroll.c) */\n#define FEATURE_FILMROLL\n")
        print(f"  platform/{name}/features.h: patched")
    else:
        print(f"  platform/{name}/features.h: already applied")
    # our objects. Rewritten every time (older versions of this fork
    # listed only filmroll.o), so the list always matches the sources.
    # (no fos_font: the fonts go on the card as ML/DATA/FOSFONT.BIN, see fos_gfx.h)
    FOS_OBJS = ["filmroll", "fos_gfx", "fos_film", "fos_ui", "fos_shelf", "fos_pages", "fos_hud", "fos_bake", "fos_studio", "fos_preview", "fos_quick", "tjpgd"]
    FOS_OBJS += ["fos_nodark", "fos_jpeg", "fos_bench"]  # Minimal JPEG-only camera workflow
    p = root / f"platform/{name}/Makefile"
    lines = p.read_text().split("\n")
    out, skip = [], False
    for line in lines:
        if skip:
            skip = line.rstrip().endswith("\\")
            continue
        if line.startswith("ML_SRC_EXTRA_OBJS +=") and ("filmroll" in line or "fos_" in line):
            skip = line.rstrip().endswith("\\")
            continue
        out.append(line)
    text = "\n".join(out)
    block = "ML_SRC_EXTRA_OBJS += " + " ".join(f"$(BUILD_DIR)/{o}.o" for o in FOS_OBJS) + "\n"
    m = re.search(r"^include[ \t]+\.\./Makefile[ \t]*\n", text, re.M)
    anchor = m.group(0) if m else None
    if anchor is None:
        sys.exit(f"  platform/{name}/Makefile: anchor not found, ML source changed?")
    text = text.replace("\n\n" + anchor, "\n" + anchor).replace(anchor, block + "\n" + anchor, 1)
    if p.read_text() != text:
        p.write_text(text)
        print(f"  platform/{name}/Makefile: patched")
    else:
        print(f"  platform/{name}/Makefile: already applied")

    p = root / f"platform/{name}/modules.included"
    mods = p.read_text().splitlines()
    if "lua" in mods:
        p.write_text("\n".join(m for m in mods if m != "lua") + "\n")
        print(f"  {name}/modules.included: lua removed")
    else:
        print(f"  {name}/modules.included: already applied")


for name in PLATFORMS:
    if not (root / "platform" / name).is_dir():
        sys.exit(f"  platform/{name}: no such camera in this Magic Lantern")
    platform(name)
