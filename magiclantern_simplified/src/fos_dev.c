/* fos_dev: developing film in the camera, a frame at a time.
 *
 * The frame's CR2 is read a chunk at a time and its lossless JPEG decoded;
 * every sample is added straight into the print pixel it falls in, so the
 * 18 million sample mosaic becomes a 720 x 480 picture without ever being
 * held: a box filter and the demosaic in one. The print then gets what
 * the desktop darkroom (Darkrooms) does: daylight balance, the camera's colours
 * to sRGB, the scanner's exposure, the film's balance, vignette and
 * halation, the film's colour (a 3D table built from the same film model),
 * and grain. Finally it is reduced to FOS_PRINT_COLOURS colours with error
 * diffusion, its own palette, for the 8-bit screen.
 *
 * Everything runs in steps that stop after a time budget, so the film OS
 * keeps animating on the same task: no threads, no locks.
 */

#include "fos.h"
#include "fos_dev.h"
#include "fos_jpeg.h"
#include <math.h>
#include <string.h>

#ifdef FOS_DEV_TEST
void fos_dev_test_sample(int x, int y, int v);
#endif

static int u16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned u32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned) p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }

/* ---------------------------------------------------------------- the film model */

static const float LUMA[3] = { 0.2126f, 0.7152f, 0.0722f };
static const float REF[7][3] = {        /* red orange yellow green cyan blue magenta */
    { 1, 0, 0 }, { 1, 0.5f, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 } };
#define PI_F 3.14159265f

const struct fos_look *fos_look_of(const char *key)
{
    for (int i = 0; i < fos_nlooks; i++)
        if (!strcmp(fos_looks[i].key, key)) return &fos_looks[i];
    for (int i = 0; i < fos_nmore_looks; i++)
        if (!strcmp(fos_more_looks[i].key, key)) return &fos_more_looks[i];
    return &fos_looks[0];
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float hue_angle(int i)
{
    static float angle[7];
    static int ready;
    if (!ready)
    {
        for (int k = 0; k < 7; k++)
        {
            float y = dot3(REF[k], LUMA);
            angle[k] = atan2f((REF[k][0] - y) / 1.5748f, (REF[k][2] - y) / 1.8556f);
        }
        ready = 1;
    }
    return angle[i];
}

static float wrap(float a)
{
    a = fmodf(a + PI_F, 2 * PI_F);
    if (a < 0) a += 2 * PI_F;
    return a - PI_F;
}

/* the characteristic curve: logistic in log2 exposure around mid grey */
static float curve(float x, float slope, float pivot, float black, float white)
{
    float xx = log2f((x > 1e-6f ? x : 1e-6f) / 0.18f) - pivot;
    float k = 4 * slope / (white - black);
    return black + (white - black) / (1 + expf(-k * xx));
}

static void shift_hues(const struct fos_look *f, float rgb[3])
{
    float y = dot3(rgb, LUMA);
    float cb = (rgb[2] - y) / 1.8556f, cr = (rgb[0] - y) / 1.5748f;
    float h = atan2f(cr, cb), c = sqrtf(cb * cb + cr * cr);
    float gate = c / 0.04f > 1 ? 1 : c / 0.04f;
    float dh = 0, cm = 1;
    const float sigma = 35 * PI_F / 180;
    for (int i = 0; i < f->nhues; i++)
    {
        const struct fos_hue_shift *s = &f->hues[i];
        float hs = hue_angle(s->from);
        float d = wrap(h - hs) / sigma;
        float w = expf(-0.5f * d * d) * gate;
        if (s->from != s->to) dh += w * s->amount * wrap(hue_angle(s->to) - hs);
        cm *= 1 + w * (s->chroma - 1);
    }
    h += dh;
    c *= cm;
    cb = c * cosf(h); cr = c * sinf(h);
    rgb[0] = y + 1.5748f * cr;
    rgb[2] = y + 1.8556f * cb;
    rgb[1] = (y - LUMA[0] * rgb[0] - LUMA[2] * rgb[2]) / LUMA[1];
}

void fos_film_color(const struct fos_look *f, const float in[3], float out[3])
{
    if (f->mono)
    {
        float v = curve(dot3(in, f->mono_mix), f->contrast[0], f->pivot[0], f->black[0], f->white[0]);
        out[0] = out[1] = out[2] = v;
        return;
    }
    float y = dot3(in, LUMA), lin[3];
    for (int c = 0; c < 3; c++)
    {
        lin[c] = y + (in[c] - y) * f->sat;
        if (lin[c] < 0) lin[c] = 0;
        out[c] = curve(lin[c], f->contrast[c], f->pivot[c], f->black[c], f->white[c]);
    }
    if (f->nhues) shift_hues(f, out);
}

/* linear sRGB of a blackbody at t kelvin (as cct_to_rgb in the lab) */
static void cct_rgb(float t, float out[3])
{
    float x, y;
    if (t <= 4000) x = -0.2661239e9f / (t * t * t) - 0.2343589e6f / (t * t) + 0.8776956e3f / t + 0.179910f;
    else x = -3.0258469e9f / (t * t * t) + 2.1070379e6f / (t * t) + 0.2226347e3f / t + 0.240390f;
    if (t <= 2222) y = -1.1063814f * x * x * x - 1.34811020f * x * x + 2.18555832f * x - 0.20219683f;
    else if (t <= 4000) y = -0.9549476f * x * x * x - 1.37418593f * x * x + 2.09137015f * x - 0.16748867f;
    else y = 3.0817580f * x * x * x - 5.87338670f * x * x + 3.75112997f * x - 0.37001483f;
    float X = x / y, Y = 1, Z = (1 - x - y) / y;
    out[0] = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    out[1] = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    out[2] = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
}

/* ---------------------------------------------------------------- the film's table */

/* The film's colour as a 3D table over Cineon log code (the density of a
 * film scan): the same thing `lut.py export` writes, and as close to the
 * lab's model. Building it takes the camera a few seconds, so it is kept
 * in memory and on the card. */
#define GRID 33
#define NGRID (GRID * GRID * GRID)
#define LOOK_MODEL 1                    /* bump when fos_film_color changes */
#define CIN_BLACK 0.0108f               /* 10^((95 - 685) * 0.002 / 0.6) */

static struct
{
    uint8_t lut[NGRID][3];
    /* linear (4096 = 1.0) -> grid position (1/256) of its Cineon code:
     * 0..4095 one by one, then up to 65535 in steps of 16 */
    uint16_t cin[4096 + 3841];
} *tab;
static int lut_film = -1, lut_built;            /* lut_built: grid points done */
static char cache_dir[128];

void fos_dev_cache(const char *dir)
{
    snprintf(cache_dir, sizeof(cache_dir), "%s", dir ? dir : "");
}

void fos_dev_release()
{
    fos_port_free(tab);
    tab = 0;
    lut_film = -1;
}

static float cineon_decode(float v)
{
    float x = powf(10, (v * 1023 - 685) / 300);
    return (x - CIN_BLACK) / (1 - CIN_BLACK);
}

static int cineon_encode(float lin)
{
    float x = (lin > 0 ? lin : 0) * (1 - CIN_BLACK) + CIN_BLACK;
    float v = (685 + 300 * log10f(x)) / 1023;
    return v < 0 ? 0 : v > 1 ? 65535 : (int) (v * 65535 + 0.5f);
}

static int tables_ready()
{
    if (tab) return 1;
    tab = fos_port_alloc(sizeof(*tab));
    if (!tab) return 0;
    for (int i = 0; i < 4096 + 3841; i++)
    {
        int code = cineon_encode((i < 4096 ? i : 4096 + (i - 4096) * 16) / 4096.0f);
        tab->cin[i] = (uint16_t) ((code * ((GRID - 1) * 256)) >> 16);
    }
    lut_film = -1;
    return 1;
}

/* linear 0..65535 (4096 = 1.0, up to 16) -> grid position in 1/256 */
FOS_INLINE int cin_pos(const uint16_t *cin, int lin)
{
    return cin[lin < 4096 ? lin : lin > 65535 ? 4096 + 3840 : 4096 + ((lin - 4096) >> 4)];
}

/* the look's numbers, so a table on the card is only used for the same look */
/* the look's numbers and its film, so a table on the card is only used
 * for the same look (tools/genfilms.py works it out the same way for the
 * tables it bakes from LUTs) */
static uint32_t look_hash(const struct fos_look *f)
{
    const uint8_t *p = (const uint8_t *) &f->balance;
    const uint8_t *e = (const uint8_t *) f->mono_mix + sizeof(f->mono_mix);
    uint32_t h = 2166136261u ^ (GRID * 256 + LOOK_MODEL);
    for (const char *k = f->key; *k; k++) h = (h ^ (uint8_t) *k) * 16777619u;
    for (; p < e; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static void *lut_file(int film, int write)
{
    if (!cache_dir[0]) return 0;
    char path[160];
    snprintf(path, sizeof(path), "%s/LOOK%02d.LUT", cache_dir, film);
    return fos_port_open(path, write);
}

static int lut_load(const struct fos_look *f, int film)
{
    void *fp = lut_file(film, 0);
    if (!fp) return 0;
    uint8_t h[8];
    int ok = fos_port_read(fp, h, 8) == 8 && !memcmp(h, "FOL1", 4) && u32le(h + 4) == look_hash(f)
          && fos_port_read(fp, tab->lut, sizeof(tab->lut)) == (int) sizeof(tab->lut);
    fos_port_close(fp);
    return ok;
}

static void lut_save(const struct fos_look *f, int film)
{
    void *fp = lut_file(film, 1);
    if (!fp) return;
    uint8_t h[8] = { 'F', 'O', 'L', '1' };
    put32(h + 4, look_hash(f));
    fos_port_write(fp, h, 8);
    fos_port_write(fp, tab->lut, sizeof(tab->lut));
    fos_port_close(fp);
}

/* build up to `count` more grid points; 1 when the table is complete */
static __attribute__((noinline)) int lut_build(const struct fos_look *f, int film, int count)
{
    if (lut_film != film)
    {
        lut_film = film;
        lut_built = lut_load(f, film) ? NGRID : 0;
    }
    if (lut_built >= NGRID) return 1;
    for (; lut_built < NGRID && count-- > 0; lut_built++)
    {
        int i = lut_built, r = i % GRID, g = (i / GRID) % GRID, b = i / (GRID * GRID);
        float lin[3] = { cineon_decode(r / (float) (GRID - 1)), cineon_decode(g / (float) (GRID - 1)),
                         cineon_decode(b / (float) (GRID - 1)) }, out[3];
        fos_film_color(f, lin, out);
        for (int c = 0; c < 3; c++)
        {
            int v = (int) (out[c] * 255 + 0.5f);
            tab->lut[i][c] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    if (lut_built < NGRID) return 0;
    lut_save(f, film);
    return 1;
}

/* the film's table, into the cache now: the build puts every film's on
 * the card, so the camera never has to mix one */
int fos_dev_table(int film)
{
    if (!tables_ready()) return 0;
    film = fos_clamp(film, 0, fos_nfilms - 1);
    lut_film = -1;
    while (!lut_build(fos_look_of(fos_films[film].key), film, NGRID)) {}
    return 1;
}

/* tetrahedral lookup at grid positions in 1/256: the four corners of the
 * cell's tetrahedron the point is in, weighted */
FOS_INLINE void lut_apply(const uint8_t (*lut)[3], int pr, int pg, int pb, uint8_t out[3])
{
    int fr = pr & 255, fg = pg & 255, fb = pb & 255;
    const uint8_t *p = lut[((pb >> 8) * GRID + (pg >> 8)) * GRID + (pr >> 8)];
    const int R = 3, G = 3 * GRID, B = 3 * GRID * GRID;
    int o1, o2, w0, w1, w2, w3;
    if (fr >= fg)
    {
        if (fg >= fb)      { o1 = R; o2 = R + G; w0 = 256 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb; }
        else if (fr >= fb) { o1 = R; o2 = R + B; w0 = 256 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg; }
        else               { o1 = B; o2 = R + B; w0 = 256 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg; }
    }
    else
    {
        if (fb >= fg)      { o1 = B; o2 = G + B; w0 = 256 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr; }
        else if (fb >= fr) { o1 = G; o2 = G + B; w0 = 256 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr; }
        else               { o1 = G; o2 = R + G; w0 = 256 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb; }
    }
    const int o3 = R + G + B;
    for (int c = 0; c < 3; c++, p++)
        out[c] = (uint8_t) ((w0 * p[0] + w1 * p[o1] + w2 * p[o2] + w3 * p[o3] + 128) >> 8);
}

/* ---------------------------------------------------------------- the frame */

#define CHUNK       (192 * 1024)        /* read from the card at a time */
#define GW          (FOS_PRINT_W / 4)   /* the halation's grid */
#define GH          (FOS_PRINT_H / 4)
#define FAST_BITS   9
#define DIFF_BITS   11
#define UNPACK      2688                /* samples decoded at a time: an EOS M slice's row */

enum { ST_OPEN, ST_DECODE, ST_KEY, ST_EXPOSE, ST_GLOW, ST_FINISH, ST_JPEG, ST_QUANT, ST_DITHER, ST_WRITE, ST_DONE, ST_FAIL };

struct huff
{
    /* a code and its difference bits together, when they fit in
     * DIFF_BITS: difference << 4 | bits used; 0 = the slower way (small,
     * so it stays in the camera's data cache) */
    int16_t fastd[1 << DIFF_BITS];
    uint16_t fast[1 << FAST_BITS];      /* len << 8 | symbol, 0 = longer than FAST_BITS */
    int maxcode[18], valptr[17], mincode[17];
    uint8_t vals[256];
};

struct box { int lo[3], hi[3]; long count; };

struct fos_dev
{
    char cr2[128], print[128], when[20], fstop[8];
    char jpeg[160];                     /* the full-colour frame as a JPEG too, or "" */
    int film, frame;
    const struct fos_look *look;
    int stage, row;                     /* row: progress through a per-row stage */
    const char *err;
    char errbuf[48];
    void *f;

    /* the file, destuffed into whole bytes of entropy data */
    uint8_t *buf;
    const uint8_t *bp, *be, *safe, *ff; /* the next byte, the end, see fill() */
    int file_pos, file_end;
    uint32_t bitbuf;
    int vbits;

    /* the raw */
    int raw_w, raw_h, orient;
    int crop_l, crop_t, crop_w, crop_h, ob_l, ob_r;
    int slices[3];
    int clrs, wide, high, bits, psv;
    struct huff *huff[4];
    struct huff tables[4];

    /* decoding */
    int jcol, comp, vpred[4];
    uint16_t samples[4 + UNPACK];
    int sl, sx, sy, sw, sx0;            /* where the next sample goes: slice, column in it, row */
    long done, total;

    /* the print being built */
    int ow, oh, ox0;                    /* picture size and its left edge on the print */
    int32_t *rowoff;                    /* raw row -> its place in the print's sums, or < 0 */
    uint16_t *seg_of, *seg_x0;          /* raw column -> its segment; a segment's first column */
    int32_t *seg_off;                   /* segment -> its place in the print's sums */
    uint8_t *seg_desc;                  /* a row's pieces: pairs (<= 4) | 0x40 a last even column | 0x80 an odd first */
    int32_t *desc_off;                  /* a piece's place in the print's sums */
    int32_t *seg_piece;                 /* a segment's first piece */
    uint8_t *rowob, *colob;             /* optical black */
    uint16_t *ne_r, *no_r, *ne_c, *no_c;    /* raw rows / columns per print line, even and odd */
    int rows_axis_x;                    /* raw rows run along the print's x (portrait) */
    uint32_t *sums;                     /* 3 per print pixel */
    int64_t black_sum;
    long black_n;
    int black16;                        /* black level, 1/16 raw units */
    int pre[3];                         /* daylight balance and white level, raw/16 -> 1/4096, << 14 */
    int mat[3][3];                      /* camera -> linear sRGB, 1/4096 */
    int mul[3];                         /* exposure and film balance, 1/4096 */
    uint16_t *lin;                      /* in place of sums: linear sRGB, 4096 = 1.0 */
    int32_t *glow;                      /* halation, a quarter of the print each way */
    uint8_t *rgb;                       /* the finished print, RGB */
    uint8_t thumb[FOS_THUMB_W * FOS_THUMB_H * 3];
    int16_t vig_x[FOS_PRINT_W], vig_y[FOS_PRINT_H];
    uint8_t gx[FOS_PRINT_W], fx[FOS_PRINT_W];   /* glow cell and weight (1/8) per column */
    unsigned seed;
    uint16_t pos[3 * FOS_PRINT_W];      /* a row's positions on the film's grid */

    /* palette */
    uint32_t *hist;
    uint8_t *inv;
    uint8_t pal[FOS_PRINT_COLOURS][3];
    int npal;
    struct box boxes[FOS_PRINT_COLOURS];
    uint8_t by_green[FOS_PRINT_COLOURS];   /* the palette in order of green */
    int16_t *err_rows;
    uint8_t *idx;
};

/* The sensor's colours, XYZ -> camera, and its white level: LibRaw's
 * colordata.cpp (the Adobe matrices), per camera the fork is built for.
 * The EOS M's is the default (and the 650D's, 700D's and 100D's: one sensor). */
#define M3(a, b, c, d, e, f, g, h, i) { { a / 10000.f, b / 10000.f, c / 10000.f }, { d / 10000.f, e / 10000.f, f / 10000.f }, \
                                        { g / 10000.f, h / 10000.f, i / 10000.f } }
#if defined(CONFIG_6D) || defined(CONFIG_70D)
static const float XYZ_CAM[3][3] = M3(7034, -804, -1014, -4420, 12564, 2058, -851, 1994, 5758);
#elif defined(CONFIG_5D3)
static const float XYZ_CAM[3][3] = M3(6722, -635, -963, -4287, 12460, 2028, -908, 2162, 5668);
#elif defined(CONFIG_5D2)
static const float XYZ_CAM[3][3] = M3(4716, 603, -830, -7798, 15474, 2480, -1496, 1937, 6651);
#elif defined(CONFIG_7D)
static const float XYZ_CAM[3][3] = M3(6844, -996, -856, -3876, 11761, 2396, -593, 1772, 6198);
#elif defined(CONFIG_50D)
static const float XYZ_CAM[3][3] = M3(4920, 616, -593, -6493, 13964, 2784, -1774, 3178, 7005);
#elif defined(CONFIG_60D)
static const float XYZ_CAM[3][3] = M3(6719, -994, -925, -4408, 12426, 2211, -887, 2129, 6051);
#elif defined(CONFIG_500D)
static const float XYZ_CAM[3][3] = M3(4763, 712, -646, -6821, 14399, 2640, -1921, 3276, 6561);
#elif defined(CONFIG_550D)
static const float XYZ_CAM[3][3] = M3(6941, -1164, -857, -3825, 11597, 2534, -416, 1540, 6039);
#elif defined(CONFIG_600D)
static const float XYZ_CAM[3][3] = M3(6461, -907, -882, -4300, 12184, 2378, -819, 1944, 5931);
#elif defined(CONFIG_1100D)
static const float XYZ_CAM[3][3] = M3(6444, -904, -893, -4563, 12308, 2535, -903, 2016, 6728);
#else   /* EOS M, 100D, 650D, 700D */
static const float XYZ_CAM[3][3] = { { 0.6602f, -0.0841f, -0.0939f }, { -0.4472f, 1.2458f, 0.2247f }, { -0.0975f, 0.2039f, 0.6148f } };
#endif
static const float RGB_XYZ[3][3] = { { 0.4124564f, 0.3575761f, 0.1804375f }, { 0.2126729f, 0.7151522f, 0.0721750f },
                                     { 0.0193339f, 0.1191920f, 0.9503041f } };
/* raw value of a full pixel (LibRaw's maximum); the EOS M's measured 13600 */
#if defined(CONFIG_6D)
#define WHITE_LEVEL 15490
#elif defined(CONFIG_70D)
#define WHITE_LEVEL 15303
#elif defined(CONFIG_5D3)
#define WHITE_LEVEL 15488
#elif defined(CONFIG_5D2)
#define WHITE_LEVEL 15600
#elif defined(CONFIG_7D) || defined(CONFIG_600D) || defined(CONFIG_1100D)
#define WHITE_LEVEL 13584
#elif defined(CONFIG_50D)
#define WHITE_LEVEL 15763
#elif defined(CONFIG_60D)
#define WHITE_LEVEL 12279
#elif defined(CONFIG_500D)
#define WHITE_LEVEL 13433
#elif defined(CONFIG_550D)
#define WHITE_LEVEL 15831
#elif defined(CONFIG_700D)
#define WHITE_LEVEL 15360
#elif defined(CONFIG_100D)
#define WHITE_LEVEL 13583
#elif defined(CONFIG_650D)
#define WHITE_LEVEL 13645
#else
#define WHITE_LEVEL 13600
#endif

static int fail(struct fos_dev *d, const char *why)
{
    d->err = why;
    d->stage = ST_FAIL;
    return -1;
}

/* a buffer couldn't be had: which one, for the log */
static int fail_mem(struct fos_dev *d, int size)
{
    snprintf(d->errbuf, sizeof(d->errbuf), "out of memory (%d KB)", (size + 1023) / 1024);
    return fail(d, d->errbuf);
}

/* ---- reading */

static int read_at(struct fos_dev *d, int offset, void *buf, int size)
{
    if (fos_port_seek(d->f, offset) < 0) return 0;
    return fos_port_read(d->f, buf, size) == size;
}


/* the value of TIFF tag `tag` in the IFD at `ifd` (offset for anything
 * longer than 4 bytes); -1 when absent */
static long tiff_tag(struct fos_dev *d, unsigned ifd, int tag, int *count)
{
    uint8_t n2[2], e[12];
    if (!read_at(d, (int) ifd, n2, 2)) return -1;
    int n = u16le(n2);
    for (int i = 0; i < n && i < 400; i++)
    {
        if (!read_at(d, (int) ifd + 2 + 12 * i, e, 12)) return -1;
        if (u16le(e) != tag) continue;
        int type = u16le(e + 2);
        unsigned cnt = u32le(e + 4);
        if (count) *count = (int) cnt;
        if (type == 3 && cnt <= 2) return u16le(e + 8);
        return (long) u32le(e + 8);
    }
    return -1;
}

static int parse_tiff(struct fos_dev *d)
{
    uint8_t h[16];
    if (!read_at(d, 0, h, 16)) return fail(d, "can't read the file");
    if (h[0] != 'I' || h[1] != 'I' || h[8] != 'C' || h[9] != 'R') return fail(d, "not a CR2 file");
    unsigned ifd0 = u32le(h + 4), raw_ifd = u32le(h + 12);
    long o = tiff_tag(d, ifd0, 0x0112, 0);
    d->orient = o > 0 ? (int) o : 1;

    /* the sensor's picture area from the Canon maker note, if it says */
    d->crop_l = -1;
    long exif = tiff_tag(d, ifd0, 0x8769, 0);
    long mn = exif > 0 ? tiff_tag(d, (unsigned) exif, 0x927C, 0) : -1;
    int cnt = 0;
    long si = mn > 0 ? tiff_tag(d, (unsigned) mn, 0x00E0, &cnt) : -1;
    if (si > 0 && cnt >= 13)
    {
        uint8_t s[26];
        if (read_at(d, (int) si, s, 26))
        {
            d->crop_l = u16le(s + 10); d->crop_t = u16le(s + 12);
            d->crop_w = u16le(s + 14) - d->crop_l + 1; d->crop_h = u16le(s + 16) - d->crop_t + 1;
            d->ob_l = u16le(s + 18); d->ob_r = u16le(s + 22);
        }
    }

    long off = tiff_tag(d, raw_ifd, 0x0111, 0);
    if (off <= 0) return fail(d, "no RAW in this file");
    d->file_pos = (int) off;
    long len = tiff_tag(d, raw_ifd, 0x0117, 0);
    d->file_end = len > 0 ? (int) (off + len) : 0x7fffffff;
    d->slices[0] = 0;
    long sl = tiff_tag(d, raw_ifd, 0xC640, &cnt);
    if (sl > 0 && cnt == 3)
    {
        uint8_t s[6];
        if (read_at(d, (int) sl, s, 6))
        {
            d->slices[0] = u16le(s); d->slices[1] = u16le(s + 2); d->slices[2] = u16le(s + 4);
        }
    }
    return 1;
}

static int build_huff(struct huff *h, const uint8_t *counts, const uint8_t *vals, int nvals)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->vals, vals, nvals > 256 ? 256 : nvals);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++)
    {
        h->valptr[len] = k;
        h->mincode[len] = code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++)
            if (len <= FAST_BITS)
            {
                int shift = FAST_BITS - len;
                for (int j = 0; j < 1 << shift; j++)
                    h->fast[(code << shift) | j] = (uint16_t) (len << 8 | vals[k]);
            }
        h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    /* the common short code and difference in one lookup */
    for (int i = 0; i < 1 << DIFF_BITS; i++)
    {
        int e = h->fast[i >> (DIFF_BITS - FAST_BITS)], len = e >> 8, s = e & 0xFF;
        if (!e || s == 16 || len + s > DIFF_BITS) continue;
        int diff = 0;
        if (s)
        {
            diff = (i >> (DIFF_BITS - len - s)) & ((1 << s) - 1);
            if (!(diff >> (s - 1))) diff -= (1 << s) - 1;
        }
        h->fastd[i] = (int16_t) (diff * 16 + len + s);
    }
    return k == nvals;
}

/* the lossless JPEG's header, up to the scan */
static int parse_ljpeg_in(struct fos_dev *d, uint8_t *hdr, int size);

static int parse_ljpeg(struct fos_dev *d)
{
    const int HDR = 2048;
    uint8_t *hdr = fos_port_alloc(HDR);         /* not on the film OS task's small stack */
    if (!hdr) return fail_mem(d, HDR);
    int r = parse_ljpeg_in(d, hdr, HDR);
    fos_port_free(hdr);
    return r;
}

static int parse_ljpeg_in(struct fos_dev *d, uint8_t *hdr, int size)
{
    int n = d->file_end - d->file_pos < size ? d->file_end - d->file_pos : size;
    if (n < 64 || !read_at(d, d->file_pos, hdr, n)) return fail(d, "can't read the RAW");
    if (hdr[0] != 0xFF || hdr[1] != 0xD8) return fail(d, "the RAW is not a lossless JPEG");
    int p = 2, have_sof = 0, ntab = 0;
    while (p + 4 <= n)
    {
        if (hdr[p] != 0xFF) return fail(d, "broken RAW header");
        int tag = hdr[p + 1], len = hdr[p + 2] << 8 | hdr[p + 3];
        const uint8_t *s = hdr + p + 4;
        if (p + 2 + len > n) return fail(d, "RAW header too long");
        if (tag == 0xC3)
        {
            d->bits = s[0];
            d->high = s[1] << 8 | s[2];
            d->wide = s[3] << 8 | s[4];
            d->clrs = s[5];
            if (((s[7] >> 4) * (s[7] & 15) - 1) & 3) return fail(d, "sRAW / mRAW: shoot full RAW");
            have_sof = 1;
        }
        else if (tag == 0xC4)
        {
            for (const uint8_t *q = s; q < s + len - 2;)
            {
                int id = *q++;
                if (id & ~3) return fail(d, "unexpected Huffman table");
                int total = 0;
                for (int i = 0; i < 16; i++) total += q[i];
                if (!build_huff(&d->tables[id], q, q + 16, total)) return fail(d, "bad Huffman table");
                if (id + 1 > ntab) ntab = id + 1;
                q += 16 + total;
            }
        }
        else if (tag == 0xDD && (s[0] | s[1]))
            return fail(d, "unsupported RAW: restart markers");
        else if (tag == 0xDA)
        {
            d->psv = s[1 + s[0] * 2];
            d->bits -= s[3 + s[0] * 2] & 15;
            d->file_pos += p + 2 + len;
            if (!have_sof || !ntab) return fail(d, "RAW header incomplete");
            /* LibRaw's rule, which Canon follows: component c takes table c,
             * or the last one defined */
            for (int c = 0; c < 4; c++) d->huff[c] = &d->tables[c < ntab ? c : ntab - 1];
            /* the same table twice (the EOS M's): one copy, half the cache */
            for (int c = 1; c < 4; c++)
                if (d->huff[c] != d->huff[0] && !memcmp(d->huff[c], d->huff[0], sizeof(struct huff))) d->huff[c] = d->huff[0];
            if (d->clrs < 1 || d->clrs > 4 || d->bits < 8 || d->bits > 16) return fail(d, "unsupported RAW layout");
            if (d->psv != 1) return fail(d, "unsupported RAW predictor");
            return 1;
        }
        p += 2 + len;
    }
    return fail(d, "RAW header not found");
}

/* the next chunk of the file after the keep bytes not yet used (at bp);
 * past the end, zeros. The JPEG's byte stuffing stays in: the bit reader
 * takes it out as it goes, rather than in a pass of its own. Returns the
 * buffer's length. */
static int refill(struct fos_dev *d, const uint8_t *bp, int keep)
{
    memmove(d->buf, bp, keep);
    int n = d->file_end - d->file_pos;
    if (n > CHUNK) n = CHUNK;
    if (n > 0 && fos_port_seek(d->f, d->file_pos) >= 0 && (n = fos_port_read(d->f, d->buf + keep, n)) > 0)
        d->file_pos += n;
    else
    {
        d->file_pos = d->file_end;
        n = 8;
        memset(d->buf + keep, 0, 8);
    }
    return keep + n;
}

/* ---- the print's geometry: which raw rows and columns land where */

static int setup_maps(struct fos_dev *d)
{
    d->raw_w = d->wide * d->clrs;
    d->raw_h = d->high;
    if (!(d->clrs & 1) && d->raw_w > 4 * d->raw_h)
    {
        /* some cameras store the sensor as a frame twice as wide and half
         * as high (as dcraw and LibRaw read it): the samples run the same */
        d->raw_w /= 2;
        d->raw_h *= 2;
    }
    if (d->slices[0] && d->slices[0] * d->slices[1] + d->slices[2] != d->raw_w)
        return fail(d, "RAW slices don't add up");
    if (d->crop_l < 0 || d->crop_l + d->crop_w > d->raw_w || d->crop_t + d->crop_h > d->raw_h || d->crop_w < 64)
    {
        /* no sensor info: the EOS M's picture area, scaled to this sensor */
        d->crop_l = d->raw_w * 84 / 5280; d->crop_t = d->raw_h * 64 / 3528;
        d->crop_w = d->raw_w - d->crop_l - d->raw_w * 12 / 5280;
        d->crop_h = d->raw_h - d->crop_t - d->raw_h * 8 / 3528;
        d->ob_l = 0; d->ob_r = d->crop_l - 12;
    }
    if (d->ob_r - d->ob_l < 16)
    {
        /* No black mask given (the EOS M gives none): the masked columns
         * left of the picture, well clear of it. The picture area Canon
         * names starts at 84, but light already reaches column 72. */
        d->ob_l = 0; d->ob_r = d->crop_l - 1;
    }
    if (d->ob_r > d->crop_l - 20) d->ob_r = d->crop_l - 20;
    int portrait = d->orient == 6 || d->orient == 8;
    d->rows_axis_x = portrait;
    d->oh = FOS_PRINT_H;
    d->ow = portrait ? FOS_PRINT_H * FOS_PRINT_H / FOS_PRINT_W : FOS_PRINT_W;
    d->ox0 = (FOS_PRINT_W - d->ow) / 2;

    d->rowoff = fos_port_alloc(d->raw_h * 4);
    d->seg_of = fos_port_alloc(d->raw_w * 2);
    d->seg_x0 = fos_port_alloc((FOS_PRINT_W + 1) * 2);
    d->seg_off = fos_port_alloc(FOS_PRINT_W * 4);
    d->rowob = fos_port_alloc(d->raw_h);
    d->colob = fos_port_alloc(d->raw_w);
    d->ne_r = fos_port_alloc(FOS_PRINT_W * 2 * 2);
    d->ne_c = fos_port_alloc(FOS_PRINT_W * 2 * 2);
    if (!d->rowoff || !d->seg_of || !d->seg_x0 || !d->seg_off || !d->rowob || !d->colob || !d->ne_r || !d->ne_c)
        return fail_mem(d, d->raw_w * 10 + d->raw_h * 5);
    d->no_r = d->ne_r + FOS_PRINT_W;
    d->no_c = d->ne_c + FOS_PRINT_W;
    memset(d->ne_r, 0, FOS_PRINT_W * 4);
    memset(d->ne_c, 0, FOS_PRINT_W * 4);

    /* raw rows: across the picture's height (landscape) or width (portrait) */
    int rlen = portrait ? d->ow : d->oh, clen = portrait ? d->oh : d->ow;
    int rflip = d->orient == 6 || d->orient == 3, cflip = d->orient == 8 || d->orient == 3;
    const int32_t OUT = -1;
    for (int y = 0; y < d->raw_h; y++)
    {
        int a = y - d->crop_t;
        d->rowob[y] = a >= 0 && a < d->crop_h;
        if (a < 0 || a >= d->crop_h) { d->rowoff[y] = OUT; continue; }
        int o = (int) ((long) a * rlen / d->crop_h);
        if (rflip) o = rlen - 1 - o;
        if (y & 1) d->no_r[o]++; else d->ne_r[o]++;
        d->rowoff[y] = 3 * (portrait ? d->ox0 + o : o * FOS_PRINT_W);
    }
    /* raw columns: segment k of them (in raw order) makes print line o */
    for (int k = 0; k <= clen; k++) d->seg_x0[k] = (uint16_t) d->crop_w;
    for (int x = 0; x < d->raw_w; x++)
    {
        int a = x - d->crop_l;
        d->colob[x] = x >= d->ob_l + 4 && x <= d->ob_r - 4;
        d->seg_of[x] = 0;
        if (a < 0 || a >= d->crop_w) continue;
        int k = (int) ((long) a * clen / d->crop_w);
        if (d->seg_x0[k] > a) d->seg_x0[k] = (uint16_t) a;
        d->seg_of[x] = (uint16_t) k;
        int o = cflip ? clen - 1 - k : k;
        if (x & 1) d->no_c[o]++; else d->ne_c[o]++;
        d->seg_off[k] = 3 * (portrait ? o * FOS_PRINT_W : d->ox0 + o);
    }
    for (int k = 0; k <= clen; k++) d->seg_x0[k] = (uint16_t) (d->seg_x0[k] + d->crop_l);
    /* the segments' shapes, the same on every row, for sum_row: when the
     * samples fit word halves four times and every segment has two */
    if (d->bits <= 14 && d->crop_w >= 2 * clen && d->crop_w <= 32 * clen)
    {
        d->seg_desc = fos_port_alloc(4 * clen);
        d->desc_off = fos_port_alloc(4 * clen * 4);
        d->seg_piece = fos_port_alloc((clen + 1) * 4);
        if (!d->seg_desc || !d->desc_off || !d->seg_piece) return fail_mem(d, 24 * clen);
        int n = 0;
        for (int k = 0; k < clen; k++)
        {
            d->seg_piece[k] = n;
            int s0 = d->seg_x0[k], len = d->seg_x0[k + 1] - s0, odd = s0 & 1, pairs = (len - odd) >> 1;
            /* the even column after the pairs: worked out before odd is
             * cleared below, or a segment that starts on an odd column and
             * takes more than one piece (portrait) gets a column too many,
             * and the row's later pieces read one column off (on the camera,
             * words not on a word boundary: lines across the picture) */
            int tail = (len - odd) & 1;
            /* four pairs at most a piece; a longer segment (portrait) in more */
            do
            {
                int p = pairs < 4 ? pairs : 4;
                pairs -= p;
                d->seg_desc[n] = (uint8_t) (p | (!pairs && tail) << 6 | odd << 7);
                d->desc_off[n++] = d->seg_off[k];
                odd = 0;
            }
            while (pairs > 0);
        }
        d->seg_piece[clen] = n;
    }

    d->sums = fos_port_alloc(FOS_PRINT_W * FOS_PRINT_H * 3 * 4);
    if (!d->sums) return fail_mem(d, FOS_PRINT_W * FOS_PRINT_H * 3 * 4);
    memset(d->sums, 0, FOS_PRINT_W * FOS_PRINT_H * 3 * 4);
    d->total = (long) d->raw_w * d->raw_h;
    d->sl = 0; d->sx = 0; d->sy = 0; d->sx0 = 0;
    d->sw = d->slices[0] ? d->slices[1] : d->raw_w;
    for (int c = 0; c < 4; c++) d->vpred[c] = 1 << (d->bits - 1);
    return 1;
}

/* ---- decoding */

/* a code longer than FAST_BITS, a bit at a time (vbits >= 16):
 * len << 8 | symbol, or 0 for a code that doesn't exist */
static int slow_symbol(const struct huff *h, uint32_t bitbuf, int vbits)
{
    int len = FAST_BITS;
    int code = (int) ((bitbuf >> (vbits - len)) & ((1 << len) - 1));
    while (len < 16 && code > h->maxcode[len])
    {
        len++;
        code = (int) ((bitbuf >> (vbits - len)) & ((1u << len) - 1));
    }
    if (code > h->maxcode[len]) return 0;
    return len << 8 | h->vals[h->valptr[len] + code - h->mincode[len]];
}

typedef uint32_t __attribute__((may_alias)) word;

/* a byte of w is 0xFF: a zero byte of ~w, (~w - 0x01010101) & w & 0x80808080,
 * and ~w - k is ~(w + k) */
#define FF_IN(w) ((w) & ~((w) + 0x01010101u))

/* the first 0xFF at or after p, or be: the bytes before it are taken
 * without looking at each, sixteen at a time here */
static __attribute__((noinline)) const uint8_t *next_ff(const uint8_t *p, const uint8_t *be)
{
    for (; p < be && ((uintptr_t) p & 3); p++)
        if (*p == 0xFF) return p;
    for (; be - p >= 16; p += 16)
    {
        const word *q = (const word *) p;
        if ((FF_IN(q[0]) | FF_IN(q[1]) | FF_IN(q[2]) | FF_IN(q[3])) & 0x80808080u) break;
    }
    while (p < be && *p != 0xFF) p++;
    return p;
}

/* The bit reader keeps its state in d between the decoder's calls, and in
 * registers in its loop, which hands it over to these for what is rare:
 * the buffer's end, the 0xFF bytes, codes longer than the one lookup. */

/* at least 25 bits, the careful way: a stuffed 0xFF 0x00 is one 0xFF; any
 * other marker ends the data, and zeros follow. Then where the next 0xFF
 * is: below safe, the next three bytes are data in the buffer and no 0xFF. */
static __attribute__((noinline)) void fill(struct fos_dev *d)
{
    uint32_t bitbuf = d->bitbuf;
    int vbits = d->vbits;
    const uint8_t *bp = d->bp, *be = d->be, *ff = d->ff;
    while (vbits <= 24)
    {
        if (be - bp < 2)
        {
            int len = refill(d, bp, (int) (be - bp));
            bp = d->buf; be = bp + len;
            ff = 0;
        }
        uint32_t c = *bp++;
        if (c == 0xFF)
        {
            if (!*bp) bp++;
            else { c = 0; bp = be; d->file_pos = d->file_end; }
        }
        bitbuf = bitbuf << 8 | c;
        vbits += 8;
    }
    d->bitbuf = bitbuf; d->vbits = vbits;
    d->bp = bp; d->be = be;
    if (!ff || bp > ff)
    {
        d->ff = next_ff(bp, be);
        d->safe = d->ff - 2;
    }
}

/* a difference the one lookup doesn't cover: the code, then its bits */
static __attribute__((noinline)) int slow_diff(struct fos_dev *d, const struct huff *h)
{
    fill(d);
    int e = h->fast[(d->bitbuf >> (d->vbits - FAST_BITS)) & ((1 << FAST_BITS) - 1)];
    if (!e) e = slow_symbol(h, d->bitbuf, d->vbits);
    d->vbits -= e >> 8;
    int s = e & 0xFF, diff = 0;
    if (s == 16) diff = -32768;
    else if (s)
    {
        if (d->vbits < s) fill(d);
        d->vbits -= s;
        diff = (int) ((d->bitbuf >> d->vbits) & ((1u << s) - 1));
        if (!(diff >> (s - 1))) diff -= (1 << s) - 1;
    }
    fill(d);
    return diff;
}

#define LOAD() (bitbuf = d->bitbuf, vbits = d->vbits, bp = d->bp, safe = d->safe)
#define SAVE() (d->bitbuf = bitbuf, d->vbits = vbits, d->bp = bp)

/* at least 24 bits: two short codes' worth */
#define FILL() \
    do \
    { \
        if (__builtin_expect(bp < safe, 1)) \
        { \
            if (vbits <= 16) { bitbuf = bitbuf << 16 | (uint32_t) bp[0] << 8 | bp[1]; bp += 2; vbits += 16; } \
            if (vbits <= 24) { bitbuf = bitbuf << 8 | *bp++; vbits += 8; } \
        } \
        else { SAVE(); fill(d); LOAD(); } \
    } while (0)

/* a difference from table h (its lookup t, read at the byte offset the
 * mask leaves); bitbuf must hold DIFF_BITS + 1:
 * the code and its bits in one lookup, as nearly always (99.8 % on the
 * EOS M's files) */
#define DIFF_OR(t, h, diff, also) \
    do \
    { \
        diff = *(const int16_t *) ((const uint8_t *) (t) + ((bitbuf >> (vbits - DIFF_BITS - 1)) & ((2 << DIFF_BITS) - 2))); \
        if (__builtin_expect(diff & 15, 1)) { vbits -= diff & 15; diff >>= 4; } \
        else { SAVE(); diff = slow_diff(d, h); LOAD(); also; } \
    } while (0)
#define DIFF(t, h, diff) DIFF_OR(t, h, diff, (void) 0)

/* n pairs of the two components (the EOS M's layout), each from the one
 * to its left: on its own, so all it works with stays in registers */
static __attribute__((noinline)) void pairs(struct fos_dev *d, uint16_t *out, int n)
{
    const struct huff *h0 = d->huff[0], *h1 = d->huff[1];
    const int16_t *t0 = h0->fastd, *t1 = h1->fastd;
    uint32_t bitbuf;
    int vbits;
    const uint8_t *bp, *safe;
    LOAD();
    int p0 = out[-2], p1 = out[-1], diff;
    uint16_t *stop = out + 2 * n;
    while (out < stop)
    {
        /* a pair takes three bytes at most: as many as safe leaves room
         * for, without looking (until a long code, which reads on) */
        int room = (int) (safe - bp) >> 2, left = (int) (stop - out) >> 1;
        if (room <= 0)
        {
            FILL();
            DIFF(t0, h0, diff); p0 += diff; out[0] = (uint16_t) p0;
            DIFF(t1, h1, diff); p1 += diff; out[1] = (uint16_t) p1;
            out += 2;
            continue;
        }
        uint16_t *block = out + 2 * (room < left ? room : left);
        for (; out < block; out += 2)
        {
            if (vbits <= 16) { bitbuf = bitbuf << 16 | (uint32_t) bp[0] << 8 | bp[1]; bp += 2; vbits += 16; }
            if (vbits <= 24) { bitbuf = bitbuf << 8 | *bp++; vbits += 8; }
            DIFF_OR(t0, h0, diff, block = out + 2); p0 += diff; out[0] = (uint16_t) p0;
            DIFF_OR(t1, h1, diff, block = out + 2); p1 += diff; out[1] = (uint16_t) p1;
        }
    }
    SAVE();
}

/* the next count samples of the lossless JPEG into out[0..count-1];
 * out[-clrs..-1] hold the ones before */
static __attribute__((noinline)) void unpack(struct fos_dev *d, uint16_t *out, int count)
{
    const int clrs = d->clrs, wide = d->wide;
    uint32_t bitbuf;
    int vbits;
    const uint8_t *bp, *safe;
    int comp = d->comp, jcol = d->jcol;
    uint16_t *end = out + count;
    while (out < end)
    {
        int diff;
        if (jcol && !comp && end - out >= clrs && (clrs == 2 || clrs == 4))
        {
            /* whole groups of the components, each from the one to its
             * left, kept in registers */
            int n = wide - jcol;
            if (n > (int) (end - out) / clrs) n = (int) (end - out) / clrs;
            jcol += n;
            if (jcol == wide) jcol = 0;
            if (clrs == 2)
            {
                pairs(d, out, n);
                out += 2 * n;
            }
            else
            {
                const struct huff *h0 = d->huff[0], *h1 = d->huff[1], *h2 = d->huff[2], *h3 = d->huff[3];
                uint16_t *stop = out + 4 * n;
                int p0 = out[-4], p1 = out[-3], p2 = out[-2], p3 = out[-1];
                LOAD();
                for (; out < stop; out += 4)
                {
                    FILL();
                    DIFF(h0->fastd, h0, diff); p0 += diff; out[0] = (uint16_t) p0;
                    DIFF(h1->fastd, h1, diff); p1 += diff; out[1] = (uint16_t) p1;
                    FILL();
                    DIFF(h2->fastd, h2, diff); p2 += diff; out[2] = (uint16_t) p2;
                    DIFF(h3->fastd, h3, diff); p3 += diff; out[3] = (uint16_t) p3;
                }
                SAVE();
            }
            continue;
        }
        /* one at a time: a row's first group (predicted from above), odd
         * layouts, the ends of the count */
        const struct huff *h = d->huff[comp];
        LOAD();
        FILL();
        DIFF(h->fastd, h, diff);
        SAVE();
        if (jcol) *out = (uint16_t) (out[-clrs] + diff);
        else *out = (uint16_t) (d->vpred[comp] += diff);
        out++;
        if (++comp == clrs)
        {
            comp = 0;
            if (++jcol == wide) jcol = 0;
        }
    }
    d->comp = comp; d->jcol = jcol;
}

/* a raw row's samples v..vend-1 into its print pixels: segment by segment
 * (next: where the next starts, at[] the sample of a column; off: where
 * the segment goes in row), its even and odd columns apart */
static __attribute__((noinline)) void sum_samples(const uint16_t *at, const uint16_t *v, const uint16_t *vend,
                                                  const uint16_t *next, const int32_t *off, uint32_t *row)
{
    while (v < vend)
    {
        const uint16_t *ve = at + *next++;
        if (ve > vend) ve = vend;
        uint32_t a = 0, b = 0;
        if ((v - at) & 1) b = *v++;
        for (; ve - v >= 2; v += 2) { a += v[0]; b += v[1]; }
        if (v < ve) a += *v++;
        uint32_t *p = row + *off++;
        p[0] += a;
        p[1] += b;
    }
}

/* a whole row of the picture the same way, in the pieces seg_desc gives
 * (n of them; off: where each goes in row), with the even columns 4-byte
 * aligned: a pair of samples as one word, and up to four pairs of 14 bits
 * summed in its halves */
static __attribute__((noinline)) void sum_row(const uint16_t *v, const uint8_t *desc, int n, const int32_t *off, uint32_t *row)
{
    for (const uint8_t *e = desc + n; desc < e; desc++, off++)
    {
        int k = *desc;
        uint32_t b = 0;
        if (k & 0x80) b = *v++;
        const word *q = (const word *) v;
        uint32_t w = 0;
        switch (k & 7)
        {
            case 4: w += q[3];  /* fall through */
            case 3: w += q[2];  /* fall through */
            case 2: w += q[1];  /* fall through */
            case 1: w += q[0];  /* fall through */
            default: break;
        }
        v += 2 * (k & 7);
        uint32_t a = w & 0xFFFF;
        if (k & 0x40) a += *v++;
        uint32_t *p = row + *off;
        p[0] += a;
        p[1] += b + (w >> 16);
    }
}

/* count samples to where they fall in the print, slice by slice, top to
 * bottom: a print pixel's run of samples along a raw row is summed in
 * registers, even and odd columns (two colours of the mosaic) apart, and
 * added once. The masked columns left of the picture give the black. */
static __attribute__((noinline)) void scatter(struct fos_dev *d, const uint16_t *src, int n)
{
    uint32_t *sums = d->sums;
    const uint16_t *seg_x0 = d->seg_x0, *seg_of = d->seg_of;
    const int32_t *seg_off = d->seg_off;
    const uint8_t *colob = d->colob;
    const int pic0 = d->crop_l, pic1 = d->crop_l + d->crop_w;
    const uint8_t *desc = d->seg_desc;
    int sx = d->sx, sy = d->sy, sw = d->sw, sx0 = d->sx0;
    int64_t black_sum = d->black_sum;
    long black_n = d->black_n;
    while (n > 0)
    {
        int run = sw - sx < n ? sw - sx : n;
        n -= run;
        const int ro = d->rowoff[sy];
#ifdef FOS_DEV_TEST
        for (int k = 0; k < run; k++) fos_dev_test_sample(sx0 + sx + k, sy, src[k]);
#endif
        if (ro >= 0)
        {
            const int x0 = sx0 + sx, x1 = x0 + run;
            const int pa = x0 > pic0 ? x0 : pic0, pb = x1 < pic1 ? x1 : pic1;
            for (int x = x0; x < x1 && x < pa; x++)
                if (colob[x]) { black_sum += src[x - x0]; black_n++; }
            if (pa < pb)
            {
                /* sample of raw column x at at[x]; RGGB at the sensor's top
                 * left: the even columns' colour, then the odd ones' */
                const uint16_t *at = src - x0;
                uint32_t *row = sums + ro + (sy & 1);
                /* the segments it touches, and those it has whole (a row
                 * of the picture is in two of Canon's slices) */
                int k0 = seg_of[pa], k1 = seg_of[pb - 1] + 1;
                int w0 = seg_x0[k0] < pa ? k0 + 1 : k0, w1 = seg_x0[k1] > pb ? k1 - 1 : k1;
                if (!desc || ((uintptr_t) at & 3) || w0 >= w1)
                    sum_samples(at, at + pa, at + pb, seg_x0 + k0 + 1, seg_off + k0, row);
                else
                {
                    if (w0 > k0) sum_samples(at, at + pa, at + seg_x0[w0], seg_x0 + k0 + 1, seg_off + k0, row);
                    const int i0 = d->seg_piece[w0];
                    sum_row(at + seg_x0[w0], desc + i0, d->seg_piece[w1] - i0, d->desc_off + i0, row);
                    if (w1 < k1) sum_samples(at, at + seg_x0[w1], at + pb, seg_x0 + w1 + 1, seg_off + w1, row);
                }
            }
        }
        src += run;
        sx += run;
        if (sx == sw)
        {
            sx = 0;
            if (++sy == d->raw_h)
            {
                sy = 0;
                sx0 += sw;
                d->sl++;
                sw = d->sl < d->slices[0] ? d->slices[1] : d->slices[2];
            }
        }
    }
    d->sx = sx; d->sy = sy; d->sw = sw; d->sx0 = sx0;
    d->black_sum = black_sum; d->black_n = black_n;
}

/* decode about n samples straight into the print's sums */
static void decode(struct fos_dev *d, long n)
{
    uint16_t *s = d->samples + 4;
    while (n > 0 && d->done < d->total)
    {
        /* to the end of a raw row, so scatter has it whole */
        int k = d->sw - d->sx;
        if (k > UNPACK) k = UNPACK;
        if (k > d->total - d->done) k = (int) (d->total - d->done);
        unpack(d, s, k);
        scatter(d, s, k);
        /* the last of each component, for the next ones' prediction */
        memmove(s - d->clrs, s + k - d->clrs, d->clrs * 2);
        d->done += k;
        n -= k;
    }
}

/* ---- finishing */

/* 65536 / n for the samples a print pixel's channel got */
static uint32_t recip[64];

/* scene-linear sRGB of print pixel i (4096 = 1.0), clipped as the lab's
 * RAW converter does, times d->mul; 0 where the picture isn't */
FOS_INLINE void pixel_linear(const struct fos_dev *d, int x, int y, int lin[3])
{
    int i = y * FOS_PRINT_W + x;
    int xr, xc;                         /* print line fed by raw rows / columns */
    if (d->rows_axis_x) { xr = x - d->ox0; xc = y; if (xr < 0 || xr >= d->ow) { lin[0] = lin[1] = lin[2] = 0; return; } }
    else { xr = y; xc = x; }
    int er = d->ne_r[xr], or_ = d->no_r[xr], ec = d->ne_c[xc], oc = d->no_c[xc];
    int n[3] = { er * ec, er * oc + or_ * ec, or_ * oc };
    int cam[3];
    for (int c = 0; c < 3; c++)
    {
        /* the mean (sums <= 16383 n, so this fits), less black, balanced */
        int m = n[c] < 64 ? (int) (d->sums[3 * i + c] * recip[n[c]] >> 12) - d->black16 : 0;
        int v = m > 0 ? (int) (((int64_t) m * d->pre[c]) >> 14) : 0;
        cam[c] = v < 0 ? 0 : v > 4096 ? 4096 : v;
    }
    for (int c = 0; c < 3; c++)
    {
        int v = (d->mat[c][0] * cam[0] + d->mat[c][1] * cam[1] + d->mat[c][2] * cam[2]) >> 12;
        v = v < 0 ? 0 : v > 4096 ? 4096 : v;
        v = (v * d->mul[c]) >> 12;
        lin[c] = v > 65535 ? 65535 : v;
    }
}

/* camera -> sRGB with daylight balance, and the black and white levels */
static void setup_colour(struct fos_dev *d)
{
    float cam_rgb[3][3], pre[3];
    for (int i = 0; i < 3; i++)
    {
        float sum = 0;
        for (int j = 0; j < 3; j++)
        {
            cam_rgb[i][j] = 0;
            for (int k = 0; k < 3; k++) cam_rgb[i][j] += XYZ_CAM[i][k] * RGB_XYZ[k][j];
            sum += cam_rgb[i][j];
        }
        for (int j = 0; j < 3; j++) cam_rgb[i][j] /= sum;
        pre[i] = 1 / sum;
    }
    float green = pre[1];
    for (int i = 0; i < 3; i++) pre[i] /= green;
    /* rgb_cam = inverse(cam_rgb) */
    float (*m)[3] = cam_rgb, inv[3][3];
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
              + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

    float black = d->black_n ? (float) d->black_sum / (float) d->black_n : 2048;
    if (black < 0 || black > WHITE_LEVEL / 2) black = 2048;     /* a damaged file */
    d->black16 = (int) (black * 16 + 0.5f);
    /* raw/16 above black -> 4096 at the white level, balanced, << 14 */
    for (int c = 0; c < 3; c++)
    {
        d->pre[c] = (int) (pre[c] * 4096 * 16384 / ((WHITE_LEVEL - black) * 16) + 0.5f);
        d->mul[c] = 4096;
        for (int j = 0; j < 3; j++) d->mat[c][j] = (int) (inv[c][j] * 4096 + (inv[c][j] < 0 ? -0.5f : 0.5f));
    }
    for (int k = 1; k < 64; k++) recip[k] = 65536 / k;
}

/* small, fast noise */
FOS_INLINE int rnd(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (int) (*s >> 16 & 0xFFFF) - 32768;
}

/* ---------------------------------------------------------------- palette */

/* median cut over a 5-5-5 histogram (struct box: with struct fos_dev) */

static void box_shrink(const uint32_t *hist, struct box *b)
{
    int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
    long count = 0;
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++)
            {
                uint32_t n = hist[r << 10 | g << 5 | bl];
                if (!n) continue;
                count += n;
                if (r < lo[0]) lo[0] = r;
                if (r > hi[0]) hi[0] = r;
                if (g < lo[1]) lo[1] = g;
                if (g > hi[1]) hi[1] = g;
                if (bl < lo[2]) lo[2] = bl;
                if (bl > hi[2]) hi[2] = bl;
            }
    if (count) { memcpy(b->lo, lo, sizeof(lo)); memcpy(b->hi, hi, sizeof(hi)); }
    b->count = count;
}

static void median_cut(struct fos_dev *d)
{
    struct box *boxes = d->boxes;
    int n = 1;
    boxes[0].lo[0] = boxes[0].lo[1] = boxes[0].lo[2] = 0;
    boxes[0].hi[0] = boxes[0].hi[1] = boxes[0].hi[2] = 31;
    box_shrink(d->hist, &boxes[0]);
    while (n < FOS_PRINT_COLOURS)
    {
        /* the box with the most pixels that can still be split, weighted
         * by its size so big smooth areas get their share */
        int best = -1;
        long best_score = 0;
        for (int i = 0; i < n; i++)
        {
            struct box *b = &boxes[i];
            int span = 0;
            for (int c = 0; c < 3; c++) if (b->hi[c] - b->lo[c] > span) span = b->hi[c] - b->lo[c];
            if (!span) continue;
            long score = b->count * (span + 1);
            if (score > best_score) { best_score = score; best = i; }
        }
        if (best < 0) break;
        struct box *b = &boxes[best];
        int axis = 0, span = -1;
        for (int c = 0; c < 3; c++) if (b->hi[c] - b->lo[c] > span) { span = b->hi[c] - b->lo[c]; axis = c; }
        /* the median along that axis */
        long half = b->count / 2, acc = 0;
        int cut = b->lo[axis];
        for (int v = b->lo[axis]; v < b->hi[axis]; v++)
        {
            int lo[3] = { b->lo[0], b->lo[1], b->lo[2] }, hi[3] = { b->hi[0], b->hi[1], b->hi[2] };
            lo[axis] = hi[axis] = v;
            for (int r = lo[0]; r <= hi[0]; r++)
                for (int g = lo[1]; g <= hi[1]; g++)
                    for (int bl = lo[2]; bl <= hi[2]; bl++)
                        acc += d->hist[r << 10 | g << 5 | bl];
            cut = v;
            if (acc >= half) break;
        }
        struct box nb = *b;
        b->hi[axis] = cut;
        nb.lo[axis] = cut + 1;
        box_shrink(d->hist, b);
        box_shrink(d->hist, &nb);
        boxes[n++] = nb;
    }
    /* each box's colour: the mean of its pixels */
    d->npal = n;
    for (int i = 0; i < n; i++)
    {
        const struct box *b = &boxes[i];
        int64_t s[3] = { 0, 0, 0 }, cnt = 0;
        for (int r = b->lo[0]; r <= b->hi[0]; r++)
            for (int g = b->lo[1]; g <= b->hi[1]; g++)
                for (int bl = b->lo[2]; bl <= b->hi[2]; bl++)
                {
                    uint32_t k = d->hist[r << 10 | g << 5 | bl];
                    s[0] += (int64_t) k * (r * 8 + 4); s[1] += (int64_t) k * (g * 8 + 4); s[2] += (int64_t) k * (bl * 8 + 4);
                    cnt += k;
                }
        for (int c = 0; c < 3; c++)
            d->pal[i][c] = (uint8_t) (cnt ? (s[c] + cnt / 2) / cnt : 0);
    }
}

/* the palette in order of green, to search outwards from the nearest
 * green and stop once green alone is further than the best so far */
static void sort_palette(struct fos_dev *d)
{
    uint8_t *by_green = d->by_green;
    for (int i = 0; i < d->npal; i++)
    {
        int j = i;
        for (; j > 0 && d->pal[by_green[j - 1]][1] > d->pal[i][1]; j--) by_green[j] = by_green[j - 1];
        by_green[j] = (uint8_t) i;
    }
}

/* the nearest palette entry for a 5-5-5 cell, worked out when first needed */
static __attribute__((noinline)) int nearest(struct fos_dev *d, int cell)
{
    int v = d->inv[cell];
    if (v) return v - 1;
    int r = (cell >> 10) * 8 + 4, g = (cell >> 5 & 31) * 8 + 4, b = (cell & 31) * 8 + 4;
    const uint8_t *by_green = d->by_green;
    int lo = 0, hi = d->npal;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        if (d->pal[by_green[mid]][1] < g) lo = mid + 1; else hi = mid;
    }
    int best = by_green[lo < d->npal ? lo : d->npal - 1], bd = 1 << 30;
    for (int up = lo, down = lo - 1; up < d->npal || down >= 0; )
    {
        int done = 1;
        if (up < d->npal)
        {
            const uint8_t *p = d->pal[by_green[up]];
            int dg = p[1] - g;
            if (4 * dg * dg < bd)
            {
                int dr = r - p[0], db = b - p[2], dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                if (dd < bd) { bd = dd; best = by_green[up]; }
                up++;
                done = 0;
            }
            else up = d->npal;
        }
        if (down >= 0)
        {
            const uint8_t *p = d->pal[by_green[down]];
            int dg = g - p[1];
            if (4 * dg * dg < bd)
            {
                int dr = r - p[0], db = b - p[2], dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                if (dd < bd) { bd = dd; best = by_green[down]; }
                down--;
                done = 0;
            }
            else down = -1;
        }
        if (done) break;
    }
    d->inv[cell] = (uint8_t) (best + 1);
    return best;
}

/* ---------------------------------------------------------------- files */

/* A print is an 8-bit BMP, so it also opens on a PC: the file and info
 * headers, 256 palette entries (1..240 the print's), our own header with
 * the frame's details, the pixels top row first, then the thumbnail. */
#define BMP_PAL     (14 + 40)
#define BMP_META    (BMP_PAL + 256 * 4)
#define META_SIZE   64
#define BMP_PIXELS  (BMP_META + META_SIZE)
#define BMP_THUMB   (BMP_PIXELS + FOS_PRINT_W * FOS_PRINT_H)
#define THUMB_SIZE  (FOS_THUMB_W * FOS_THUMB_H * 3)
#define BMP_SIZE    (BMP_THUMB + THUMB_SIZE)


static int write_print(const char *path, int film, int frame, const char *when, const char *fstop,
                       const uint8_t pal[][3], int npal, const uint8_t *px, const uint8_t *thumb, int x0, int w)
{
    static uint8_t h[BMP_PIXELS];
    memset(h, 0, sizeof(h));
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, BMP_SIZE);
    put32(h + 10, BMP_PIXELS);
    put32(h + 14, 40);
    put32(h + 18, FOS_PRINT_W);
    put32(h + 22, (uint32_t) -FOS_PRINT_H);         /* top-down */
    h[26] = 1; h[28] = 8;
    put32(h + 34, FOS_PRINT_W * FOS_PRINT_H);
    put32(h + 38, 2835); put32(h + 42, 2835);       /* 72 dpi */
    put32(h + 46, 256);
    for (int i = 0; i < npal && i < FOS_PRINT_COLOURS; i++)
    {
        uint8_t *q = h + BMP_PAL + 4 * (i + 1);
        q[0] = pal[i][2]; q[1] = pal[i][1]; q[2] = pal[i][0];
    }
    uint8_t *m = h + BMP_META;
    memcpy(m, "FOP2", 4);
    m[4] = (uint8_t) film;
    m[6] = (uint8_t) frame; m[7] = (uint8_t) (frame >> 8);
    if (when) memcpy(m + 8, when, strlen(when) < 19 ? strlen(when) : 19);
    if (fstop) memcpy(m + 28, fstop, strlen(fstop) < 7 ? strlen(fstop) : 7);
    m[36] = (uint8_t) x0; m[37] = (uint8_t) (x0 >> 8);         /* the picture's columns */
    m[38] = (uint8_t) w; m[39] = (uint8_t) (w >> 8);

    void *f = fos_port_open(path, 1);
    if (!f) return 0;
    int ok = fos_port_write(f, h, BMP_PIXELS) == BMP_PIXELS
          && fos_port_write(f, px, FOS_PRINT_W * FOS_PRINT_H) == FOS_PRINT_W * FOS_PRINT_H
          && fos_port_write(f, thumb, THUMB_SIZE) == THUMB_SIZE;
    fos_port_close(f);
    return ok;
}

static int is_print(const uint8_t *h)
{
    return h[0] == 'B' && h[1] == 'M' && u32le(h + 10) == BMP_PIXELS && u32le(h + 18) == FOS_PRINT_W
        && !memcmp(h + BMP_META, "FOP2", 4);
}

int fos_print_thumb(const char *path, uint8_t *thumb)
{
    void *f = fos_port_open(path, 0);
    if (!f) return 0;
    static uint8_t h[BMP_PIXELS];
    int ok = fos_port_read(f, h, BMP_PIXELS) == BMP_PIXELS && is_print(h)
          && fos_port_seek(f, BMP_THUMB) >= 0 && fos_port_read(f, thumb, THUMB_SIZE) == THUMB_SIZE;
    fos_port_close(f);
    return ok;
}

int fos_print_load(const char *path, struct fos_print *p)
{
    memset(p, 0, sizeof(*p));
    void *f = fos_port_open(path, 0);
    if (!f) return 0;
    static uint8_t h[BMP_PIXELS];
    int ok = fos_port_read(f, h, BMP_PIXELS) == BMP_PIXELS && is_print(h);
    if (ok)
    {
        const uint8_t *m = h + BMP_META;
        p->film = m[4];
        p->frame = u16le(m + 6);
        memcpy(p->when, m + 8, 19);
        memcpy(p->fstop, m + 28, 7);
        p->x0 = u16le(m + 36);
        p->w = u16le(m + 38);
        if (!p->w || p->x0 + p->w > FOS_PRINT_W) p->x0 = 0, p->w = FOS_PRINT_W;
        for (int i = 0; i < FOS_PRINT_COLOURS; i++)
        {
            const uint8_t *q = h + BMP_PAL + 4 * (i + 1);
            p->pal[i][0] = q[2]; p->pal[i][1] = q[1]; p->pal[i][2] = q[0];
        }
        p->px = fos_port_alloc(FOS_PRINT_W * FOS_PRINT_H);
        ok = p->px && fos_port_read(f, p->px, FOS_PRINT_W * FOS_PRINT_H) == FOS_PRINT_W * FOS_PRINT_H;
    }
    fos_port_close(f);
    if (!ok) fos_print_free(p);
    return ok;
}

void fos_print_free(struct fos_print *p)
{
    fos_port_free(p->px);
    p->px = 0;
}

/* ---------------------------------------------------------------- quantize an RGB print */

/* histogram, palette, error diffusion: rows y0..y1-1 of the dithering per
 * call (the histogram and palette when y0 == 0) */
static int quant_alloc(struct fos_dev *d)
{
    d->hist = fos_port_alloc(32768 * 4);
    d->inv = fos_port_alloc(32768);
    d->err_rows = fos_port_alloc((FOS_PRINT_W + 2) * 3 * 2 * 2);
    if (!d->idx) d->idx = fos_port_alloc(FOS_PRINT_W * FOS_PRINT_H);
    if (!d->hist || !d->inv || !d->err_rows || !d->idx) return 0;
    memset(d->hist, 0, 32768 * 4);
    memset(d->inv, 0, 32768);
    memset(d->err_rows, 0, (FOS_PRINT_W + 2) * 3 * 2 * 2);
    for (int i = 0; i < FOS_PRINT_W * FOS_PRINT_H; i++)
    {
        const uint8_t *p = d->rgb + 3 * i;
        d->hist[(p[0] >> 3) << 10 | (p[1] >> 3) << 5 | p[2] >> 3]++;
    }
    median_cut(d);
    sort_palette(d);
    return 1;
}

static __attribute__((noinline)) void dither_rows(struct fos_dev *d, int y0, int y1)
{
    const int W = FOS_PRINT_W;
    const uint8_t *inv = d->inv;
    const uint8_t (*pal)[3] = d->pal;
    for (int y = y0; y < y1; y++)
    {
        /* serpentine: every other row right to left */
        const int dir = y & 1 ? -1 : 1, st = 3 * dir;
        int16_t *cur = d->err_rows + ((y & 1) ? (W + 2) * 3 : 0);
        int16_t *nxt = d->err_rows + ((y & 1) ? 0 : (W + 2) * 3);
        memset(nxt, 0, (W + 2) * 3 * 2);
        const int x = dir > 0 ? 0 : W - 1;
        const uint8_t *p = d->rgb + 3 * (y * W + x);
        uint8_t *o = d->idx + y * W + x;
        int16_t *c = cur + (x + 1) * 3, *n = nxt + (x + 1) * 3;
        for (int k = 0; k < W; k++, p += st, o += dir, c += st, n += st)
        {
            int r = p[0] + (c[0] >> 4), g = p[1] + (c[1] >> 4), b = p[2] + (c[2] >> 4);
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            int cell = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
            int i = inv[cell] ? inv[cell] - 1 : nearest(d, cell);
            *o = (uint8_t) (i + 1);
            const uint8_t *q = pal[i];
            /* Floyd-Steinberg, in 1/16 */
            int e = r - q[0];
            c[st] = (int16_t) (c[st] + e * 7); n[-st] = (int16_t) (n[-st] + e * 3); n[0] = (int16_t) (n[0] + e * 5); n[st] = (int16_t) (n[st] + e);
            e = g - q[1];
            c[st + 1] = (int16_t) (c[st + 1] + e * 7); n[1 - st] = (int16_t) (n[1 - st] + e * 3); n[1] = (int16_t) (n[1] + e * 5); n[st + 1] = (int16_t) (n[st + 1] + e);
            e = b - q[2];
            c[st + 2] = (int16_t) (c[st + 2] + e * 7); n[2 - st] = (int16_t) (n[2 - st] + e * 3); n[2] = (int16_t) (n[2] + e * 5); n[st + 2] = (int16_t) (n[st + 2] + e);
        }
    }
}

/* ---------------------------------------------------------------- one frame */

struct fos_dev *fos_dev_start(const char *cr2, const char *print, int film, int frame,
                              const char *when, const char *fstop)
{
    struct fos_dev *d = fos_port_alloc(sizeof(struct fos_dev));
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    snprintf(d->cr2, sizeof(d->cr2), "%s", cr2);
    snprintf(d->print, sizeof(d->print), "%s", print);
    snprintf(d->when, sizeof(d->when), "%s", when ? when : "");
    snprintf(d->fstop, sizeof(d->fstop), "%s", fstop ? fstop : "");
    d->film = fos_clamp(film, 0, fos_nfilms - 1);
    d->frame = frame;
    d->look = fos_look_of(fos_films[d->film].key);
    d->seed = 12345u + (unsigned) frame * 7919u;
    d->stage = ST_OPEN;
    return d;
}

void fos_dev_jpeg(struct fos_dev *d, const char *path)
{
    if (d) snprintf(d->jpeg, sizeof(d->jpeg), "%s", path ? path : "");
}

void fos_dev_free(struct fos_dev *d)
{
    if (!d) return;
    if (d->f) fos_port_close(d->f);
    fos_port_free(d->buf); fos_port_free(d->rowoff); fos_port_free(d->seg_of);
    fos_port_free(d->seg_x0); fos_port_free(d->seg_off); fos_port_free(d->seg_desc); fos_port_free(d->desc_off); fos_port_free(d->seg_piece);
    fos_port_free(d->rowob); fos_port_free(d->colob); fos_port_free(d->ne_r); fos_port_free(d->ne_c);
    fos_port_free(d->sums); fos_port_free(d->glow); fos_port_free(d->rgb);
    fos_port_free(d->hist); fos_port_free(d->inv); fos_port_free(d->err_rows); fos_port_free(d->idx);
    fos_port_free(d);
}

const uint8_t *fos_dev_thumb(const struct fos_dev *d)
{
    return d && d->stage == ST_DONE ? d->thumb : 0;
}

const char *fos_dev_error(const struct fos_dev *d)
{
    return d ? d->err : "out of memory";
}

int fos_dev_doing(const struct fos_dev *d)
{
    if (!d) return FOS_DEV_FAILED;
    switch (d->stage)
    {
        case ST_OPEN: return FOS_DEV_MIXING;
        case ST_DECODE: return lut_film == d->film && lut_built >= NGRID ? FOS_DEV_DEVELOPING : FOS_DEV_MIXING;
        case ST_KEY: case ST_EXPOSE: case ST_GLOW: case ST_FINISH: return FOS_DEV_FIXING;
        case ST_JPEG: case ST_QUANT: case ST_DITHER: case ST_WRITE: return FOS_DEV_DRYING;
        case ST_DONE: return FOS_DEV_DONE;
    }
    return FOS_DEV_FAILED;
}

/* how far along, 0..1000: decoding is most of the work */
static int progress(const struct fos_dev *d)
{
    const int H = FOS_PRINT_H;
    switch (d->stage)
    {
        case ST_OPEN: return 0;
        case ST_DECODE: return d->total ? (int) ((int64_t) d->done * 800 / d->total) : 0;   /* 64 bits: long is 32 on the camera */
        case ST_KEY: return 800;
        case ST_EXPOSE: return 805 + d->row * 60 / H;
        case ST_GLOW: return 865;
        case ST_FINISH: return 870 + d->row * 64 / H;
        case ST_JPEG: return 934;
        case ST_QUANT: return 940;
        case ST_DITHER: return 950 + d->row * 45 / H;
        case ST_WRITE: return 995;
        case ST_DONE: return 1000;
    }
    return -1;
}

void fos_dev_picture(const struct fos_dev *d, int *x0, int *w)
{
    *x0 = d && d->rows_axis_x ? d->ox0 : 0;
    *w = d && d->rows_axis_x ? d->ow : FOS_PRINT_W;
}

int fos_dev_progress(const struct fos_dev *d)
{
    return d ? progress(d) : -1;
}

/* the lab scanner: the log-average luminance to a fixed key (negatives),
 * or what was exposed (slides); then the film's balance */
static void stage_key(struct fos_dev *d)
{
    const int W = FOS_PRINT_W, H = FOS_PRINT_H;
    float ev = 1.0f;
    if (!d->look->slide)
    {
        /* counted in a histogram so the logs are few */
        uint32_t *hist = (uint32_t *) d->rgb;       /* free until ST_FINISH */
        memset(hist, 0, 4097 * 4);
        int n = 0;
        for (int y = 0; y < H; y += 4)                /* as the lab: every fourth pixel */
            for (int x = 0; x < W; x += 4)
            {
                int lin[3];
                if (d->rows_axis_x && (x < d->ox0 || x >= d->ox0 + d->ow)) continue;
                pixel_linear(d, x, y, lin);
                hist[(871 * lin[0] + 2929 * lin[1] + 296 * lin[2]) >> 12]++;
                n++;
            }
        float acc = 0;
        for (int v = 0; v <= 4096; v++)
            if (hist[v]) acc += (float) hist[v] * logf(v / 4096.0f + 1e-4f);
        float key = n ? expf(acc / (float) n) : 0.11f;
        ev = log2f(0.11f / key);
        ev = ev < -2 ? -2 : ev > 3.5f ? 3.5f : ev;
#ifdef FOS_DEV_TEST
        fprintf(stderr, "scanner: key %.4f, %.3f EV; black %d/16, pre %d %d %d\n", (double) key, (double) ev, d->black16, d->pre[0], d->pre[1], d->pre[2]);
#endif
    }
    float k = exp2f(ev + d->look->ev);
    float bal[3] = { 1, 1, 1 };
    if (d->look->balance != 5500)
    {
        float a[3], b[3];
        cct_rgb(5500, a);
        cct_rgb((float) d->look->balance, b);
        for (int c = 0; c < 3; c++) bal[c] = a[c] / b[c];
        for (int c = 0; c < 3; c++) bal[c] /= a[1] / b[1];
    }
    for (int c = 0; c < 3; c++) d->mul[c] = (int) (k * bal[c] * 4096 + 0.5f);
}

/* Exposing a row goes in two passes, each small enough for the
 * registers: the camera's colours, then linear sRGB. */

/* n pixels' sums to camera colour (4096 = white): the means (of counts
 * fixed a, b times along x ra[], rb[]: the even and odd raw rows and
 * columns), less black, balanced */
static __attribute__((noinline)) void expose_cam(const struct fos_dev *d, const uint32_t *sm, uint16_t *cam, int n,
                                                 int a, int b, const uint16_t *ra, const uint16_t *rb)
{
    const int black = d->black16, p0 = d->pre[0], p1 = d->pre[1], p2 = d->pre[2];
    for (const uint16_t *e = ra + n; ra < e; ra++, rb++, sm += 3, cam += 3)
    {
        int n0 = a * *ra, n1 = a * *rb + b * *ra, n2 = b * *rb;
        int c0 = 0, c1 = 0, c2 = 0;
        /* (sums <= 16383 n, so this fits) */
        if (n0 < 64) { int m = (int) (sm[0] * recip[n0] >> 12) - black; if (m > 0) { c0 = (m * p0) >> 14; if (c0 > 4096) c0 = 4096; } }
        if (n1 < 64) { int m = (int) (sm[1] * recip[n1] >> 12) - black; if (m > 0) { c1 = (m * p1) >> 14; if (c1 > 4096) c1 = 4096; } }
        if (n2 < 64) { int m = (int) (sm[2] * recip[n2] >> 12) - black; if (m > 0) { c2 = (m * p2) >> 14; if (c2 > 4096) c2 = 4096; } }
        cam[0] = (uint16_t) c0; cam[1] = (uint16_t) c1; cam[2] = (uint16_t) c2;
    }
}

/* n pixels' camera colour to linear sRGB (4096 = 1.0) at o, exposed and
 * balanced for the film; the highlights into the glow row g from x */
static __attribute__((noinline)) void expose_lin(const struct fos_dev *d, const uint16_t *cam, uint16_t *o, int n,
                                                 int32_t *g, int x)
{
    const int m00 = d->mat[0][0], m01 = d->mat[0][1], m02 = d->mat[0][2];
    const int m10 = d->mat[1][0], m11 = d->mat[1][1], m12 = d->mat[1][2];
    const int m20 = d->mat[2][0], m21 = d->mat[2][1], m22 = d->mat[2][2];
    const int u0 = d->mul[0], u1 = d->mul[1], u2 = d->mul[2];
    const int thr = (int) (d->look->hal_thr * 4096);
    for (const uint16_t *e = cam + 3 * n; cam < e; cam += 3, o += 3, x++)
    {
        int c0 = cam[0], c1 = cam[1], c2 = cam[2];
        int r = (m00 * c0 + m01 * c1 + m02 * c2) >> 12;
        int gg = (m10 * c0 + m11 * c1 + m12 * c2) >> 12;
        int bb = (m20 * c0 + m21 * c1 + m22 * c2) >> 12;
        r = r < 0 ? 0 : r > 4096 ? 4096 : r;
        gg = gg < 0 ? 0 : gg > 4096 ? 4096 : gg;
        bb = bb < 0 ? 0 : bb > 4096 ? 4096 : bb;
        r = (r * u0) >> 12; gg = (gg * u1) >> 12; bb = (bb * u2) >> 12;
        if (r > 65535) r = 65535;
        if (gg > 65535) gg = 65535;
        if (bb > 65535) bb = 65535;
        o[0] = (uint16_t) r; o[1] = (uint16_t) gg; o[2] = (uint16_t) bb;
        if (g)
        {
            int l = (871 * r + 2929 * gg + 296 * bb) >> 12;
            if (l > thr) g[x >> 2] += l - thr;
        }
    }
}

/* rows y0..y1-1 to linear sRGB, in place over the sums (a row's sums are
 * read before it is written, and its values only ever overwrite sums
 * already read); the highlights go into the halation grid on the way. The
 * same sums as pixel_linear, walked along the row. */
static __attribute__((noinline)) void stage_expose(struct fos_dev *d, int y0, int y1)
{
    const int W = FOS_PRINT_W;
    uint16_t *lin = (uint16_t *) d->sums;
    for (int y = y0; y < y1; y++)
    {
        int32_t *g = d->glow ? d->glow + (y >> 2) * GW : 0;
        int xa = 0, xb = W;
        /* the counts of the samples summed: rows fixed and columns along the
         * row (landscape), or the other way round (portrait) */
        int a, b;
        const uint16_t *ra, *rb;
        if (d->rows_axis_x)
        {
            xa = d->ox0; xb = d->ox0 + d->ow;
            a = d->ne_c[y]; b = d->no_c[y]; ra = d->ne_r; rb = d->no_r;
        }
        else { a = d->ne_r[y]; b = d->no_r[y]; ra = d->ne_c; rb = d->no_c; }
        expose_cam(d, d->sums + 3 * (y * W + xa), d->pos, xb - xa, a, b, ra, rb);
        memset(lin + 3 * y * W, 0, 6 * xa);
        memset(lin + 3 * (y * W + xb), 0, 6 * (W - xb));
        expose_lin(d, d->pos, lin + 3 * (y * W + xa), xb - xa, g, xa);
    }
    d->lin = lin;
}

/* halation: the highlights' glow, spread about as the lab's gaussian
 * (0.6 % of the frame) by two [1 2 1] passes each way on the grid */
static void stage_glow(struct fos_dev *d)
{
    int32_t *g = d->glow, tmp[GW > GH ? GW : GH];
    for (int i = 0; i < GW * GH; i++) g[i] >>= 4;   /* the cell's mean */
    for (int pass = 0; pass < 2; pass++)
    {
        for (int y = 0; y < GH; y++)
        {
            int32_t *r = g + y * GW;
            for (int x = 0; x < GW; x++)
                tmp[x] = (r[x > 0 ? x - 1 : 0] + 2 * r[x] + r[x < GW - 1 ? x + 1 : GW - 1]) >> 2;
            memcpy(r, tmp, sizeof(int32_t) * GW);
        }
        for (int x = 0; x < GW; x++)
        {
            for (int y = 0; y < GH; y++)
                tmp[y] = (g[(y > 0 ? y - 1 : 0) * GW + x] + 2 * g[y * GW + x] + g[(y < GH - 1 ? y + 1 : GH - 1) * GW + x]) >> 2;
            for (int y = 0; y < GH; y++) g[y * GW + x] = tmp[y];
        }
    }
}

/* a cell of the glow grid and the weight of the next, in 1/8, for print
 * line p: cell centres sit at 4j + 1.5 */
static void glow_pos(int p, int n, uint8_t *cell, uint8_t *frac)
{
    int t = 2 * p - 3;
    if (t < 0) { *cell = 0; *frac = 0; return; }
    int j = t >> 3;
    if (j >= n - 1) { *cell = (uint8_t) (n - 2); *frac = 8; return; }
    *cell = (uint8_t) j;
    *frac = (uint8_t) (t & 7);
}

/* Finishing a row of the print goes in three passes, each small enough
 * for the registers: where its pixels fall on the film's table, the
 * table, the grain. */

/* n pixels from lin: the vignette (vig[x] for pixel x, in 1/4096) and the
 * halation (the glow row grow mixed along it by gx, fx; hal: its colour)
 * on the linear light, then its place on the grid */
static __attribute__((noinline)) void finish_pos(const uint16_t *lin, uint16_t *pos, int n, int vy, const int16_t *vx,
                                                 const int32_t *grow, const uint8_t *gx, const uint8_t *fx, const int *hal)
{
    const uint16_t *cin = tab->cin;
    if (!grow)
    {
        for (const uint16_t *e = lin + 3 * n; lin < e; lin += 3, pos += 3, vx++)
        {
            int vig = vy - *vx;
            pos[0] = cin_pos(cin, (lin[0] * vig) >> 12);
            pos[1] = cin_pos(cin, (lin[1] * vig) >> 12);
            pos[2] = cin_pos(cin, (lin[2] * vig) >> 12);
        }
        return;
    }
    const int h0 = hal[0], h1 = hal[1], h2 = hal[2];
    for (const uint16_t *e = lin + 3 * n; lin < e; lin += 3, pos += 3, vx++, gx++, fx++)
    {
        int vig = vy - *vx;
        int a = *gx, f = *fx;
        int glow = (grow[a] * (8 - f) + grow[a + 1] * f) >> 6;
        pos[0] = cin_pos(cin, ((lin[0] * vig) >> 12) + ((glow * h0) >> 8));
        pos[1] = cin_pos(cin, ((lin[1] * vig) >> 12) + ((glow * h1) >> 8));
        pos[2] = cin_pos(cin, ((lin[2] * vig) >> 12) + ((glow * h2) >> 8));
    }
}

static __attribute__((noinline)) void finish_lut(const uint16_t *pos, uint8_t *o, int n)
{
    const uint8_t (*lut)[3] = tab->lut;
    for (const uint16_t *e = pos + 3 * n; pos < e; pos += 3, o += 3)
        lut_apply(lut, pos[0], pos[1], pos[2], o);
}

/* grain at screen size: the lab's, softened by the downscale (grain: its
 * strength), strongest in the mid tones */
static __attribute__((noinline)) unsigned finish_grain(uint8_t *o, int n, unsigned seed, int grain, int mono)
{
    for (uint8_t *e = o + 3 * n; o < e; o += 3)
    {
        int l = (o[0] * 54 + o[1] * 183 + o[2] * 19) >> 8;
        int wgt = (l * (255 - l)) >> 6;
        int shared = rnd(&seed);
        for (int c = 0; c < 3; c++)
        {
            int k = mono ? shared : (shared * 3 + rnd(&seed) * 2) / 4;
            int v = o[c] + (int) (((long) k * grain >> 15) * wgt >> 16);
            o[c] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    return seed;
}

/* row y of the print: vignette, halation, the film's table, grain */
static __attribute__((noinline)) void stage_finish(struct fos_dev *d, int y)
{
    const int W = FOS_PRINT_W, H = FOS_PRINT_H;
    const struct fos_look *L = d->look;
    int pw = d->rows_axis_x ? d->ow : W, px0 = d->rows_axis_x ? d->ox0 : 0;
    if (y == 0)
    {
        /* vignette, as the lab: 1 - 0.12 r^2, r^2 = (x^2 + y^2) / 2 over
         * -1..1; the x and y parts, in 1/4096 */
        for (int x = 0; x < pw; x++) { float t = 2.0f * x / (pw - 1) - 1; d->vig_x[x] = (int16_t) (0.06f * t * t * 4096); }
        for (int k = 0; k < H; k++) { float t = 2.0f * k / (H - 1) - 1; d->vig_y[k] = (int16_t) (0.06f * t * t * 4096); }
        for (int x = 0; x < W; x++) glow_pos(x, GW, &d->gx[x], &d->fx[x]);
    }
    int hal[3];
    for (int c = 0; c < 3; c++) hal[c] = d->glow ? (int) (L->halation * L->hal_tint[c] * 256) : 0;
    /* the glow between the grid's two rows about this one, in 1/8 */
    int32_t grow[GW];
    if (d->glow)
    {
        uint8_t gy, fy;
        glow_pos(y, GH, &gy, &fy);
        const int32_t *g0 = d->glow + gy * GW, *g1 = g0 + GW;
        for (int j = 0; j < GW; j++) grow[j] = g0[j] * (8 - fy) + g1[j] * fy;
    }
    uint8_t *o = d->rgb + 3 * y * W;
    memset(o, 0, 3 * px0);
    memset(o + 3 * (px0 + pw), 0, 3 * (W - px0 - pw));
    finish_pos(d->lin + 3 * (y * W + px0), d->pos, pw, 4096 - d->vig_y[y], d->vig_x,
               d->glow ? grow : 0, d->gx + px0, d->fx + px0, hal);
    finish_lut(d->pos, o + 3 * px0, pw);
    int grain = (int) (L->grain * 255 * 0.55f * 256);
    if (grain) d->seed = finish_grain(o + 3 * px0, pw, d->seed, grain, L->mono);
}

/* the thumbnail: 6 x 6 boxes, a row of them at a time */
static void make_thumb(struct fos_dev *d)
{
    const int W = FOS_PRINT_W;
    int acc[FOS_THUMB_W * 3];
    for (int ty = 0; ty < FOS_THUMB_H; ty++)
    {
        memset(acc, 0, sizeof(acc));
        for (int j = 0; j < 6; j++)
        {
            const uint8_t *p = d->rgb + 3 * (ty * 6 + j) * W;
            for (int tx = 0; tx < FOS_THUMB_W; tx++, p += 18)
            {
                int *a = acc + 3 * tx;
                a[0] += p[0] + p[3] + p[6] + p[9] + p[12] + p[15];
                a[1] += p[1] + p[4] + p[7] + p[10] + p[13] + p[16];
                a[2] += p[2] + p[5] + p[8] + p[11] + p[14] + p[17];
            }
        }
        for (int i = 0; i < FOS_THUMB_W * 3; i++)
            d->thumb[3 * ty * FOS_THUMB_W + i] = (uint8_t) ((acc[i] * 455 + 8192) >> 14);   /* / 36 */
    }
}

int fos_dev_step(struct fos_dev *d, int budget_ms)
{
    if (!d) return -1;
    int t0 = fos_port_ms(), cap = fos_port_dev_cap(), slices = 0;
    const int W = FOS_PRINT_W, H = FOS_PRINT_H, N = W * H;
    while ((fos_port_ms() - t0 < budget_ms || d->stage == ST_OPEN) && (!cap || slices++ < cap))
    {
        switch (d->stage)
        {
            case ST_OPEN:
                if (!tables_ready()) return fail_mem(d, (int) sizeof(*tab));
                d->f = fos_port_open(d->cr2, 0);
                if (!d->f) return fail(d, "can't open the RAW file");
                if (parse_tiff(d) < 0 || parse_ljpeg(d) < 0 || setup_maps(d) < 0) return -1;
                {
                    char msg[320];
                    const char *name = strrchr(d->cr2, '/');
                    snprintf(msg, sizeof(msg), "dev: %s raw %dx%d (ljpeg %dx%d, %d comps, %d bits) slices %d x %d + %d, "
                             "picture %d,%d %dx%d, black cols %d..%d, orientation %d",
                             name ? name + 1 : d->cr2, d->raw_w, d->raw_h, d->wide, d->high, d->clrs, d->bits,
                             d->slices[0], d->slices[1], d->slices[2], d->crop_l, d->crop_t, d->crop_w, d->crop_h,
                             d->ob_l, d->ob_r, d->orient);
                    fos_port_log(msg);
                }
                d->buf = fos_port_alloc(CHUNK + 16);
                if (!d->buf) return fail_mem(d, CHUNK + 16);
                d->bp = d->be = d->buf;
                d->safe = d->buf;               /* the first read fills it */
                d->ff = 0;
                d->stage = ST_DECODE;
                break;
            case ST_DECODE:
                /* the film's table first, a little at a time */
                if (!lut_build(d->look, d->film, 64)) break;
                decode(d, 1 << 14);
                if (d->done >= d->total)
                {
                    fos_port_close(d->f);
                    d->f = 0;
                    fos_port_free(d->buf); d->buf = 0;
                    d->rgb = fos_port_alloc(N * 3);
                    if (!d->rgb) return fail_mem(d, N * 3);
                    if (d->look->halation > 0)
                    {
                        d->glow = fos_port_alloc(GW * GH * 4);
                        if (d->glow) memset(d->glow, 0, GW * GH * 4);
                    }
                    setup_colour(d);
                    d->stage = ST_KEY;
                    return progress(d);         /* the latent image can be read once more */
                }
                break;
            case ST_KEY:
                stage_key(d);
                d->stage = ST_EXPOSE;
                d->row = 0;
                break;
            case ST_EXPOSE:
                stage_expose(d, d->row, d->row + 8);
                d->row += 8;
                if (d->row >= H) d->stage = d->glow ? ST_GLOW : ST_FINISH, d->row = 0;
                break;
            case ST_GLOW:
                stage_glow(d);
                d->stage = ST_FINISH;
                d->row = 0;
                break;
            case ST_FINISH:
                stage_finish(d, d->row);
                if (++d->row == H)
                {
                    make_thumb(d);
                    fos_port_free(d->sums); d->sums = 0; d->lin = 0;
                    fos_port_free(d->glow); d->glow = 0;
                    d->stage = d->jpeg[0] ? ST_JPEG : ST_QUANT;
                }
                break;
            case ST_JPEG:
            {
                /* the picture only: a portrait frame without the print's sides */
                int x0 = d->rows_axis_x ? d->ox0 : 0, w = d->rows_axis_x ? d->ow : W;
                char desc[48];
                snprintf(desc, sizeof(desc), "%s%s%s", fos_films[d->film].brand, fos_films[d->film].brand[0] ? " " : "", fos_films[d->film].name);
                struct fos_jpeg_meta m = { d->when, desc, d->fstop, 0, 0 };
                if (!fos_jpeg_write(d->jpeg, d->rgb + 3 * x0, 3 * W, w, H, 92, &m))
                {
                    char msg[200];
                    snprintf(msg, sizeof(msg), "dev: can't write %s", d->jpeg);
                    fos_port_log(msg);
                }
                d->stage = ST_QUANT;
                break;
            }
            case ST_QUANT:
                if (!quant_alloc(d)) return fail_mem(d, 32768 * 5 + FOS_PRINT_W * FOS_PRINT_H);
                d->stage = ST_DITHER;
                d->row = 0;
                break;
            case ST_DITHER:
                dither_rows(d, d->row, d->row + 8);
                d->row += 8;
                if (d->row >= H) d->stage = ST_WRITE;
                break;
            case ST_WRITE:
                if (!write_print(d->print, d->film, d->frame, d->when, d->fstop, d->pal, d->npal, d->idx, d->thumb,
                                 d->rows_axis_x ? d->ox0 : 0, d->rows_axis_x ? d->ow : W))
                    return fail(d, "can't write the print");
                d->stage = ST_DONE;
                return 1000;
            case ST_DONE:
                return 1000;
            default:
                return -1;
        }
    }
    return progress(d);
}

/* ---------------------------------------------------------------- the contact sheet */

int fos_dev_sheet(const char *sheet, int film, const char *const *prints, int frames)
{
    struct fos_dev *d = fos_dev_start("", sheet, film, 0, "", "");
    if (!d) return 0;
    const int W = FOS_PRINT_W, H = FOS_PRINT_H;
    d->rgb = fos_port_alloc(W * H * 3);
    uint8_t *thumb = fos_port_alloc(FOS_THUMB_W * FOS_THUMB_H * 3);
    if (!d->rgb || !thumb) { fos_port_free(thumb); fos_dev_free(d); return 0; }
    /* strips of six frames on black, each frame inset by its rebate */
    memset(d->rgb, 0, W * H * 3);
    int rows = (frames + 5) / 6;
    int y0 = (H - rows * FOS_THUMB_H) / 2;
    for (int i = 0; i < frames; i++)
    {
        if (!prints[i] || !fos_print_thumb(prints[i], thumb)) continue;
        int cx = (i % 6) * FOS_THUMB_W + FOS_SHEET_FX, cy = y0 + (i / 6) * FOS_THUMB_H + FOS_SHEET_FY;
        for (int y = 0; y < FOS_SHEET_FH; y++)
            for (int x = 0; x < FOS_SHEET_FW; x++)
            {
                int sx = x * FOS_THUMB_W / FOS_SHEET_FW, sy = y * FOS_THUMB_H / FOS_SHEET_FH;
                memcpy(d->rgb + 3 * ((cy + y) * W + cx + x), thumb + 3 * (sy * FOS_THUMB_W + sx), 3);
            }
        if (i == 0) memcpy(d->thumb, thumb, sizeof(d->thumb));      /* the roll's own thumbnail */
    }
    fos_port_free(thumb);
    int ok = quant_alloc(d);
    if (ok)
    {
        dither_rows(d, 0, H);
        ok = write_print(sheet, film, 0, "", "", d->pal, d->npal, d->idx, d->thumb, 0, W);
    }
    fos_dev_free(d);
    return ok;
}

/* ---------------------------------------------------------------- the latent image */

/* where print pixel (px, py) comes from on the sensor: its middle */
static void raw_of(const struct fos_dev *d, int px, int py, int *rx, int *ry)
{
    int rlen, clen, a, b;
    if (d->rows_axis_x) { rlen = d->ow; clen = d->oh; a = px - d->ox0; b = py; }
    else { rlen = d->oh; clen = d->ow; a = py; b = px; }
    if (d->orient == 6 || d->orient == 3) a = rlen - 1 - a;
    if (d->orient == 8 || d->orient == 3) b = clen - 1 - b;
    *ry = d->crop_t + (int) ((long) (2 * a + 1) * d->crop_h / (2 * rlen));
    *rx = d->crop_l + (int) ((long) (2 * b + 1) * d->crop_w / (2 * clen));
}

int fos_dev_preview(const struct fos_dev *d, uint8_t *lum, int w, int h)
{
    if (!d || (d->stage != ST_DECODE && d->stage != ST_KEY) || !d->sums) return 0;
    const int W = FOS_PRINT_W, H = FOS_PRINT_H;
    for (int ty = 0; ty < h; ty++)
    {
        int py = (2 * ty + 1) * H / (2 * h);
        for (int tx = 0; tx < w; tx++)
        {
            int px = (2 * tx + 1) * W / (2 * w);
            uint8_t *o = &lum[ty * w + tx];
            *o = 255;                               /* blank paper */
            if (d->rows_axis_x && (px < d->ox0 || px >= d->ox0 + d->ow)) continue;
            int rx, ry;
            raw_of(d, px, py, &rx, &ry);
            int reached = d->done >= d->total || rx < d->sx0 || (rx < d->sx0 + d->sw && ry < d->sy);
            if (!reached) continue;
            /* green, roughly exposed: enough to see the picture come */
            int xr = d->rows_axis_x ? px - d->ox0 : py, xc = d->rows_axis_x ? py : px;
            int n = d->ne_r[xr] * d->no_c[xc] + d->no_r[xr] * d->ne_c[xc];
            if (!n) continue;
            int g = (int) (d->sums[3 * (py * W + px) + 1] / n) - 2048;
            int v = g <= 0 ? 0 : g >= 2900 ? 4096 : g * 4096 / 2900;
            *o = (uint8_t) (fos_isqrt((unsigned) v * 16) * 255 / 256);
        }
    }
    return 1;
}
