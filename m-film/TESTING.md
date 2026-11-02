# Testing M-Film on your camera

M-Film runs on the **Canon EOS M**. The same code now **builds** for 14 more cameras (100D, 650D, 700D,
6D, 70D, 5D Mark III, 550D, 600D, 60D, 1100D, 500D, 50D, 5D Mark II), but nobody has tried it on one
of those yet. If you have one, you can be the first: it takes about
20 minutes, and you can always go back.

> **Is it safe?** Magic Lantern runs from the card and changes nothing permanent in the camera.
> The worst that usually happens is that the camera doesn't start with the card in: take the
> battery and the card out, and it starts normally again. Still, this is untested software on your
> camera, at your own risk. Keep a fully charged battery and don't try it in the middle of a shoot.

## 1. Get the files

From the [latest release](../../releases/latest) (or the newest run under *Actions*), download
`M-Film-camera-<your camera>.zip`:

<!-- cameras:start -->
| Camera | Canon firmware it needs | File |
|---|---|---|
| Canon EOS M | 2.0.2 | `M-Film-camera-EOSM.202.zip` |
| Canon EOS 100D / Rebel SL1 / Kiss X7 | 1.0.1 | `M-Film-camera-100D.101.zip` |
| Canon EOS 650D / Rebel T4i / Kiss X6i | 1.0.4 | `M-Film-camera-650D.104.zip` |
| Canon EOS 700D / Rebel T5i / Kiss X7i | 1.1.5 | `M-Film-camera-700D.115.zip` |
| Canon EOS 6D | 1.1.6 | `M-Film-camera-6D.116.zip` |
| Canon EOS 70D | 1.1.2 | `M-Film-camera-70D.112.zip` |
| Canon EOS 5D Mark III (firmware 1.1.3) | 1.1.3 | `M-Film-camera-5D3.113.zip` |
| Canon EOS 5D Mark III (firmware 1.2.3) | 1.2.3 | `M-Film-camera-5D3.123.zip` |
| Canon EOS 550D / Rebel T2i / Kiss X4 | 1.0.9 | `M-Film-camera-550D.109.zip` |
| Canon EOS 600D / Rebel T3i / Kiss X5 | 1.0.2 | `M-Film-camera-600D.102.zip` |
| Canon EOS 60D | 1.1.1 | `M-Film-camera-60D.111.zip` |
| Canon EOS 1100D / Rebel T3 / Kiss X50 | 1.0.5 | `M-Film-camera-1100D.105.zip` |
| Canon EOS 500D / Rebel T1i / Kiss X3 | 1.1.1 | `M-Film-camera-500D.111.zip` |
| Canon EOS 50D | 1.0.9 | `M-Film-camera-50D.109.zip` |
| Canon EOS 5D Mark II | 2.1.2 | `M-Film-camera-5D2.212.zip` |
<!-- cameras:end -->

The firmware must match exactly (MENU → last settings tab → *Firmware ver.*).

## 2. Install

1. Format a card **in the camera**, then copy everything from the zip to the card's root.
2. Camera off, card in, camera on. MENU → *Firmware ver.* → update (first time only: this lets the
   camera start Magic Lantern from the card).
3. Switch the camera off and on.

## 3. What to try, and what to tell us

Go through these and note what happens (a photo of the screen helps a lot):

- [ ] The camera starts, and the **film shelf** appears (on a DSLR, from the normal shooting screen
      with LiveView off; give it up to 15 seconds).
- [ ] **Colours** look right (shelf canisters: Portra yellow, Ektar red; System → Screen colour has a chart).
- [ ] **Buttons**: the dial and arrows move along the shelf, SET loads a film, MENU goes back,
      half-shutter returns to the camera. Which buttons do nothing?
- [ ] **Trash** opens Home; **PLAY** opens the prints instead of Canon's playback.
- [ ] Load a roll and shoot a few frames **through the viewfinder**, and a few in **LiveView**.
      Does the frame count go up? Is the HUD drawn in LiveView?
- [ ] Rewind the roll and **develop it in the camera** (Darkroom). Do prints come out? How long per frame?
- [ ] Open the card in the **M-Film program** on a computer: are the rolls and films there?

Then [open an issue](../../issues/new/choose) with: your camera and firmware, what worked, what didn't,
and the file **`ML/FILMROLL.LOG`** from the card attached.

### If it doesn't start

Battery out, card out. Then please tell us, because it most likely means the program is too big for
that camera's memory (on the EOS M the limit is about 727 KB at boot; the other builds are 670–750 KB, and
nobody knows their limits yet). Say
which camera and firmware, and whether the card light stayed on.

## 4. Remove it

Format the card in the camera. To stop the camera looking for Magic Lantern at all, run
`ML-SETUP.FIR` again from *Firmware ver.* and choose to remove the boot flag.

Thank you! Every camera someone tries moves it from *testing* to *working* in the
[list on the front page](README.md#cameras).
