/* textest: the label textures unrolled, for checking the art */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fos.h"

void sim_fonts();             /* fonts.c */
void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }
/* fos_film.c's private layout */
struct can_view { int film, exposures; uint16_t *tex; uint8_t ink_ramp[16]; uint16_t ink_lv[16]; int ninks; };
int main()
{
    fos_gfx_init();
    sim_fonts();
    FILE *f = fopen("out/tex.ppm", "wb");
    fprintf(f, "P6 320 %d 255\n", 150 * fos_nfilms);
    for (int film = 0; film < fos_nfilms; film++)
    {
        struct can_view *c = (struct can_view *) fos_can_new(film, 36);
        for (int v = 0; v < 150; v++)
            for (int u = 0; u < 320; u++)
            {
                uint16_t t = c->tex[u * 150 + v];
                int k = (t >> 8) & 15 ? ((t >> 4) & 15) : (t & 15);
                uint32_t rgb = fos_pal[fos_ix(FC(c->ink_ramp[k], c->ink_lv[k] * 255 / 4095))];
                fputc(rgb >> 16, f); fputc((rgb >> 8) & 255, f); fputc(rgb & 255, f);
            }
    }
    fclose(f);
    return 0;
}
