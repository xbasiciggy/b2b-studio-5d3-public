# Third-party software

- **Magic Lantern** (GPL-2.0): the camera side builds on it. https://magiclantern.fm
- **Film-Luts** by Yahia Angelo (MIT License, below): the colour LUTs of 63 film stocks. The `.cube` files are
  *not* in this repository: the camera build (`ml-dev/films/getluts.py`) and Darkrooms download them from
  https://github.com/YahiaAngelo/Film-Luts. The camera's card files are made from them, though: the colour
  tables in `ML/DARKROOM` and the sample photos in `ML/SAMPLES`, so the notice below comes with them.
- **Barlow** font family (SIL Open Font License 1.1): `ml-dev/filmos/fonts/OFL.txt`.
- Film names and box art are descriptive and made for this project; they are not affiliated with or endorsed
  by Kodak, Fujifilm, CineStill, Ilford, Agfa or others. "Canon" and "EOS M" are trademarks of Canon Inc.;
  this project is not affiliated with Canon. Canon firmware is not included.

## Film-Luts licence

```
MIT License

Copyright (c) 2024 Yahia

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
