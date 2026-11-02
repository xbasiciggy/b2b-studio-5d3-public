/* fos_gfx: drawing for the film OS.
 *
 * The screen is the EOS M 8-bit bitmap overlay. Our palette is a set of
 * tone ramps, each running from near-black through a hue to a light tint,
 * so every colour is "a ramp and a level along it". Shading, gradients,
 * fades and anti-aliasing all become level changes, and ordered dithering
 * between neighbouring ramp entries keeps them smooth with 254 colours.
 *
 * Plain C, no ML headers: the same code runs on the camera and in the
 * simulator (sim/).
 */

#ifndef _fos_gfx_h_
#define _fos_gfx_h_

#include <stdint.h>

/* the camera build warns (and fails) when gcc declines an inline at -Os,
 * so the few hot helpers are forced */
#define FOS_INLINE static inline __attribute__((always_inline))

#define FOS_W 720
#define FOS_H 480

enum
{
    RAMP_GRAY,          /* warm neutral, also black and white ink */
    RAMP_METAL,         /* cool steel for canister caps */
    RAMP_AMBER,         /* UI accent and Kodak yellow */
    RAMP_RED,
    RAMP_ORANGE,
    RAMP_CRIMSON,
    RAMP_GREEN,
    RAMP_VIOLET,
    RAMP_BLUE,
    RAMP_LEADER,        /* unprocessed film base */
    RAMP_DOTS,          /* the dot-matrix background: 32 entries whose colours cycle (fos_gfx_dots_phase) */
    RAMP_COUNT
};

/* a colour: ramp in the high byte, level 0..255 in the low byte */
typedef uint16_t fcol;
#define FC(ramp, level)     ((fcol) (((ramp) << 8) | (level)))
#define FC_RAMP(c)          ((c) >> 8)
#define FC_LEVEL(c)         ((c) & 0xFF)

/* shading works with 12-bit levels */
#define LV_MAX  4095
#define LV12(level8)        ((level8) * 4095 / 255)

/* opacity 0..256 */
#define OPAQUE  256

/* palette index 0 is Canon's transparent entry: LiveView shows through */
#define IX_CLEAR 0

struct fos_canvas
{
    uint8_t *px;
    int pitch;
    int w, h;
    int cx0, cy0, cx1, cy1;     /* clip rectangle, cx1 / cy1 exclusive */
};

struct fos_glyph { uint16_t off; uint8_t w, h; int8_t ox, oy; uint16_t adv; };
struct fos_kern  { uint8_t a, b; int8_t k; };

struct fos_font
{
    const uint8_t *bits;            /* run-length coded 4-bit coverage */
    const struct fos_glyph *glyphs;
    const uint8_t *lut;             /* byte -> glyph, 0xFF = none */
    const struct fos_kern *kern;    /* sorted by (a, b), 1/16 px */
    int nkern;
    int ascent, descent;
};

/* The fonts live in a file on the card (ML/DATA/FOSFONT.BIN, made from
 * fos_font.c by tools/fontblob.c), not in autoexec.bin: on the EOS M every
 * byte of autoexec.bin is taken from Canon's own memory at boot, and too
 * much stops the camera from starting. Until fos_fonts_load succeeds,
 * text draws nothing. */
extern struct fos_font fos_font_xs, fos_font_sm, fos_font_md, fos_font_mdb,
                       fos_font_lg, fos_font_xl, fos_font_num,
                       fos_font_lbig, fos_font_lname, fos_font_lsm;
#define FOS_FONT_FILE "FOSFONT.BIN"
#define FOS_FONTS(X) X(xs) X(sm) X(md) X(mdb) X(lg) X(xl) X(num) X(lbig) X(lname) X(lsm)   /* the file's order */
/* blob: the file's contents, kept for as long as the fonts are used;
 * 1 when it holds every font */
int fos_fonts_load(const uint8_t *blob, int size);
int fos_fonts_ready();

/* characters beyond ASCII in our strings (see tools/mkfont.py) */
#define T_DOT       "\x80"      /* middle dot */
#define T_DEG       "\x81"
#define T_TIMES     "\x82"
#define T_NDASH     "\x83"
#define T_MDASH     "\x84"
#define T_APOS      "\x85"
#define T_ELLIPSIS  "\x86"
#define T_BULLET    "\x87"
#define T_LSAQUO    "\x88"      /* single angle quotes */
#define T_RSAQUO    "\x89"

/* 0x00RRGGBB for each palette index we own (1..254) */
extern uint32_t fos_pal[256];

/* What the overlay gets for palette index ix: Y << 16 | U << 8 | V, U and
 * V signed bytes. std 0 is Canon's own conversion (BT.601 Y'CbCr, scaled
 * so white sits at white_y, the luma of Canon's white: 235 on the EOS M);
 * std 1 is full-range Rec.709. sat, warm and bright are the screen colour
 * settings, -6..6, 0 = as designed. */
uint32_t fos_screen_yuv(int ix, int white_y, int std, int sat, int warm, int bright);
uint32_t fos_screen_yuv_rgb(uint32_t rgb, int white_y, int std, int sat, int warm, int bright);   /* any 0xRRGGBB */

/* back to 0xRRGGBB, as a screen decoding Canon's way would show it */
uint32_t fos_screen_rgb(uint32_t yuv, int white_y);

void fos_gfx_init();

/* palette index of a flat colour */
uint8_t fos_ix(fcol c);

/* palette index for a 12-bit level, dithered for pixel (x, y) */
uint8_t fos_ix_dither(int ramp, int lv, int x, int y);

/* the same, inline, for inner loops */
extern uint8_t fos_ramp_base[RAMP_COUNT];
extern uint8_t fos_ramp_last[RAMP_COUNT];     /* entries - 1 */
extern int fos_ramp_mul[RAMP_COUNT];          /* lv * mul >> 16 = position in 1/16 entries */
extern const uint8_t fos_bayer[4][4];

FOS_INLINE int fos_ramp_pos(int ramp, int lv)
{
    return ((lv < 0 ? 0 : lv) * fos_ramp_mul[ramp]) >> 16;
}

FOS_INLINE uint8_t fos_ixd(int ramp, int pos16, int x, int y)
{
    int p = (pos16 + fos_bayer[y & 3][x & 3]) >> 4;
    return fos_ramp_base[ramp] + (p > fos_ramp_last[ramp] ? fos_ramp_last[ramp] : p);
}

/* the colour behind a palette index, and its luminance */
fcol fos_colour_of(uint8_t ix);
int  fos_luma(uint8_t ix);

/* the same as tables, for inner loops */
extern uint8_t  fos_rev_ramp[256];
extern uint16_t fos_rev_lv[256];              /* 12-bit */
extern uint8_t  fos_luma_tab[256];
extern uint8_t  fos_chroma_tab[256];
extern uint16_t fos_lum2lv[RAMP_COUNT][256];  /* luminance -> level, per ramp */

/* a pixel at k/256 of its brightness, dithered again */
FOS_INLINE uint8_t fos_darken(uint8_t ix, int k, int x, int y)
{
    int r = fos_rev_ramp[ix];
    return fos_ixd(r, fos_ramp_pos(r, (fos_rev_lv[ix] * k) >> 8), x, y);
}

/* dst with k/256 of src glazed over it (reflections): luminance blends,
 * the ramp is whichever brings more colour */
FOS_INLINE uint8_t fos_glaze(uint8_t dst, uint8_t src, int k, int x, int y)
{
    int ld = fos_luma_tab[dst];
    int l = ld + (((fos_luma_tab[src] - ld) * k) >> 8);
    int r = fos_chroma_tab[src] * k >= fos_chroma_tab[dst] * (256 - k) ? fos_rev_ramp[src] : fos_rev_ramp[dst];
    return fos_ixd(r, fos_ramp_pos(r, fos_lum2lv[r][l]), x, y);
}

/* blend a 12-bit level of a ramp over a pixel (opacity 0..256) */
uint8_t fos_mix(uint8_t dst, int ramp, int lv, int alpha);
uint8_t fos_mix_dither(uint8_t dst, int ramp, int lv, int alpha, int x, int y);

/* the colour a of the way from (ra, la) to (rb, lb), as a ramp and level */
void fos_lerp2(int ra, int la, int rb, int lb, int a, int *ramp, int *lv);

/* canvas */
void fos_canvas_init(struct fos_canvas *cv, uint8_t *px, int pitch, int w, int h);
void fos_clip(struct fos_canvas *cv, int x0, int y0, int x1, int y1);
void fos_noclip(struct fos_canvas *cv);

void fos_fill(struct fos_canvas *cv, int x, int y, int w, int h, fcol c);
void fos_fill_ix(struct fos_canvas *cv, int x, int y, int w, int h, uint8_t ix);
void fos_blend(struct fos_canvas *cv, int x, int y, int w, int h, fcol c, int alpha);
void fos_scale_levels(struct fos_canvas *cv, int x, int y, int w, int h, int k);   /* k/256 */
void fos_vgrad(struct fos_canvas *cv, int x, int y, int w, int h, int ramp, int lv0, int lv1);

/* anti-aliased shapes; coordinates in 1/256 px where named *8 */
void fos_rrect(struct fos_canvas *cv, int x, int y, int w, int h, int r, fcol c, int alpha);
void fos_rrect_line(struct fos_canvas *cv, int x, int y, int w, int h, int r, int t, fcol c, int alpha);
void fos_disc(struct fos_canvas *cv, int cx8, int cy8, int r8, fcol c, int alpha);
void fos_ring(struct fos_canvas *cv, int cx8, int cy8, int r8, int t8, fcol c, int alpha);
void fos_line(struct fos_canvas *cv, int x0_8, int y0_8, int x1_8, int y1_8, int w8, fcol c, int alpha);
void fos_convex(struct fos_canvas *cv, const int *xy8, int n, fcol c, int alpha);
void fos_tri(struct fos_canvas *cv, int cx, int cy, int size, int dir, fcol c, int alpha);   /* dir 0 right, 1 down, 2 left, 3 up */

/* text: y is the baseline; track is extra spacing in 1/16 px */
int fos_text(struct fos_canvas *cv, const struct fos_font *f, int x, int y, const char *s, fcol c, int alpha, int track);
int fos_text_w(const struct fos_font *f, const char *s, int track);
int fos_text_c(struct fos_canvas *cv, const struct fos_font *f, int cx, int y, const char *s, fcol c, int alpha, int track);
int fos_text_r(struct fos_canvas *cv, const struct fos_font *f, int rx, int y, const char *s, fcol c, int alpha, int track);

/* text onto any 8-bit buffer with someone else's palette: map[coverage]
 * (1..15) is the index to write, 0 leaves the pixel alone */
int fos_text_map(uint8_t *px, int pitch, int w, int h, const struct fos_font *f, int x, int y,
                 const char *s, const uint8_t map[16], int track);

/* small integer helpers */
int  fos_isqrt(unsigned int v);
void fos_gfx_dots_theme(int contour);      /* 0 dots (blue-violet-red-amber loop), 1 contour (red-green-blue loop) */
void fos_gfx_dots_phase(int phase);       /* rotate the colours of RAMP_DOTS; push them with fos_port_palette_part */
int  fos_sin(int a);    /* a: 0..65535 = one turn; result -4096..4096 */
int  fos_cos(int a);

FOS_INLINE int fos_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
