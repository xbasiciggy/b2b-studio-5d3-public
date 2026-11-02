/* the film OS fonts from out/FOSFONT.BIN (make), loaded as the camera
 * loads them from its card */

#include <stdio.h>
#include <stdlib.h>
#include "../fos_gfx.h"

void sim_fonts()
{
    const char *path = getenv("FOS_FONT") ? getenv("FOS_FONT") : "out/" FOS_FONT_FILE;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t) n);         /* kept: the fonts point into it */
    if (!blob || fread(blob, 1, (size_t) n, f) != (size_t) n || !fos_fonts_load(blob, (int) n))
    {
        fprintf(stderr, "%s: not a font file\n", path);
        exit(1);
    }
    fclose(f);
}
