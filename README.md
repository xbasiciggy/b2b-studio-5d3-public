# B2B Studio for the Canon EOS 5D Mark III (public beta)

A film-style camera interface for the **Canon EOS 5D Mark III, Canon firmware 1.2.3**, running Magic Lantern. Shoot JPEG as usual
and every picture is baked in the background with a film look (a 3D colour table); your Canon originals are never touched.

![status](https://img.shields.io/badge/status-experimental%20beta-orange)

## Try it

1. You need a 5D Mark III on Canon firmware **1.2.3** that already runs [Magic Lantern](https://magiclantern.fm) (5D3.123).
2. Download `B2B-Studio-5D3.123-beta1.zip` from the [Releases](../../releases) page.
3. **Back up your card**, then follow `START-HERE.txt` in the zip (copy `autoexec.bin` and the `ML` folder to the card).

It is experimental: it has been run on one camera. A bad build can stop Magic Lantern from starting; the camera works normally without the card.
Baking takes tens of seconds per picture on the camera's processor.

## What is in this release

- 72 film looks from the M-Film project, plus **Cold Vibes**.
- Film shelf, Home, Images, Develop screens; red / orange / neon-green colour themes; grain and halation options;
  a film-camera "top plate" on the back screen when LiveView is off.
- No logo splash.

## Credits

**Built on [M-Film](https://github.com/HNGSeries/M-Film) by HNGSeries**: the film OS (canister shelf, Home, UI engine, darkroom, look system) is their work. Thank you.
Also: [Magic Lantern](https://magiclantern.fm) via [reticulatedpines/magiclantern_simplified](https://github.com/reticulatedpines/magiclantern_simplified) (GPL-2.0),
[Film-Luts](https://github.com/YahiaAngelo/Film-Luts) by Yahia Angelo (MIT), TJpgDec by ChaN, Barlow by Jeremy Tribby (SIL OFL 1.1).
Pinned bases: M-Film `faf9b5ef902fede56a9b9ab2c80cc13f54b07fdb`, Magic Lantern `ed3e7c0dfacfdeaf3349dee73edf9e3621217462`.
Notices: `m-film/THIRD_PARTY.md`, `m-film/LICENSE`, `resources/LICENSES/`.

## Build from source

On Linux with `gcc-arm-none-eabi` (and newlib), `build-essential`, `zip`, Python 3.10 with numpy, scipy, pillow, segno and docutils: `bash BUILD-MINIMAL.sh`.
The card files end up in `m-film/ml-dev/out/fork-5D3.123/` and in the zip the script writes.

## Licence

GPL-2.0, like M-Film and Magic Lantern (`m-film/LICENSE`). Film names are descriptive only and not affiliated with Kodak, Fujifilm, CineStill,
Ilford, Agfa or others; "Canon" and "EOS" are trademarks of Canon Inc. This project is not affiliated with Canon. Canon firmware is not included.
