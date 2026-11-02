/* sim: runs the film OS on a PC with a scripted dial, and saves what the
 * camera screen would show, frame by frame.
 *
 *   ./sim script.txt out/frames/name     (scripts in scenes/)
 *
 * A script line is "<ms> <key>" (key names below) or "<ms> end"; "#" starts
 * a comment. "roll <film> <shot> <exp>" before the first event puts a roll in
 * the camera, "open home|shelf|boot" picks the first screen.
 * Time is simulated: every presented frame costs FRAME_MS, so the result
 * plays at roughly the pace the camera would manage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../fos.h"
#include "../fos_ui.h"
#include "../fos_dev.h"
#include "../fos_studio.h"

void sim_fonts();             /* fonts.c */
void sim_films();             /* films.c */

#define FRAME_MS 22

static int now_ms;
static int frame_no;
static int frame_dirty;
static int last_dump = -1000;
static const char *outdir;
static uint8_t screen[FOS_W * FOS_H];

struct ev { int t; int key; };
static struct ev evs[256];
static int nev, next_ev, end_t = 4000;

static int settings[FS_COUNT] = {
    [FS_HUD] = 1, [FS_LEDS] = 1, [FS_FOCUS] = 1, [FS_WIND] = 1,
    [FS_APKEYS] = 0, [FS_LENS_WIDE] = 6, [FS_FSTOP] = 10,
    [FS_STARTUP] = 2, [FS_PLAYLOCK] = 1, [FS_BOOTANIM] = 1, [FS_DEBUG] = 0,
    [FS_FILM] = 0, [FS_EXP] = 0, [FS_THEME] = 0, [FS_PLATE] = 1, [FS_PORTRAIT] = 1, [FS_GRAIN] = 0, [FS_HALATION] = 0,
};
static struct fos_roll roll = { 1, 12, 1, 36, 14, 0, "2026-10-21 18:02", 3,
    { { 14, "21:47", "2" }, { 13, "21:40", "2" }, { 12, "21:33", "2.8" } } };

int fos_port_ms() { return now_ms; }

/* the palette on the screen now, 0xRRGGBB; each dumped frame gets a copy */
static uint32_t cur_pal[256];

static void dump()
{
    char path[512];
    if (!strcmp(outdir, "-")) { frame_no++; last_dump = now_ms; return; }   /* measuring only */
    snprintf(path, sizeof(path), "%s/f_%05d_%06d.idx", outdir, frame_no, now_ms);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(screen, 1, sizeof(screen), f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/f_%05d_%06d.pal", outdir, frame_no++, now_ms);
    f = fopen(path, "wb");
    if (f) { fwrite(cur_pal, 4, 256, f); fclose(f); }
    last_dump = now_ms;
}

void fos_port_sleep(int ms)
{
    /* FOS_FULL=1: every frame redrawn whole, to check the partial redraws */
    static int full = -1;
    if (full < 0) full = getenv("FOS_FULL") != 0;
    if (full) ui_clobbered = 1;
    if (frame_dirty)
    {
        now_ms += FRAME_MS;
        if (now_ms - last_dump >= 30) dump();
        frame_dirty = 0;
    }
    now_ms += ms;
}

int fos_port_key()
{
    if (next_ev < nev && evs[next_ev].t <= now_ms)
        return evs[next_ev++].key;
    return FK_NONE;
}

int fos_port_abort()
{
    return now_ms > end_t;
}

void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        memcpy(&screen[y * FOS_W], &rows[(y - y0) * pitch], FOS_W);
    frame_dirty = 1;
}

/* the palette as the camera would show it: through the screen colour
 * settings and Canon's conversion, and back */
void fos_port_palette()
{
    char path[512];
    if (!strcmp(outdir, "-")) return;
    uint32_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = i == 0 || i == 255 ? fos_pal[i] :
            settings[FS_SCR_STD] == 1 ? fos_pal[i] :   /* Rec.709: what a BT.601 screen shows is not modelled */
            fos_screen_rgb(fos_screen_yuv(i, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    memcpy(cur_pal, pal, sizeof(cur_pal));
    frame_dirty = 1;
    snprintf(path, sizeof(path), "%s/../../pal.bin", outdir);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(pal, 4, 256, f); fclose(f); }
}

void fos_port_photo(const uint8_t (*pal)[3], int n, int k)
{
    for (int i = 1; i < 255; i++)
    {
        const uint8_t *c = i <= n ? pal[i - 1] : i >= FOS_FIXED_BASE && i < FOS_FIXED_BASE + FOS_FIXED_N ?
                           fos_fixed_rgb[i - FOS_FIXED_BASE] : (const uint8_t *) "\0\0\0";
        uint32_t rgb = (uint32_t) (c[0] * k >> 8) << 16 | (uint32_t) (c[1] * k >> 8) << 8 | (uint32_t) (c[2] * k >> 8);
        cur_pal[i] = settings[FS_SCR_STD] == 1 ? rgb :
            fos_screen_rgb(fos_screen_yuv_rgb(rgb, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    }
    frame_dirty = 1;
}

void fos_port_log(const char *msg) { fprintf(stderr, "[%6d ms] %s\n", now_ms, msg); }
int  fos_port_dev_cap() { return 12; }

void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }

void fos_port_roll(struct fos_roll *r) { *r = roll; }
int  fos_port_get(int s) { return s >= 0 && s < FS_COUNT ? settings[s] : 0; }
void fos_port_set(int s, int v)
{
    if (s >= 0 && s < FS_COUNT) settings[s] = v;
    if (s == FS_SCR_STD || s == FS_SCR_SAT || s == FS_SCR_WARM || s == FS_SCR_BRIGHT) fos_port_palette();
}

void fos_port_action(int a, int x, int y)
{
    fprintf(stderr, "[%6d ms] action %d (%d, %d)\n", now_ms, a, x, y);
    if (a == FA_LOAD)
    {
        roll.loaded = 1; roll.number++; roll.film = x; roll.exposures = fos_exposure_choices[y];
        roll.shot = 0; roll.nrecent = 0;
        strcpy(roll.loaded_at, "2026-10-22 09:41");
    }
    if (a == FA_REWIND) roll.loaded = 0;
    if (a == FA_BUSY) fprintf(stderr, "[%6d ms] %s\n", now_ms, x ? "busy: developing" : "busy: done");
}

void fos_port_clock(char *buf, int size) { snprintf(buf, size, "09:41"); }
int  fos_port_lens_manual() { return 1; }
const char *fos_port_lens_name() { return "Manual lens"; }
#ifndef MFILM_VERSION
#define MFILM_VERSION "dev"
#endif
const char *fos_port_version() { return MFILM_VERSION; }
void fos_port_stat(const struct fos_stat *s) { }

static int key_of(const char *s)
{
    static const struct { const char *n; int k; } names[] = {
        { "left", FK_LEFT }, { "right", FK_RIGHT }, { "up", FK_UP }, { "down", FK_DOWN },
        { "wl", FK_WHEEL_L }, { "wr", FK_WHEEL_R }, { "set", FK_SET }, { "menu", FK_MENU },
        { "info", FK_INFO }, { "play", FK_PLAY }, { "home", FK_HOME },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].n)) return names[i].k;
    fprintf(stderr, "unknown key %s\n", s);
    exit(1);
}

void fos_port_bake_status(char *out,int size)
{
    const char *status=getenv("FOS_STATUS");snprintf(out,size,"%s",status?status:"");
}

/* the logo for the shutter animation, from the simulated card: ML/data/FOSLOGO.BIN */
const uint8_t *fos_port_logo(int *w,int *h)
{
    static uint8_t *bits;static int lw,lh,tried;
    if(!tried){tried=1;char path[512];snprintf(path,sizeof(path),"%sML/data/FOSLOGO.BIN",getenv("FOS_CARD")?getenv("FOS_CARD"):"out/card/");
     FILE *f=fopen(path,"rb");if(f){uint8_t head[8];if(fread(head,1,8,f)==8&&!memcmp(head,"FLG1",4)){lw=head[4]|head[5]<<8;lh=head[6]|head[7]<<8;int n=(lw+7)/8*lh;bits=malloc(n);if(bits&&fread(bits,1,n,f)!=(size_t)n){free(bits);bits=0;}}fclose(f);}}
    if(!bits)return 0;*w=lw;*h=lh;return bits;
}

void fos_port_bake_info(struct fos_bake_info *info)
{
    memset(info,0,sizeof(*info));info->remaining=-1;
    const char *queued=getenv("FOS_BAKE_QUEUED"),*pct=getenv("FOS_BAKE_PERCENT");
    if(!queued)return;
    info->queued=atoi(queued);info->active=info->queued>0;info->percent=pct?atoi(pct):0;
    info->paused=getenv("FOS_BAKE_PAUSED")!=0;
    const char *eta=getenv("FOS_BAKE_ETA");if(eta)info->remaining=atoi(eta);
    info->completed=2;snprintf(info->current,sizeof(info->current),"P0720041.JPG");
    info->next_count=info->queued>1?info->queued-1:0;if(info->next_count>3)info->next_count=3;
    for(int i=0;i<info->next_count;i++)snprintf(info->next[i],sizeof(info->next[0]),"IMG_%04d.JPG",42+i);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sim script outdir\n"); return 1; }
    outdir = argv[2];
    if (strcmp(outdir, "-")) mkdir(outdir, 0755);
    int open = FOS_OPEN_SHELF;
    if (getenv("FOS_THEME")) settings[FS_THEME] = atoi(getenv("FOS_THEME"));
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    char line[256];
    int hold = 0;
    while (fgets(line, sizeof(line), f))
    {
        char a[64], b[64];
        int t, n1, n2, n3;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "roll %d %d %d", &n1, &n2, &n3) == 3)
        {
            roll.loaded = 1; roll.film = n1; roll.shot = n2; roll.exposures = n3;
            continue;
        }
        if (!strncmp(line, "noroll", 6)) { roll.loaded = 0; continue; }
        if (sscanf(line, "open %63s", a) == 1)
        {
            open = !strcmp(a, "home") ? FOS_OPEN_HOME : !strcmp(a, "boot") ? FOS_OPEN_BOOT :
                   !strcmp(a, "darkroom") ? FOS_OPEN_DARKROOM : !strcmp(a, "prints") ? FOS_OPEN_PRINTS :
                   !strcmp(a, "end") ? FOS_OPEN_END : FOS_OPEN_SHELF;
            continue;
        }
        if (sscanf(line, "%d %63s", &t, b) == 2)
        {
            if (!strcmp(b, "end")) { end_t = t; continue; }
            int k = key_of(b);
            evs[nev].t = t; evs[nev].key = k; nev++;
            /* the dial's presses come with a release, like on the camera;
             * "hold" before a key keeps it down for 1.5 s */
            if (k >= FK_LEFT && k <= FK_DOWN)
            {
                evs[nev].t = t + (hold ? 1500 : 110); evs[nev].key = k | FK_RELEASE; nev++;
            }
            hold = 0;
        }
        else if (sscanf(line, "%d hold", &t) == 1) hold = 1;
    }
    fclose(f);
    for (int i = 1; i < nev; i++)
        for (int j = i; j > 0 && evs[j].t < evs[j - 1].t; j--)
        {
            struct ev tmp = evs[j]; evs[j] = evs[j - 1]; evs[j - 1] = tmp;
        }
    fos_ui_init();
    sim_fonts();
    sim_films();
    fos_port_palette();
    fos_ui_run(open);
    if (frame_dirty) dump();
    fprintf(stderr, "%d frames, %d ms\n", frame_no, now_ms);
    return 0;
}
