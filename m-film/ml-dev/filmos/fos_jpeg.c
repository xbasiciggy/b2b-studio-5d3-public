/* fos_jpeg: a small baseline JPEG writer for the developed frames - 4:2:0,
 * the standard Huffman tables, integer DCT (the camera has no FPU), and
 * EXIF with the date, the film and the f-stop.
 */

#pragma GCC optimize ("O2")   /* hot pixel loops: the camera build is -Os */
#include "fos_jpeg.h"
#include "fos.h"
#include <string.h>

/* ---------------------------------------------------------------- tables */

static const uint8_t zigzag[64] =
{
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* the example tables of the standard (K.1, K.2), in natural order */
static const uint8_t std_q[2][64] =
{
    { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
      14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
      49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 },
    { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
      47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
      99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 },
};

/* the standard's Huffman tables (K.3): code counts by length, then values */
static const uint8_t dc_bits[2][16] = { { 0, 1, 5, 1, 1, 1, 1, 1, 1 }, { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1 } };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_bits[2][16] =
{
    { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d },
    { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },
};
static const uint8_t ac_vals[2][162] =
{
    { 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
      0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
      0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
      0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
      0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
      0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
      0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
      0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
      0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
      0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
      0xf9, 0xfa },
    { 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
      0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
      0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
      0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
      0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
      0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
      0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
      0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
      0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
      0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
      0xf9, 0xfa },
};

uintptr_t fos_jpeg_state_addr;          /* where the encoder's state lives, for the log */
/* ordinary cached heap if the port has one; tools that build this file alone fall back to the normal allocator */
__attribute__((weak)) void *fos_port_alloc_hot(int size) { return fos_port_alloc(size); }

/* ---------------------------------------------------------------- output */

#ifndef FOS_JPEG_SLOW
#define FOS_JPEG_SLOW 0         /* 1 builds without the whole-block fast path, the reference for tests/jpegcheck.py */
#endif

struct jw
{
    void *f;
    int width, height, row, dc[3];
    uint8_t buf[32768];
    int n, ok;
    uint32_t bits;
    int nbits;
    uint16_t dc_code[2][12], ac_code[2][256];
    uint8_t dc_len[2][12], ac_len[2][256];
    int32_t recip[2][64];       /* 65536 / (8 q), zigzag order */
    uint8_t q[2][64];           /* zigzag order, as written */
};

static void flush(struct jw *w)
{
    if (w->n && fos_port_write(w->f, w->buf, w->n) != w->n) w->ok = 0;
    w->n = 0;
}

static void put8(struct jw *w, int b)
{
    if (w->n == (int) sizeof(w->buf)) flush(w);
    w->buf[w->n++] = (uint8_t) b;
}

static void put16(struct jw *w, int v) { put8(w, v >> 8); put8(w, v); }

static void put_bits(struct jw *w, uint32_t code, int len)
{
    w->bits = w->bits << len | (code & ((1u << len) - 1));
    w->nbits += len;
    if (w->n > (int) sizeof(w->buf) - 16) flush(w);       /* room for the up to 8 bytes (with stuffing) this can write */
    uint8_t *out = w->buf + w->n;
    while (w->nbits >= 8)
    {
        int b = (int) (w->bits >> (w->nbits - 8)) & 0xFF;
        *out++ = (uint8_t) b;
        if (b == 0xFF) *out++ = 0;          /* stuffed */
        w->nbits -= 8;
    }
    w->n = (int) (out - w->buf);
}

static void huff_codes(const uint8_t *bits, const uint8_t *vals, uint16_t *code, uint8_t *len)
{
    int c = 0, k = 0;
    for (int l = 1; l <= 16; l++)
    {
        for (int i = 0; i < bits[l - 1]; i++, k++)
        {
            code[vals[k]] = (uint16_t) c++;
            len[vals[k]] = (uint8_t) l;
        }
        c <<= 1;
    }
}

/* ---------------------------------------------------------------- one block */

/* forward DCT, integer (libjpeg's islow, its constants in 1/8192): the
 * output 8 times too large */
enum { C0298 = 2446, C0390 = 3196, C0541 = 4433, C0765 = 6270, C0899 = 7373, C1175 = 9633,
       C1501 = 12299, C1847 = 15137, C1961 = 16069, C2053 = 16819, C2562 = 20995, C3072 = 25172 };
static void fdct(int32_t *d)
{
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < 8; i++)
        {
            int32_t *p = pass ? d + i : d + i * 8;
            const int s = pass ? 8 : 1;
            int32_t t0 = p[0] + p[7 * s], t7 = p[0] - p[7 * s], t1 = p[s] + p[6 * s], t6 = p[s] - p[6 * s];
            int32_t t2 = p[2 * s] + p[5 * s], t5 = p[2 * s] - p[5 * s], t3 = p[3 * s] + p[4 * s], t4 = p[3 * s] - p[4 * s];
            int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
            const int sh = pass ? 15 : 11;
            if (pass) { p[0] = (t10 + t11 + 2) >> 2; p[4 * s] = (t10 - t11 + 2) >> 2; }
            else      { p[0] = (t10 + t11) * 4;      p[4 * s] = (t10 - t11) * 4; }
            int32_t z1 = (t12 + t13) * C0541;
            p[2 * s] = (z1 + t13 * C0765 + (1 << (sh - 1))) >> sh;
            p[6 * s] = (z1 - t12 * C1847 + (1 << (sh - 1))) >> sh;
            int32_t a = t4 + t7, b = t5 + t6, c = t4 + t6, e = t5 + t7;
            int32_t z5 = (c + e) * C1175;
            t4 *= C0298; t5 *= C2053; t6 *= C3072; t7 *= C1501;
            a *= -C0899; b *= -C2562; c = c * -C1961 + z5; e = e * -C0390 + z5;
            p[7 * s] = (t4 + a + c + (1 << (sh - 1))) >> sh;
            p[5 * s] = (t5 + b + e + (1 << (sh - 1))) >> sh;
            p[3 * s] = (t6 + b + c + (1 << (sh - 1))) >> sh;
            p[s]     = (t7 + a + e + (1 << (sh - 1))) >> sh;
        }
    }
}

static int bit_size(int v)
{
    if (v < 0) v = -v;
    return v ? 32 - __builtin_clz((unsigned) v) : 0;    /* one instruction on the camera's ARMv5 */
}

/* a block of samples (level-shifted) to the file; t: 0 luma, 1 chroma */
static void block(struct jw *w, int32_t *d, int t, int *dc_prev)
{
    fdct(d);
    int q[64];
    for (int i = 0; i < 64; i++)
    {
        int32_t v = d[zigzag[i]], r = w->recip[t][i];
        q[i] = v >= 0 ? (int) ((v * r + 32768) >> 16) : -(int) ((-v * r + 32768) >> 16);
    }
    int diff = q[0] - *dc_prev, n = bit_size(diff);
    *dc_prev = q[0];
    put_bits(w, w->dc_code[t][n], w->dc_len[t][n]);
    if (n) put_bits(w, (uint32_t) (diff < 0 ? diff - 1 : diff), n);
    int run = 0;
    for (int i = 1; i < 64; i++)
    {
        if (!q[i]) { run++; continue; }
        while (run > 15) { put_bits(w, w->ac_code[t][0xF0], w->ac_len[t][0xF0]); run -= 16; }
        int s = bit_size(q[i]), sym = run << 4 | s;
        put_bits(w, w->ac_code[t][sym], w->ac_len[t][sym]);
        put_bits(w, (uint32_t) (q[i] < 0 ? q[i] - 1 : q[i]), s);
        run = 0;
    }
    if (run) put_bits(w, w->ac_code[t][0], w->ac_len[t][0]);
}

/* ---------------------------------------------------------------- EXIF */

/* a tiny TIFF: IFD0 (description, make, model, software, date) and the
 * EXIF IFD (date taken, f-number) */
static int exif(uint8_t *out, const struct fos_jpeg_meta *m)
{
    char date[20] = "";
    if (m->when && strlen(m->when) >= 16)
    {
        /* "2026-10-22 17:45" -> "2026:10:22 17:45:00" */
        memcpy(date, m->when, 16);
        date[4] = date[7] = ':';
        memcpy(date + 16, ":00", 4);
    }
    int fnum = 0;                               /* x10 */
    if (m->fstop && *m->fstop)
    {
        const char *s = m->fstop;
        while (*s >= '0' && *s <= '9') fnum = fnum * 10 + (*s++ - '0');
        fnum *= 10;
        if (*s == '.' && s[1] >= '0' && s[1] <= '9') fnum += s[1] - '0';
    }
    struct { int tag, type; const char *s; } ifd0[] =
    {
        { 0x010E, 2, m->desc }, { 0x010F, 2, "Canon" }, { 0x0110, 2, fos_port_model() },
        { 0x0131, 2, "M-FILM darkroom" }, { 0x0132, 2, date },
    };
    const int n0 = 6, n1 = 1 + (fnum > 0);
    int p = 8, data = 8 + 2 + n0 * 12 + 4 + 2 + n1 * 12 + 4;
    memcpy(out, "II*\0\x08\0\0\0", 8);
#define W16(v) (out[p] = (uint8_t) (v), out[p + 1] = (uint8_t) ((v) >> 8), p += 2)
#define W32(v) (W16((v) & 0xFFFF), W16((uint32_t) (v) >> 16))
    W16(n0);
    for (int i = 0; i < n0 - 1; i++)
    {
        const char *s = ifd0[i].s && *ifd0[i].s ? ifd0[i].s : " ";
        int len = (int) strlen(s) + 1;
        W16(ifd0[i].tag); W16(2); W32(len);
        if (len <= 4) { memset(out + p, 0, 4); memcpy(out + p, s, len); p += 4; }
        else { W32(data); memcpy(out + data, s, len); data += (len + 1) & ~1; }
    }
    int exif_at = 8 + 2 + n0 * 12 + 4;
    W16(0x8769); W16(4); W32(1); W32(exif_at);
    W32(0);
    W16(n1);                                    /* tags in ascending order */
    if (fnum > 0)
    {
        W16(0x829D); W16(5); W32(1); W32(data);
        int q = data;
        out[q] = (uint8_t) fnum; out[q + 1] = (uint8_t) (fnum >> 8); out[q + 2] = out[q + 3] = 0;
        out[q + 4] = 10; out[q + 5] = out[q + 6] = out[q + 7] = 0;
        data += 8;
    }
    W16(0x9003); W16(2); W32(20); W32(data);
    memcpy(out + data, date[0] ? date : "                   ", 20);
    data += 20;
    W32(0);
#undef W16
#undef W32
    return data;
}

/* ---------------------------------------------------------------- the file */

void *fos_jpeg_begin(const char *path, int width, int height,
                   int quality, const struct fos_jpeg_meta *meta)
{
    struct jw *w = fos_port_alloc_hot(sizeof(struct jw));
    fos_jpeg_state_addr = (uintptr_t) w;   /* its tables and 32 KB buffer are read for every coefficient: cached heap */
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->ok = 1;
    w->width = width; w->height = height;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int t = 0; t < 2; t++)
    {
        for (int i = 0; i < 64; i++)
        {
            int v = (std_q[t][zigzag[i]] * scale + 50) / 100;
            v = v < 1 ? 1 : v > 255 ? 255 : v;
            w->q[t][i] = (uint8_t) v;
            w->recip[t][i] = 65536 / (8 * v);
        }
        huff_codes(dc_bits[t], dc_vals, w->dc_code[t], w->dc_len[t]);
        huff_codes(ac_bits[t], ac_vals[t], w->ac_code[t], w->ac_len[t]);
    }
    w->f = fos_port_open(path, 1);
    if (!w->f) { fos_port_free(w); return 0; }

    put16(w, 0xFFD8);
    /* JFIF */
    put16(w, 0xFFE0); put16(w, 16);
    static const uint8_t jfif[14] = { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    for (int i = 0; i < 14; i++) put8(w, jfif[i]);
    /* EXIF */
    if (meta && meta->exif_blob && meta->exif_length > 0 && meta->exif_length <= 65533)
    {
        put16(w, 0xFFE1); put16(w, meta->exif_length + 2);
        for (int i=0;i<meta->exif_length;i++) put8(w,meta->exif_blob[i]);
    }
    else if (meta)
    {
        uint8_t ex[512];
        memset(ex, 0, sizeof(ex));
        int n = exif(ex, meta);
        put16(w, 0xFFE1); put16(w, n + 8);
        static const uint8_t tag[6] = { 'E', 'x', 'i', 'f', 0, 0 };
        for (int i = 0; i < 6; i++) put8(w, tag[i]);
        for (int i = 0; i < n; i++) put8(w, ex[i]);
    }
    /* quantisation, frame, Huffman tables */
    for (int t = 0; t < 2; t++)
    {
        put16(w, 0xFFDB); put16(w, 67); put8(w, t);
        for (int i = 0; i < 64; i++) put8(w, w->q[t][i]);
    }
    put16(w, 0xFFC0); put16(w, 17); put8(w, 8); put16(w, height); put16(w, width); put8(w, 3);
    put8(w, 1); put8(w, 0x22); put8(w, 0);
    put8(w, 2); put8(w, 0x11); put8(w, 1);
    put8(w, 3); put8(w, 0x11); put8(w, 1);
    for (int t = 0; t < 2; t++)
    {
        int nd = 0, na = 0;
        for (int i = 0; i < 16; i++) { nd += dc_bits[t][i]; na += ac_bits[t][i]; }
        put16(w, 0xFFC4); put16(w, 2 + 17 + nd + 17 + na);
        put8(w, t);
        for (int i = 0; i < 16; i++) put8(w, dc_bits[t][i]);
        for (int i = 0; i < nd; i++) put8(w, dc_vals[i]);
        put8(w, 0x10 | t);
        for (int i = 0; i < 16; i++) put8(w, ac_bits[t][i]);
        for (int i = 0; i < na; i++) put8(w, ac_vals[t][i]);
    }
    put16(w, 0xFFDA); put16(w, 12); put8(w, 3);
    put8(w, 1); put8(w, 0x00); put8(w, 2); put8(w, 0x11); put8(w, 3); put8(w, 0x11);
    put8(w, 0); put8(w, 63); put8(w, 0);

    return w;
}

int fos_jpeg_rows(void *writer, const uint8_t *rgb, int stride, int height)
{
    struct jw *w = writer;
    if (!w || height < 1 || height > 16 || w->row + height > w->height) return 0;
    int width = w->width;
    int *dc = w->dc;
    int32_t blk[64], cb[64], cr[64];
    int32_t yb[4][64];
    for (int my = 0; my < height; my += 16)
        for (int mx = 0; mx < width; mx += 16)
        {
            if (!FOS_JPEG_SLOW && height - my >= 16 && width - mx >= 16)
            {
                /* a whole 16 x 16 block of the picture: no edge clamping, and chroma from the sums of each 2 x 2 (the same
                 * integers as adding the four pixels' terms, with a quarter of the multiplies) */
                for (int y2 = 0; y2 < 8; y2++)
                {
                    const uint8_t *r0 = rgb + (my + 2 * y2) * stride + 3 * mx, *r1 = r0 + stride;
                    int32_t *yrow0 = &yb[(y2 >> 2) * 2][((2 * y2) & 7) * 8], *yrow1 = yrow0 + 8;
                    int32_t *yrow0r = &yb[(y2 >> 2) * 2 + 1][((2 * y2) & 7) * 8], *yrow1r = yrow0r + 8;
                    for (int x2 = 0; x2 < 8; x2++, r0 += 6, r1 += 6)
                    {
                        int32_t *a0 = x2 < 4 ? yrow0 : yrow0r, *a1 = x2 < 4 ? yrow1 : yrow1r;
                        const int xo = (x2 & 3) * 2;
                        int r, g, bl, rs, gs, bs;
                        r = r0[0]; g = r0[1]; bl = r0[2]; rs = r; gs = g; bs = bl;
                        a0[xo] = ((19595 * r + 38470 * g + 7471 * bl + 32768) >> 16) - 128;
                        r = r0[3]; g = r0[4]; bl = r0[5]; rs += r; gs += g; bs += bl;
                        a0[xo + 1] = ((19595 * r + 38470 * g + 7471 * bl + 32768) >> 16) - 128;
                        r = r1[0]; g = r1[1]; bl = r1[2]; rs += r; gs += g; bs += bl;
                        a1[xo] = ((19595 * r + 38470 * g + 7471 * bl + 32768) >> 16) - 128;
                        r = r1[3]; g = r1[4]; bl = r1[5]; rs += r; gs += g; bs += bl;
                        a1[xo + 1] = ((19595 * r + 38470 * g + 7471 * bl + 32768) >> 16) - 128;
                        cb[y2 * 8 + x2] = (-11059 * rs - 21709 * gs + 32768 * bs + 131072) >> 18;
                        cr[y2 * 8 + x2] = (32768 * rs - 27439 * gs - 5329 * bs + 131072) >> 18;
                    }
                }
                for (int b = 0; b < 4; b++) block(w, yb[b], 0, &dc[0]);
                block(w, cb, 1, &dc[1]);
                block(w, cr, 1, &dc[2]);
                continue;
            }
            memset(cb, 0, sizeof(cb));
            memset(cr, 0, sizeof(cr));
            for (int b = 0; b < 4; b++)
            {
                int bx = mx + (b & 1) * 8, by = my + (b >> 1) * 8;
                for (int y = 0; y < 8; y++)
                {
                    const uint8_t *row = rgb + (by + y < height ? by + y : height - 1) * stride;
                    for (int x = 0; x < 8; x++)
                    {
                        const uint8_t *px = row + 3 * (bx + x < width ? bx + x : width - 1);
                        int r = px[0], g = px[1], bl = px[2];
                        blk[y * 8 + x] = ((19595 * r + 38470 * g + 7471 * bl + 32768) >> 16) - 128;
                        int k = ((b >> 1) * 4 + (y >> 1)) * 8 + (b & 1) * 4 + (x >> 1);
                        cb[k] += -11059 * r - 21709 * g + 32768 * bl;
                        cr[k] += 32768 * r - 27439 * g - 5329 * bl;
                    }
                }
                block(w, blk, 0, &dc[0]);
            }
            for (int i = 0; i < 64; i++) { cb[i] = (cb[i] + 131072) >> 18; cr[i] = (cr[i] + 131072) >> 18; }
            block(w, cb, 1, &dc[1]);
            block(w, cr, 1, &dc[2]);
        }
    w->row += height;
    return w->ok;
}

int fos_jpeg_end(void *writer)
{
    struct jw *w = writer;
    if (!w) return 0;
    if (w->row != w->height) w->ok = 0;
    if (w->nbits) put_bits(w, 0x7F, 8 - w->nbits);     /* pad with ones */
    put16(w, 0xFFD9);
    flush(w);
    fos_port_close(w->f);
    int ok = w->ok;
    fos_port_free(w);
    return ok;
}

int fos_jpeg_write(const char *path, const uint8_t *rgb, int stride, int width, int height,
                   int quality, const struct fos_jpeg_meta *meta)
{
    void *w = fos_jpeg_begin(path, width, height, quality, meta);
    if (!w) return 0;
    for (int y = 0; y < height; y += 16)
        if (!fos_jpeg_rows(w, rgb + y * stride, stride, height-y < 16 ? height-y : 16)) break;
    return fos_jpeg_end(w);
}
