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
    }
    return 0;
}

#define DIG_W   15
#define DIG_H   26
#define SEG_T   3
#define DIG_GAP 6

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

    /* which film, quietly */
    if (h->film)
    {
        int w = fos_text_w(&fos_font_xs, h->film, 32);
        text_gray(&fos_font_xs, W - 22 - w, base - 6, h->film, 40, 32);
    }

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
