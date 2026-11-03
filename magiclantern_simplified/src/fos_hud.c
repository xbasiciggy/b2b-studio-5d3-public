/* fos_hud: the SLR viewfinder over LiveView - an aperture scale with the
 * meter needle on the right, a strip of red seven-segment LEDs below.
 *
 * Unlike the film OS screens this draws with Canon's palette (LiveView
 * shows through index 0, and Canon keeps drawing its own bits), so it
 * takes the indices it may use from the caller and draws straight into
 * the overlay buffer.
 */

#include "fos.h"

#define W        720
#define H        480
#define STRIP_W  64                     /* meter strip on the right */
#define BAR_H    44                     /* LED readout at the bottom */
#define VIEW_W   (W - STRIP_W)
#define VIEW_H   (H - BAR_H)
#define THIRD_PX 14                     /* scale spacing per 1/3 stop */
#define SCALE_TOP 26                    /* y of f/22; f/1 sits 27 thirds below */

static uint8_t *vr;
static int vp;
static const struct fos_hud_palette *pal;

static void fill(int x, int y, int w, int h, uint8_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++)
        memset(vr + (y + j) * vp + x, c, w > 0 ? w : 0);
}

static int fstop_y(int index)
{
    return SCALE_TOP + (fos_nfstops - 1 - index) * THIRD_PX;
}

/* text through a coverage map: grey on black, or red LEDs (two levels) */
static int text_gray(const struct fos_font *f, int x, int y, const char *s, int pct, int track)
{
    uint8_t map[16];
    map[0] = 0;
    for (int i = 1; i < 16; i++)
        map[i] = i < 3 ? 0 : pal->gray[pct * i / 15];
    return fos_text_map(vr, vp, W, H, f, x, y, s, map, track);
}

static int text_led(const struct fos_font *f, int x, int y, const char *s, int on, int track)
{
    uint8_t map[16];
    for (int i = 0; i < 16; i++)
        map[i] = i < 5 ? 0 : !on ? pal->gray[i < 10 ? 6 : 9] : i < 10 ? pal->dark_red : pal->red;
    return fos_text_map(vr, vp, W, H, f, x, y, s, map, track);
}

/* ---------------------------------------------------------------- seven segments */

/* segments a..g as bits 0..6 */
static int seg_bits(int ch)
{
    static const uint8_t digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    switch (ch)
    {
        case '-': return 0x40;
        case 'E': return 0x79;
        case 'n': return 0x54;
        case 'd': return 0x5E;
        case 'F': return 0x71;
        case 'b': return 0x7C;
        case 'u': return 0x1C;
        case 'L': return 0x38;
        case 'S': return 0x6D;
        case 'A': return 0x77;
        case 't': return 0x78;
        case 'o': return 0x5C;
    }
    return 0;
}

static int DIG_W = 15, DIG_H = 26, SEG_T = 3, DIG_GAP = 6;     /* the plate draws them bigger */
static void seg_size(int w, int h, int t, int gap) { DIG_W = w; DIG_H = h; SEG_T = t; DIG_GAP = gap; }

/* one slanted digit; off segments are drawn faintly, like the unlit LEDs
 * you can just see behind a real viewfinder mask */
static void seg_digit(int x, int y, int bits, uint8_t on, uint8_t off)
{
    int half = DIG_H / 2;
    for (int r = 0; r < DIG_H; r++)
    {
        int slant = (DIG_H - 1 - r) * 3 / DIG_H;    /* leans right like the real ones */
        int yy = y + r;
        /* horizontals: a at the top, g in the middle, d at the bottom */
        int hseg = -1;
        if (r < SEG_T) hseg = 0;
        else if (r >= half - 1 && r < half - 1 + SEG_T) hseg = 6;
        else if (r >= DIG_H - SEG_T) hseg = 3;
        if (hseg >= 0)
        {
            /* chamfered ends: shorter where the segment meets the verticals */
            int inset = hseg == 6 ? 2 : (r == 0 || r == DIG_H - 1) ? 2 : 1;
            fill(x + slant + inset + 1, yy, DIG_W - 2 * inset - 2, 1, (bits >> hseg) & 1 ? on : off);
        }
        /* verticals: f and b above the middle, e and c below */
        int upper = r < half;
        if (r > 1 && r < DIG_H - 2 && r != half - 1 + SEG_T / 2)
        {
            int left = upper ? 5 : 4, right = upper ? 1 : 2;
            fill(x + slant, yy, SEG_T - (r == 2 || r == DIG_H - 3 ? 1 : 0), 1, (bits >> left) & 1 ? on : off);
            fill(x + slant + DIG_W - SEG_T, yy, SEG_T, 1, (bits >> right) & 1 ? on : off);
        }
    }
}

/* a string of digits; '.' is a dot after the previous digit, '"' the
 * seconds mark; returns the width */
static int seg_text(int x, int y, const char *s, int lit)
{
    uint8_t on = lit ? pal->red : pal->gray[9], off = pal->gray[4];
    int x0 = x;
    for (; *s; s++)
    {
        if (*s == '.')
        {
            fill(x - DIG_GAP + 1, y + DIG_H - SEG_T, SEG_T, SEG_T, on);
            x += 2;
            continue;
        }
        if (*s == '"')
        {
            fill(x - 2, y, 2, 6, on);
            fill(x + 2, y, 2, 6, on);
            x += 8;
            continue;
        }
        seg_digit(x, y, seg_bits(*s), on, off);
        x += DIG_W + DIG_GAP;
    }
    return x - x0;
}

/* the meter's pointer: a red wedge pointing at the scale */
static void needle(int y, uint8_t c)
{
    int x0 = W - STRIP_W + 4;
    for (int r = -7; r <= 7; r++)
        fill(x0, y + r, 16 - 2 * (r < 0 ? -r : r), 1, c);
}

/* the look the next frame gets: a red tab, its name, and any baked looks that are on */
static void look_badge(int bx, int by, const struct fos_hud *h)
{
    if (!h->film) return;
    const struct fos_font *f = &fos_font_xs;
    char tags[32];
    tags[0] = 0;
    if (h->grain) snprintf(tags, sizeof(tags), "GRAIN");
    if (h->halation) snprintf(tags + strlen(tags), sizeof(tags) - strlen(tags), "%sHALATION", tags[0] ? "  " : "");
    int nw = fos_text_w(f, h->film, 24), tw = tags[0] ? fos_text_w(f, tags, 24) : 0;
    int bw = 8 + 8 + 8 + nw + (tw ? 14 + tw : 0) + 10, bh = 26;
    fill(bx, by, bw, bh, pal->gray[30]);
    fill(bx + 1, by + 1, bw - 2, bh - 2, pal->black);
    fill(bx + 1, by + 1, 6, bh - 2, pal->red);
    text_gray(f, bx + 22, by + 18, h->film, 92, 24);
    if (tw) text_led(f, bx + 22 + nw + 14, by + 18, tags, 1, 24);
}

/* ---------------------------------------------------------------- the HUD */

void fos_hud_draw(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    vr = vram; vp = pitch; pal = p;
    uint8_t bg = p->black;
    char label[8];

    /* the meter strip */
    int sx = W - STRIP_W;
    fill(sx, 0, STRIP_W, VIEW_H, bg);
    fill(sx, 0, 1, VIEW_H, p->gray[18]);
    /* red zones: more light than f/22 can take, less than the lens can open to */
    fill(sx + 27, 4, 4, SCALE_TOP - 11, p->dark_red);
    fill(sx + 27, fstop_y(h->widest) + 9, 4, 20, p->dark_red);
    for (int i = 1; i < fos_nfstops; i++)
    {
        int y = fstop_y(i);
        int usable = i >= h->widest;
        if ((i - 1) % 3)
        {
            fill(sx + 27, y, 4, 1, p->gray[usable ? 30 : 14]);      /* thirds */
            continue;
        }
        fos_format_fstop(label, sizeof(label), i);
        fill(sx + 24, y, 8, 1, p->gray[usable ? 55 : 22]);
        text_gray(&fos_font_xs, sx + 36, y + 5, label, i == h->fstop ? 100 : usable ? 62 : 26, 0);
    }
    /* the aperture you set: white tick */
    fill(sx + 20, fstop_y(h->fstop) - 1, 14, 3, p->white);
    /* the needle points at the aperture the meter asks for, and blinks
     * at either end when that is out of reach */
    if (h->lit && h->meter)
    {
        int y;
        int out = h->want > fos_nfstops - 1 || h->want < h->widest;
        if (h->want > fos_nfstops - 1) y = SCALE_TOP - 12;
        else if (h->want < h->widest) y = fstop_y(h->widest) + 18;
        else y = fstop_y(h->want);
        if (!out || h->blink)
            needle(y, p->red);
    }

    /* the LED strip */
    int by = VIEW_H;
    fill(0, by, W, BAR_H, bg);
    fill(0, by, W, 1, p->gray[18]);
    int ty = by + (BAR_H - DIG_H) / 2 + 1;
    int base = ty + DIG_H;

    /* wind reminder: blinks whether or not the meter is on */
    text_led(&fos_font_mdb, 20, base - 3, "WIND", h->need_wind && h->blink, 24);

    /* shutter speed: 1/125 shows as 125, slow speeds keep their seconds mark */
    seg_text(132, ty, h->shutter, h->lit);

    /* aperture */
    text_gray(&fos_font_xs, 262, base - 4, "f", h->lit ? 70 : 30, 0);
    fos_format_fstop(label, sizeof(label), h->fstop);
    seg_text(274, ty, label, h->lit);

    /* meter on */
    for (int r = -6; r <= 6; r++)
    {
        int w2 = fos_isqrt(36 - r * r);
        fill(392 - w2, by + BAR_H / 2 + r, 2 * w2 + 1, 1, h->lit ? p->red : p->gray[9]);
    }

    /* the frame counter, END at the end */
    int lit_count = h->lit || h->finished;
    if (h->finished)
        seg_text(452, ty, h->blink ? "End" : "   ", 1);
    else
    {
        char n[8];
        snprintf(n, sizeof(n), "%d", h->frame);
        int w = seg_text(452, ty, n, lit_count);
        snprintf(n, sizeof(n), "/%d", h->exposures);
        text_gray(&fos_font_sm, 452 + w + 2, base - 2, n, lit_count ? 55 : 28, 0);
    }

    look_badge(14, 12, h);

    /* focusing screen: microprism collar and split-image circle */
    if (h->focus_screen)
    {
        int cx = VIEW_W / 2, cy = VIEW_H / 2;
        for (int a = 0; a < 360; a += 2)
        {
            int c = fos_cos(a * 65536 / 360), s = fos_sin(a * 65536 / 360);
            int x = cx + c * 62 / 4096, y = cy + s * 62 / 4096;
            fill(x, y, 1, 1, p->gray[70]);
            x = cx + c * 28 / 4096; y = cy + s * 28 / 4096;
            fill(x, y, 1, 1, p->white);
        }
        fill(cx - 28, cy, 57, 1, p->white);
    }
}

/* make our strips transparent again so LiveView shows through */
void fos_hud_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p)
{
    vr = vram; vp = pitch; pal = p;
    fill(W - STRIP_W, 0, STRIP_W, VIEW_H, p->clear);
    fill(0, VIEW_H, W, BAR_H, p->clear);
    fill(VIEW_W / 2 - 64, VIEW_H / 2 - 64, 129, 129, p->clear);
}

/* ---------------------------------------------------------------- the back screen */

/* a rounded rectangle: each row is inset by the circle of radius r */
static void rrect(int x, int y, int w, int hh, int r, uint8_t c)
{
    if (r > hh / 2) r = hh / 2;
    for (int j = 0; j < hh; j++)
    {
        int e = j < hh - 1 - j ? j : hh - 1 - j, in = 0;
        if (e < r) in = r - fos_isqrt((unsigned) (r * r - (r - e) * (r - e)));
        fill(x + in, y + j, w - 2 * in, 1, c);
    }
}

static int seg_width(const char *s)
{
    int w = 0;
    for (; *s; s++) w += *s == '.' ? 2 : *s == '"' ? 8 : DIG_W + DIG_GAP;
    return w > DIG_GAP ? w - DIG_GAP : w;
}

/* the glossy red tube of the bake bar: a lit top, the body, a shaded underside (Canon's palette has one red and one dark red) */
static void tube(int x, int y, int w, int hh, int r, const struct fos_hud_palette *p, uint8_t body, uint8_t shade)
{
    for (int j = 0; j < hh; j++)
    {
        int e = j < hh - 1 - j ? j : hh - 1 - j, in = 0;
        if (e < r) in = r - fos_isqrt((unsigned) (r * r - (r - e) * (r - e)));
        uint8_t a = body, b = body;
        if (j == 0 || j >= hh - 2) a = b = shade;
        else if (j == 1) { a = p->gray[85]; b = body; }
        else if (j == 2) { a = body; b = p->gray[60]; }
        else if (j >= hh - 4) { a = body; b = shade; }
        for (int i = in; i < w - in; i++) vr[(y + j) * vp + x + i] = ((x + i + j) & 1) ? a : b;
    }
}

/* a small picture icon for the quality chip: a frame, a sun, and a mountain that is solid for Fine and hollow for Normal */
static void pic_icon(int x, int y, int fine, uint8_t c)
{
    fill(x, y, 18, 1, c);
    fill(x, y + 13, 18, 1, c);
    fill(x, y, 1, 14, c);
    fill(x + 17, y, 1, 14, c);
    fill(x + 12, y + 3, 2, 2, c);                           /* the sun */
    for (int r = 0; r < 8; r++)                             /* the mountain: peak at x+6, base on the bottom row */
    {
        int half = r, left = x + 7 - half, wd = 2 * half + 1;
        if (left < x + 2) { wd -= x + 2 - left; left = x + 2; }
        if (left + wd > x + 16) wd = x + 16 - left;
        if (wd <= 0) continue;
        if (fine || r == 7) fill(left, y + 5 + r, wd, 1, c);
        else { fill(left, y + 5 + r, 1, 1, c); fill(left + wd - 1, y + 5 + r, 1, 1, c); }
    }
}


/* ---------------------------------------------------------------- the modern plate */

/* a rounded block with a vertical gradient of greys (negative = black), c0 at the top and c1 at the bottom */
static void rgrad(int x, int y, int w, int hh, int r, int c0, int c1)
{
    if (r > hh / 2) r = hh / 2;
    for (int j = 0; j < hh; j++)
    {
        int e = j < hh - 1 - j ? j : hh - 1 - j, in = 0;
        if (e < r) in = r - fos_isqrt((unsigned) (r * r - (r - e) * (r - e)));
        int c = c0 + (c1 - c0) * j / (hh > 1 ? hh - 1 : 1);
        fill(x + in, y + j, w - 2 * in, 1, c < 0 ? pal->black : pal->gray[c > 100 ? 100 : c]);
    }
}

/* a raised card: a shadow below, a lit rim, a gradient face and a line of light along the top edge */
static void raised(int x, int y, int w, int hh, int r)
{
    rrect(x - 1, y + 4, w + 2, hh + 2, r, pal->black);
    rrect(x, y, w, hh, r, pal->gray[50]);
    rgrad(x + 1, y + 1, w - 2, hh - 2, r - 1, 42, 14);
    fill(x + r, y + 1, w - 2 * r, 1, pal->gray[66]);
    fill(x + r, y + hh - 1, w - 2 * r, 1, pal->black);
}

/* a recessed well: dark at the top, a little lighter below, a light lip along the bottom */
static void well(int x, int y, int w, int hh, int r)
{
    rrect(x, y, w, hh, r, pal->black);
    rgrad(x + 1, y + 1, w - 2, hh - 2, r - 1, -1, 12);
    fill(x + r, y + hh, w - 2 * r, 1, pal->gray[34]);
}

/* a readout in red with a darker extrusion under it */
static void digits_modern(int cx, int base, const char *s)
{
    const struct fos_font *f = &fos_font_xl;
    const int w = fos_text_w(f, s, 0), x = cx - w / 2;
    uint8_t edge[16], face[16];
    for (int i = 0; i < 16; i++) { edge[i] = i < 4 ? 0 : pal->dark_red; face[i] = i < 4 ? 0 : i < 8 ? pal->dark_red : pal->red; }
    fos_text_map(vr, vp, W, H, f, x + 2, base + 3, s, edge, 0);
    fos_text_map(vr, vp, W, H, f, x + 1, base + 2, s, edge, 0);
    fos_text_map(vr, vp, W, H, f, x, base, s, face, 0);
}

static void plate_modern(const struct fos_hud *h)
{
    const struct fos_hud_palette *p = pal;
    /* the background: black at the top fading into dark red, a dither of the two (up to 26 of 64 pixels, an 8 x 8 ordered pattern) */
    for (int y = 0; y < H; y++)
    {
        const int cov = y * 26 / H;
        uint8_t row8[8];                                              /* the pattern repeats every 8 pixels: work out one period, copy it across */
        for (int x = 0; x < 8; x++)
        {
            int v = 0;
            for (int i = 0; i < 3; i++) v |= (((y >> i) & 1) << 1 | (((x ^ y) >> i) & 1)) << (2 * (2 - i));
            row8[x] = v < cov ? p->dark_red : p->black;
        }
        for (int x = 0; x < W; x += 8) memcpy(vr + y * vp + x, row8, 8);
    }

    /* the top: the look, its baked options, and the picture quality */
    look_badge(32, 20, h);
    if (h->quality[0])
    {
        const int tw = fos_text_w(&fos_font_sm, h->quality, 16);
        text_gray(&fos_font_sm, W - 32 - tw, 44, h->quality, 90, 16);
        pic_icon(W - 32 - tw - 28, 29, h->quality_fine, p->gray[82]);
    }

    /* three cards: shutter, aperture, ISO */
    static const char *const label[3] = { "SHUTTER", "APERTURE", "ISO" };
    char text[3][12];
    snprintf(text[0], sizeof(text[0]), "%s", h->shutter[0] ? h->shutter : "--");
    fos_format_fstop(text[1], sizeof(text[1]), h->fstop);
    if (h->iso > 0) snprintf(text[2], sizeof(text[2]), "%d", h->iso);
    else snprintf(text[2], sizeof(text[2]), "Auto");
    const int cw = 208, gap = 16, cy = 62, ch = 128;
    for (int i = 0; i < 3; i++)
    {
        const int x = 32 + i * (cw + gap);
        raised(x, cy, cw, ch, 20);
        text_gray(&fos_font_xs, x + 18, cy + 24, label[i], 85, 28);
        rrect(x + cw - 22, cy + 17, 6, 6, 3, p->red);                 /* the small lamp */
        well(x + 12, cy + 36, cw - 24, ch - 48, 12);
        digits_modern(x + cw / 2, cy + 36 + (ch - 48) / 2 + 18, text[i]);
    }

    /* the light meter in a card of its own */
    raised(32, 208, W - 64, 64, 18);
    well(46, 222, W - 92, 36, 10);
    const int cx = W / 2, step = 44, my = 240;
    for (int k = -6; k <= 6; k++)
    {
        int whole = k % 3 == 0, len = whole ? 14 : 7;
        fill(cx + k * step, my - len / 2, 1, len, p->gray[whole ? 58 : 36]);
    }
    fill(cx - 1, my - 15, 3, 31, p->white);
    if (h->meter)
    {
        int off = h->want - h->fstop, out = off < -6 || off > 6;
        if (off < -6) off = -6;
        if (off > 6) off = 6;
        if (!out || h->blink)
            for (int r = 0; r <= 12; r++) fill(cx + off * step - (12 - r), my - 9 + r, 2 * (12 - r) + 1, 1, p->red);
    }

    /* the camera's other readings as raised pills; empty ones are left out */
    {
        char c[6][16];
        int n = 0, low = 0, clock = -1;
        if (h->mode[0]) snprintf(c[n++], sizeof(c[0]), "%s", h->mode);
        if (h->focal > 0) snprintf(c[n++], sizeof(c[0]), "%d mm", h->focal);
        if (h->wb[0]) snprintf(c[n++], sizeof(c[0]), "WB %s", h->wb);
        if (h->shots_left >= 0) snprintf(c[n++], sizeof(c[0]), "%d left", h->shots_left);
        if (h->battery >= 0) { low = n; snprintf(c[n++], sizeof(c[0]), "%d%%", h->battery); }
        if (h->clock[0]) { clock = n; snprintf(c[n++], sizeof(c[0]), "%s", h->clock); }
        int x = 32;
        for (int i = 0; i < n; i++)
        {
            const int icon = (h->battery >= 0 && i == low) || i == clock, cwid = fos_text_w(&fos_font_sm, c[i], 0) + 26 + (icon ? 18 : 0);
            const int alert = h->battery >= 0 && h->battery <= 15 && i == low;
            rrect(x, 297, cwid, 28, 14, p->black);
            rrect(x, 296, cwid, 28, 14, alert ? (p->alert ? p->alert : p->red) : p->gray[44]);
            rgrad(x + 1, 297, cwid - 2, 26, 13, 34, 16);
            fill(x + 10, 297, cwid - 20, 1, p->gray[58]);
            int tx = x + 13;
            if (i == low && h->battery >= 0)
            {
                fill(tx, 305, 12, 1, p->gray[70]); fill(tx, 311, 12, 1, p->gray[70]); fill(tx, 305, 1, 7, p->gray[70]); fill(tx + 11, 305, 1, 7, p->gray[70]);
                fill(tx + 12, 307, 2, 3, p->gray[70]); fill(tx + 2, 307, h->battery > 15 ? 7 : 2, 3, alert ? p->alert ? p->alert : p->red : p->red);
                tx += 18;
            }
            else if (i == clock)
            {
                rrect(tx, 303, 11, 11, 5, p->gray[70]); rrect(tx + 1, 304, 9, 9, 4, p->gray[20]);
                fill(tx + 5, 305, 1, 4, p->gray[70]); fill(tx + 5, 308, 3, 1, p->gray[70]);
                tx += 18;
            }
            text_gray(&fos_font_sm, tx, 315, c[i], alert ? 100 : 86, 0);
            x += cwid + 8;
        }
    }

    /* the bake queue: its text, and a rounded tube on a plain track */
    const int by = 384, bw = W - 64, bh = 14;
    text_gray(&fos_font_sm, 32, 372, h->status[0] ? h->status : "NOTHING TO DEVELOP", h->bake_queued ? 90 : 45, 0);
    rrect(32, by, bw, bh, 7, p->black);
    rrect(33, by + 1, bw - 2, bh - 2, 6, p->gray[8]);
    if (h->bake_queued)
    {
        int fw = bh;
        uint8_t body = p->red, shade = p->dark_red;
        if (h->bake_active || h->bake_paused)
        {
            fw = (bw - 4) * h->bake_percent / 100;
            if (fw < bh) fw = bh;
            if (h->bake_paused && h->blink) { body = p->gray[40]; shade = p->gray[20]; }
        }
        else if (h->blink) { body = p->dark_red; shade = p->dark_red; }
        else fw = bh * 3;
        tube(34, by + 1, fw - 2 > bw - 4 ? bw - 4 : fw - 2, bh - 2, 6, p, body, shade);
    }

    const char *hint = "TRASH  Home      PLAY  Images";
    text_gray(&fos_font_xs, W - 32 - fos_text_w(&fos_font_xs, hint, 24), H - 22, hint, 45, 24);
    text_gray(&fos_font_xs, 32, H - 22, "B2B STUDIO", 30, 40);
}


/* the look menu over the plate: a column of names with the chosen one marked; the dial moves it */
static void plate_menu(const struct fos_hud *h)
{
    if (h->menu_n < 1) return;
    const struct fos_hud_palette *p = pal;
    const int x = 150, y = 58, w = 420, rows = h->menu_n > 9 ? 9 : h->menu_n, ph = 74 + rows * 34;
    rrect(x - 2, y + 4, w + 4, ph, 20, p->black);
    rrect(x, y, w, ph, 20, p->gray[50]);
    rrect(x + 1, y + 1, w - 2, ph - 2, 19, p->gray[10]);
    text_gray(&fos_font_xs, x + 24, y + 30, "LOOK", 85, 28);
    const char *hint = "DIAL  CHOOSE     SET  DONE";
    text_gray(&fos_font_xs, x + w - 24 - fos_text_w(&fos_font_xs, hint, 20), y + 30, hint, 50, 20);
    fill(x + 20, y + 42, w - 40, 1, p->gray[28]);
    for (int r = 0; r < rows; r++)
    {
        const int ry = y + 52 + r * 34, d = r > h->menu_row ? r - h->menu_row : h->menu_row - r;
        if (r == h->menu_row) rrect(x + 14, ry, w - 28, 30, 15, p->red);
        const char *n = h->menu_name[r] ? h->menu_name[r] : "";
        text_gray(&fos_font_md, x + 32, ry + 22, n, r == h->menu_row ? 100 : d == 1 ? 78 : 55, 0);
    }
}

void fos_hud_plate(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    vr = vram; vp = pitch; pal = p;
    if (h->style == 1) { plate_modern(h); plate_menu(h); return; }
    fill(0, 0, W, H, p->black);

    /* the top: the look the next frame gets, and the speed of the film */
    look_badge(28, 24, h);
    /* top right: the picture quality, an icon and the size ("S1", "L", "RAW+M"); the icon's mountain is solid for Fine */
    if (h->quality[0])
    {
        const int tw = fos_text_w(&fos_font_sm, h->quality, 16);
        text_gray(&fos_font_sm, W - 28 - tw, 48, h->quality, 90, 16);
        pic_icon(W - 28 - tw - 28, 33, h->quality_fine, p->gray[82]);
    }
    fill(28, 70, W - 56, 1, p->gray[18]);

    /* three windows: shutter, aperture, frame. Unlit segments show faintly behind the lit ones. */
    static const char *const label[3] = { "SHUTTER", "APERTURE", "ISO" };
    const int wx[3] = { 28, 260, 492 }, wy = 108, ww = 200, wh = 100;
    char text[3][12];
    snprintf(text[0], sizeof(text[0]), "%s", h->shutter[0] ? h->shutter : "--");
    fos_format_fstop(text[1], sizeof(text[1]), h->fstop);
    if (h->iso > 0) snprintf(text[2], sizeof(text[2]), "%d", h->iso);
    else snprintf(text[2], sizeof(text[2]), "Auto");
    seg_size(30, 56, 6, 10);
    for (int i = 0; i < 3; i++)
    {
        text_gray(&fos_font_xs, wx[i] + 6, 100, label[i], 45, 28);
        rrect(wx[i], wy, ww, wh, 16, p->gray[18]);
        rrect(wx[i] + 1, wy + 1, ww - 2, wh - 2, 15, p->gray[5]);
        int lit = 1;                                                    /* the readouts are always lit on the plate; only the meter pointer waits for a half-press */
        if (seg_width(text[i]) > ww - 28) seg_size(22, 40, 4, 8);        /* six-digit ISOs: smaller digits */
        int dx = wx[i] + (ww - seg_width(text[i])) / 2 + (i == 1 ? 14 : 0);
        seg_text(dx, wy + (wh - DIG_H) / 2, text[i], lit);
        seg_size(30, 56, 6, 10);
    }
    seg_size(15, 26, 3, 6);
    text_gray(&fos_font_md, wx[1] + 18, wy + 70, "f", 72, 0);

    /* the light meter: a third of a stop every 40 px, whole stops longer, a red pointer when the meter is on */
    const int cx = W / 2, step = 40, my = 292;
    fill(cx - 6 * step - 4, my, 12 * step + 9, 2, p->gray[30]);
    for (int k = -6; k <= 6; k++)
    {
        int whole = k % 3 == 0, x = cx + k * step, len = whole ? 20 : 10, wd = k == 0 ? 3 : 1;
        fill(x - wd / 2, my - len, wd, len, k == 0 ? p->white : p->gray[whole ? 60 : 32]);
        if (whole)
        {
            char t[6];
            if (k == 0) snprintf(t, sizeof(t), "0"); else snprintf(t, sizeof(t), "%+d", k / 3);
            text_gray(&fos_font_xs, x - fos_text_w(&fos_font_xs, t, 0) / 2, my + 22, t, 55, 0);
        }
    }
    text_gray(&fos_font_xs, 28, my + 4, "UNDER", 40, 28);
    text_gray(&fos_font_xs, W - 28 - fos_text_w(&fos_font_xs, "OVER", 28), my + 4, "OVER", 40, 28);
    if (h->meter)
    {
        int off = h->want - h->fstop, out = off < -6 || off > 6;
        if (off < -6) off = -6;
        if (off > 6) off = 6;
        if (!out || h->blink)
            for (int r = 0; r <= 14; r++) fill(cx + off * step - (14 - r), my - 40 + r, 2 * (14 - r) + 1, 1, p->red);
    }


    /* the camera's other readings as chips between the meter and the bake bar; empty ones are left out */
    {
        char c[6][16];
        int n = 0, low = 0;
        if (h->mode[0]) snprintf(c[n++], sizeof(c[0]), "%s", h->mode);
        if (h->focal > 0) snprintf(c[n++], sizeof(c[0]), "%d mm", h->focal);
        if (h->wb[0]) snprintf(c[n++], sizeof(c[0]), "WB %s", h->wb);
        if (h->shots_left >= 0) snprintf(c[n++], sizeof(c[0]), "%d left", h->shots_left);
        if (h->battery >= 0) { low = n; snprintf(c[n++], sizeof(c[0]), "%d%%", h->battery); }
        if (h->clock[0]) snprintf(c[n++], sizeof(c[0]), "%s", h->clock);
        int total = -10, cw[6];
        for (int i = 0; i < n; i++) { cw[i] = fos_text_w(&fos_font_sm, c[i], 0) + 28; total += cw[i] + 10; }
        int x = (W - total) / 2;
        for (int i = 0; i < n; i++)
        {
            int alert = h->battery >= 0 && h->battery <= 15 && i == low;
            rrect(x, 338, cw[i], 30, 15, alert ? (p->alert ? p->alert : p->red) : p->gray[18]);
            rrect(x + 1, 339, cw[i] - 2, 28, 14, p->gray[5]);
            text_gray(&fos_font_sm, x + 14, 359, c[i], alert ? 100 : 82, 0);
            x += cw[i] + 10;
        }
    }

    /* the bake queue, where a film camera has its frames-left bar: a rounded red tube that fills as the picture is developed */
    const int by = 408, bw = W - 56, bh = 16;
    const char *state = h->status[0] ? h->status : "NOTHING TO DEVELOP";
    text_gray(&fos_font_sm, 28, by - 8, state, h->bake_queued ? 88 : 40, 0);
    rrect(28, by, bw, bh, 8, p->gray[18]);
    rrect(29, by + 1, bw - 2, bh - 2, 7, p->gray[5]);
    if (h->bake_queued)
    {
        int fw = bh;
        uint8_t body = p->red, shade = p->dark_red;
        if (h->bake_active || h->bake_paused)
        {
            fw = (bw - 4) * h->bake_percent / 100;
            if (fw < bh) fw = bh;
            if (h->bake_paused && h->blink) { body = p->gray[40]; shade = p->gray[20]; }
        }
        else if (h->blink) { body = p->dark_red; shade = p->dark_red; }      /* preparing the review: a small block that pulses */
        else fw = bh * 3;
        tube(30, by + 1, fw - 2 > bw - 4 ? bw - 4 : fw - 2, bh - 2, 7, p, body, shade);
    }

    /* the buttons */
    const char *hint = "TRASH  Home      PLAY  Images";
    text_gray(&fos_font_xs, W - 28 - fos_text_w(&fos_font_xs, hint, 24), H - 22, hint, 40, 24);
    text_gray(&fos_font_xs, 28, H - 22, "B2B STUDIO", 25, 40);
    plate_menu(h);
}

/* the screen goes back to Canon's: transparent, so what Canon draws (or LiveView) shows */
void fos_hud_plate_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p)
{
    vr = vram; vp = pitch; pal = p;
    fill(0, 0, W, H, p->clear);
}
