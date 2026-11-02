/* fos: the film OS - the EOS M's own user interface on top of Magic Lantern.
 *
 * Everything the user sees (film shelf, home, settings, roll status,
 * transitions) lives in fos_ui.c and draws with fos_gfx. It talks to the
 * camera only through the fos_port_* functions below, which filmroll.c
 * implements on the camera and sim/sim.c implements on a PC.
 */

#ifndef _fos_h_
#define _fos_h_

#include "fos_gfx.h"
#include <stdio.h>
#include <string.h>

#define FOS_NAME "B2B STUDIO"

/* the first n characters of src (fewer if it is shorter) into dst: the
 * camera's snprintf is Canon's, which doesn't know "%.5s" */
static inline char *fos_copy(char *dst, int size, const char *src, int n)
{
    int i = 0;
    for (; i < n && i < size - 1 && src[i]; i++) dst[i] = src[i];
    if (size > 0) dst[i] = 0;
    return dst;
}

/* ---------------------------------------------------------------- films */

#define PS_STANDARD   0x81      /* raw PROP_PICTURE_STYLE values */
#define PS_LANDSCAPE  0x83
#define PS_MONO       0x86

/* the shelves the films stand on */
enum { FOS_GROUP_COLOUR, FOS_GROUP_SLIDE, FOS_GROUP_BW, FOS_GROUP_CINE, FOS_GROUP_PRESETS, FOS_NGROUPS };
extern const char *const fos_group_names[FOS_NGROUPS];

/* a canister label drawn from parts (the films from ML/DATA/FOSFILMS.BIN;
 * the built-in ones have their own painting in fos_film.c). An ink is a
 * ramp and a level. */
enum { FOS_LABEL_BAND, FOS_LABEL_CAP, FOS_LABEL_STRIPES };
struct fos_label
{
    uint8_t style;
    uint8_t leader;             /* level of a grey (B&W) leader, 0 = colour film */
    uint8_t bg[2], band[2], accent[2], text[2], num[2];
    char top[20];               /* small, above the name */
    char word[14];              /* the name */
    char num_s[8];              /* big: digits and H, P, T */
    char sub[18];               /* small, under the number */
};

struct fos_film
{
    const char *key;            /* must match films/films.py */
    const char *brand;
    const char *name;
    const char *process;        /* "C-41", "E-6", "B&W" */
    const char *desc;
    int box_iso;                /* rated speed */
    int iso;                    /* what the camera is set to (EOS M has no ISO 50) */
    int picstyle;               /* in-camera preview only; the look comes from the lab */
    int contrast, saturation, color_tone, sharpness;
    int kelvin;
    int wbs_gm;                 /* + green, - magenta (-9..9) */
    int wbs_ba;                 /* + amber, - blue    (-9..9) */
    uint8_t grain, punch, colour, warmth;       /* 1..5, for the shelf */
    const char *shortname;      /* in folder names: "Portra400" */
    int group;                  /* FOS_GROUP_* */
    const struct fos_label *label;              /* NULL: painted by hand */
};

/* the built-in films, then those from ML/DATA/FOSFILMS.BIN once loaded */
extern const struct fos_film *fos_films;
extern int fos_nfilms;
#define FOS_FILM_FILE "FOSFILMS.BIN"
/* blob: the file's contents, kept while the films are used; 1 when usable */
int fos_films_load(const uint8_t *blob, int size);

extern const int fos_exposure_choices[];        /* 36, 24, 12 */
#define FOS_NEXP 3

/* f-numbers x100 in 1/3 stops; see fos_film.c */
extern const int fos_fstops[];
extern const int fos_nfstops;
void fos_format_fstop(char *buf, int size, int index);

/* ---------------------------------------------------------------- keys */

enum
{
    FK_NONE,
    FK_LEFT, FK_RIGHT, FK_UP, FK_DOWN,      /* the four presses of the rear dial */
    FK_WHEEL_L, FK_WHEEL_R,                 /* turning it */
    FK_SET, FK_MENU, FK_INFO, FK_PLAY,
    FK_HOME,                                /* what used to open the ML menu */
    FK_RELEASE = 0x100,                     /* or-ed in: a press above ended */
};

/* ---------------------------------------------------------------- settings */

enum
{
    FS_HUD, FS_LEDS, FS_FOCUS, FS_WIND,
    FS_APKEYS, FS_LENS_WIDE, FS_FSTOP,
    FS_STARTUP, FS_PLAYLOCK, FS_BOOTANIM, FS_DEBUG,
    FS_FILM, FS_EXP,
    FS_SCR_STD,         /* screen colour: 0 Canon's conversion, 1 Rec.709 */
    FS_SCR_SAT, FS_SCR_WARM, FS_SCR_BRIGHT,     /* -FOS_SCR_STEPS..FOS_SCR_STEPS */
    FS_THEME,           /* colour theme of the Home and Film menus: 0 red, 1 orange, 2 neon green */
    FS_PLATE,                   /* the back screen as a film camera's top plate while LiveView is off */
    FS_GRAIN, FS_HALATION,      /* baked looks: 0 off, 1 light, 2 medium, 3 strong */
    FS_COUNT
};
#define FOS_SCR_STEPS 6

enum
{
    FA_LOAD,            /* a = film, b = exposure choice */
    FA_REWIND,          /* end the roll in the camera */
    FA_REAPPLY,         /* film settings again (after changing WB by hand) */
    FA_CANON_MENU,      /* leave and open Canon's menu */
    FA_SELFTEST,        /* measure memory and speed, write the results to the log */
    FA_BUSY,            /* a = 1: long work (developing) begins, keep the camera awake; 0: done */
};

#define FOS_RECENT 5
struct fos_roll
{
    int loaded;
    int number;
    int film;
    int exposures;
    int shot;
    int need_wind;
    char loaded_at[20];         /* "2026-08-08 09:10" */
    int nrecent;                /* newest first */
    struct { int no; char time[6]; char fstop[6]; } recent[FOS_RECENT];
};

/* screens fos_ui_run can open on; BOOT_LV plays the boot animation and
 * goes straight back to the camera */
enum { FOS_OPEN_SHELF, FOS_OPEN_HOME, FOS_OPEN_BOOT, FOS_OPEN_BOOT_LV,
       FOS_OPEN_PRINTS,     /* PLAY: the latest prints */
       FOS_OPEN_END,        /* the roll has just been finished */
       FOS_OPEN_DARKROOM, FOS_OPEN_REVIEW };

/* ---------------------------------------------------------------- the port */

int fos_port_bake_yield(void);
int fos_port_background_abort(void);
void fos_port_bake_progress(int,int);
const char *fos_port_review_name(void);
void fos_port_bake_status(char *,int);
int  fos_port_ms();
void fos_port_sleep(int ms);
int  fos_port_key();                        /* next key event, FK_NONE if none */
int  fos_port_abort();                      /* leave right away (half-shutter, power off) */
void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1);  /* rows = row y0 */
void fos_port_palette();
void fos_port_palette_part(int first, int n);   /* only these palette entries (the cycling dots) */                    /* load fos_pal[1..254] */
void *fos_port_alloc(int size);             /* big, temporary buffers */
void fos_port_free(void *p);
/* files on the card, for developing; fos_port_open returns NULL on failure */
void *fos_port_open(const char *path, int write);
int  fos_port_read(void *f, void *buf, int size);          /* bytes read */
int  fos_port_write(void *f, const void *buf, int size);   /* bytes written */
int  fos_port_seek(void *f, int offset);                   /* from the start; < 0 on failure */
void fos_port_close(void *f);
const char *fos_port_card();                                /* the card's root, ending in '/' */
/* every entry of a directory, until fn returns 0; < 0 if it can't be read */
int  fos_port_list(const char *dir, int (*fn)(void *ctx, const char *name, int is_dir), void *ctx);
int  fos_port_mkdir(const char *path);                      /* 0 on success or if it exists */

void fos_port_roll(struct fos_roll *r);
int  fos_port_get(int setting);
void fos_port_set(int setting, int value);
void fos_port_action(int action, int a, int b);
void fos_port_clock(char *buf, int size);   /* "14:32" */
int  fos_port_lens_manual();
const char *fos_port_lens_name();
const char *fos_port_version();
const char *fos_port_model();               /* "Canon EOS M": the camera it runs on */
/* photographs: palette entries 1..n become pal's colours at k/256 of
 * their brightness, FOS_FIXED_BASE.. the fixed overlay colours (fos_dev.h);
 * fos_port_palette() goes back to the film OS's own */
void fos_port_photo(const uint8_t (*pal)[3], int n, int k);
void fos_port_log(const char *msg);         /* the debug log, if it is on */
/* developing works in slices until a time budget is spent; the simulator's
 * clock stands still meanwhile, so it caps the slices instead (0: no cap) */
int  fos_port_dev_cap();

/* frame times, summed over about a second of animation; open_ms is how
 * long the film OS took to show its first frame (first report only) */
struct fos_stat { int frames, draw_ms, present_ms, rest_ms, worst_ms, open_ms, cans_ms; };   /* cans_ms: part of draw_ms */
void fos_port_stat(const struct fos_stat *s);   /* keep it, don't log while animating */

/* ---------------------------------------------------------------- the viewfinder HUD */

/* what the HUD shows; fos_hud.c draws it with Canon's palette */
struct fos_hud
{
    int lit;                    /* LEDs on (half-press, or always) */
    int blink;                  /* 0/1, toggles a few times a second */
    int meter;                  /* the meter has a reading */
    int fstop, widest, want;    /* indices into fos_fstops; want = what the meter asks for */
    char shutter[12];           /* "125", "2\"", "0.5\"" */
    int frame, exposures, finished, need_wind;
    int focus_screen;
    const char *film;
    int grain, halation;        /* baked looks on (0 off, 1..3), shown beside the look's name */
    /* the back screen (fos_hud_plate) */
    int iso;                    /* 0 = auto */
    int bake_queued, bake_active, bake_paused, bake_percent;
    char status[56];            /* the bake status line, empty when nothing is waiting */
    char mode[8], wb[12], clock[8];     /* exposure mode (M, Av...), white balance, the time: chips on the plate, empty = left out */
    char quality[12];                   /* picture quality as ML names it: "S1", "L", "RAW+S2"; empty = left out */
    int quality_fine;                   /* the JPEG is Fine, not Normal */
    int focal, battery, shots_left;     /* mm (0 = unknown), percent (-1 unknown), pictures left on the card (-1 unknown) */
};

struct fos_hud_palette
{
    uint8_t clear, black, white, red, dark_red;
    uint8_t gray[101];          /* percent -> index */
    uint8_t alert;              /* a warning colour that stays red whatever red/dark_red hold (0: use red) */
};

void fos_hud_draw(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p);
void fos_hud_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p);
/* the whole screen as a film camera's top plate (LiveView off): look, shutter, aperture, frame count, the light meter,
 * and the bake queue. Canon's own palette, like the HUD; fos_hud_plate_clear gives the screen back. */
void fos_hud_plate(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p);
void fos_hud_plate_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p);

/* ---------------------------------------------------------------- the UI */

void fos_ui_init();
void fos_ui_run(int screen);

/* canister art, fos_film.c; quality 2 = everything, 1 = no texture
 * filtering, 0 = no reflections either (the UI lowers it on a slow camera) */
extern int fos_can_quality;
struct fos_can;
struct fos_can *fos_can_new(int film, int exposures);
void fos_can_free(struct fos_can *c);
void fos_can_set_exposures(struct fos_can *c, int exposures);

/* draw a canister standing on floor_y, centred on cx (1/256 px);
 * scale 256 = full size, rot = turn (0..65535, 0 = label to the front),
 * light 0..256 dims it for depth, leader 0 = none, 256 = the usual tongue,
 * more = pulled further out, reflect 0..256 = its mirror image in the floor,
 * fast = moving too quickly to need the label filtered between texels */
struct fos_can_pose { int cx8, floor_y, scale, rot, light, leader, reflect, fast; };
void fos_can_draw(struct fos_canvas *cv, struct fos_can *c, const struct fos_can_pose *p);
int  fos_can_height(int scale);
int  fos_can_width(int scale);

#endif
