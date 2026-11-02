/* the films beyond the built-in ones from out/FOSFILMS.BIN (make), loaded
 * as the camera loads them from its card; without the file, the built-in
 * films only */

#include <stdio.h>
#include <stdlib.h>
#include "../fos.h"

void sim_films()
{
    const char *path = getenv("FOS_FILMS") ? getenv("FOS_FILMS") : "out/" FOS_FILM_FILE;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t) n);         /* kept: the films point into it */
    if (!blob || fread(blob, 1, (size_t) n, f) != (size_t) n || !fos_films_load(blob, (int) n))
    {
        fprintf(stderr, "%s: not a film file\n", path);
        free(blob);
    }
    fclose(f);
}
