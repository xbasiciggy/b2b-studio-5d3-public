/* fontblob: the film OS fonts (fos_font.c) as the file the camera loads at
 * start, ML/DATA/FOSFONT.BIN (format: fos_fonts_load in fos_gfx.c). They
 * stay out of autoexec.bin, whose every byte the EOS M takes from Canon's
 * own memory at boot.
 *
 *   cc -I.. -o fontblob fontblob.c && ./fontblob FOSFONT.BIN
 */

#include <stdio.h>
#include <string.h>
#include "../fos_font.c"

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) };
    fwrite(b, 1, 4, f);
}

static void pad(FILE *f, long n)
{
    static const uint8_t z[4];
    fwrite(z, 1, (size_t) ((4 - n % 4) % 4), f);
}

static void font(FILE *f, const struct fos_font *ft, const uint8_t *bits, uint32_t nbits, uint32_t nglyphs)
{
    put32(f, nbits);
    put32(f, nglyphs);
    put32(f, (uint32_t) ft->nkern);
    put32(f, (uint32_t) ft->ascent);
    put32(f, (uint32_t) ft->descent);
    fwrite(bits, 1, nbits, f);
    pad(f, nbits);
    for (uint32_t i = 0; i < nglyphs; i++)
    {
        const struct fos_glyph *g = &ft->glyphs[i];
        uint8_t b[8] = { (uint8_t) g->off, (uint8_t) (g->off >> 8), g->w, g->h, (uint8_t) g->ox, (uint8_t) g->oy,
                         (uint8_t) g->adv, (uint8_t) (g->adv >> 8) };
        fwrite(b, 1, 8, f);
    }
    fwrite(ft->lut, 1, 256, f);
    for (int i = 0; i < ft->nkern; i++)
    {
        uint8_t b[3] = { ft->kern[i].a, ft->kern[i].b, (uint8_t) ft->kern[i].k };
        fwrite(b, 1, 3, f);
    }
    pad(f, (long) ft->nkern * 3);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: fontblob FOSFONT.BIN\n"); return 2; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    fwrite("FOSF", 1, 4, f);
    put32(f, 1);
#define COUNT(n) + 1
    put32(f, 0 FOS_FONTS(COUNT));
#define FONT(n) font(f, &fos_font_##n##_data, n##_bits, sizeof(n##_bits), sizeof(n##_glyphs) / sizeof(n##_glyphs[0]));
    FOS_FONTS(FONT)
    long size = ftell(f);
    fclose(f);
    printf("%s: %ld bytes\n", argv[1], size);
    return 0;
}
