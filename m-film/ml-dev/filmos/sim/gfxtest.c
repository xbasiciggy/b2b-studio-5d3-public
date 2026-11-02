/* gfxtest: draws the palette and every primitive once, for eyeballing.
 *   make gfxtest && ./gfxtest out/gfxtest.idx && python3 topng.py out/gfxtest.idx */

#include <stdio.h>
#include <stdlib.h>
#include "../fos_gfx.h"

void sim_fonts();             /* fonts.c */

int main(int argc, char **argv)
{
    static uint8_t px[FOS_W * FOS_H];
    struct fos_canvas cv;
    fos_gfx_init();
    sim_fonts();
    fos_canvas_init(&cv, px, FOS_W, FOS_W, FOS_H);

    fos_fill(&cv, 0, 0, FOS_W, FOS_H, FC(RAMP_GRAY, 10));

    /* palette strip */
    for (int i = 1; i < 255; i++)
        fos_fill_ix(&cv, 8 + (i - 1) % 127 * 5, 8 + (i - 1) / 127 * 14, 5, 12, i);

    /* dithered gradients per ramp */
    for (int r = 0; r < RAMP_COUNT; r++)
        for (int x = 0; x < 340; x++)
            for (int y = 0; y < 14; y++)
                px[(40 + r * 16 + y) * FOS_W + 8 + x] = fos_ix_dither(r, x * LV_MAX / 339, 8 + x, 40 + r * 16 + y);

    /* dark background gradient, like the stage */
    fos_vgrad(&cv, 360, 40, 352, 160, RAMP_GRAY, LV12(26), LV12(6));

    /* shapes */
    fos_rrect(&cv, 380, 60, 140, 44, 12, FC(RAMP_GRAY, 34), OPAQUE);
    fos_rrect_line(&cv, 530, 60, 160, 44, 22, 2, FC(RAMP_AMBER, 185), OPAQUE);
    fos_text_c(&cv, &fos_font_mdb, 610, 89, "36 EXP", FC(RAMP_AMBER, 185), OPAQUE, 16);
    fos_text_c(&cv, &fos_font_md, 450, 89, "Load roll", FC(RAMP_GRAY, 236), OPAQUE, 0);
    fos_disc(&cv, 420 * 256, 150 * 256, 30 * 256 + 128, FC(RAMP_RED, 150), OPAQUE);
    fos_ring(&cv, 500 * 256, 150 * 256, 30 * 256, 3 * 256, FC(RAMP_GRAY, 200), OPAQUE);
    fos_line(&cv, 560 * 256, 120 * 256, 640 * 256, 185 * 256, 3 * 256, FC(RAMP_GRAY, 230), OPAQUE);
    fos_tri(&cv, 670, 150, 18, 0, FC(RAMP_AMBER, 185), OPAQUE);
    fos_tri(&cv, 700, 150, 18, 2, FC(RAMP_AMBER, 185), 160);

    /* text in every size */
    int y = 230;
    const struct fos_font *fonts[] = { &fos_font_xs, &fos_font_sm, &fos_font_md, &fos_font_mdb, &fos_font_lg, &fos_font_xl };
    const char *names[] = { "xs 13 KODAK" T_DOT "PORTRA 400" T_DOT "C-41", "sm 16 Soft warm skin, gentle highlights",
                            "md 20 Viewfinder  LEDs on half-press", "mdb 20 ROLL 012  14/36", "lg 27 Choose your film",
                            "xl 50 Portra 400  AVATAR To" };
    for (int i = 0; i < 6; i++)
    {
        y += fonts[i]->ascent + 4;
        fos_text(&cv, fonts[i], 12, y, names[i], FC(RAMP_GRAY, 236), OPAQUE, i == 0 ? 24 : 0);
        y += fonts[i]->descent;
    }
    fos_text(&cv, &fos_font_num, 520, 460, "24", FC(RAMP_AMBER, 185), OPAQUE, 0);
    fos_text(&cv, &fos_font_lg, 400, 300, "Amber on grey", FC(RAMP_AMBER, 185), OPAQUE, 0);
    fos_rrect(&cv, 395, 318, 220, 40, 8, FC(RAMP_AMBER, 185), OPAQUE);
    fos_text(&cv, &fos_font_lg, 405, 347, "White on amber", FC(RAMP_GRAY, 250), OPAQUE, 0);

    FILE *f = fopen(argc > 1 ? argv[1] : "out/gfxtest.idx", "wb");
    fwrite(px, 1, sizeof(px), f);
    fclose(f);
    f = fopen("out/pal.bin", "wb");
    fwrite(fos_pal, 4, 256, f);
    fclose(f);
    return 0;
}
