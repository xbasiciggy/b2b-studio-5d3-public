# Taking M-Film further

How to bring M-Film to another camera, or add films.
It was made on the **Canon EOS M (firmware 2.0.2)**, the only camera it has been tried on. The camera
side now *builds* for every DIGIC 4 and 5 camera Magic Lantern supports; see [the list](README.md#cameras)
and [TESTING.md](TESTING.md).

> ภาษาไทยสั้นๆ: ในกล้อง หน้าจอ Film OS (`ml-dev/filmos/`) แยกจากตัวกล้องผ่านชั้น `fos_port_*` แล้ว ที่ต้องทำใหม่ต่อรุ่นคือ
> ส่วนที่ต่อกับ Magic Lantern (`ml-dev/fork/`): ปุ่ม, palette จอ, จุดดักเมนู, ขนาดไฟล์ที่กล้องบูตไหว
> และต้องมีกล้องรุ่นนั้นไว้ทดสอบจริง อ่านต่อด้านล่าง

---

## The parts, and how hard each is to move

| Part | Where | Tied to the EOS M? |
|---|---|---|
| Film OS: the screens on the camera | `ml-dev/filmos/` | Little. Talks to the camera only through `fos_port_*`. |
| The Magic Lantern fork | `ml-dev/fork/`, `ml-dev/build_fork.sh` | Completely. Redone for every camera. |

The desktop program, Darkrooms, is a separate closed-source program and isn't covered here.

### 1. Film OS (moderate)

All of `ml-dev/filmos/fos_*.c` draws into its own 720 × 480, 8-bit canvas and reaches the
camera only through the functions declared at the end of `fos.h`:

```
fos_port_ms, fos_port_sleep, fos_port_key, fos_port_abort      time, keys
fos_port_present, fos_port_palette                              the screen
fos_port_alloc, fos_port_free                                   memory
fos_port_open/read/write/seek/close, list, mkdir, card           files
fos_port_roll, get, set, action, clock, lens_*, photo, ...       camera state
```

Two implementations exist: the camera's (`ml-dev/fork/src/filmroll.c`) and the simulator's
(`ml-dev/filmos/sim/`, runs on a PC and writes PNGs: `make -C ml-dev/filmos/sim`). A new camera
needs a new `filmroll.c`-like file; the screens stay as they are. Things to check per camera:

- **Screen.** 720 × 480 is Magic Lantern's bitmap size on every camera it supports, so layouts carry
  over. The palette trick (`palette_ours` / `palette_canon` in `filmroll.c`, 254 colours written into
  the bitmap overlay's palette as Y/Cb/Cr) works on DIGIC 4 and 5; check that colours look right
  (System → Screen colour has a test chart).
- **Keys.** The EOS M has few buttons and a touch screen, so the Film OS uses SET, the four arrows,
  the wheel, PLAY, MENU, INFO, half-shutter and two-finger touch (`MODULE_KEY_*` in `filmroll.c`).
  Bodies with a top dial, Q button or joystick should map them; keep half-shutter = "leave".
- **Memory and size.** The in-camera darkroom (`fos_dev.c`) decodes the CR2 in slices to stay small,
  and the whole build must stay under what the camera can boot: on the EOS M about 727 KB of ML at
  boot (`build_fork.sh` checks it; above that the camera hangs with the card light on). Fonts and film
  data live on the card (`ML/DATA/FOSFONT.BIN`, `FOSFILMS.BIN`) for this reason. Every camera has a
  different limit: find yours by adding things back until it no longer starts.
- **The in-camera darkroom** expects the EOS M's CR2 (5280 × 3528 sensor, lossless JPEG in vertical
  slices, RGGB, black 2048). Other DIGIC 4/5 bodies write the same kind of CR2 but with other sizes,
  slice layouts and borders: `fos_dev.c` reads them from the file, but test with that camera's RAWs
  (`ml-dev/filmos/sim/devtest.c` develops a CR2 on the PC exactly as the camera would).

### 2. The Magic Lantern fork (hard, per camera)

Magic Lantern itself is built per camera and firmware (`platform/<CAMERA>.<FW>/`). The fork changes,
all in `ml-dev/fork/apply_fork.py` (applied to a `magiclantern_simplified` checkout):

1. `src/gui-common.c`: our key handler runs before the modules' (`handle_filmroll_keys`).
2. `src/zebra.c`: ML's LiveView overlays step aside while the Film OS draws (`filmroll_overlays_active`).
3. `src/menu.c`: the ML menu never opens; anything asking for it gets the Film OS home
   (`filmroll_menu_requested`).
4. `platform/EOSM.202/features.h`: `#define FEATURE_FILMROLL`.
5. `platform/EOSM.202/Makefile`: our objects (`filmroll.o fos_*.o`).
6. `modules/Makefile`: lua left out (didn't build with the toolchain used).

4 to 6 are per camera: `apply_fork.py SRC <platform> ...` does them for any platform, and
`build_fork.sh <platform>` builds one (`ml-dev/out/fork-<platform>/`). What is already done for the
other cameras:

- **Shooting screen.** On the EOS M the Film OS opens from LiveView; on a DSLR also from Canon's
  shooting screen with LiveView off (`shooting_screen()` in `filmroll.c`, `FOS_LV_ONLY`).
- **Sensor colours.** `fos_dev.c` picks the camera's matrix and white level by `CONFIG_<camera>`
  (from LibRaw's `colordata.cpp`); the decoder is checked on every sensor size
  (`mkcr2 --sensor W,H,LEFT,TOP` makes a RAW of that size, `devtest` decodes it: 0 samples wrong on all).
- **The camera's name** in About and in the prints' EXIF comes from ML (`fos_port_model()`).

Still unknown on each, and only a camera can tell: whether it boots with a build this size, how the
palette looks, which keys arrive, and whether the darkroom has the memory it needs.

### Adding a camera to the list

1. Add or change its line in `ml-dev/cameras.txt` (status `testing` once it builds, `working` once
   someone has tried it; the boot limit when you know it).
2. `python3 ml-dev/tools/cameras.py` rewrites the tables in README.md, README.th.md and TESTING.md and
   the cameras CI builds.

## Which cameras are worth trying

All the DIGIC 4 and 5 cameras build already ([the list](README.md#cameras)); what they need is someone
with the camera. The closest to the EOS M are the **100D, 650D and 700D**: the same sensor, the same
DIGIC 5. DIGIC 6 and later (80D, 200D, EOS R …) wait for Magic Lantern itself, and the 7D doesn't build
in Magic Lantern's current source.

## How to go about a port

1. **Own the camera.** Every step needs trying on the body: there is no emulator that runs this.
   Keep a spare card with plain Magic Lantern (or none) to go back to.
2. Build stock Magic Lantern for it and boot it. Note how big the boot image is.
3. Add only `FEATURE_FILMROLL` with a tiny `filmroll.c` that draws one Film OS screen. Check colours
   and that it starts.
4. Map keys; get the shelf and Home working. Then the roll, the HUD, the film count.
5. Watch the boot size at every step. Leave the darkroom (`fos_dev.c`, `fos_dark.c`) for last.
6. Keep the debug log on (`filmroll.debug`, on by default; it writes `ML/FILMROLL.LOG` on the card)
   and read it after each try.
7. Put camera-specific code behind `#ifdef CONFIG_<CAMERA>` or in its own port file, so the
   EOS M keeps building. Send a pull request with what you tested and on which firmware.

## Adding a film

- Its look: `ml-dev/films/films.py` (a model: curves, colour, grain, halation), or a LUT listed in
  `ml-dev/films/filmluts.py`.
- Its canister label: `ml-dev/filmos/tools/catalog.py`; regenerate with `tools/genfilms.py`.
- Rebuild: the camera gets it in `ML/DATA/FOSFILMS.BIN`.

## Ground rules

- Magic Lantern can brick nothing permanently, but a bad build can leave the camera hanging at
  power on: take the battery out, put the old card back. Say so to people you hand builds to.
- Never commit Canon firmware, film LUTs you don't have the right to share, or other people's photos.
- The camera side is GPL-2.0 (from Magic Lantern); keep forks open.
