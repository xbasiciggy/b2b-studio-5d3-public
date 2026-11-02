#ifndef FOS_PREVIEW_H
#define FOS_PREVIEW_H
#include <stdint.h>
/* A review picture of up to 720x480: indices into a palette of up to 240 colours
 * chosen for this picture (BVP3). Rows arrive from the decoder in order; they are
 * area-averaged down to the review size, then reduced to the palette with error
 * diffusion when the cache is saved. Without enough memory for that, the old fixed
 * 6x6x6 cube and ordered dither are used instead (the palette is stored either way).
 * The first five members keep their original order: callers fill them positionally. */
#define FOS_PREVIEW_COLOURS 240
struct fos_preview {
    uint8_t *pixels; int w, h, sw, sh;
    int npal;                           /* palette entries in use; pixel values are 1..npal */
    uint8_t pal[FOS_PREVIEW_COLOURS][3];
    uint8_t *rgb;                       /* full-colour work picture, w x h, until quantised */
    uint32_t *acc;                      /* one output row's sums */
    uint16_t *colmap, *colcnt;          /* source column -> output column; source columns per output column */
    int cur, nrows;                     /* output row being summed, source rows in it so far */
    uint32_t taken;                     /* capture time from the EXIF (see fos_exif_taken), 0 = unknown */
    int early;                          /* a smaller first review, to be replaced by the bake's full-size one */
};
int fos_preview_init(struct fos_preview *, int, int);
void fos_preview_rows(struct fos_preview *, const uint8_t *, int, int);
void fos_preview_free(struct fos_preview *);
int fos_preview_save(const char *, struct fos_preview *, int);
int fos_preview_load(const char *, struct fos_preview *, int *);
uint32_t fos_exif_taken(const uint8_t *exif, int size);   /* sortable capture time from an "Exif\0\0" block, 0 if none */
uint32_t fos_preview_taken(const char *jpeg);              /* the capture time stored in its cache, 0 if none */
int fos_preview_exists(const char *);   /* a valid current-format cache sits beside this JPEG */
int fos_preview_final(const char *);    /* ...and it is the full-size one, not the early small review */
void fos_lut_pixel(const uint8_t *, const uint8_t *, uint8_t *);            /* tetrahedral */
void fos_lut_pixel_trilinear(const uint8_t *, const uint8_t *, uint8_t *);  /* reference, for tests */
int fos_quick_preview(const char *, const char *, int);
int fos_plain_preview(const char *);    /* cache for a finished JPEG: no LUT is applied again */
/* Reduce a w x h RGB picture to a palette of its own (as the review cache does). 1 on success; free with fos_preview_free. */
int fos_quantise_rgb(const uint8_t *rgb, int w, int h, struct fos_preview *out);

/* Zoom: the finished JPEG decoded at 1/2 or full size, keeping a window of it in memory (RGB565) so panning
 * inside the window is instant. Coordinates are in the picture as displayed (after rotation). */
struct fos_zoom {
    int dw, dh;                         /* displayed size at this decode scale */
    int wx, wy, ww, wh;                 /* the window held in memory */
    uint16_t *px;
};
int fos_zoom_open(struct fos_zoom *z, const char *jpeg, int scale, int orientation, int cx, int cy, int (*progress)(int pct));
void fos_zoom_view(const struct fos_zoom *z, int vx, int vy, uint8_t *rgb720x480);   /* black where there is no picture */
int fos_zoom_holds(const struct fos_zoom *z, int vx, int vy);                          /* 720x480 at vx,vy lies inside the window */
void fos_zoom_free(struct fos_zoom *z);
#endif
