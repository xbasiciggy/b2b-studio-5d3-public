/* fos_nodark: the darkroom's place in a build that leaves it out.
 *
 * Some cameras can't spare the memory the in-camera darkroom takes at boot
 * (fos_dark.c, fos_dev.c, fos_lab.c, fos_looks.c, fos_jpeg.c: about 60 KB).
 * Built with build_fork.sh --no-darkroom, those are replaced by this: the
 * Darkroom and Prints screens say where the rolls are developed instead
 * (Darkrooms, on a computer), and the shelf has no sample prints.
 */

#include "fos_ui.h"
#include "fos_dev.h"

#define ND_X    24

static void nd_bake()
{
    ui_bg_plain();
    ui_to_bg(1);
    ui_status_bar("DARKROOM");
    fos_text(cv, &fos_font_lg, ND_X, 82, "Darkroom", C_TEXT, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, ND_X, 120, "This camera's M" T_DOT "FILM has no darkroom of its own,", C_TEXT2, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, ND_X, 144, "to leave the camera the memory it needs.", C_TEXT2, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, ND_X, 184, "Your rolls are on the card, every frame in RAW:", C_TEXT3, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, ND_X, 208, "develop them with Darkrooms on a computer.", C_ACCENT2, OPAQUE, 0);
    ui_to_bg(0);
}

static void nd_draw()
{
    if (!ui_clobbered) return;
    ui_clobbered = 0;
    ui_bg_restore(0, 0, FOS_W, FOS_H);
    ui_damage_all();
    static const struct hint h[] = { { "MENU", "Back" } };
    ui_hints(h, 1);
}

static void nd_key(int k)
{
    if (k == FK_MENU || k == FK_SET || k == FK_PLAY) ui_go(SCR_HOME, 2, GO_POP);
}

void dark_enter(int arg) { nd_bake(); }
void dark_free() {}
void dark_key(int k) { nd_key(k); }
int  dark_update(int dt) { return 0; }
void dark_draw() { nd_draw(); }

void dark_queue_clear() {}
void dark_queue_add(int roll) {}
void develop_enter(int arg) { nd_bake(); }
void develop_leave() {}
void develop_key(int k) { nd_key(k); }
int  develop_update(int dt) { return 0; }
void develop_draw() { nd_draw(); }

void prints_enter(int arg) { nd_bake(); }
void prints_leave() {}
void prints_key(int k) { nd_key(k); }
int  prints_update(int dt) { return 0; }
void prints_draw() { nd_draw(); }

/* the shelf's sample prints are made by the darkroom's code */
int  fos_print_load(const char *path, struct fos_print *p) { return 0; }
void fos_print_free(struct fos_print *p) {}
