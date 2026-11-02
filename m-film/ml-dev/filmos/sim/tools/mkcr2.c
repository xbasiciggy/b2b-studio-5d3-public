/* mkcr2: write test CR2 files shaped like the EOS M's, for the in-camera
 * development engine (fos_dev.c) and its simulator runs.
 *
 *   mkcr2 out.CR2 [--scene chart|street] [--small|--half] [--clrs 2|4] [--slices N,W,LAST] [--fold]
 *                 [--sensor W,H,LEFT,TOP]   another camera's sensor (LibRaw's identify.cpp table)
 *                 [--portrait] [--ev 0] [--raw out.u16]
 *                 [--image photo.pfm]       a photo instead of a made-up scene: linear sRGB,
 *                                           0.18 = mid grey (Portable Float Map, 3 channels)
 *
 * The scene is made in linear sRGB, turned into what the sensor would
 * record (the EOS M's colour matrix and daylight balance from LibRaw, an
 * RGGB mosaic, 14 bits with black at 2048, noise, optical black borders),
 * and written as Canon does: a TIFF with the raw in its own IFD as a
 * lossless JPEG cut into vertical slices, and the sensor's borders in the
 * Canon maker note. LibRaw reads the result (sim/tools/checkcr2.py).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the camera */

static int RAW_W = 5280, RAW_H = 3528;          /* the whole sensor read out */
static int CROP_L = 84, CROP_T = 64, CROP_W = 5184, CROP_H = 3456;   /* the picture */
#define BLACK 2048
#define WHITE 13600

/* XYZ -> camera, EOS M (LibRaw colordata, same as the 650D) */
static const double xyz_cam[3][3] = {
    { 0.6602, -0.0841, -0.0939 }, { -0.4472, 1.2458, 0.2247 }, { -0.0975, 0.2039, 0.6148 } };
static const double rgb_xyz[3][3] = {
    { 0.4124564, 0.3575761, 0.1804375 }, { 0.2126729, 0.7151522, 0.0721750 }, { 0.0193339, 0.1191920, 0.9503041 } };
static double cam_rgb[3][3], pre_mul[3];

static void camera_init()
{
    /* as dcraw: camera <- sRGB, rows normalised to 1; their sums are the
     * daylight multipliers */
    for (int i = 0; i < 3; i++)
    {
        double sum = 0;
        for (int j = 0; j < 3; j++)
        {
            cam_rgb[i][j] = 0;
            for (int k = 0; k < 3; k++) cam_rgb[i][j] += xyz_cam[i][k] * rgb_xyz[k][j];
            sum += cam_rgb[i][j];
        }
        for (int j = 0; j < 3; j++) cam_rgb[i][j] /= sum;
        pre_mul[i] = 1 / sum;
    }
    for (int i = 0; i < 3; i++) pre_mul[i] /= pre_mul[1] ? pre_mul[1] : 1;
}

/* ---------------------------------------------------------------- scenes */

static double srgb_to_lin(double v)
{
    v /= 255;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static const unsigned char checker[24][3] = {
    { 115, 82, 68 }, { 194, 150, 130 }, { 98, 122, 157 }, { 87, 108, 67 }, { 133, 128, 177 }, { 103, 189, 170 },
    { 214, 126, 44 }, { 80, 91, 166 }, { 193, 90, 99 }, { 94, 60, 108 }, { 157, 188, 64 }, { 224, 163, 46 },
    { 56, 61, 150 }, { 70, 148, 73 }, { 175, 54, 60 }, { 231, 199, 31 }, { 187, 86, 149 }, { 8, 133, 161 },
    { 243, 243, 242 }, { 200, 200, 200 }, { 160, 160, 160 }, { 122, 122, 121 }, { 85, 85, 85 }, { 52, 52, 52 },
};

static int scene_kind = 0;      /* 0 chart, 1 street */
static float *photo;            /* --image: linear sRGB, rows top to bottom */
static int photo_w, photo_h;
static int fold = 0;            /* --fold */

static double hash01(unsigned x, unsigned y)
{
    unsigned h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0;
}

/* linear sRGB at (u, v) in 0..1 across the picture; 0.18 = mid grey */
static int load_pfm(const char *path)
{
    FILE *f = fopen(path, "rb");
    char kind[3] = { 0 };
    double scale;
    if (!f || fscanf(f, "%2s %d %d %lf", kind, &photo_w, &photo_h, &scale) != 4 || strcmp(kind, "PF") || fgetc(f) == EOF)
    {
        fprintf(stderr, "%s: not a colour PFM\n", path);
        return 0;
    }
    size_t n = (size_t) photo_w * photo_h * 3;
    float *rows = malloc(n * sizeof(float));
    photo = malloc(n * sizeof(float));
    if (fread(rows, sizeof(float), n, f) != n) { fprintf(stderr, "%s: too short\n", path); return 0; }
    fclose(f);
    /* PFM rows go bottom to top; little endian (scale < 0) is all this reads */
    for (int y = 0; y < photo_h; y++)
        memcpy(photo + (size_t) y * photo_w * 3, rows + (size_t) (photo_h - 1 - y) * photo_w * 3, photo_w * 3 * sizeof(float));
    free(rows);
    return 1;
}

static void scene(double u, double v, double rgb[3])
{
    if (photo)
    {
        /* bilinear, so a smaller photo still gives a smooth mosaic */
        double x = u * photo_w - 0.5, y = v * photo_h - 0.5;
        int x0 = (int) floor(x), y0 = (int) floor(y);
        double fx = x - x0, fy = y - y0;
        for (int i = 0; i < 3; i++) rgb[i] = 0;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
            {
                int px = x0 + dx < 0 ? 0 : x0 + dx >= photo_w ? photo_w - 1 : x0 + dx;
                int py = y0 + dy < 0 ? 0 : y0 + dy >= photo_h ? photo_h - 1 : y0 + dy;
                double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                const float *p = photo + ((size_t) py * photo_w + px) * 3;
                for (int i = 0; i < 3; i++) rgb[i] += w * p[i];
            }
        return;
    }
    if (scene_kind == 0)
    {
        /* a colour checker on a grey card, a step wedge under it, sky above */
        if (v < 0.22)
        {
            double t = v / 0.22;
            rgb[0] = 0.10 + 0.15 * t; rgb[1] = 0.20 + 0.15 * t; rgb[2] = 0.45 + 0.08 * t;
            /* the sun, burnt out, top right */
            double dx = u - 0.86, dy = v - 0.09, d = sqrt(dx * dx + dy * dy * 2.25);
            if (d < 0.035) rgb[0] = rgb[1] = rgb[2] = 30;
            return;
        }
        double gx = (u - 0.12) / 0.76, gy = (v - 0.27) / 0.48;
        if (gx >= 0 && gx < 1 && gy >= 0 && gy < 1)
        {
            int cx = (int) (gx * 6), cy = (int) (gy * 4);
            double fx = gx * 6 - cx, fy = gy * 4 - cy;
            if (fx > 0.08 && fx < 0.92 && fy > 0.08 && fy < 0.92)
            {
                const unsigned char *c = checker[cy * 6 + cx];
                for (int i = 0; i < 3; i++) rgb[i] = srgb_to_lin(c[i]);
                return;
            }
            rgb[0] = rgb[1] = rgb[2] = 0.035;          /* the chart's black frame */
            return;
        }
        if (v > 0.82 && v < 0.94 && u > 0.08 && u < 0.92)
        {
            int step = (int) ((u - 0.08) / 0.84 * 11);
            rgb[0] = rgb[1] = rgb[2] = 0.18 * pow(2, step - 6.5);   /* -6.5 .. +3.5 stops */
            return;
        }
        rgb[0] = rgb[1] = rgb[2] = 0.18 * (0.9 + 0.2 * u);       /* grey card, a little uneven */
        return;
    }
    /* street at golden hour: sky, a warm wall with windows, a red door,
     * green plants, a shadowed pavement */
    double horizon = 0.38;
    if (v < horizon)
    {
        double t = v / horizon;
        rgb[0] = 0.25 + 0.55 * t; rgb[1] = 0.35 + 0.35 * t; rgb[2] = 0.65 - 0.15 * t;
        double dx = u - 0.2, dy = v - 0.28, d = sqrt(dx * dx + dy * dy);
        if (d < 0.03) rgb[0] = rgb[1] = rgb[2] = 40;                    /* low sun */
        else if (d < 0.12) { double g = (0.12 - d) / 0.09; rgb[0] += 2 * g; rgb[1] += 1.2 * g; rgb[2] += 0.4 * g; }
        return;
    }
    double wall[3] = { 0.55, 0.32, 0.18 };
    for (int i = 0; i < 3; i++) rgb[i] = wall[i] * (0.8 + 0.4 * hash01((unsigned) (u * 400), (unsigned) (v * 300)));
    if (u > 0.55 && u < 0.65 && v > 0.5 && v < 0.85) { rgb[0] = 0.32; rgb[1] = 0.03; rgb[2] = 0.035; }   /* door */
    for (int k = 0; k < 3; k++)
    {
        double wx = 0.1 + k * 0.15;
        if (u > wx && u < wx + 0.08 && v > 0.45 && v < 0.6) { rgb[0] = 0.04; rgb[1] = 0.05; rgb[2] = 0.07; }
    }
    if (v > 0.72 && (u < 0.5 || u > 0.7))
    {
        double n = hash01((unsigned) (u * 900), (unsigned) (v * 700));
        rgb[0] = 0.03 + 0.03 * n; rgb[1] = 0.09 + 0.08 * n; rgb[2] = 0.03 + 0.02 * n;      /* plants */
    }
    if (v > 0.9) { rgb[0] = 0.05; rgb[1] = 0.055; rgb[2] = 0.07; }                      /* shade */
}

/* ---------------------------------------------------------------- the sensor */

static uint16_t *raw;

static double gauss()
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(6.283185307 * v);
}

static void expose(int portrait, double ev)
{
    raw = malloc((size_t) RAW_W * RAW_H * 2);
    double gain = 1400 * pow(2, ev);            /* mid grey ~ 3 stops under white */
    for (int y = 0; y < RAW_H; y++)
        for (int x = 0; x < RAW_W; x++)
        {
            double val = 0;
            /* as on the EOS M, light reaches 12 columns and rows outside
             * the picture area Canon names */
            if (x >= CROP_L - 12 * RAW_W / 5280 && y >= CROP_T - 12 * RAW_H / 3528)
            {
                double u = (x - CROP_L + 0.5) / CROP_W, v = (y - CROP_T + 0.5) / CROP_H;
                if (portrait)
                {
                    /* the camera on its side (orientation 6: the picture is
                     * the sensor turned a quarter clockwise) */
                    double t = u; u = 1 - v; v = t;
                }
                double lin[3], cam[3];
                scene(u, v, lin);
                for (int i = 0; i < 3; i++)
                {
                    cam[i] = 0;
                    for (int j = 0; j < 3; j++) cam[i] += cam_rgb[i][j] * lin[j];
                    cam[i] /= pre_mul[i];
                }
                int c = (y & 1) * 2 + (x & 1);        /* RGGB */
                double e = (c == 0 ? cam[0] : c == 3 ? cam[2] : cam[1]) / 0.18 * gain;
                if (e < 0) e = 0;
                val = e + sqrt(e + 1) * 0.6 * gauss();
            }
            double dn = BLACK + val + 6 * gauss();      /* read noise; borders: optical black */
            raw[(size_t) y * RAW_W + x] = (uint16_t) (dn < 0 ? 0 : dn > 16383 ? 16383 : dn > WHITE + 300 ? WHITE + 300 : dn);
        }
}

/* ---------------------------------------------------------------- lossless JPEG */

static unsigned char *out;
static size_t out_len, out_cap;

static void put8(int b)
{
    if (out_len == out_cap) out = realloc(out, out_cap = out_cap ? out_cap * 2 : 1 << 20);
    out[out_len++] = (unsigned char) b;
}

static uint64_t bitacc;
static int bitn;

static void putbits(unsigned v, int n)
{
    bitacc = (bitacc << n) | (v & ((1u << n) - 1));
    bitn += n;
    while (bitn >= 8)
    {
        int b = (int) (bitacc >> (bitn - 8)) & 0xFF;
        put8(b);
        if (b == 0xFF) put8(0);                 /* byte stuffing */
        bitn -= 8;
    }
}

static void flushbits()
{
    if (bitn) putbits(0x7F, 8 - bitn);          /* pad with ones */
}

struct huff { unsigned char bits[17], vals[17]; int nvals; unsigned code[17]; int len[17]; };

/* JPEG Annex K.2: code lengths from frequencies, at most 16 bits */
static void huff_build(struct huff *h, const long freq_in[17])
{
    long freq[18];
    int codesize[18] = { 0 }, others[18];
    for (int i = 0; i < 17; i++) freq[i] = freq_in[i];
    freq[17] = 1;                               /* reserved: no code is all ones */
    for (int i = 0; i < 18; i++) others[i] = -1;
    for (;;)
    {
        int v1 = -1, v2 = -1;
        for (int i = 0; i < 18; i++)
            if (freq[i] && (v1 < 0 || freq[i] <= freq[v1])) v1 = i;
        for (int i = 0; i < 18; i++)
            if (freq[i] && i != v1 && (v2 < 0 || freq[i] <= freq[v2])) v2 = i;
        if (v2 < 0) break;
        freq[v1] += freq[v2];
        freq[v2] = 0;
        codesize[v1]++;
        while (others[v1] >= 0) { v1 = others[v1]; codesize[v1]++; }
        others[v1] = v2;
        codesize[v2]++;
        while (others[v2] >= 0) { v2 = others[v2]; codesize[v2]++; }
    }
    int bits[40] = { 0 };
    for (int i = 0; i < 18; i++) if (codesize[i]) bits[codesize[i]]++;
    for (int i = 39; i > 16; i--)
        while (bits[i] > 0)
        {
            int j = i - 2;
            while (!bits[j]) j--;
            bits[i] -= 2; bits[i - 1]++; bits[j + 1] += 2; bits[j]--;
        }
    int i = 16;
    while (!bits[i]) i--;
    bits[i]--;                                  /* drop the reserved code */
    memset(h, 0, sizeof(*h));
    for (int l = 1; l <= 16; l++) h->bits[l] = (unsigned char) bits[l];
    /* symbols by code size, then value */
    int n = 0;
    for (int l = 1; l <= 32; l++)
        for (int s = 0; s < 17; s++)
            if (codesize[s] == l) h->vals[n++] = (unsigned char) s;
    h->nvals = n;
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++, code <<= 1)
        for (int c = 0; c < h->bits[l]; c++, k++)
        {
            h->code[h->vals[k]] = code++;
            h->len[h->vals[k]] = l;
        }
}

static int ssss(int d)
{
    int a = d < 0 ? -d : d, n = 0;
    while (a) { n++; a >>= 1; }
    return n;
}

/* the Huffman table of component c: LibRaw (and so Canon) take table c,
 * or the last one defined, whatever the scan header says */
#define TABLE(c) ((c) < 1 ? 0 : 1)

static void ljpeg_write(int clrs, const int slices[3])
{
    /* --fold: the frame twice as wide and half as high, as some cameras
     * store it (the samples run the same) */
    int jwide = fold ? 2 * RAW_W : RAW_W, wide = jwide / clrs, high = fold ? RAW_H / 2 : RAW_H;
    size_t n = (size_t) RAW_W * RAW_H;
    /* the samples in the order Canon stores them: slice by slice */
    uint16_t *seq = malloc(n * 2);
    size_t k = 0;
    for (int s = 0; s <= slices[0]; s++)
    {
        int x0 = s * slices[1], w = s < slices[0] ? slices[1] : slices[2];
        for (int y = 0; y < RAW_H; y++)
            for (int x = 0; x < w; x++)
                seq[k++] = raw[(size_t) y * RAW_W + x0 + x];
    }
    /* differences, as the decoder will predict them */
    int *diff = malloc(n * sizeof(int));
    int vpred[4];
    for (int c = 0; c < clrs; c++) vpred[c] = 1 << 13;
    long freq[4][17];
    memset(freq, 0, sizeof(freq));
    for (int r = 0; r < high; r++)
        for (int col = 0; col < wide; col++)
            for (int c = 0; c < clrs; c++)
            {
                size_t i = (size_t) r * jwide + col * clrs + c;
                int pred = col ? seq[i - clrs] : vpred[c];
                int d = (int) seq[i] - pred;
                if (!col) vpred[c] = seq[i];
                diff[i] = d;
                freq[TABLE(c)][ssss(d)]++;
            }
    struct huff h[2];
    huff_build(&h[0], freq[0]);
    huff_build(&h[1], freq[1]);
    int ntab = clrs > 1 ? 2 : 1;

    put8(0xFF); put8(0xD8);
    for (int t = 0; t < ntab; t++)
    {
        put8(0xFF); put8(0xC4);
        int len = 2 + 1 + 16 + h[t].nvals;
        put8(len >> 8); put8(len & 0xFF);
        put8(t);
        for (int l = 1; l <= 16; l++) put8(h[t].bits[l]);
        for (int v = 0; v < h[t].nvals; v++) put8(h[t].vals[v]);
    }
    put8(0xFF); put8(0xC3);
    int len = 8 + 3 * clrs;
    put8(len >> 8); put8(len & 0xFF);
    put8(14); put8(high >> 8); put8(high & 0xFF); put8(wide >> 8); put8(wide & 0xFF); put8(clrs);
    for (int c = 0; c < clrs; c++) { put8(c + 1); put8(0x11); put8(0); }
    put8(0xFF); put8(0xDA);
    len = 6 + 2 * clrs;
    put8(len >> 8); put8(len & 0xFF);
    put8(clrs);
    for (int c = 0; c < clrs; c++) { put8(c + 1); put8(TABLE(c) << 4); }
    put8(1); put8(0); put8(0);                  /* predictor 1, Pt 0 */
    for (size_t i = 0; i < n; i++)
    {
        int c = (int) (i % jwide) % clrs, d = diff[i], s = ssss(d);
        const struct huff *t = &h[ntab > 1 ? TABLE(c) : 0];
        putbits(t->code[s], t->len[s]);
        if (s && s < 16) putbits(d < 0 ? (unsigned) (d + (1 << s) - 1) : (unsigned) d, s);
    }
    flushbits();
    put8(0xFF); put8(0xD9);
    free(seq);
    free(diff);
}

/* ---------------------------------------------------------------- TIFF */

static unsigned char *tif;
static size_t tif_len;

static void w16(size_t at, int v) { tif[at] = v & 0xFF; tif[at + 1] = (v >> 8) & 0xFF; }
static void w32(size_t at, unsigned v) { for (int i = 0; i < 4; i++) tif[at + i] = (v >> (8 * i)) & 0xFF; }

struct entry { int tag, type; unsigned count, value; };

/* an IFD at `at`; returns its size */
static size_t ifd(size_t at, const struct entry *e, int n, unsigned next)
{
    w16(at, n);
    for (int i = 0; i < n; i++)
    {
        size_t p = at + 2 + 12 * i;
        w16(p, e[i].tag); w16(p + 2, e[i].type); w32(p + 4, e[i].count);
        if (e[i].type == 3 && e[i].count == 1) { w16(p + 8, e[i].value); w16(p + 10, 0); }
        else w32(p + 8, e[i].value);
    }
    w32(at + 2 + 12 * n, next);
    return 2 + 12 * (size_t) n + 4;
}

static size_t put_bytes(size_t at, const void *p, size_t n)
{
    memcpy(tif + at, p, n);
    return at + n;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: mkcr2 out.CR2 [--scene chart|street] [--small|--half] [--clrs 2|4] "
                        "[--slices N,W,LAST] [--portrait] [--ev x] [--raw out.u16] [--seed n] [--image photo.pfm]\n");
        return 1;
    }
    int clrs = 2, portrait = 0, slices[3] = { 2, 1760, 1760 };
    double ev = 0;
    const char *raw_out = 0;
    unsigned seed = 1;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--small"))
        {
            /* a quarter of the size in each direction, same layout */
            RAW_W = 1320; RAW_H = 882; CROP_L = 20; CROP_T = 16; CROP_W = 1296; CROP_H = 864;
            slices[0] = 2; slices[1] = 440; slices[2] = 440;
        }
        else if (!strcmp(argv[i], "--half"))
        {
            /* half the size each way: enough samples per screen pixel for every colour */
            RAW_W = 2640; RAW_H = 1764; CROP_L = 42; CROP_T = 32; CROP_W = 2592; CROP_H = 1728;
            slices[0] = 2; slices[1] = 880; slices[2] = 880;
        }
        else if (!strcmp(argv[i], "--sensor") && i + 1 < argc)
        {
            /* the raw frame and where the picture starts in it; three slices as wide as they can be */
            sscanf(argv[++i], "%d,%d,%d,%d", &RAW_W, &RAW_H, &CROP_L, &CROP_T);
            CROP_W = (RAW_W - CROP_L) & ~1; CROP_H = (RAW_H - CROP_T) & ~1;
            slices[0] = 2; slices[1] = (RAW_W / 3) & ~3; slices[2] = RAW_W - 2 * slices[1];
        }
        else if (!strcmp(argv[i], "--clrs") && i + 1 < argc) clrs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--slices") && i + 1 < argc)
            sscanf(argv[++i], "%d,%d,%d", &slices[0], &slices[1], &slices[2]);
        else if (!strcmp(argv[i], "--portrait")) portrait = 1;
        else if (!strcmp(argv[i], "--fold")) fold = 1;
        else if (!strcmp(argv[i], "--ev") && i + 1 < argc) ev = atof(argv[++i]);
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw_out = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene_kind = !strcmp(argv[++i], "street");
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) { if (!load_pfm(argv[++i])) return 1; }
    }
    if (slices[0] * slices[1] + slices[2] != RAW_W || RAW_W % clrs)
    {
        fprintf(stderr, "slices %d x %d + %d don't make %d columns\n", slices[0], slices[1], slices[2], RAW_W);
        return 1;
    }
    srand(seed);
    camera_init();
    expose(portrait, ev);
    ljpeg_write(clrs, slices);

    tif = calloc(1, out_len + 65536);
    /* header, the CR2 signature and where the raw IFD is */
    tif[0] = 'I'; tif[1] = 'I'; w16(2, 42); w32(4, 16);
    tif[8] = 'C'; tif[9] = 'R'; tif[10] = 2; tif[11] = 0;
    size_t ifd0 = 16, exif = 512, mnote = 1024, ifd3 = 2048, data = 4096;
    /* strings and arrays */
    size_t make = data, model = data + 16, date = data + 48, sinfo = data + 80, thumb = data + 128;
    put_bytes(make, "Canon", 6);
    put_bytes(model, "Canon EOS M", 12);
    put_bytes(date, "2026:10:22 17:45:00", 20);
    int si[17] = { 34, RAW_W, RAW_H, 1, 1, CROP_L, CROP_T, CROP_L + CROP_W - 1, CROP_T + CROP_H - 1,
                   0, 0, 0, 0, 0, 0, 0, 0 };    /* no black mask: the EOS M gives none */
    for (int i = 0; i < 17; i++) w16(sinfo + 2 * i, si[i]);
    static const unsigned char tiny_jpeg[] = { 0xFF, 0xD8, 0xFF, 0xD9 };
    put_bytes(thumb, tiny_jpeg, sizeof(tiny_jpeg));
    size_t rawdata = data + 256;
    put_bytes(rawdata, out, out_len);
    tif_len = rawdata + out_len;

    struct entry e0[] = {
        { 0x0100, 4, 1, CROP_W }, { 0x0101, 4, 1, CROP_H }, { 0x0102, 3, 1, 8 }, { 0x0103, 3, 1, 6 },
        { 0x010F, 2, 6, (unsigned) make }, { 0x0110, 2, 12, (unsigned) model },
        { 0x0111, 4, 1, (unsigned) thumb }, { 0x0112, 3, 1, portrait ? 6 : 1 },
        { 0x0117, 4, 1, sizeof(tiny_jpeg) }, { 0x0132, 2, 20, (unsigned) date }, { 0x8769, 4, 1, (unsigned) exif },
    };
    ifd(ifd0, e0, sizeof(e0) / sizeof(e0[0]), (unsigned) ifd3);
    struct entry ex[] = {
        { 0x8827, 3, 1, 400 }, { 0x9003, 2, 20, (unsigned) date }, { 0x927C, 7, 2 + 12 * 2 + 4, (unsigned) mnote },
    };
    ifd(exif, ex, sizeof(ex) / sizeof(ex[0]), 0);
    struct entry mn[] = { { 0x0010, 4, 1, 0x80000331 }, { 0x00E0, 3, 17, (unsigned) sinfo } };
    ifd(mnote, mn, 2, 0);
    w32(12, (unsigned) ifd3);
    struct entry e3[] = {
        { 0x0103, 3, 1, 6 }, { 0x0111, 4, 1, (unsigned) rawdata }, { 0x0117, 4, 1, (unsigned) out_len },
        { 0xC5D8, 4, 1, 1 }, { 0xC5E0, 4, 1, 1 }, { 0xC640, 3, 3, (unsigned) (sinfo + 64) },
    };
    w16(sinfo + 64, slices[0]); w16(sinfo + 66, slices[1]); w16(sinfo + 68, slices[2]);
    ifd(ifd3, e3, sizeof(e3) / sizeof(e3[0]), 0);

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    fwrite(tif, 1, tif_len, f);
    fclose(f);
    if (raw_out)
    {
        f = fopen(raw_out, "wb");
        fwrite(raw, 2, (size_t) RAW_W * RAW_H, f);
        fclose(f);
    }
    fprintf(stderr, "%s: %dx%d raw, %d components, slices %d x %d + %d, %.1f MB\n", argv[1], RAW_W, RAW_H,
            clrs, slices[0], slices[1], slices[2], tif_len / 1048576.0);
    return 0;
}
