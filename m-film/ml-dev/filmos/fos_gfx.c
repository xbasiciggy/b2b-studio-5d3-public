/* fos_gfx: palette ramps, dithering, anti-aliased shapes and text.
 * See fos_gfx.h for the idea. Everything here is integer maths except the
 * one-time palette build. */

#include "fos_gfx.h"
#include "fos_dev.h"
#include <string.h>

/* ---------------------------------------------------------------- ramps */

struct ramp_def
{
    uint32_t lo, mid, hi;   /* near-black, hue, light tint (0xRRGGBB) */
    int mid_at;             /* level of the hue, 0 = straight lo -> hi */
    int n;                  /* palette entries */
};

/* 254 entries in all (index 0 and 255 stay Canon's) */
static const struct ramp_def ramp_defs[RAMP_COUNT] =
{
    [RAMP_GRAY]    = { 0x040303, 0,        0xFFFCF6,   0, 86 },
    [RAMP_METAL]   = { 0x050608, 0x878F98, 0xF4F7FA, 150, 22 },
    [RAMP_AMBER]   = { 0x080402, 0xF4A81C, 0xFFF2D2, 185, 24 },
    [RAMP_RED]     = { 0x080202, 0xDF262D, 0xFFD8D4, 150, 18 },
    [RAMP_ORANGE]  = { 0x080302, 0xEA5D1E, 0xFFDEC9, 160, 16 },
    [RAMP_CRIMSON] = { 0x060102, 0x9C1320, 0xF6C4C8, 120, 16 },
    [RAMP_GREEN]   = { 0x020604, 0x13894B, 0xD0F5DF, 140, 18 },
    [RAMP_VIOLET]  = { 0x040208, 0x5A2D8A, 0xE9DCF7, 120, 16 },
    [RAMP_BLUE]    = { 0x020508, 0x1E77C6, 0xD5EBFF, 150, 16 },
    [RAMP_LEADER]  = { 0x050403, 0x4C3B2E, 0xDCCAB6, 130, 22 },
};

uint32_t fos_pal[256];

uint8_t fos_ramp_base[RAMP_COUNT];
uint8_t fos_ramp_last[RAMP_COUNT];
int fos_ramp_mul[RAMP_COUNT];
#define ramp_base fos_ramp_base
#define ramp_mul fos_ramp_mul
static uint8_t ramp_n[RAMP_COUNT];

uint8_t  fos_rev_ramp[256];
uint16_t fos_rev_lv[256];
uint8_t  fos_luma_tab[256];
uint8_t  fos_chroma_tab[256];
uint16_t fos_lum2lv[RAMP_COUNT][256];
#define rev_ramp fos_rev_ramp
#define rev_lv fos_rev_lv
#define luma_tab fos_luma_tab
#define chroma_tab fos_chroma_tab
#define lum2lv fos_lum2lv

const uint8_t fos_bayer[4][4] =
{
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static int chan(uint32_t c, int s) { return (c >> s) & 0xFF; }

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int tmax)
{
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8)
    {
        int v = chan(a, s) + (chan(b, s) - chan(a, s)) * t / tmax;
        out |= (uint32_t) fos_clamp(v, 0, 255) << s;
    }
    return out;
}

static int rgb_luma(uint32_t c)
{
    return (54 * chan(c, 16) + 183 * chan(c, 8) + 19 * chan(c, 0) + 128) >> 8;
}

static int rgb_chroma(uint32_t c)
{
    int r = chan(c, 16), g = chan(c, 8), b = chan(c, 0);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx - mn;
}

FOS_INLINE int ix_near(int ramp, int lv)
{
    int n = ramp_n[ramp];
    int p = ((lv < 0 ? 0 : lv) * ramp_mul[ramp] + (8 << 16)) >> 20;
    return ramp_base[ramp] + (p >= n ? n - 1 : p);
}

FOS_INLINE int ix_dith(int ramp, int lv, int x, int y)
{
    int n = ramp_n[ramp];
    int p = (((lv < 0 ? 0 : lv) * ramp_mul[ramp] >> 16) + fos_bayer[y & 3][x & 3]) >> 4;
    return ramp_base[ramp] + (p >= n ? n - 1 : p);
}

void fos_gfx_init()
{
    int ix = 1;
    for (int r = 0; r < RAMP_COUNT; r++)
    {
        const struct ramp_def *d = &ramp_defs[r];
        ramp_base[r] = ix;
        ramp_n[r] = d->n;
        fos_ramp_last[r] = d->n - 1;
        ramp_mul[r] = (d->n - 1) * 16 * 65536 / LV_MAX;
        for (int i = 0; i < d->n; i++, ix++)
        {
            int lv = i * 255 / (d->n - 1);      /* 0..255 along the ramp */
            uint32_t c;
            if (!d->mid_at)
                c = lerp_rgb(d->lo, d->hi, lv, 255);
            else if (lv <= d->mid_at)
                c = lerp_rgb(d->lo, d->mid, lv, d->mid_at);
            else
                c = lerp_rgb(d->mid, d->hi, lv - d->mid_at, 255 - d->mid_at);
            fos_pal[ix] = c;
            rev_ramp[ix] = r;
            rev_lv[ix] = i * LV_MAX / (d->n - 1);
            luma_tab[ix] = rgb_luma(c);
            chroma_tab[ix] = rgb_chroma(c);
        }
    }

    /* Canon's entries (and anything unused) read as black */
    for (int i = ix; i < 256; i++)
        rev_ramp[i] = RAMP_GRAY;
    rev_ramp[0] = RAMP_GRAY;

    /* luminance -> level, per ramp (ramps get lighter monotonically) */
    for (int r = 0; r < RAMP_COUNT; r++)
    {
        int b = ramp_base[r], n = ramp_n[r];
        int j = 0;
        for (int l = 0; l < 256; l++)
        {
            while (j < n - 2 && luma_tab[b + j + 1] < l)
                j++;
            int l0 = luma_tab[b + j], l1 = luma_tab[b + j + 1];
            int f = l1 > l0 ? fos_clamp((l - l0) * 256 / (l1 - l0), 0, 256) : 0;
            int lv = (j * 256 + f) * LV_MAX / ((n - 1) * 256);
            lum2lv[r][l] = fos_clamp(lv, 0, LV_MAX);
        }
    }
}

/* ---------------------------------------------------------------- screen colour */

uint32_t fos_screen_yuv(int ix, int white_y, int std, int sat, int warm, int bright)
{
    return fos_screen_yuv_rgb(fos_pal[ix], white_y, std, sat, warm, bright);
}

uint32_t fos_screen_yuv_rgb(uint32_t rgb, int white_y, int std, int sat, int warm, int bright)
{
    int c[3] = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF };
    /* warmer: more red, less blue, white included */
    c[0] = fos_clamp(c[0] * (100 + 3 * warm) / 100, 0, 255);
    c[2] = fos_clamp(c[2] * (100 - 3 * warm) / 100, 0, 255);
    /* brighter: the mid tones lift, black and white stay */
    for (int i = 0; i < 3; i++)
        c[i] = fos_clamp(c[i] + bright * 6 * c[i] * (255 - c[i]) / (100 * 255), 0, 255);
    int r = c[0], g = c[1], b = c[2];
    int s = 100 + 6 * sat;              /* colour, % */
    int y, u, v;
    if (std == 1)
    {
        y = (217 * r + 732 * g + 73 * b) / 1024;
        u = (-117 * r - 394 * g + 512 * b) / 1024;
        v = (512 * r - 465 * g - 46 * b) / 1024;
    }
    else
    {
        /* BT.601 Y'CbCr, all of it scaled so white lands on Canon's white:
         * Canon's own entries on the EOS M decode this way to clean
         * primaries (dark red 202,0,0; blue 0,0,170; green 0,155,0) */
        int yf = (299 * r + 587 * g + 114 * b) / 1000;
        y = yf * white_y / 255;
        u = (b - yf) * 564 / 1000 * white_y / 255;
        v = (r - yf) * 713 / 1000 * white_y / 255;
    }
    y = fos_clamp(y, 0, 255);
    u = fos_clamp(u * s / 100, -127, 127);
    v = fos_clamp(v * s / 100, -127, 127);
    return (uint32_t) y << 16 | (uint32_t) (u & 0xFF) << 8 | (uint32_t) (v & 0xFF);
}

uint32_t fos_screen_rgb(uint32_t yuv, int white_y)
{
    int y = (yuv >> 16) & 0xFF, u = (int8_t) ((yuv >> 8) & 0xFF), v = (int8_t) (yuv & 0xFF);
    /* undo the scale, then BT.601 */
    y = y * 255 / white_y; u = u * 255 / white_y; v = v * 255 / white_y;
    int r = y + v * 1000 / 713;
    int b = y + u * 1000 / 564;
    int g = (1000 * y - 299 * r - 114 * b) / 587;
    return (uint32_t) fos_clamp(r, 0, 255) << 16 | (uint32_t) fos_clamp(g, 0, 255) << 8 | (uint32_t) fos_clamp(b, 0, 255);
}

uint8_t fos_ix(fcol c)
{
    return ix_near(FC_RAMP(c) % RAMP_COUNT, LV12(FC_LEVEL(c)));
}

uint8_t fos_ix_dither(int ramp, int lv, int x, int y)
{
    return ix_dith(ramp, lv, x, y);
}

fcol fos_colour_of(uint8_t ix)
{
    return FC(rev_ramp[ix], rev_lv[ix] * 255 / LV_MAX);
}

int fos_luma(uint8_t ix)
{
    return luma_tab[ix];
}

/* the blend of (ramp, lv) over dst at opacity a: within one ramp it is a
 * level blend; across ramps it keeps the luminance and takes the ramp that
 * contributes more colour, so amber text on grey gets amber edges and
 * white text on amber gets light-amber ones */
static void mix_target(uint8_t dst, int ramp, int lv, int a, int *out_ramp, int *out_lv)
{
    int rd = rev_ramp[dst];
    if (rd == ramp)
    {
        *out_ramp = ramp;
        *out_lv = rev_lv[dst] + (((lv - rev_lv[dst]) * a) >> 8);
        return;
    }
    int src = ix_near(ramp, lv);
    int ld = luma_tab[dst], ls = luma_tab[src];
    int l = ld + (((ls - ld) * a) >> 8);
    int r = chroma_tab[src] * a >= chroma_tab[dst] * (256 - a) ? ramp : rd;
    *out_ramp = r;
    *out_lv = lum2lv[r][fos_clamp(l, 0, 255)];
}

void fos_lerp2(int ra, int la, int rb, int lb, int a, int *ramp, int *lv)
{
    if (ra == rb)
    {
        *ramp = ra;
        *lv = la + (((lb - la) * a) >> 8);
        return;
    }
    int ia = ix_near(ra, la), ib = ix_near(rb, lb);
    int l = luma_tab[ia] + (((luma_tab[ib] - luma_tab[ia]) * a) >> 8);
    int r = chroma_tab[ib] * a >= chroma_tab[ia] * (256 - a) ? rb : ra;
    *ramp = r;
    *lv = lum2lv[r][fos_clamp(l, 0, 255)];
}

uint8_t fos_mix(uint8_t dst, int ramp, int lv, int a)
{
    if (a >= OPAQUE) return ix_near(ramp, lv);
    if (a <= 0) return dst;
    int r, l;
    mix_target(dst, ramp, lv, a, &r, &l);
    return ix_near(r, l);
}

uint8_t fos_mix_dither(uint8_t dst, int ramp, int lv, int a, int x, int y)
{
    if (a >= OPAQUE) return ix_dith(ramp, lv, x, y);
    if (a <= 0) return dst;
    int r, l;
    mix_target(dst, ramp, lv, a, &r, &l);
    return ix_dith(r, l, x, y);
}

/* ---------------------------------------------------------------- maths */

int fos_isqrt(unsigned int v)
{
    if (!v) return 0;
    unsigned int r = 0, bit = 1u << ((31 - __builtin_clz(v)) & ~1);
    while (bit)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* quarter-wave sine, 4096 = 1.0 */
static const int16_t sin_q[65] =
{
       0,  101,  201,  301,  401,  501,  601,  700,  799,  897,  995, 1092, 1189, 1285, 1380, 1474,
    1567, 1660, 1751, 1842, 1931, 2019, 2106, 2191, 2276, 2359, 2440, 2520, 2598, 2675, 2751, 2824,
    2896, 2967, 3035, 3102, 3166, 3229, 3290, 3349, 3406, 3461, 3513, 3564, 3612, 3659, 3703, 3745,
    3784, 3822, 3857, 3889, 3920, 3948, 3973, 3996, 4017, 4036, 4052, 4065, 4076, 4085, 4091, 4095,
    4096,
};

int fos_sin(int a)
{
    a &= 0xFFFF;
    int q = a >> 14;                /* quadrant */
    int t = a & 0x3FFF;             /* 0..16383 within it */
    if (q & 1) t = 0x4000 - t;
    int i = t >> 8, f = t & 0xFF;
    int v = i >= 64 ? 4096 : sin_q[i] + (((sin_q[i + 1] - sin_q[i]) * f) >> 8);
    return q & 2 ? -v : v;
}

int fos_cos(int a)
{
    return fos_sin(a + 0x4000);
}

/* ---------------------------------------------------------------- canvas */

void fos_canvas_init(struct fos_canvas *cv, uint8_t *px, int pitch, int w, int h)
{
    cv->px = px;
    cv->pitch = pitch;
    cv->w = w;
    cv->h = h;
    fos_noclip(cv);
}

void fos_noclip(struct fos_canvas *cv)
{
    cv->cx0 = 0; cv->cy0 = 0; cv->cx1 = cv->w; cv->cy1 = cv->h;
}

void fos_clip(struct fos_canvas *cv, int x0, int y0, int x1, int y1)
{
    cv->cx0 = fos_clamp(x0, 0, cv->w);
    cv->cy0 = fos_clamp(y0, 0, cv->h);
    cv->cx1 = fos_clamp(x1, cv->cx0, cv->w);
    cv->cy1 = fos_clamp(y1, cv->cy0, cv->h);
}

/* clip a rectangle; returns 0 if nothing is left */
static int clip_rect(const struct fos_canvas *cv, int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < cv->cx0) x0 = cv->cx0;
    if (y0 < cv->cy0) y0 = cv->cy0;
    if (x1 > cv->cx1) x1 = cv->cx1;
    if (y1 > cv->cy1) y1 = cv->cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

#define PX(cv, x, y) ((cv)->px[(y) * (cv)->pitch + (x)])

void fos_fill_ix(struct fos_canvas *cv, int x, int y, int w, int h, uint8_t ix)
{
    if (!clip_rect(cv, &x, &y, &w, &h)) return;
    uint32_t word = ix * 0x01010101u;
    for (int j = 0; j < h; j++)
    {
        uint8_t *p = &PX(cv, x, y + j);
        int n = w;
        while (n && ((uintptr_t) p & 3)) { *p++ = ix; n--; }
        while (n >= 4) { *(uint32_t *) p = word; p += 4; n -= 4; }
        while (n--) *p++ = ix;
    }
}

void fos_fill(struct fos_canvas *cv, int x, int y, int w, int h, fcol c)
{
    fos_fill_ix(cv, x, y, w, h, fos_ix(c));
}

void fos_blend(struct fos_canvas *cv, int x, int y, int w, int h, fcol c, int alpha)
{
    if (alpha >= OPAQUE) { fos_fill(cv, x, y, w, h, c); return; }
    if (alpha <= 0 || !clip_rect(cv, &x, &y, &w, &h)) return;
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            PX(cv, i, j) = fos_mix_dither(PX(cv, i, j), ramp, lv, alpha, i, j);
}

void fos_scale_levels(struct fos_canvas *cv, int x, int y, int w, int h, int k)
{
    if (!clip_rect(cv, &x, &y, &w, &h)) return;
    /* every index, darkened, for the four pixels of a 2 x 2 dither */
    uint8_t lut[4][256];
    for (int ph = 0; ph < 4; ph++)
        for (int i = 0; i < 256; i++)
            lut[ph][i] = ix_dith(rev_ramp[i], (rev_lv[i] * k) >> 8, ph & 1, ph >> 1);
    for (int j = y; j < y + h; j++)
    {
        uint8_t *p = &PX(cv, x, j);
        int n = w, i = x;
        for (; n && ((uintptr_t) p & 3); n--, p++, i++)
            *p = lut[(j & 1) * 2 + (i & 1)][*p];
        const uint8_t *l0 = lut[(j & 1) * 2 + (i & 1)], *l1 = lut[(j & 1) * 2 + ((i + 1) & 1)];
        for (; n >= 4; n -= 4, p += 4, i += 4)
        {
            uint32_t v = *(uint32_t *) p;
            *(uint32_t *) p = l0[v & 0xFF] | l1[(v >> 8) & 0xFF] << 8 | l0[(v >> 16) & 0xFF] << 16 | (uint32_t) l1[v >> 24] << 24;
        }
        for (; n; n--, p++, i++)
            *p = lut[(j & 1) * 2 + (i & 1)][*p];
    }
}

void fos_vgrad(struct fos_canvas *cv, int x, int y, int w, int h, int ramp, int lv0, int lv1)
{
    int y_top = y, h_all = h;
    if (h_all <= 0 || !clip_rect(cv, &x, &y, &w, &h)) return;
    int base = ramp_base[ramp], last = ramp_n[ramp] - 1;
    for (int j = y; j < y + h; j++)
    {
        int lv = lv0 + (lv1 - lv0) * (j - y_top) / (h_all > 1 ? h_all - 1 : 1);
        int pos = ((lv < 0 ? 0 : lv) * ramp_mul[ramp]) >> 16;
        /* the four dithered indices of this row */
        uint8_t ix[4];
        for (int k = 0; k < 4; k++)
        {
            int q = (pos + fos_bayer[j & 3][k]) >> 4;
            ix[k] = base + (q > last ? last : q);
        }
        uint8_t *p = &PX(cv, x, j);
        for (int i = x; i < x + w; i++)
            *p++ = ix[i & 3];
    }
}

/* ---------------------------------------------------------------- AA shapes */

FOS_INLINE void put_cov(struct fos_canvas *cv, int x, int y, int ramp, int lv, int ix_solid, int cov, int alpha)
{
    if (cov <= 0) return;
    int a = cov >= 256 ? alpha : (cov * alpha) >> 8;
    if (a >= OPAQUE) PX(cv, x, y) = ix_solid;
    else if (a > 0) PX(cv, x, y) = fos_mix(PX(cv, x, y), ramp, lv, a);
}

/* pixels x0..x1-1 of row y fully covered */
static void put_run(struct fos_canvas *cv, int x0, int x1, int y, int ramp, int lv, int ix_solid, int alpha)
{
    if (x1 <= x0) return;
    uint8_t *p = &PX(cv, x0, y);
    if (alpha >= OPAQUE) { memset(p, ix_solid, x1 - x0); return; }
    if (alpha <= 0) return;
    for (int x = x0; x < x1; x++, p++)
        *p = fos_mix(*p, ramp, lv, alpha);
}

/* coverage (0..256) of pixel (i, j) in a w x h rounded rect at the origin */
static int rrect_cov(int i, int j, int w, int h, int r)
{
    if (i < 0 || j < 0 || i >= w || j >= h) return 0;
    int cx, cy;
    if (i < r) cx = r; else if (i >= w - r) cx = w - r; else return 256;
    if (j < r) cy = r; else if (j >= h - r) cy = h - r; else return 256;
    /* distance from the pixel centre to the corner circle centre, 1/16 px */
    int dx = (i * 16 + 8) - cx * 16;
    int dy = (j * 16 + 8) - cy * 16;
    if ((i < r && dx > 0) || (i >= w - r && dx < 0)) dx = 0;
    if ((j < r && dy > 0) || (j >= h - r && dy < 0)) dy = 0;
    int d = fos_isqrt(dx * dx + dy * dy) - r * 16;     /* 1/16 px, + outside */
    return fos_clamp(128 - d * 16, 0, 256);
}

void fos_rrect(struct fos_canvas *cv, int x, int y, int w, int h, int r, fcol c, int alpha)
{
    if (w <= 0 || h <= 0) return;
    r = fos_clamp(r, 0, (w < h ? w : h) / 2);
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);
    /* straight middle band in one go */
    if (alpha >= OPAQUE)
        fos_fill_ix(cv, x, y + r, w, h - 2 * r, solid);
    else
        fos_blend(cv, x, y + r, w, h - 2 * r, c, alpha);
    for (int j = 0; j < h; j++)
    {
        if (j >= r && j < h - r) continue;
        int py = y + j;
        if (py < cv->cy0 || py >= cv->cy1) continue;
        /* the corners, then the straight part between them in one run */
        for (int i = 0; i < w; i++)
        {
            if (i == r && w - r > r)
            {
                put_run(cv, fos_clamp(x + r, cv->cx0, cv->cx1), fos_clamp(x + w - r, cv->cx0, cv->cx1),
                        py, ramp, lv, solid, alpha);
                i = w - r - 1;
                continue;
            }
            int px = x + i;
            if (px < cv->cx0 || px >= cv->cx1) continue;
            put_cov(cv, px, py, ramp, lv, solid, rrect_cov(i, j, w, h, r), alpha);
        }
    }
}

void fos_rrect_line(struct fos_canvas *cv, int x, int y, int w, int h, int r, int t, fcol c, int alpha)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    r = fos_clamp(r, 0, (w < h ? w : h) / 2);
    int ri = r > t ? r - t : 0;
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);
    for (int j = 0; j < h; j++)
    {
        int py = y + j;
        if (py < cv->cy0 || py >= cv->cy1) continue;
        int inner_row = j >= t + ri && j < h - t - ri;
        int border_row = j < t || j >= h - t;          /* straight top / bottom edge */
        int cut = r > t ? r : t;
        for (int i = 0; i < w; i++)
        {
            if (inner_row && i >= t && i < w - t) { i = w - t - 1; continue; }
            if (!inner_row && i == cut && w - cut > cut)
            {
                /* between the corners the border is solid or empty */
                if (border_row)
                    put_run(cv, fos_clamp(x + cut, cv->cx0, cv->cx1), fos_clamp(x + w - cut, cv->cx0, cv->cx1),
                            py, ramp, lv, solid, alpha);
                i = w - cut - 1;
                continue;
            }
            int px = x + i;
            if (px < cv->cx0 || px >= cv->cx1) continue;
            int cov = rrect_cov(i, j, w, h, r) - rrect_cov(i - t, j - t, w - 2 * t, h - 2 * t, ri);
            put_cov(cv, px, py, ramp, lv, solid, cov, alpha);
        }
    }
}

/* distance (1/16 px, + outside) of a pixel at squared distance d2 (1/16 px)
 * from a circle of radius r16: near the edge of a big circle the square
 * root is as good as linear, d = (d2 - r^2) / 2r */
FOS_INLINE int circle_d(int d2, int r16, int inv2r)
{
    if (inv2r) return ((d2 - r16 * r16) * inv2r) >> 16;
    return fos_isqrt((unsigned) d2) - r16;
}

/* inv2r for circle_d, or 0 when the circle is too small to approximate */
static int circle_inv(int r16)
{
    return r16 >= 16 * 12 ? 65536 / (2 * r16) : 0;
}

void fos_disc(struct fos_canvas *cv, int cx8, int cy8, int r8, fcol c, int alpha)
{
    if (r8 <= 0) return;
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);
    int r16 = r8 >> 4, inv2r = circle_inv(r16);
    int y0 = (cy8 - r8) >> 8, y1 = (cy8 + r8 + 255) >> 8;
    int x0 = (cx8 - r8) >> 8, x1 = (cx8 + r8 + 255) >> 8;
    y0 = fos_clamp(y0, cv->cy0, cv->cy1); y1 = fos_clamp(y1, cv->cy0, cv->cy1);
    x0 = fos_clamp(x0, cv->cx0, cv->cx1); x1 = fos_clamp(x1, cv->cx0, cv->cx1);
    for (int y = y0; y < y1; y++)
    {
        int dy = ((y * 256 + 128) - cy8) >> 4;
        int cx16 = cx8 >> 4;
        /* pixels whose centre is half a pixel inside the edge are solid */
        int in = r16 - 8;
        int half = in > 0 && in * in > dy * dy ? fos_isqrt(in * in - dy * dy) : -1;
        int sx0 = half >= 0 ? (cx16 - half - 8 + 15) >> 4 : x1;
        int sx1 = half >= 0 ? (cx16 + half - 8) >> 4 : x0 - 1;
        int s0 = fos_clamp(sx0, x0, x1), s1 = fos_clamp(sx1 + 1, s0, x1);
        /* nothing beyond half a pixel outside the edge */
        int out = r16 + 8;
        if (out * out <= dy * dy) continue;
        int ohalf = fos_isqrt(out * out - dy * dy);
        int xa = fos_clamp(((cx16 - ohalf - 8) >> 4) - 1, x0, x1), xb = fos_clamp(((cx16 + ohalf - 8) >> 4) + 2, xa, x1);
        for (int x = xa; x < xb; x++)
        {
            if (x == s0 && s1 > s0)
            {
                put_run(cv, s0, s1, y, ramp, lv, solid, alpha);
                x = s1 - 1;
                continue;
            }
            int dx = ((x * 256 + 128) - cx8) >> 4;
            int d = circle_d(dx * dx + dy * dy, r16, inv2r);
            put_cov(cv, x, y, ramp, lv, solid, fos_clamp(128 - d * 16, 0, 256), alpha);
        }
    }
}

void fos_ring(struct fos_canvas *cv, int cx8, int cy8, int r8, int t8, fcol c, int alpha)
{
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);
    int ro = r8 + t8 / 2 + 256;
    int y0 = fos_clamp((cy8 - ro) >> 8, cv->cy0, cv->cy1), y1 = fos_clamp((cy8 + ro + 255) >> 8, cv->cy0, cv->cy1);
    int x0 = fos_clamp((cx8 - ro) >> 8, cv->cx0, cv->cx1), x1 = fos_clamp((cx8 + ro + 255) >> 8, cv->cx0, cv->cx1);
    int r16 = r8 >> 4, h16 = t8 >> 5, inv2r = circle_inv(r16);
    int inner = r16 - h16 - 16, outer = r16 + h16 + 16;     /* nothing inside / beyond these */
    for (int y = y0; y < y1; y++)
    {
        int dy = ((y * 256 + 128) - cy8) >> 4;
        if (dy * dy >= outer * outer) continue;
        int ox = fos_isqrt(outer * outer - dy * dy);
        int ix = inner > 0 && dy * dy < inner * inner ? fos_isqrt(inner * inner - dy * dy) : -1;
        int xa = fos_clamp(((cx8 - ox * 16) >> 8) - 1, x0, x1), xb = fos_clamp(((cx8 + ox * 16) >> 8) + 2, x0, x1);
        for (int x = xa; x < xb; x++)
        {
            int dx = ((x * 256 + 128) - cx8) >> 4;
            if (ix > 0 && dx > -ix && dx < ix)
            {
                /* across the hole */
                x = fos_clamp(((cx8 + ix * 16 - 128 + 255) >> 8) - 1, x, xb);
                continue;
            }
            int d = circle_d(dx * dx + dy * dy, r16, inv2r);
            if (d < 0) d = -d;
            put_cov(cv, x, y, ramp, lv, solid, fos_clamp(128 - (d - h16) * 16, 0, 256), alpha);
        }
    }
}

void fos_line(struct fos_canvas *cv, int x0_8, int y0_8, int x1_8, int y1_8, int w8, fcol c, int alpha)
{
    /* a thin quad: the segment pushed out by half the width both ways */
    int dx = (x1_8 - x0_8) >> 4, dy = (y1_8 - y0_8) >> 4;
    int len = fos_isqrt((unsigned) (dx * dx + dy * dy));
    if (!len) return;
    int ox = -dy * (w8 / 2) / len, oy = dx * (w8 / 2) / len;
    int q[8] = { x0_8 + ox, y0_8 + oy, x1_8 + ox, y1_8 + oy, x1_8 - ox, y1_8 - oy, x0_8 - ox, y0_8 - oy };
    fos_convex(cv, q, 4, c, alpha);
}

/* convex polygon, vertices in 1/256 px, either winding */
#define MAX_POLY 12
void fos_convex(struct fos_canvas *cv, const int *xy8, int n, fcol c, int alpha)
{
    if (n < 3 || n > MAX_POLY) return;
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);

    /* signed area tells the winding, so normals can point outwards */
    int64_t area = 0;
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        area += (int64_t) xy8[2 * i] * xy8[2 * j + 1] - (int64_t) xy8[2 * j] * xy8[2 * i + 1];
    }
    int sgn = area > 0 ? 1 : -1;

    /* edge lines: nx * x + ny * y - c, unit normal in 1/4096, distance in 1/256 px;
     * inv = 2^24 / nx turns "where does this edge cross" into a multiply */
    int nx[MAX_POLY], ny[MAX_POLY], nc[MAX_POLY], inv[MAX_POLY];
    int ymin = 1 << 30, ymax = -(1 << 30), xmin = 1 << 30, xmax = -(1 << 30);
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        int ex = xy8[2 * j] - xy8[2 * i], ey = xy8[2 * j + 1] - xy8[2 * i + 1];
        /* length in 1/16 px; long edges (the shutter blades) measured coarser so
         * the squares fit */
        int sh = ((ex < 0 ? -ex : ex) | (ey < 0 ? -ey : ey)) >= (1 << 19) ? 7 : 4;
        int len = fos_isqrt((unsigned) ((ex >> sh) * (ex >> sh) + (ey >> sh) * (ey >> sh))) << (sh - 4);
        if (!len) len = 1;
        nx[i] = sgn * (ey >> 4) * 4096 / len;
        ny[i] = -sgn * (ex >> 4) * 4096 / len;
        nc[i] = (int) (((int64_t) nx[i] * xy8[2 * i] + (int64_t) ny[i] * xy8[2 * i + 1]) >> 12);
        inv[i] = nx[i] ? (1 << 24) / nx[i] : 0;
        if (xy8[2 * i + 1] < ymin) ymin = xy8[2 * i + 1];
        if (xy8[2 * i + 1] > ymax) ymax = xy8[2 * i + 1];
        if (xy8[2 * i] < xmin) xmin = xy8[2 * i];
        if (xy8[2 * i] > xmax) xmax = xy8[2 * i];
    }
    int py0 = fos_clamp((ymin >> 8) - 1, cv->cy0, cv->cy1), py1 = fos_clamp((ymax >> 8) + 2, cv->cy0, cv->cy1);
    int px0 = fos_clamp((xmin >> 8) - 1, cv->cx0, cv->cx1), px1 = fos_clamp((xmax >> 8) + 2, cv->cx0, cv->cx1);

    for (int y = py0; y < py1; y++)
    {
        int yc = y * 256 + 128;
        /* [in0, in1]: every edge at least half a pixel inside (solid);
         * [out0, out1]: no edge more than half a pixel outside */
        int in0 = px0 * 256, in1 = px1 * 256, out0 = px0 * 256, out1 = px1 * 256;
        int empty = 0, nosolid = 0;
        for (int i = 0; i < n; i++)
        {
            /* d(x) = nx * x / 4096 + base  (1/256 px) */
            int base = (int) (((int64_t) ny[i] * yc) >> 12) - nc[i];
            if (!nx[i])
            {
                if (base > 128) empty = 1;
                if (base > -128) nosolid = 1;
                continue;
            }
            int xa = (int) (((int64_t) (-128 - base) * inv[i]) >> 12);
            int xb = (int) (((int64_t) (128 - base) * inv[i]) >> 12);
            if (nx[i] > 0) { if (xa < in1) in1 = xa; if (xb < out1) out1 = xb; }
            else           { if (xa > in0) in0 = xa; if (xb > out0) out0 = xb; }
        }
        if (empty || out1 < out0) continue;
        if (nosolid) in1 = in0 - 1;
        int sx0 = fos_clamp((out0 >> 8) - 1, px0, px1), sx1 = fos_clamp((out1 >> 8) + 2, px0, px1);
        /* whole pixels at least half a pixel inside every edge: one run */
        int s0 = sx1, s1 = sx1;
        if (in1 > in0)
        {
            s0 = fos_clamp((in0 + 255) >> 8, sx0, sx1);
            s1 = fos_clamp(in1 >> 8, s0, sx1);
        }
        for (int x = sx0; x < sx1; x++)
        {
            int xc = x * 256 + 128;
            if (x == s0 && s1 > s0)
            {
                put_run(cv, s0, s1, y, ramp, lv, solid, alpha);
                x = s1 - 1;
                continue;
            }
            /* pixel centres are on the canvas, so this fits in 32 bits */
            int dmax = -(1 << 30);
            for (int i = 0; i < n; i++)
            {
                int d = ((nx[i] * xc + ny[i] * yc) >> 12) - nc[i];
                if (d > dmax) dmax = d;
            }
            put_cov(cv, x, y, ramp, lv, solid, fos_clamp(128 - dmax, 0, 256), alpha);
        }
    }
}

void fos_tri(struct fos_canvas *cv, int cx, int cy, int size, int dir, fcol c, int alpha)
{
    /* an arrow head pointing right, rotated by dir quarter turns */
    static const int base[6] = { 1, 0, -1, -1, -1, 1 };
    int pts[6];
    for (int i = 0; i < 3; i++)
    {
        int x = base[2 * i], y = base[2 * i + 1];
        for (int k = 0; k < dir; k++) { int t = x; x = -y; y = t; }
        pts[2 * i] = cx * 256 + 128 + x * size * 128;
        pts[2 * i + 1] = cy * 256 + 128 + y * size * 128;
    }
    fos_convex(cv, pts, 3, c, alpha);
}

/* ---------------------------------------------------------------- text */

/* ---- the fonts, from the card */

#define FONT_DEF(n) struct fos_font fos_font_##n;
FOS_FONTS(FONT_DEF)
#define FONT_SLOT(n) &fos_font_##n,
static struct fos_font *const font_slots[] = { FOS_FONTS(FONT_SLOT) };
#define NFONTS ((int) (sizeof(font_slots) / sizeof(font_slots[0])))
static int fonts_ready;

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }

/* "FOSF", version 1, the number of fonts; each font: bits, glyphs, kerning
 * pairs, ascent, descent (u32), then its bits, glyphs, lut and kerning,
 * each part starting on 4 bytes */
int fos_fonts_load(const uint8_t *b, int size)
{
    fonts_ready = 0;
    if (!b || size < 12 || memcmp(b, "FOSF", 4) || get32(b + 4) != 1 || (int) get32(b + 8) != NFONTS
        || sizeof(struct fos_glyph) != 8 || sizeof(struct fos_kern) != 3)
        return 0;
    int at = 12;
    for (int i = 0; i < NFONTS; i++)
    {
        if (at + 20 > size) return 0;
        uint32_t nbits = get32(b + at), nglyphs = get32(b + at + 4), nkern = get32(b + at + 8);
        int asc = (int) get32(b + at + 12), desc = (int) get32(b + at + 16);
        at += 20;
        uint32_t need = ((nbits + 3) & ~3u) + nglyphs * 8 + 256 + ((nkern * 3 + 3) & ~3u);
        if (nbits > (uint32_t) size || nglyphs > 256 || nkern > 65536 || (uint32_t) (size - at) < need) return 0;
        struct fos_font *f = font_slots[i];
        f->bits = b + at;               at += (nbits + 3) & ~3u;
        f->glyphs = (const struct fos_glyph *) (b + at);    at += nglyphs * 8;
        f->lut = b + at;                at += 256;
        f->kern = nkern ? (const struct fos_kern *) (b + at) : 0;
        at += (nkern * 3 + 3) & ~3u;
        f->nkern = (int) nkern;
        f->ascent = asc;
        f->descent = desc;
        /* every glyph the lut names must exist */
        for (int c = 0; c < 256; c++)
            if (f->lut[c] != 0xFF && f->lut[c] >= nglyphs) { memset(f, 0, sizeof(*f)); return 0; }
    }
    fonts_ready = 1;
    return 1;
}

int fos_fonts_ready()
{
    return fonts_ready;
}

static const struct fos_glyph *glyph_of(const struct fos_font *f, int ch)
{
    if (!f->lut) return 0;              /* not loaded */
    int i = f->lut[ch & 0xFF];
    return i == 0xFF ? 0 : &f->glyphs[i];
}

static int kern_of(const struct fos_font *f, int a, int b)
{
    int lo = 0, hi = f->nkern - 1;
    int key = a << 8 | b;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        int k = f->kern[mid].a << 8 | f->kern[mid].b;
        if (k == key) return f->kern[mid].k;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static void draw_glyph(struct fos_canvas *cv, const struct fos_font *f, const struct fos_glyph *g,
                       int x, int y, int ramp, int lv, int solid, int alpha)
{
    int gx = x + g->ox, gy = y + g->oy;
    int w = g->w, h = g->h;
    if (!w || !h) return;
    if (gx >= cv->cx1 || gy >= cv->cy1 || gx + w <= cv->cx0 || gy + h <= cv->cy0) return;

    const uint8_t *p = f->bits + g->off;
    int col = 0, row = 0;
    int inside = gx >= cv->cx0 && gx + w <= cv->cx1 && gy >= cv->cy0 && gy + h <= cv->cy1;
    int memo_dst = -1, memo_a = -1, memo_out = 0;
    while (row < h)
    {
        int b = *p++;
        int run, v;
        if (b & 0x80) { run = (b & 0x7F) + 1; v = 0; }
        else          { run = (b >> 4) + 1;  v = b & 15; }
        if (!v)
        {
            col += run;
            while (col >= w) { col -= w; row++; }
            continue;
        }
        int a = v == 15 ? alpha : (v * 17 * alpha) >> 8;
        if (inside)
        {
            /* runs never pass the end of the bitmap, but may wrap rows */
            uint8_t *q = &PX(cv, gx + col, gy + row);
            if (a >= OPAQUE)
                while (run--)
                {
                    *q++ = solid;
                    if (++col == w) { col = 0; row++; q = &PX(cv, gx, gy + row); }
                }
            else
                while (run--)
                {
                    /* the backdrop is a couple of dithered indices: remember the last blend */
                    if (*q != memo_dst || a != memo_a)
                    {
                        memo_dst = *q; memo_a = a;
                        memo_out = fos_mix(*q, ramp, lv, a);
                    }
                    *q++ = memo_out;
                    if (++col == w) { col = 0; row++; q = &PX(cv, gx, gy + row); }
                }
            continue;
        }
        while (run--)
        {
            int px = gx + col, py = gy + row;
            if (px >= cv->cx0 && px < cv->cx1 && py >= cv->cy0 && py < cv->cy1)
            {
                if (a >= OPAQUE) PX(cv, px, py) = solid;
                else PX(cv, px, py) = fos_mix(PX(cv, px, py), ramp, lv, a);
            }
            if (++col == w) { col = 0; row++; }
        }
    }
}

int fos_text_w(const struct fos_font *f, const char *s, int track)
{
    int pen = 0, prev = 0;
    for (; *s; s++)
    {
        int ch = (uint8_t) *s;
        const struct fos_glyph *g = glyph_of(f, ch);
        if (!g) continue;
        if (prev) pen += kern_of(f, prev, ch) + track;
        pen += g->adv;
        prev = ch;
    }
    return (pen + 8) >> 4;
}

int fos_text(struct fos_canvas *cv, const struct fos_font *f, int x, int y, const char *s, fcol c, int alpha, int track)
{
    if (alpha <= 0) return fos_text_w(f, s, track);
    int ramp = FC_RAMP(c), lv = LV12(FC_LEVEL(c));
    int solid = fos_ix(c);
    int pen = x * 16, prev = 0;
    for (; *s; s++)
    {
        int ch = (uint8_t) *s;
        const struct fos_glyph *g = glyph_of(f, ch);
        if (!g) continue;
        if (prev) pen += kern_of(f, prev, ch) + track;
        draw_glyph(cv, f, g, (pen + 8) >> 4, y, ramp, lv, solid, alpha);
        pen += g->adv;
        prev = ch;
    }
    return ((pen + 8) >> 4) - x;
}

int fos_text_c(struct fos_canvas *cv, const struct fos_font *f, int cx, int y, const char *s, fcol c, int alpha, int track)
{
    int w = fos_text_w(f, s, track);
    fos_text(cv, f, cx - w / 2, y, s, c, alpha, track);
    return w;
}

int fos_text_r(struct fos_canvas *cv, const struct fos_font *f, int rx, int y, const char *s, fcol c, int alpha, int track)
{
    int w = fos_text_w(f, s, track);
    fos_text(cv, f, rx - w, y, s, c, alpha, track);
    return w;
}

int fos_text_map(uint8_t *px, int pitch, int w, int h, const struct fos_font *f, int x, int y,
                 const char *s, const uint8_t map[16], int track)
{
    int pen = x * 16, prev = 0;
    for (; *s; s++)
    {
        int ch = (uint8_t) *s;
        const struct fos_glyph *g = glyph_of(f, ch);
        if (!g) continue;
        if (prev) pen += kern_of(f, prev, ch) + track;
        int gx = ((pen + 8) >> 4) + g->ox, gy = y + g->oy;
        const uint8_t *p = f->bits + g->off;
        int col = 0, row = 0;
        while (row < g->h)
        {
            int b = *p++;
            int run, v;
            if (b & 0x80) { run = (b & 0x7F) + 1; v = 0; }
            else          { run = (b >> 4) + 1;  v = b & 15; }
            while (run--)
            {
                int qx = gx + col, qy = gy + row;
                if (v && map[v] && qx >= 0 && qx < w && qy >= 0 && qy < h)
                    px[qy * pitch + qx] = map[v];
                if (++col == g->w) { col = 0; row++; }
            }
        }
        pen += g->adv;
        prev = ch;
    }
    return ((pen + 8) >> 4) - x;
}

/* the overlay's colours over a photograph (a print, or the camera's own
 * picture): black, a panel, greys up to white for text on it, the accent,
 * red, a line, and a dim grey */
uint8_t fos_fixed_rgb[FOS_FIXED_N][3] =
{
    { 0, 0, 0 }, { 22, 21, 20 }, { 52, 50, 48 }, { 88, 85, 82 }, { 128, 124, 120 }, { 170, 166, 160 },
    { 210, 206, 200 }, { 244, 241, 236 }, { 236, 168, 52 }, { 150, 104, 34 }, { 214, 64, 52 }, { 60, 58, 56 },
    { 112, 108, 104 }, { 112, 22, 28 },     /* the last is the dark red of the bake bar's shading */
};

/* The colour themes. RAMP_RED is rebuilt in the theme's hue (same shape as the red ramp), and the review's red and its dark
 * shade are the fixed palette's PX_RED and PX_SPARE. */
static const struct ramp_def accent_ramps[3] =
{
    { 0x080202, 0xDF262D, 0xFFD8D4, 150, 18 },      /* red */
    { 0x080402, 0xF2731A, 0xFFE0C6, 150, 18 },      /* orange */
    { 0x020A04, 0x39FF14, 0xE2FFD9, 150, 18 },      /* neon green */
};
static const uint8_t accent_fixed[3][2][3] =        /* PX_RED, PX_SPARE */
{
    { { 214, 64, 52 },  { 112, 22, 28 } },
    { { 242, 120, 28 }, { 124, 52, 8 } },
    { { 57, 255, 20 },  { 16, 112, 12 } },
};
void fos_gfx_accent(int theme)
{
    if (theme < 0 || theme > 2) theme = 0;
    const struct ramp_def *d = &accent_ramps[theme];
    int ix = ramp_base[RAMP_RED];
    for (int i = 0; i < d->n; i++, ix++)
    {
        int lv = i * 255 / (d->n - 1);
        uint32_t c = lv <= d->mid_at ? lerp_rgb(d->lo, d->mid, lv, d->mid_at) : lerp_rgb(d->mid, d->hi, lv - d->mid_at, 255 - d->mid_at);
        fos_pal[ix] = c;
        luma_tab[ix] = rgb_luma(c);
        chroma_tab[ix] = rgb_chroma(c);
    }
    memcpy(fos_fixed_rgb[10], accent_fixed[theme][0], 3);
    memcpy(fos_fixed_rgb[13], accent_fixed[theme][1], 3);
}
