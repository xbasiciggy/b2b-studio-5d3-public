/* bench: how long one shelf frame takes to draw (host), for budgeting */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../fos_ui.h"

void sim_fonts();             /* fonts.c */
void sim_films();             /* films.c */
void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }
int fos_port_get(int s) { return 0; }
void fos_port_set(int s, int v) {}
void fos_port_action(int a, int x, int y) {}
void fos_port_roll(struct fos_roll *r) { r->loaded = 0; }
void fos_port_clock(char *b, int n) { snprintf(b, n, "09:41"); }
int fos_port_ms() { return 0; }
void fos_port_sleep(int ms) {}
int fos_port_key() { return 0; }
int fos_port_abort() { return 1; }
void fos_port_present(const uint8_t *r, int p, int a, int b) {}
void fos_port_palette() {}
int fos_port_lens_manual() { return 1; }
const char *fos_port_lens_name() { return ""; }
const char *fos_port_version() { return ""; }
void fos_port_stat(const struct fos_stat *s) { }
static double now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main()
{
    static uint8_t a[FOS_W * FOS_H], b[FOS_W * FOS_H];
    static struct fos_canvas c;
    fos_gfx_init();
    sim_fonts();
    sim_films();
    fos_canvas_init(&c, a, FOS_W, FOS_W, FOS_H);
    cv = &c; ui_bg = b;
    shelf_enter(1);
    for (int i = 0; i < fos_nfilms; i++) ui_can(i);
    double t0 = now();
    int n = getenv("N") ? atoi(getenv("N")) : 300;
    for (int i = 0; i < n; i++) { if (i % 24 == 0) shelf_key(i % 48 ? FK_WHEEL_R : FK_WHEEL_L); shelf_update(16); shelf_draw(); }
    double t1 = now();
    printf("shelf frame: %.3f ms on this PC\n", (t1 - t0) / n);
    return 0;
}
