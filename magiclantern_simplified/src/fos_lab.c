/* fos_lab: the rolls on the card and their prints, for the darkroom.
 *
 * A roll is what its logs say: DCIM/xxxCANON/ROLL_nnn.TXT, written by the
 * camera frame by frame (a roll that ran into a new DCIM folder has a log
 * in each). Its prints are kept in ML/DARKROOM/Rnnn/, Fnn.BMP for frame nn
 * and SHEET.BMP for the contact sheet; a roll is developed when they are
 * all there.
 */

#include "fos.h"
#include "fos_dev.h"
#include <string.h>
#include <stdlib.h>

#define LOG_MAX (16 * 1024)

void fos_lab_print_path(char *buf, int size, int roll, int frame)
{
    if (frame) snprintf(buf, size, "%sML/DARKROOM/R%03d/F%02d.BMP", fos_port_card(), roll, frame);
    else snprintf(buf, size, "%sML/DARKROOM/R%03d/SHEET.BMP", fos_port_card(), roll);
}

void fos_lab_cache_dir(char *buf, int size)
{
    snprintf(buf, size, "%sML/DARKROOM", fos_port_card());
}

static int first_only(void *ctx, const char *name, int is_dir) { return 0; }

static int dir_exists(const char *path)
{
    return fos_port_list(path, first_only, 0) >= 0;
}

/* a folder of ours: it holds the roll's tag file */
static int tagged(const char *dir, int roll)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/ROLL%03d.TXT", dir, roll);
    void *f = fos_port_open(path, 0);
    if (f) fos_port_close(f);
    return f != 0;
}

static int make_tagged(const char *dir, int roll, int film)
{
    if (fos_port_mkdir(dir) < 0) return 0;
    char path[128], text[96];
    snprintf(path, sizeof(path), "%s/ROLL%03d.TXT", dir, roll);
    void *f = fos_port_open(path, 1);
    if (!f) return 0;
    int n = snprintf(text, sizeof(text), "roll = %03d\nfilm = %s\n", roll, fos_films[film].key);
    fos_port_write(f, text, n);
    fos_port_close(f);
    return 1;
}

int fos_lab_dcim_dir(char *buf, int size, int roll, int film, const char *when)
{
    static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int m = when && strlen(when) >= 10 ? atoi(when + 5) : 0, d = m ? atoi(when + 8) : 0;
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    film = fos_clamp(film, 0, fos_nfilms - 1);
    const char *film_name = fos_films[film].shortname ? fos_films[film].shortname : fos_films[film].key;
    /* "Sep0906 Portra400", "Sep0906 Portra400 2" for another roll that day */
    for (int k = 1; k < 10; k++)
    {
        if (k == 1) snprintf(buf, size, "%sDCIM/%s%02d%02d %s", fos_port_card(), months[m - 1], m, d, film_name);
        else snprintf(buf, size, "%sDCIM/%s%02d%02d %s %d", fos_port_card(), months[m - 1], m, d, film_name, k);
        if (dir_exists(buf)) { if (tagged(buf, roll)) return 0; continue; }
        if (make_tagged(buf, roll, film) && dir_exists(buf)) return 0;
        break;                          /* no long names on this card */
    }
    /* SEP0906, SEP0906B, ... */
    char up[4] = { months[m - 1][0], (char) (months[m - 1][1] - 32), (char) (months[m - 1][2] - 32), 0 };
    for (int k = 0; k < 26; k++)
    {
        char suffix[2] = { (char) (k ? 'A' + k : 0), 0 };
        snprintf(buf, size, "%sDCIM/%s%02d%02d%s", fos_port_card(), up, m, d, suffix);
        if (dir_exists(buf)) { if (tagged(buf, roll)) return 0; continue; }
        if (make_tagged(buf, roll, film)) return 0;
        return -1;
    }
    return -1;
}

int fos_lab_prepare(int roll)
{
    char path[64];
    snprintf(path, sizeof(path), "%sML/DARKROOM", fos_port_card());
    if (fos_port_mkdir(path)) return -1;
    snprintf(path, sizeof(path), "%sML/DARKROOM/R%03d", fos_port_card(), roll);
    return fos_port_mkdir(path);
}

static int film_index(const char *key)
{
    for (int i = 0; i < fos_nfilms; i++)
        if (!strcmp(fos_films[i].key, key)) return i;
    return 0;
}

/* ---------------------------------------------------------------- reading logs */

/* one log, a line at a time: header lines ("film = portra400") and
 * frames ("12  IMG_0012.CR2  2026-08-08 09:15:15  f/2.8") */
struct log_reader
{
    void (*header)(void *ctx, const char *key, const char *value);
    void (*frame)(void *ctx, int no, const char *file, const char *when, const char *fstop);
    void *ctx;
};

static void read_log(const char *path, const struct log_reader *r)
{
    char *buf = fos_port_alloc(LOG_MAX + 1);
    if (!buf) return;
    void *f = fos_port_open(path, 0);
    int n = f ? fos_port_read(f, buf, LOG_MAX) : 0;
    if (f) fos_port_close(f);
    buf[n > 0 ? n : 0] = 0;
    for (char *line = buf; *line; )
    {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        while (*line == ' ') line++;
        char *eq = strchr(line, '=');
        if (*line == '#' || !*line) { line = next; continue; }
        if (eq && !(line[0] >= '0' && line[0] <= '9'))
        {
            char *k = line, *v = eq + 1, *e = eq;
            while (e > k && e[-1] == ' ') e--;
            *e = 0;
            while (*v == ' ') v++;
            if (r->header) r->header(r->ctx, k, v);
        }
        else if (line[0] >= '0' && line[0] <= '9')
        {
            char *tok[5];
            int nt = 0;
            for (char *p = line; *p && nt < 5; )
            {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                tok[nt++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = 0;
            }
            if (nt >= 2 && r->frame)
            {
                char when[20] = "";
                char hm[6];
                if (nt >= 4) snprintf(when, sizeof(when), "%s %s", tok[2], fos_copy(hm, sizeof(hm), tok[3], 5));
                r->frame(r->ctx, atoi(tok[0]), tok[1], when, nt >= 5 && !strncmp(tok[4], "f/", 2) ? tok[4] + 2 : "");
            }
        }
        line = next;
    }
    fos_port_free(buf);
}

/* every ROLL_nnn.TXT on the card: fn(ctx, folder, file name, roll number) */
struct each_log { void (*fn)(void *ctx, const char *folder, const char *name, int number); void *ctx; const char *folder; };

static int log_entry(void *ctx, const char *name, int is_dir)
{
    struct each_log *e = ctx;
    if (!is_dir && !strncmp(name, "ROLL_", 5) && strlen(name) == 12
        && (!strcmp(name + 8, ".TXT") || !strcmp(name + 8, ".txt")))
        e->fn(e->ctx, e->folder, name, atoi(name + 5));
    return 1;
}

static int folder_entry(void *ctx, const char *name, int is_dir)
{
    struct each_log *e = ctx;
    /* DCIM folders: three digits and five characters, "100CANON" */
    if (!is_dir || strlen(name) != 8 || name[0] < '1' || name[0] > '9') return 1;
    char dir[64];
    snprintf(dir, sizeof(dir), "%sDCIM/%s", fos_port_card(), name);
    struct each_log sub = *e;
    sub.folder = name;
    fos_port_list(dir, log_entry, &sub);
    return 1;
}

static void each_log(void (*fn)(void *, const char *, const char *, int), void *ctx)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "%sDCIM", fos_port_card());
    struct each_log e = { fn, ctx, 0 };
    fos_port_list(dir, folder_entry, &e);
}

/* ---------------------------------------------------------------- the catalogue */

struct scan { struct fos_lab_roll *rolls; int n, max; struct fos_lab_roll *cur; };

static void scan_header(void *ctx, const char *k, const char *v)
{
    struct fos_lab_roll *r = ((struct scan *) ctx)->cur;
    if (!strcmp(k, "film")) r->film = film_index(v);
    else if (!strcmp(k, "exposures")) r->exposures = atoi(v);
    else if (!strcmp(k, "loaded") && (!r->loaded[0] || strncmp(v, r->loaded, 16) < 0))
        fos_copy(r->loaded, sizeof(r->loaded), v, 16);
}

static void scan_frame(void *ctx, int no, const char *file, const char *when, const char *fstop)
{
    struct fos_lab_roll *r = ((struct scan *) ctx)->cur;
    if (no > r->shot) r->shot = no;
    if (strcmp(when, r->last) > 0) snprintf(r->last, sizeof(r->last), "%s", when);
}

static void scan_log(void *ctx, const char *folder, const char *name, int number)
{
    struct scan *s = ctx;
    struct fos_lab_roll *r = 0;
    for (int i = 0; i < s->n; i++)
        if (s->rolls[i].number == number) r = &s->rolls[i];
    if (!r)
    {
        if (s->n >= s->max) return;
        r = &s->rolls[s->n++];
        memset(r, 0, sizeof(*r));
        r->number = number;
        r->exposures = 36;
    }
    char path[64];
    snprintf(path, sizeof(path), "%sDCIM/%s/%s", fos_port_card(), folder, name);
    s->cur = r;
    struct log_reader rd = { scan_header, scan_frame, s };
    read_log(path, &rd);
}

static int print_entry(void *ctx, const char *name, int is_dir)
{
    struct fos_lab_roll *r = ctx;
    if (is_dir) return 1;
    if (!strcmp(name, "SHEET.BMP")) r->sheet = 1;
    else if (name[0] == 'F' && strlen(name) == 7 && !strcmp(name + 3, ".BMP"))
    {
        int no = atoi(name + 1);
        if (no >= 1 && no <= r->shot) r->developed++;
    }
    return 1;
}

/* our folders in DCIM (not Canon's "100CANON"): which roll each holds, by
 * its tag file, and how many JPEGs */
struct jpeg_scan { struct fos_lab_roll *rolls; int n; int roll, count; };

static int jpeg_file(void *ctx, const char *name, int is_dir)
{
    struct jpeg_scan *s = ctx;
    int len = (int) strlen(name);
    if (is_dir) return 1;
    if (len == 11 && !strncmp(name, "ROLL", 4) && !strcmp(name + 7, ".TXT")) s->roll = atoi(name + 4);
    else if (len > 4 && !strcmp(name + len - 4, ".JPG")) s->count++;
    return 1;
}

static int jpeg_folder(void *ctx, const char *name, int is_dir)
{
    struct jpeg_scan *s = ctx;
    if (!is_dir || (strlen(name) == 8 && name[0] >= '1' && name[0] <= '9')) return 1;
    char dir[128];
    snprintf(dir, sizeof(dir), "%sDCIM/%s", fos_port_card(), name);
    s->roll = -1;
    s->count = 0;
    fos_port_list(dir, jpeg_file, s);
    for (int i = 0; i < s->n; i++)
        if (s->rolls[i].number == s->roll) s->rolls[i].jpegs += s->count;
    return 1;
}

int fos_lab_scan(struct fos_lab_roll *rolls, int max)
{
    struct scan s = { rolls, 0, max, 0 };
    each_log(scan_log, &s);
    struct fos_roll cam;
    fos_port_roll(&cam);
    for (int i = 0; i < s.n; i++)
    {
        struct fos_lab_roll *r = &rolls[i];
        char dir[64];
        snprintf(dir, sizeof(dir), "%sML/DARKROOM/R%03d", fos_port_card(), r->number);
        fos_port_list(dir, print_entry, r);
        r->in_camera = cam.loaded && cam.number == r->number;
    }
    char dcim[64];
    snprintf(dcim, sizeof(dcim), "%sDCIM", fos_port_card());
    struct jpeg_scan js = { rolls, s.n, -1, 0 };
    fos_port_list(dcim, jpeg_folder, &js);
    /* newest first */
    for (int i = 1; i < s.n; i++)
        for (int j = i; j > 0; j--)
        {
            struct fos_lab_roll *a = &rolls[j - 1], *b = &rolls[j];
            int c = strcmp(a->loaded, b->loaded);
            if (c > 0 || (c == 0 && a->number > b->number)) break;
            struct fos_lab_roll t = *a; *a = *b; *b = t;
        }
    return s.n;
}

/* ---------------------------------------------------------------- one roll's frames */

struct frames { struct fos_lab_frame *frames; int max, roll; const char *folder; };

static void frames_frame(void *ctx, int no, const char *file, const char *when, const char *fstop)
{
    struct frames *s = ctx;
    if (no < 1 || no > s->max) return;
    struct fos_lab_frame *f = &s->frames[no - 1];
    f->no = no;
    snprintf(f->cr2, sizeof(f->cr2), "%sDCIM/%s/%s", fos_port_card(), s->folder, file);
    snprintf(f->when, sizeof(f->when), "%s", when);
    snprintf(f->fstop, sizeof(f->fstop), "%s", fstop);
}

static void frames_log(void *ctx, const char *folder, const char *name, int number)
{
    struct frames *s = ctx;
    if (number != s->roll) return;
    char path[64];
    snprintf(path, sizeof(path), "%sDCIM/%s/%s", fos_port_card(), folder, name);
    s->folder = folder;
    struct log_reader rd = { 0, frames_frame, s };
    read_log(path, &rd);
}

int fos_lab_frames(int roll, struct fos_lab_frame *frames, int max)
{
    memset(frames, 0, sizeof(*frames) * max);
    struct frames s = { frames, max, roll, 0 };
    each_log(frames_log, &s);
    int n = 0;
    for (int i = 0; i < max; i++)
    {
        if (frames[i].no) n = i + 1;
        else frames[i].no = i + 1;
    }
    /* a frame whose file has gone (deleted on the camera, or never a RAW)
     * can't be developed */
    for (int i = 0; i < n; i++)
    {
        if (!frames[i].cr2[0]) { frames[i].missing = 1; continue; }
        void *f = fos_port_open(frames[i].cr2, 0);
        if (f) fos_port_close(f);
        else frames[i].missing = 1;
    }
    return n;
}
