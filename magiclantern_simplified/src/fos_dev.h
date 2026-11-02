/* fos_dev: developing film in the camera.
 *
 * A frame's CR2 RAW becomes a print for the screen: 720 x 480 in its own
 * palette of FOS_PRINT_COLOURS colours. Developing runs in steps of a few
 * tens of ms, so the film OS keeps animating on the same task while the
 * camera works through a roll.
 */

#ifndef _fos_dev_h_
#define _fos_dev_h_

#include <stdint.h>

/* ---------------------------------------------------------------- looks */

/* a film's look, from films/films.py (see fos_looks.c; Darkrooms on the desktop uses the same) */
struct fos_hue_shift { int8_t from, to; float amount, chroma; };
struct fos_look
{
    const char *key;            /* matches fos_films[].key */
    int balance;                /* K the film is balanced for */
    int slide;                  /* reversal film: no scanner exposure */
    float ev, sat;
    float contrast[3], pivot[3], black[3], white[3];
    int nhues;
    struct fos_hue_shift hues[4];
    float halation, hal_thr, hal_tint[3];
    float grain, grain_size;
    int mono;
    float mono_mix[3];
};
extern const struct fos_look fos_looks[];
extern const int fos_nlooks;
/* the looks of the films from ML/DATA/FOSFILMS.BIN (fos_films_load) */
extern const struct fos_look *fos_more_looks;
extern int fos_nmore_looks;
const struct fos_look *fos_look_of(const char *key);

/* the film's colour for one scene-linear sRGB value (0.18 = mid grey):
 * the print, display sRGB 0..1. Same model as film_color() in the lab. */
void fos_film_color(const struct fos_look *f, const float lin[3], float out[3]);

/* ---------------------------------------------------------------- prints */

#define FOS_PRINT_W         720
#define FOS_PRINT_H         480
#define FOS_PRINT_COLOURS   240         /* palette indices 1..240 */
#define FOS_FIXED_BASE      241         /* 241..254: the same in every palette */
#define FOS_FIXED_N         14          /* 241..254 */
#define FOS_THUMB_W         120
#define FOS_THUMB_H         80

struct fos_print
{
    int frame;                  /* 0 = the roll's contact sheet */
    int film;                   /* index into fos_films */
    char when[20];              /* "2026-10-22 17:45" */
    char fstop[8];
    int x0, w;                  /* the picture's columns (a portrait frame is narrower) */
    uint8_t pal[FOS_PRINT_COLOURS][3];
    uint8_t *px;                /* FOS_PRINT_W x FOS_PRINT_H */
};

/* while a photograph is up, the overlay draws with these */
enum { PX_BLACK = FOS_FIXED_BASE, PX_PANEL, PX_G1, PX_G2, PX_G3, PX_G4, PX_G5, PX_WHITE,
       PX_AMBER, PX_AMBER2, PX_RED, PX_LINE, PX_DIM, PX_SPARE };
extern const uint8_t fos_fixed_rgb[FOS_FIXED_N][3];

int  fos_print_load(const char *path, struct fos_print *p);     /* allocates p->px; 0 on failure */
void fos_print_free(struct fos_print *p);
int  fos_print_thumb(const char *path, uint8_t *thumb);         /* FOS_THUMB_W x FOS_THUMB_H RGB */

/* ---------------------------------------------------------------- developing */

struct fos_dev;

/* where the film tables are kept between sessions (NULL: not kept) */
void fos_dev_cache(const char *dir);
/* build film's table into the cache now (the build makes the card's copies) */
int fos_dev_table(int film);
/* also write the frame in full colour as a JPEG at path (before the first step) */
void fos_dev_jpeg(struct fos_dev *d, const char *path);
/* forget the film table (leaving the darkroom) */
void fos_dev_release();

/* develop one frame: cr2 in, a print file out */
struct fos_dev *fos_dev_start(const char *cr2, const char *print, int film, int frame,
                              const char *when, const char *fstop);
/* work for about budget_ms; returns progress 0..1000, 1000 when the print
 * is written, or < 0 when the frame can't be developed */
int fos_dev_step(struct fos_dev *d, int budget_ms);
/* the finished frame, small, RGB: for the contact sheet and the animation */
const uint8_t *fos_dev_thumb(const struct fos_dev *d);
const char *fos_dev_error(const struct fos_dev *d);
/* what it is doing, for the darkroom's animation */
enum { FOS_DEV_MIXING, FOS_DEV_DEVELOPING, FOS_DEV_FIXING, FOS_DEV_DRYING, FOS_DEV_DONE, FOS_DEV_FAILED };
int fos_dev_doing(const struct fos_dev *d);
int fos_dev_progress(const struct fos_dev *d);      /* 0..1000, as fos_dev_step returned */
void fos_dev_picture(const struct fos_dev *d, int *x0, int *w);   /* the picture's columns on the print */
void fos_dev_free(struct fos_dev *d);

/* the print in progress, w x h, as the latent image coming up: a rough
 * luminance, 255 (paper) where nothing has been read yet; 0 if there is
 * nothing to show (not reading the RAW) */
int fos_dev_preview(const struct fos_dev *d, uint8_t *lum, int w, int h);

/* the roll's contact sheet from its frames' prints (NULL: not
 * developed); written as a print with frame 0. Frame i sits in cell
 * (i % 6, i / 6) of FOS_THUMB_W x FOS_THUMB_H cells, rows centred */
#define FOS_SHEET_FX 6
#define FOS_SHEET_FY 4
#define FOS_SHEET_FW 108
#define FOS_SHEET_FH 72
int fos_dev_sheet(const char *sheet, int film, const char *const *prints, int frames);

/* ---------------------------------------------------------------- the rolls on the card (fos_lab.c) */

#define FOS_MAX_FRAMES 40

struct fos_lab_roll
{
    int number;
    int film;                   /* index into fos_films */
    int exposures;
    int shot;                   /* frames in its logs */
    int developed;              /* frames with a print */
    int sheet;                  /* its contact sheet is there */
    int jpegs;                  /* JPEGs in its folder in DCIM */
    int in_camera;              /* still loaded: rewind it before developing */
    char loaded[17];            /* "2026-10-22 17:45" */
    char last[17];              /* its last frame */
};

struct fos_lab_frame
{
    int no;
    char cr2[64];
    int missing;                /* the file isn't there (deleted, or shot as JPEG) */
    char when[17];
    char fstop[6];
};

int  fos_lab_scan(struct fos_lab_roll *rolls, int max);                 /* newest first; how many */
int  fos_lab_frames(int roll, struct fos_lab_frame *frames, int max);   /* frame order; how many */
void fos_lab_print_path(char *buf, int size, int roll, int frame);      /* frame 0: the contact sheet */
int  fos_lab_prepare(int roll);                                         /* its print folder; 0 = ok */
void fos_lab_cache_dir(char *buf, int size);                            /* where film tables are kept */
/* the roll's folder of JPEGs in DCIM, made if need be: "Sep0906 Portra400"
 * (month, month number, day of its first frame; when: "2026-09-06 10:15")
 * or SEP0906 where the card takes no long names. 0 = ok */
int  fos_lab_dcim_dir(char *buf, int size, int roll, int film, const char *when);

#endif
