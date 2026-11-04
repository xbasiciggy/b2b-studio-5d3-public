/* fos_ui.h: shared between the film OS screens (fos_ui.c, fos_shelf.c,
 * fos_pages.c). Not used by the camera glue. */

#ifndef _fos_ui_h_
#define _fos_ui_h_

#include "fos.h"

/* ---------------------------------------------------------------- style */

#define C_BG        FC(RAMP_GRAY, 11)
#define C_PANEL     FC(RAMP_GRAY, 21)
#define C_PANEL_HI  FC(RAMP_GRAY, 32)
#define C_LINE      FC(RAMP_GRAY, 40)
#define C_TEXT      FC(RAMP_GRAY, 240)
#define C_TEXT2     FC(RAMP_GRAY, 170)
#define C_TEXT3     FC(RAMP_GRAY, 112)
#define C_ACCENT    FC(RAMP_RED, 150)
#define C_ACCENT2   FC(RAMP_RED, 108)
#define C_RED       FC(RAMP_RED, 150)

#define STATUS_H    36
#define HINTS_Y     444

/* ---------------------------------------------------------------- shared state */

extern struct fos_canvas *cv;       /* what the screens draw into */
extern uint8_t *ui_bg;              /* background of the current screen, same layout */
extern int ui_now;                  /* ms */
extern struct fos_roll ui_roll;

/* set when something else drew over the canvas (the shutter, a screen
 * change): screens that redraw only what moved must redraw it all */
extern int ui_clobbered;

/* rows that changed this frame (the loop presents them) */
extern int ui_hints_dark;
int ui_theme(void);
void ui_bg_theme_rows(int y0, int y1);
void ui_damage(int y0, int y1);
void ui_damage_all();

/* ---------------------------------------------------------------- animation */

struct spring { float x, v; };

/* steps towards target; returns 1 while still moving */
int spring_step(struct spring *s, float target, int dt, float hz, float zeta);
float ease_out(float t);            /* cubic */
float ease_in_out(float t);
float clampf(float v, float lo, float hi);

/* ---------------------------------------------------------------- navigation */

enum { SCR_NONE, SCR_BOOT, SCR_SHELF, SCR_HOME, SCR_PAGE, SCR_ROLL, SCR_DARK, SCR_DEVELOP, SCR_PRINTS, SCR_BAKE_STATUS, SCR_LOOKS };
enum { GO_PUSH = 1, GO_POP = -1, GO_FADE = 0 };

void ui_go(int screen, int arg, int how);
void ui_leave();                    /* back to LiveView, shutter opening on it */
int  ui_from();                     /* screen we came from */
int  ui_settled();                  /* no screen change, shutter or iris under way */

/* ---------------------------------------------------------------- widgets */

void ui_bg_restore(int x, int y, int w, int h);
void ui_to_bg(int on);              /* widgets draw into ui_bg instead of the screen */
void ui_bg_plain();                 /* the usual dark backdrop into ui_bg */
void ui_status_bar(const char *title);

struct hint { const char *key; const char *label; };
void ui_hints(const struct hint *h, int n);
int  ui_keycap(int x, int base, const char *key);   /* returns width */

void ui_dots(int x, int y, int n, int filled, fcol on);   /* 1..5 rating */

/* text over a photograph (its palette up, fos_port_photo): coverage onto
 * the fixed greys, or the amber; align 0 left, 1 centred, 2 right of x.
 * Returns the width. */
int  ui_photo_text(const struct fos_font *f, int x, int y, const char *s, int accent, int align);

/* a short message in a pill above the hints, fading after a moment */
void ui_toast(const char *msg);
int  ui_toast_up();                 /* 1 while one is showing */

/* a question sliding up from the bottom; s = 0..1 how far it is in */
void ui_sheet(float s, const char *title, const char *body, const char *yes, const char *no);
/* the same with a third answer on INFO */
void ui_sheet3(float s, const char *title, const char *body, const char *yes, const char *alt, const char *no);

enum { ICON_FILM, ICON_ROLL, ICON_FINDER, ICON_LENS, ICON_SYSTEM, ICON_PRINTS };
void ui_icon(int kind, int cx, int cy, fcol c, int alpha);

/* the aperture over the whole screen: 0 = closed, 4096 = gone; the
 * opening shows what is already on the canvas */
void ui_iris(int open, int angle);

/* a lens aperture inside a barrel of radius R px (same blades, smaller) */
void ui_aperture(int cx, int cy, int R, int open, int angle);

/* ---------------------------------------------------------------- screens */

void shelf_enter(int arg);
void shelf_leave();
void shelf_key(int key);
int  shelf_update(int dt);
void shelf_draw();

void home_enter(int arg);
void home_key(int key);
int  home_update(int dt);
void home_draw();

void page_enter(int arg);
void page_key(int key);
int  page_update(int dt);
void page_draw();

void roll_enter(int arg);
void roll_key(int key);
int  roll_update(int dt);
void roll_draw();

/* the darkroom (fos_dark.c): arg is a roll number, 0 for the newest */
void dark_enter(int arg);
void dark_free();                   /* its memory, when the film OS closes */
void dark_key(int key);
int  dark_update(int dt);
void dark_draw();

/* developing the rolls queued with dark_queue (arg unused) */
void dark_queue_clear();
void dark_queue_add(int roll);
void develop_enter(int arg);
void develop_leave();
void develop_key(int key);
int  develop_update(int dt);
void develop_draw();

/* prints: arg is a roll number (0: the newest developed); PRINTS_ALONE
 * or-ed in when opened by PLAY from the camera */
#define PRINTS_ALONE 0x10000
void prints_enter(int arg);
void prints_leave();
void prints_key(int key);
int  prints_update(int dt);
void prints_draw();

/* the canisters, kept while the UI is open (fos_shelf.c) */
struct fos_can *ui_can(int film);
void ui_cans_free();
extern int ui_cans_ms;              /* time spent drawing canisters, for the frame stats */
void ui_cans_exposures(int exposures);

#endif
