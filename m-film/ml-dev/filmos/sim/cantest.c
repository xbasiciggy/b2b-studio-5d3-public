/* cantest: every canister, front and turned, for checking the label art */

#include <stdio.h>
#include <stdlib.h>
#include "../fos.h"

void sim_fonts();             /* fonts.c */
void sim_films();             /* films.c */

void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }

int main(int argc, char **argv)
{
    static uint8_t px[FOS_W * FOS_H];
    struct fos_canvas cv;
    fos_gfx_init();
    sim_fonts();
    sim_films();
    fos_canvas_init(&cv, px, FOS_W, FOS_W, FOS_H);
    int page = argc > 1 ? atoi(argv[1]) : 0;
    int rot = argc > 2 ? atoi(argv[2]) * 65536 / 360 : 0;

    fos_vgrad(&cv, 0, 0, FOS_W, 300, RAMP_GRAY, LV12(30), LV12(12));
    fos_vgrad(&cv, 0, 300, FOS_W, 180, RAMP_GRAY, LV12(16), LV12(5));

    /* four stocks per page */
    for (int i = 0; i < 4; i++)
    {
        int film = (page * 4 + i) % fos_nfilms;
        struct fos_can *c = fos_can_new(film, 36);
        struct fos_can_pose p = { (100 + i * 175) * 256, 300, 256, rot, 256, 256, 70, 0 };
        fos_can_draw(&cv, c, &p);
        fos_text_c(&cv, &fos_font_sm, 100 + i * 175, 380, fos_films[film].name, FC(RAMP_GRAY, 200), OPAQUE, 0);
        fos_can_free(c);
    }
    FILE *f = fopen("out/cantest.idx", "wb");
    fwrite(px, 1, sizeof(px), f);
    fclose(f);
    f = fopen("out/pal.bin", "wb");
    fwrite(fos_pal, 4, 256, f);
    fclose(f);
    return 0;
}
