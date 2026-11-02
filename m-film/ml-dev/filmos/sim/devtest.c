/* devtest: develops CR2 files the way the camera does, on a PC.
 *
 *   ./out/devtest [-f film] [-r frame.u16] in.CR2 out.FOP [out.ppm]
 *   ./out/devtest -S in.CR2 dir    the shelf's samples: in.CR2 with every film, dir/Fnnn.BMP
 *
 * -r checks the RAW decoder against the samples mkcr2 wrote (--raw);
 * out.ppm is the print as the screen would show it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../fos.h"
#include "../fos_dev.h"

int fos_port_ms()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
void *fos_port_alloc(int size) { return malloc(size); }
int fos_port_dev_cap() { return 0; }
void fos_port_log(const char *msg) { fprintf(stderr, "%s\n", msg); }
void fos_port_free(void *p) { free(p); }

/* the decoder's samples, to compare with mkcr2's */
static uint16_t *raw;
static int raw_w, raw_h;
static long bad, seen;

void fos_dev_test_sample(int x, int y, int v)
{
    seen++;
    if (!raw) return;
    if (x >= raw_w || y >= raw_h || raw[(long) y * raw_w + x] != v)
    {
        if (bad++ < 5) fprintf(stderr, "sample %d,%d: %d, expected %d\n", x, y, v,
                               x < raw_w && y < raw_h ? raw[(long) y * raw_w + x] : -1);
    }
}

static int load_u16(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    /* mkcr2 --raw: just the samples, of the full or the --small sensor */
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 2;
    fseek(f, 0, SEEK_SET);
    if (n == 5280L * 3528) { raw_w = 5280; raw_h = 3528; }
    else if (n == 2640L * 1764) { raw_w = 2640; raw_h = 1764; }
    else if (n == 1320L * 882) { raw_w = 1320; raw_h = 882; }
    else
    {
        /* mkcr2 --sensor: the other cameras' sensors (LibRaw's identify.cpp) */
        static const int other[][2] = { { 5568, 3708 }, { 5920, 3950 }, { 5792, 3804 }, { 5344, 3516 },
                                        { 5360, 3516 }, { 4832, 3204 }, { 4832, 3228 }, { 4352, 2874 } };
        for (unsigned i = 0; i < sizeof(other) / sizeof(other[0]); i++)
            if (n == (long) other[i][0] * other[i][1]) { raw_w = other[i][0]; raw_h = other[i][1]; }
    }
    if (!raw_w) { fprintf(stderr, "%s: unknown size\n", path); fclose(f); return 0; }
    raw = malloc((size_t) n * 2);
    size_t got = fread(raw, 2, (size_t) n, f);
    fclose(f);
    return got == (size_t) n;
}

static int film_index(const char *key)
{
    for (int i = 0; i < fos_nfilms; i++)
        if (!strcmp(fos_films[i].key, key)) return i;
    fprintf(stderr, "no film %s\n", key);
    exit(2);
}

void sim_films();             /* films.c */

int main(int argc, char **argv)
{
    sim_films();
    int film = 0, a = 1;
    const char *rawpath = 0;
    if (argc == 3 && !strcmp(argv[1], "-T"))
    {
        /* -T dir: every film's table into dir, as the camera keeps them */
        fos_dev_cache(argv[2]);
        for (int i = 0; i < fos_nfilms; i++)
            if (!fos_dev_table(i)) return 1;
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "-S"))
    {
        /* FOS_LUT_DIR: the tables the camera will use (made with -T, then baked from LUTs) */
        if (getenv("FOS_LUT_DIR")) fos_dev_cache(getenv("FOS_LUT_DIR"));
        for (int i = 0; i < fos_nfilms; i++)
        {
            char out[512];
            snprintf(out, sizeof(out), "%s/F%03d.BMP", argv[3], i);
            struct fos_dev *d = fos_dev_start(argv[2], out, i, 1, "", "");
            int p;
            while ((p = fos_dev_step(d, 40)) >= 0 && p < 1000) {}
            if (p < 0) { fprintf(stderr, "%s, %s: %s\n", argv[2], fos_films[i].key, fos_dev_error(d)); fos_dev_free(d); return 1; }
            fos_dev_free(d);
        }
        return 0;
    }
    for (; a < argc && argv[a][0] == '-'; a += 2)
    {
        if (a + 1 >= argc) break;
        if (!strcmp(argv[a], "-f")) film = film_index(argv[a + 1]);
        else if (!strcmp(argv[a], "-r")) rawpath = argv[a + 1];
    }
    if (argc - a < 2) { fprintf(stderr, "usage: devtest [-f film] [-r frame.u16] in.CR2 out.FOP [out.ppm]\n"
                                        "       devtest -T dir    (the films' tables)\n"
                                        "       devtest -S in.CR2 dir    (the shelf's samples)\n"); return 2; }
    if (rawpath && !load_u16(rawpath)) return 1;

    /* FOS_LUT_DIR: keep the film tables there, as the camera does */
    if (getenv("FOS_LUT_DIR")) fos_dev_cache(getenv("FOS_LUT_DIR"));
    int t0 = fos_port_ms(), steps = 0, p = 0;
    struct fos_dev *d = fos_dev_start(argv[a], argv[a + 1], film, 1, "2026-10-22 17:45", "2.8");
    while ((p = fos_dev_step(d, 40)) >= 0 && p < 1000) steps++;
    if (p < 0) { fprintf(stderr, "%s: %s\n", argv[a], fos_dev_error(d)); fos_dev_free(d); return 1; }
    printf("%s: %d ms, %d steps\n", argv[a], fos_port_ms() - t0, steps);
    fos_dev_free(d);
    if (rawpath)
    {
        printf("decoder: %ld samples, %ld wrong (of %ld)\n", seen, bad, (long) raw_w * raw_h);
        if (bad || seen != (long) raw_w * raw_h) return 1;
    }

    if (argc - a >= 3)
    {
        struct fos_print pr;
        if (!fos_print_load(argv[a + 1], &pr)) { fprintf(stderr, "can't load the print\n"); return 1; }
        FILE *f = fopen(argv[a + 2], "wb");
        fprintf(f, "P6\n%d %d\n255\n", FOS_PRINT_W, FOS_PRINT_H);
        for (int i = 0; i < FOS_PRINT_W * FOS_PRINT_H; i++)
        {
            int k = pr.px[i];
            uint8_t black[3] = { 0, 0, 0 };
            fwrite(k >= 1 && k <= FOS_PRINT_COLOURS ? pr.pal[k - 1] : black, 1, 3, f);
        }
        fclose(f);
        fos_print_free(&pr);
    }
    return 0;
}
