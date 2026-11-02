/* filmroll: a Canon camera as a film camera (built into this ML fork).
 * Made on the EOS M; 100D, 650D, 700D and others build from the same code.
 *
 * The camera side of the film OS (ml-dev/filmos): rolls, the roll log the
 * PC lab reads, film settings, the viewfinder HUD, and the fos_port_*
 * functions through which the film OS takes the screen and the buttons.
 *
 *  - the film OS is the camera's own interface: the film shelf at start
 *    up, home and its settings pages instead of the Magic Lantern menu.
 *    The ML menu is gone: what used to open it (two-finger tap, long
 *    press on DOWN, modules) opens home.
 *  - an SLR-viewfinder HUD in LiveView: aperture scale with meter needle,
 *    red LED readout that lights on half-shutter, frame counter, wind
 *  - PLAY opens the prints the camera has developed, never Canon's
 *    playback of undeveloped frames
 *  - a finished roll is developed in the camera (fos_dev.c): its RAWs
 *    become prints in ML/DARKROOM, shown on their own palettes
 *  - manual lenses: the f-stop is dialled in and logged for the lab
 *
 * Loading a roll sets picture style, contrast / saturation / colour tone,
 * white balance and ISO for that stock, and starts a roll log next to the
 * images (DCIM/xxxCANON/ROLL_nnn.TXT) that the PC film lab reads
 * (the desktop darkroom, Darkrooms). Film keys must match films/films.py.
 *
 * Shots are detected by watching the file number, so every frame is
 * logged even in burst mode.
 */

#include <dryos.h>
#include <module.h>
#include <property.h>
#include <menu.h>
#include <config.h>
#include <lens.h>
#include <picstyle.h>
#include <beep.h>
#include <bmp.h>
#include <fio-ml.h>
#include <fileprefix.h>
#include <notify_box.h>
#include <zebra.h>
#include <propvalues.h>
#include <gui-common.h>
#include <tasks.h>
#include <mem.h>
#include <powersave.h>
#include <vram.h>
#include <shoot.h>
#include <battery.h>
#include "filmroll.h"
#include "fos.h"
#include "fos_dev.h"
#include "fos_bake.h"
#include "fos_preview.h"
#include "fos_bench.h"
#include "fos_jpeg.h"
#include "tjpgd.h"
#include "fos_studio.h"

#include <stdarg.h>

extern int ml_started;
extern void CancelDateTimer();

/* user settings */
static CONFIG_INT("filmroll.film",      film_sel,         0);
static CONFIG_INT("filmroll.exp",       exp_sel,          0);
static CONFIG_INT("filmroll.hud",       hud_enabled,      0);
static CONFIG_INT("filmroll.focus",     focus_screen,     1);
static CONFIG_INT("filmroll.leds",      leds_on_half,     1);
static CONFIG_INT("filmroll.wind",      wind_mode,        0);   /* 0 off, 1 reminder, 2 block shutter */
static CONFIG_INT("filmroll.playlock",  play_lock,        0);
static CONFIG_INT("filmroll.startup",   startup_picker,   2);   /* 0 off, 1 when no film, 2 always */
static CONFIG_INT("filmroll.bootanim",  boot_anim,        0);
static CONFIG_INT("filmroll.colour",   ui_theme_cfg,     0);
static CONFIG_INT("filmroll.plate",     plate_enabled,    1);
static CONFIG_INT("filmroll.portrait",  portrait_cfg,     1);
static CONFIG_INT("filmroll.grain",     grain_cfg,        0);
static CONFIG_INT("filmroll.halation",  halation_cfg,     0);
static CONFIG_INT("filmroll.debug",     debug_log,        1);
static CONFIG_INT("filmroll.lens_wide", lens_widest,      6);   /* index into fos_fstops: f/1.8 */
static CONFIG_INT("filmroll.fstop",     manual_fstop,     6);
static CONFIG_INT("filmroll.ap_keys",   aperture_keys,    0);   /* 0 wheel in Av, 1 up/down, 2 menu only */
static CONFIG_INT("filmroll.scr_std",   scr_std,          0);   /* screen colour: 0 Canon, 1 Rec.709 */
static CONFIG_INT("filmroll.scr_sat",   scr_sat,          0);   /* -6..6 */
static CONFIG_INT("filmroll.scr_warm",  scr_warm,         0);
static CONFIG_INT("filmroll.scr_bright", scr_bright,      0);

/* roll state, kept across power cycles */
static CONFIG_INT("filmroll.loaded",    roll_loaded,      0);
static CONFIG_INT("filmroll.roll",      roll_number,      0);
static CONFIG_INT("filmroll.roll_film", roll_film,        0);
static CONFIG_INT("filmroll.roll_exp",  roll_exposures,   36);
static CONFIG_INT("filmroll.shot",      frames_shot,      0);
static CONFIG_INT("filmroll.lastfile",  last_file_number, -1);
static CONFIG_INT("filmroll.logdir",    roll_folder,      0);
static CONFIG_INT("filmroll.needwind",  need_wind,        0);

static char log_path[64];

static volatile int leds_until = 0;     /* keep LEDs lit until this ms clock */
static volatile int hud_dirty = 1;

#define ROLL_FINISHED (roll_loaded && frames_shot >= roll_exposures)
#define ROLL_IN_PROGRESS (roll_loaded && frames_shot < roll_exposures)

static volatile int end_due = 0;        /* the roll was just finished: ms when to ask about it */
static volatile int dev_busy = 0;       /* developing: keep the camera awake */

/* the last frames, for the roll page (also read back from the log) */
static struct { int no; char time[6]; char fstop[6]; } recent[FOS_RECENT];
static int nrecent = 0;
static char loaded_at[20];
static int recent_valid = 0;

/* ---------------------------------------------------------------- debug log */

/* field-test trace in ML/FILMROLL.LOG; turn off in System once things work */
static struct semaphore *film_log_lock;
static void dbg(const char *fmt, ...)
{
    if (!debug_log)
        return;

    char buf[192];
    int n = snprintf(buf, sizeof(buf), "%7d  ", get_ms_clock());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    if(!film_log_lock)film_log_lock=create_named_semaphore("film_log",SEM_CREATE_UNLOCKED);
    if(!film_log_lock||take_semaphore(film_log_lock,0)!=0)return;
    FILE *f = FIO_CreateFileOrAppend("ML/FILMROLL.LOG");
    if (!f){give_semaphore(film_log_lock);return;}
    my_fprintf(f, "%s\n", buf);
    FIO_CloseFile(f);
    give_semaphore(film_log_lock);
}

/* The camera waiting for a picture, the screen the film OS opens from.
 * On the EOS M that is LiveView; a DSLR (100D, 650D, 700D ...) is mostly
 * shot through its optical viewfinder, so there it is also Canon's shooting
 * screen with LiveView off. */
#ifdef CONFIG_EOSM
#define FOS_LV_ONLY 1
#else
#define FOS_LV_ONLY 0
#endif
static int shooting_screen()
{
    return (lv || !FOS_LV_ONLY) && display_idle() && !is_movie_mode();
}
static int plate_visible();

#define DBG_STATE(what) dbg("%s: ml=%d lv=%d idle=%d movie=%d mode=%d gui=%d", what, \
                            ml_started, lv, display_idle(), is_movie_mode(), shooting_mode, CURRENT_GUI_MODE)

/* ---------------------------------------------------------------- log file */

static void update_log_path()
{
    snprintf(log_path, sizeof(log_path), "%s:/DCIM/%03d%s/ROLL_%03d.TXT",
             get_shooting_card()->drive_letter, roll_folder, get_dcim_dir_suffix(),
             roll_number);
}

static void log_append(const char *text)
{
    FILE *f = FIO_CreateFileOrAppend(log_path);
    if (!f)
    {
        NotifyBox(3000, "filmroll: can't write %s", log_path);
        return;
    }
    my_fprintf(f, "%s", text);
    FIO_CloseFile(f);
}

static void format_now(char *buf, int size)
{
    struct tm now;
    LoadCalendarFromRTC(&now);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);
}

static void write_header(const char *note)
{
    char when[32];
    char header[320];
    format_now(when, sizeof(when));
    snprintf(header, sizeof(header),
             "# ML film mode roll log (filmroll module)%s\n"
             "roll = %03d\nfilm = %s\nexposures = %d\nloaded = %s\n\n",
             note, roll_number, fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].key, roll_exposures, when);
    log_append(header);
}

/* the roll page wants the loading time and the last frames: after a power
 * cycle they only exist in the log, so read them back once */
static void read_back_log()
{
    recent_valid = 1;
    nrecent = 0;
    loaded_at[0] = 0;
    int size = 0;
    char *buf = (char *) read_entire_file(log_path, &size);
    if (!buf)
        return;
    for (char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "loaded = ", 9))
            fos_copy(loaded_at, sizeof(loaded_at), line + 9, 16);
        else if (line[0] >= '0' && line[0] <= '9')
        {
            /* "12  IMG_0012.CR2  2026-08-08 09:15:15  f/2.8" */
            char *tok[5];
            int n = 0;
            for (char *p = line; *p && n < 5; )
            {
                while (*p == ' ') p++;
                if (!*p) break;
                tok[n++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = 0;
            }
            if (n >= 4)
            {
                /* newest first, keep FOS_RECENT */
                for (int i = MIN(nrecent, FOS_RECENT - 1); i > 0; i--)
                    recent[i] = recent[i - 1];
                recent[0].no = atoi(tok[0]);
                fos_copy(recent[0].time, sizeof(recent[0].time), tok[3], 5);
                snprintf(recent[0].fstop, sizeof(recent[0].fstop), "%s",
                         n >= 5 && !strncmp(tok[4], "f/", 2) ? tok[4] + 2 : "?");
                nrecent = MIN(nrecent + 1, FOS_RECENT);
            }
        }
        line = next;
    }
    fio_free(buf);
}

/* ---------------------------------------------------------------- camera settings */

static int iso_to_raw(int iso)
{
    int raw = 72;                       /* ISO 100 */
    for (int i = 100; i < iso; i *= 2)
        raw += 8;                       /* one stop */
    return raw;
}

static void apply_film(int index)
{
    const struct fos_film *f = &fos_films[COERCE(index, 0, fos_nfilms - 1)];

    int ps = f->picstyle;
    prop_request_change_wait(PROP_PICTURE_STYLE, &ps, 4, 1000);
    msleep(100);

    /* these change the current picture style, so it must be selected first */
    lens_set_contrast(f->contrast);
    msleep(50);
    lens_set_sharpness(f->sharpness);
    msleep(50);
    if (f->picstyle != PS_MONO)
    {
        lens_set_saturation(f->saturation);
        msleep(50);
        lens_set_color_tone(f->color_tone);
        msleep(50);
    }

    lens_set_kelvin(f->kelvin);
    lens_set_wbs_gm(f->wbs_gm);
    lens_set_wbs_ba(f->wbs_ba);
    lens_set_rawiso(iso_to_raw(f->iso));
}

/* Film is shot blind: the darkroom develops the RAWs, and there is no
 * image review to peek at. ML doesn't change these Canon settings itself
 * (writing the picture quality property can upset Canon's firmware), so
 * it asks for them. */
static void film_capture_check()
{
    if (!roll_loaded) return;
    int raw = (pic_quality & 0xFE00FF) == (PICQ_RAW & 0xFE00FF);
    int review_off = image_review_time == 0;
    if (raw && review_off) return;
    dbg("capture: quality %x, review %d", pic_quality, image_review_time);
    NotifyBox(6000, "Film: in Canon's menu set %s%s%s", raw ? "" : "Image quality RAW",
              !raw && !review_off ? " and " : "", review_off ? "" : "Image review Off");
}

/* ---------------------------------------------------------------- aperture */

/* manual (adapted) lenses report no aperture; the user dials it in instead */
static int lens_is_manual()
{
    return !lens_info.raw_aperture;
}

/* index into fos_fstops for the aperture in use */
static int current_fstop()
{
    if (lens_is_manual())
        return COERCE(manual_fstop, lens_widest, fos_nfstops - 1);

    int f100 = lens_info.aperture * 10;         /* lens_info.aperture is f x10 */
    int best = 0;
    for (int i = 1; i < fos_nfstops; i++)
        if (ABS(fos_fstops[i] - f100) < ABS(fos_fstops[best] - f100))
            best = i;
    return best;
}

static void step_manual_fstop(int dir)
{
    manual_fstop = COERCE(current_fstop() + dir, lens_widest, fos_nfstops - 1);
}

/* ---------------------------------------------------------------- rolls */

/* start a roll: state and log now, camera settings once the film OS has
 * let go of the screen (apply_film blocks for a while) */
static void load_roll(int film, int exposures)
{
    roll_film = COERCE(film, 0, fos_nfilms - 1);
    roll_exposures = exposures;
    roll_folder = get_shooting_card()->folder_number;

    /* next roll number whose log doesn't exist yet, so a reset counter
     * (new settings file, card from another camera) never mixes two rolls */
    for (int tries = 0; tries < 999; tries++)
    {
        roll_number = roll_number % 999 + 1;
        update_log_path();
        if (!is_file(log_path))
            break;
    }
    frames_shot = 0;
    need_wind = 0;
    last_file_number = get_shooting_card()->file_number;
    roll_loaded = 1;
    update_log_path();
    write_header("");

    char when[32];
    format_now(when, sizeof(when));
    fos_copy(loaded_at, sizeof(loaded_at), when, 16);
    nrecent = 0;
    recent_valid = 1;

    hud_dirty = 1;
    dbg("roll %03d loaded: %s, %d exp", roll_number, fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].key, roll_exposures);
}

static void rewind_roll()
{
    if (!roll_loaded) return;
    char when[32], line[80];
    format_now(when, sizeof(when));
    snprintf(line, sizeof(line), "\n# rewound %s after %d of %d frames\n", when, frames_shot, roll_exposures);
    log_append(line);
    roll_loaded = 0;
    need_wind = 0;
    hud_dirty = 1;
    dbg("roll %03d rewound at %d/%d", roll_number, frames_shot, roll_exposures);
}

static void record_frame(int file_number)
{
    if (frames_shot >= roll_exposures)
    {
        beep();
        return;
    }

    frames_shot++;

    char when[32];
    char aperture[8];
    char line[112];
    format_now(when, sizeof(when));
    fos_format_fstop(aperture, sizeof(aperture), current_fstop());
    snprintf(line, sizeof(line), "%d  %s%04d.CR2  %s  f/%s\n",
             frames_shot, get_file_prefix(), file_number, when, aperture);
    log_append(line);

    for (int i = MIN(nrecent, FOS_RECENT - 1); i > 0; i--)
        recent[i] = recent[i - 1];
    recent[0].no = frames_shot;
    fos_copy(recent[0].time, sizeof(recent[0].time), when + 11, 5);
    snprintf(recent[0].fstop, sizeof(recent[0].fstop), "%s", aperture);
    nrecent = MIN(nrecent + 1, FOS_RECENT);

    leds_until = get_ms_clock() + 2500;
    hud_dirty = 1;

    if (frames_shot == roll_exposures)
    {
        need_wind = 0;
        beep_times(2);
        end_due = get_ms_clock() + 1500;        /* then ask: develop it now? */
        return;
    }

    if (wind_mode)
        need_wind = 1;
}

/* log every new file number since the last poll */
/* Find the actual JPEG rather than assuming its prefix/card matches RAW. */
static int find_capture_jpeg(char *path, int size, int number)
{
    for(int drive=0;drive<2;drive++) {
        char root[16];snprintf(root,sizeof(root),"%c:/DCIM/",'A'+drive);
        struct fio_file dir; struct fio_dirent *dh=FIO_FindFirstEx(root,&dir);
        if(IS_ERROR(dh))continue;
        do {
            if(!(dir.mode & ATTR_DIRECTORY) || dir.name[0]=='.')continue;
            char folder[64];snprintf(folder,sizeof(folder),"%s%s/",root,dir.name);
            struct fio_file file;struct fio_dirent *fh=FIO_FindFirstEx(folder,&file);
            if(IS_ERROR(fh))continue;
            int found=0;
            do {
                int n=strlen(file.name);
                if(n<8 || strcasecmp(file.name+n-4,".JPG"))continue;
                char digits[5];memcpy(digits,file.name+n-8,4);digits[4]=0;
                if(atoi(digits)!=number)continue;
                snprintf(path,size,"%s%s",folder,file.name);found=1;break;
            }while(FIO_FindNextEx(fh,&file)==0);
            FIO_FindClose(fh);
            if(found){FIO_FindClose(dh);return 1;}
        }while(FIO_FindNextEx(dh,&dir)==0);
        FIO_FindClose(dh);
    }
    return 0;
}
#define BAKE_QUEUE 24
static volatile int ui_running;
struct bake_job {volatile int state;int number,film,observed_size,stable_since,ready_at,quick_ok,created_ms;char source[96],dest[64];};
static struct bake_job bake_jobs[BAKE_QUEUE];
static volatile unsigned bake_head,bake_tail;
static volatile int bake_active,bake_done,bake_total,bake_start,bake_paused_ms,bake_paused;
static volatile int review_pending,bake_revision;
static volatile int bake_completed,bake_failed;
int fos_port_bake_revision(void){return bake_revision;}
void fos_port_live_started(void){review_pending=1;}
static char review_file[13];
static void open_ui(int screen);
const char *fos_port_review_name(void){return review_file;}
int fos_port_background_abort(void){return ml_shutdown_requested;}
int fos_port_bake_yield(void){
 int start=get_ms_clock();
 while(!ml_shutdown_requested&&(get_halfshutter_pressed()||!job_state_ready_to_take_pic())){
  bake_paused=1;msleep(20);
 }
 bake_paused_ms+=get_ms_clock()-start;bake_paused=0;
 return ml_shutdown_requested;
}
/* Time left, from how fast the last few seconds went (not the whole bake so far: a slow start while previews were being made
 * used to give estimates of many minutes). Paused time is left out. Rows per second x 16; 0 = not known yet. */
/* The queue can hold BAKE_QUEUE pictures, so head - tail can never be more. If it ever is (a corrupted counter, seen once as
 * "552 queued" on the camera), say so in the log and start the queue again instead of showing or acting on the number. */
static unsigned bake_queued(void){
 unsigned head=bake_head,tail=bake_tail;
 if(head-tail>BAKE_QUEUE){
  static int logged;
  if(!logged){logged=1;dbg("bake queue: impossible count, head %d tail %d (cap %d): queue reset",(int)head,(int)tail,BAKE_QUEUE);}
  bake_tail=head;return 0;
 }
 return head-tail;
}
static int eta_t,eta_d,eta_rate;
static int bake_eta_s(void){
 if(eta_rate<=0||bake_total<=bake_done)return -1;
 int s=(int)((int64_t)(bake_total-bake_done)*16/eta_rate);
 return s>5999?5999:s;
}
void fos_port_bake_progress(int done,int total){
 bake_done=done;bake_total=total;
 int t=get_ms_clock()-bake_paused_ms;
 if(!eta_t||done<eta_d){eta_t=t;eta_d=done;eta_rate=0;}
 else if(t-eta_t>=500){
  int r=(int)((int64_t)(done-eta_d)*16000/(t-eta_t));
  eta_rate=eta_rate?(eta_rate*3+r)/4:r;eta_t=t;eta_d=done;
 }
}
void fos_port_bake_status(char *out,int size){
 unsigned queued=bake_queued();
 if(!queued){out[0]=0;return;}
 if(!bake_active){snprintf(out,size,"Preparing preview  |  %d queued",queued);return;}
 int pct=bake_total?bake_done*100/bake_total:0;
 if(bake_paused){snprintf(out,size,"Bake paused for capture  |  %d queued",queued);return;}
 int remain=bake_eta_s();
 if(pct>=5&&remain>=0){
  snprintf(out,size,"Bake %d%c  ~%ds left  |  %d queued",pct,'%',remain,queued);   /* not "%%": the camera's snprintf takes an argument for it, shifting every number after it */
 }else snprintf(out,size,"Bake %d%c  estimating...  |  %d queued",pct,'%',queued);
}
void fos_port_bake_info(struct fos_bake_info *info){
 memset(info,0,sizeof(*info));info->remaining=-1;
 info->completed=bake_completed;info->failed=bake_failed;
 unsigned count=bake_queued(),tail=bake_tail;
 if(!count)return;
 info->queued=count;info->active=bake_active;info->paused=bake_paused;
 if(info->active&&bake_total>0){
  int done=bake_done,total=bake_total;info->percent=COERCE(done*100/total,0,100);
  if(info->percent>=5&&done>0&&!info->paused)info->remaining=bake_eta_s();
 }
 for(unsigned i=0;i<count && i<4;i++){
  struct bake_job *job=&bake_jobs[(tail+i)%BAKE_QUEUE];
  const char *path=job->dest[0]?job->dest:job->source;
  const char *name=strrchr(path,'/');name=name?name+1:path;
  if(i==0)snprintf(info->current,sizeof(info->current),"%s",name);
  else{snprintf(info->next[info->next_count],sizeof(info->next[0]),"%s",name);info->next_count++;}
 }
}
int fos_port_bake_pending(const char *name){
 for(unsigned i=bake_tail;i<bake_head;i++){
  struct bake_job *job=&bake_jobs[i%BAKE_QUEUE];const char *base=strrchr(job->dest,'/');
  if(base && !strcmp(base+1,name) && job->state!=4)return 1;
 }return 0;
}
static void poll_frames(void){
 static int previous=-1;
 int number=get_shooting_card()->file_number;
 if(previous<0){previous=number;return;}
 if(number==previous)return;
 int count=number-previous;if(count<0)count+=9999;
 if(count>50){previous=number;return;}
 for(int n=0;n<count;n++){
  int captured=(previous+n)%9999+1;
  if(bake_head-bake_tail>=BAKE_QUEUE){NotifyBox(3000,"Bake queue full: Canon originals kept");dbg("bake queue full: frame %d",captured);continue;}
  struct bake_job *job=&bake_jobs[bake_head%BAKE_QUEUE];memset(job,0,sizeof(*job));
  job->number=captured;job->film=COERCE(film_sel,0,fos_nfilms-1);job->observed_size=-1;job->created_ms=get_ms_clock();
  snprintf(job->source,sizeof(job->source),"%s:/DCIM/%03d%s/%s%04d.JPG",get_shooting_card()->drive_letter,get_shooting_card()->folder_number,get_dcim_dir_suffix(),get_file_prefix(),captured);
  bake_head++;
  dbg("bake queue: frame %d added, head %d tail %d",captured,(int)bake_head,(int)bake_tail);
 }
 previous=number;
}
static int job_ready(struct bake_job *job){
 if(get_halfshutter_pressed()||!job_state_ready_to_take_pic())return 0;
 if(!job->ready_at)job->ready_at=get_ms_clock();
 if(!is_file(job->source)&&!find_capture_jpeg(job->source,sizeof(job->source),job->number)){
  if(get_ms_clock()-job->ready_at>8000){dbg("JPEG missing: %s",job->source);NotifyBox(4000,"Bake: JPEG missing, RAW retained");job->state=4;}
  return 0;
 }
 int size=FIO_GetFileSize_direct(job->source);
 if(size!=job->observed_size){job->observed_size=size;job->stable_since=get_ms_clock();return 0;}
 if(size<4||get_ms_clock()-job->stable_since<200)return 0;
 char dir[24];snprintf(dir,sizeof(dir),"%sPIXELJPG",fos_port_card());FIO_CreateDirectory(dir);
 for(int i=0;i<10000;i++){
  snprintf(job->dest,sizeof(job->dest),"%s/P%03d%04d.JPG",dir,job->film,(job->number+i)%10000);
  char cache[64];snprintf(cache,sizeof(cache),"%s",job->dest);memcpy(cache+strlen(cache)-4,".BVP",5);
  if(!is_file(job->dest)&&!is_file(cache))return 1;
 }
 job->state=4;NotifyBox(4000,"Bake output folder full");return 0;
}
static void quick_preview_task(void *unused){
 (void)unused;
 while(!ml_started)msleep(100);
 TASK_LOOP {
  for(unsigned index=bake_tail;index<bake_head;index++){
   struct bake_job *job=&bake_jobs[index%BAKE_QUEUE];
   if(job->state||!job_ready(job))continue;
   job->state=1;int start=get_ms_clock();
   {const char *name=strrchr(job->dest,'/');snprintf(review_file,sizeof(review_file),"%s",name?name+1:job->dest);}
   int live_serial=fos_live.serial;                                     /* the review opens as soon as rows are coming (fos_port_live_started) */
   job->quick_ok=fos_quick_preview(job->source,job->dest,job->film);
   dbg("quick preview: %s result %d %d ms (queued %d ms before it started)",job->source,job->quick_ok,get_ms_clock()-start,start-job->created_ms);
   if(job->quick_ok&&fos_live.serial==live_serial)review_pending=1;   /* no live build happened: open it now */
   job->state=2;
   break;
  }
  msleep(30);
 }
}
static void background_bake_task(void *unused){
 (void)unused;
 while(!ml_started)msleep(100);
 TASK_LOOP {
  if(bake_tail==bake_head){msleep(50);continue;}
  struct bake_job *job=&bake_jobs[bake_tail%BAKE_QUEUE];
  if(job->state==4){bake_failed++;bake_tail++;continue;}
  if(job->state!=2){msleep(30);continue;}
  job->state=3;bake_active=1;bake_done=0;bake_total=1;eta_t=0;eta_rate=0;bake_paused_ms=0;bake_start=get_ms_clock();
  powersave_prohibit();int ok=fos_bake(job->source,job->dest,job->film);powersave_permit();
  dbg("background bake: %s -> %s result %d error %s; %d ms from shutter to done, %d ms of it paused for Canon/half-press",job->source,job->dest,ok,fos_bake_error(),get_ms_clock()-job->created_ms,bake_paused_ms);
  if(!ok&&!ml_shutdown_requested)NotifyBox(5000,"Bake: %s",fos_bake_error());
  if(ok&&!job->quick_ok){const char *name=strrchr(job->dest,'/');snprintf(review_file,sizeof(review_file),"%s",name?name+1:job->dest);review_pending=1;}
  if(ok)bake_completed++;else bake_failed++;
  bake_active=0;job->state=4;bake_revision++;bake_tail++;dbg("bake queue: done, head %d tail %d",(int)bake_head,(int)bake_tail);msleep(10);
 }
}
static void bake_overlay(void){
 static int last,last_bar,visible,fill16;int now=get_ms_clock();
 if(ui_running||!shooting_screen()||get_halfshutter_pressed()||!job_state_ready_to_take_pic())return;
 if(plate_visible())return;                     /* the back screen has its own bake bar and status line */
 if(now-last_bar<125)return;
 last_bar=now;
 struct fos_bake_info bi;fos_port_bake_info(&bi);
 uint8_t *v=bmp_vram_real();if(!v)return;
 /* a slim bar above the text: red fill easing toward the real percent, a bright sweep, white leading edge */
 const int X=12,W=560,Y=446,H=5;
 if(bi.queued){
  for(int y=Y;y<Y+H;y++)memset(v+y*BMPPITCH+X,COLOR_BLACK,W);
  int target=bi.paused?fill16:bi.percent*16;
  if(!bi.active&&!bi.paused){int span=W-80,t=(now/6)%(2*span),pos=t<span?t:2*span-t;for(int y=Y;y<Y+H;y++)memset(v+y*BMPPITCH+X+pos,COLOR_RED,80);fill16=0;}
  else{
   if(target<fill16)fill16=target;else if(target>fill16){int st=(target-fill16)/4;fill16+=st?st:1;}
   int fill=W*fill16/1600;if(fill<4&&bi.percent>0)fill=4;
   for(int y=Y;y<Y+H;y++)memset(v+y*BMPPITCH+X,bi.paused&&((now/500)&1)?COLOR_GRAY(40):COLOR_RED,fill);
   if(!bi.paused&&fill>10){int cycle=fill+80,c=(now/4)%cycle-40;
    for(int y=Y;y<Y+H;y++)for(int dx=-14;dx<=14;dx++){int x=c+dx;if(x>=0&&x<fill-2&&((dx>-6&&dx<6)||((x+y)&1)==0))v[y*BMPPITCH+X+x]=COLOR_WHITE;}}
   if(fill>=2)for(int y=Y;y<Y+H;y++)memset(v+y*BMPPITCH+X+fill-2,COLOR_WHITE,2);
  }
  if(bi.active||bi.paused)bmp_printf(FONT_SMALL,X+W+10,Y-8,"%3d%%",fill16/16);
  visible=1;
 }
 if(now-last>=500){
  last=now;
  char text[80];fos_port_bake_status(text,sizeof(text));
  if(text[0]){bmp_printf(FONT_SMALL,12,456,"%s",text);visible=1;}
  else if(visible){
   for(int y=438;y<480;y++)memset(v+y*BMPPITCH+12,0,660);
   visible=0;fill16=0;
  }
 }
}

int fos_bake_remove(const char *path) { return FIO_RemoveFile(path); }

/* ---------------------------------------------------------------- the film OS host */

static volatile int ui_running = 0;     /* the film OS owns the screen and the buttons */
static volatile int ui_abort = 0;
static int ui_after = 0;                /* what to do once it has let go */
static int ui_last_mode_poke = 0;
static int ui_last_palette = 0;

enum { AFTER_APPLY = 1, AFTER_CANON_MENU = 2 };

/* buttons, from the GUI task to the film OS task */
#define KEYQ 32
static volatile int keyq[KEYQ];
static volatile int keyq_in = 0, keyq_out = 0;

static void keyq_push(int k)
{
    int next = (keyq_in + 1) % KEYQ;
    if (next != keyq_out)
    {
        keyq[keyq_in] = k;
        keyq_in = next;
    }
}

/* Our palette is fos_pal (RGB); the bitmap overlay wants Y, Cb, Cr and
 * an opacity code, which we copy from Canon's own white. One entry is
 * remembered so we notice when other code puts Canon's palette back. */
#define PALETTE_PROBE 200
static uint32_t palette_probe = 0;

/* a photograph on the screen: its colours in 1..photo_n, at photo_k/256 */
static int photo_mode = 0, photo_n = 0, photo_k = 256;
static uint8_t photo_pal[FOS_PRINT_COLOURS][3];

static void palette_ours_range(int first, int last);
static void palette_ours()
{
    palette_ours_range(1, 255);
}

static void palette_ours_range(int first, int last)
{
    if (!DISPLAY_IS_ON) return;
    /* Canon's white tells the opacity and where white sits in luma
     * (its own conversion puts it at 235, not 255) */
    uint32_t white = LCD_Palette[3 * COLOR_WHITE + 2];
    uint32_t opaque = white & 0xFF000000;
    if ((opaque >> 24) < 2) opaque = 0x03000000;
    int white_y = (white >> 16) & 0xFF;
    if (white_y < 180) white_y = 235;
    for (int i = first; i < last && i < 255; i++)
    {
        uint32_t entry;
        if (!photo_mode)
            entry = opaque | fos_screen_yuv(i, white_y, scr_std, scr_sat, scr_warm, scr_bright);
        else
        {
            const uint8_t *c = i <= photo_n ? photo_pal[i - 1] :
                               i >= FOS_FIXED_BASE && i < FOS_FIXED_BASE + FOS_FIXED_N ? fos_fixed_rgb[i - FOS_FIXED_BASE] : 0;
            uint32_t rgb = c ? (uint32_t) (c[0] * photo_k >> 8) << 16 | (uint32_t) (c[1] * photo_k >> 8) << 8 | (uint32_t) (c[2] * photo_k >> 8) : 0;
            entry = opaque | fos_screen_yuv_rgb(rgb, white_y, scr_std, scr_sat, scr_warm, scr_bright);
        }
        if (i == PALETTE_PROBE) palette_probe = entry;
        EngDrvOut(LCD_Palette[3 * i], entry);
        EngDrvOut(LCD_Palette[3 * i + 0x300], entry);
    }
}

static void palette_canon()
{
    for (int i = 1; i < 255; i++)
    {
        EngDrvOut(LCD_Palette[3 * i], LCD_Palette[3 * i + 2]);
        EngDrvOut(LCD_Palette[3 * i + 0x300], LCD_Palette[3 * i + 2]);
    }
}

/* Take the display from Canon's GUI the same way the ML menu does:
 * switch Canon to the ML menu GUI mode (LiveView stops drawing its
 * overlays and stops handling keys), stop Canon from flipping its own
 * bitmap buffer, then turn the bitmap layer back on for us. */
/* The logo (ML/DATA/FOSLOGO.BIN: "FLG1", width, height as u16, then 1 bit per pixel, rows padded
 * to bytes), white on black, drawn as soon as the film OS takes the screen so the switch from
 * Canon's own screen is not seen. Without the file the screen just goes black. */
static uint8_t *logo_bits;
static int logo_w, logo_h, logo_tried;

static void logo_load()
{
    if (!logo_tried)
    {
        logo_tried = 1;
        int size = 0;
        uint8_t *raw = read_entire_file("ML/DATA/FOSLOGO.BIN", &size);
        if (raw)
        {
            int w = size > 8 ? raw[4] | raw[5] << 8 : 0, h = size > 8 ? raw[6] | raw[7] << 8 : 0;
            int row = (w + 7) / 8;
            if (!memcmp(raw, "FLG1", 4) && w > 0 && w <= 720 && h > 0 && h <= 480 && size >= 8 + row * h)
            {
                logo_bits = malloc(row * h);
                if (logo_bits) { memcpy(logo_bits, raw + 8, row * h); logo_w = w; logo_h = h; }
            }
            fio_free(raw);
        }
        dbg("logo: %s", logo_bits ? "loaded" : "ML/DATA/FOSLOGO.BIN missing or invalid");
    }
}

/* the logo mask for the shutter animation (1 bit per pixel, rows padded to bytes), or 0 */
const uint8_t *fos_port_logo(int *w, int *h)
{
    logo_load();
    if (!logo_bits) return 0;
    *w = logo_w; *h = logo_h;
    return logo_bits;
}

static int film_logo_draw(int with_logo)
{
    logo_load();
    uint8_t *v = bmp_vram_real();
    if (!v) return 0;
    for (int y = 0; y < 480; y++) memset(v + y * BMPPITCH, COLOR_BLACK, 720);
    if (!with_logo || !logo_bits) return 1;
    /* half size, smoothed, exactly as the shutter animation draws it (fos_ui.c shutter_fill), so the logo does not change size
     * when the film OS takes over: each pixel covers 2 x 2 of the mask */
    int row = (logo_w + 7) / 8, dw = logo_w / 2, dh = logo_h / 2, ox = (720 - dw) / 2, oy = (480 - dh) / 2;
#define LOGO_BIT(x, y) ((logo_bits[(y) * row + ((x) >> 3)] & (0x80 >> ((x) & 7))) != 0)
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
        {
            int n = LOGO_BIT(2 * x, 2 * y) + LOGO_BIT(2 * x + 1, 2 * y) + LOGO_BIT(2 * x, 2 * y + 1) + LOGO_BIT(2 * x + 1, 2 * y + 1);
            if (n) v[(oy + y) * BMPPITCH + ox + x] = n == 4 ? COLOR_WHITE : COLOR_GRAY(n * 25);
        }
#undef LOGO_BIT
    return 1;
}

static void film_ui_open(int open)
{
    /* the logo only when the main menu opens from Canon's screen, not for a shortcut into Images or the shelf */
    const int show_logo = open == FOS_OPEN_HOME;
    const int t0 = get_ms_clock();
    NotifyBoxHide();
    dbg("open %d: lv %d display %d bmp %d vram %x", open, lv, DISPLAY_IS_ON, bmp_is_on(), (unsigned) bmp_vram_real());
    int covered = 0;
    if (!lv && DISPLAY_IS_ON)                        /* every open from Canon's own screen, the boot shelf too, is covered: black, and the logo for the main menu */
    {
        if (!bmp_is_on()) bmp_on();                  /* the first open after boot: the layer may not be showing yet */
        covered = film_logo_draw(show_logo);
    }
    if (lv) bmp_off();
    SetGUIRequestMode(GUIMODE_ML_MENU);
    msleep(200);
    canon_gui_disable_front_buffer();
    CancelDateTimer();
    /* clrscr() leaves the layer transparent, and the video layer behind it holds stale memory (noise, 5D III 2026-11-01), so
     * with the logo cover up it is not cleared at all: the cover is simply drawn again. */
    if (covered) film_logo_draw(show_logo); else clrscr();
    const int t1 = get_ms_clock();
    bmp_on();
    const int t2 = get_ms_clock();
    if (show_logo)
    {
        film_logo_draw(1);                          /* and again once the bitmap layer is ours */
        if (logo_bits) msleep(450);                 /* a moment to read it before the shutter opens */
    }
    ui_last_mode_poke = get_ms_clock();
    dbg("open %d: cover %d, switch %d ms, layer on %d ms, total %d ms", open, covered, t1 - t0, t2 - t1, ui_last_mode_poke - t0);
}

static void film_ui_close()
{
    palette_canon();
    if (lv) bmp_off();
    SetGUIRequestMode(0);
    msleep(100);
    canon_gui_enable_front_buffer(0);
    clrscr();
    redraw();
    if (lv) bmp_on();
}

/* frame times are kept while the film OS runs and logged when it closes:
 * writing the card in the middle of an animation would stall it */
static struct fos_stat stat_all;

static void ui_task(int open)
{
    keyq_in = keyq_out = 0;
    ui_abort = 0;
    ui_after = 0;
    film_ui_open(open);
    DBG_STATE("film os: open");
    memset(&stat_all, 0, sizeof(stat_all));

    fos_ui_run(open);

    film_ui_close();
    ui_running = 0;
    hud_dirty = 1;
    DBG_STATE("film os: closed");
    if (stat_all.frames)
    {
        struct fos_stat *s = &stat_all;
        int n = s->frames, total = s->draw_ms + s->present_ms + s->rest_ms;
        dbg("film os: %d frames in %d ms = %d/s; per frame %d ms draw (%d canisters) + %d present + %d rest, worst %d, open %d ms, quality %d",
            n, total, total ? n * 1000 / total : 0, s->draw_ms / n, s->cans_ms / n, s->present_ms / n, s->rest_ms / n,
            s->worst_ms, s->open_ms, fos_can_quality);
    }

    if (ui_after & AFTER_APPLY)
    {
        /* Display-referred LUT: keep Canon exposure, WB and picture style. */
        beep();
        film_capture_check();
    }
    if (dev_busy)
    {
        /* left in the middle of developing */
        dev_busy = 0;
        powersave_permit();
    }
    if (ui_after & AFTER_CANON_MENU)
        fake_simple_button(BGMT_MENU);
}

static void open_ui(int screen)
{
    if (ui_running)
        return;
    if (!fos_fonts_ready())
    {
        NotifyBox(5000, "Film OS: ML/DATA/" FOS_FONT_FILE " is missing");
        return;
    }
    ui_running = 1;
    /* the same priority as ML's own menu redraw: Canon's tasks still come
     * first, and the film OS rests between frames */
    task_create("film_os", 0x1a, 0x4000, ui_task, (void *) screen);
}

/* ---- what the film OS asks of the camera (see fos.h) */

int fos_port_ms()
{
    return get_ms_clock();
}

void fos_port_sleep(int ms)
{
    msleep(ms);
    int now = get_ms_clock();

    /* the Canon dialog under us times out unless poked (CONFIG_MENU_TIMEOUT_FIX) */
    if (lv && now - ui_last_mode_poke > 2000)
    {
        SetGUIRequestMode(GUIMODE_ML_MENU);
        ui_last_mode_poke = now;
    }
    /* other code (ML's powersave, clear overlays) may put Canon's palette
     * back: check one entry every frame, rewrite all of them now and then */
    static int last_wake = 0;
    if (dev_busy && now - last_wake > 5000)
    {
        idle_wakeup_reset_counters(-10);
        last_wake = now;
    }
    uint32_t probe = shamem_read(LCD_Palette[3 * PALETTE_PROBE]);
    if ((probe && probe != palette_probe) || now - ui_last_palette > 1000)
    {
        palette_ours();
        ui_last_palette = now;
    }
}

int fos_port_key()
{
    if (keyq_out == keyq_in)
        return FK_NONE;
    int k = keyq[keyq_out];
    keyq_out = (keyq_out + 1) % KEYQ;
    return k;
}

int fos_port_abort()
{
    if (ui_abort || ml_shutdown_requested || !DISPLAY_IS_ON)
        return 1;
    if (get_halfshutter_pressed())
        return 1;
    /* a Canon dialog took over (low battery, card door...): give the mode
     * request time to land, then let go if Canon keeps something else up */
    static int mismatch_since = 0;
    int now = get_ms_clock();
    if (CURRENT_GUI_MODE == GUIMODE_ML_MENU)
        mismatch_since = 0;
    else if (!mismatch_since)
        mismatch_since = now;
    else if (now - mismatch_since > 1500)
    {
        dbg("film os: Canon GUI mode %d, leaving", CURRENT_GUI_MODE);
        mismatch_since = 0;
        return 1;
    }
    return 0;
}

void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1)
{
    uint8_t *vram = bmp_vram_real();
    if (!vram || !DISPLAY_IS_ON)
        return;
    y0 = COERCE(y0, 0, FOS_H);
    y1 = COERCE(y1, y0, FOS_H);
    for (int y = y0; y < y1; y++)
        memcpy(vram + y * BMPPITCH, rows + (y - y0) * pitch, FOS_W);
}

/* only some palette entries (the cycling dots) */
void fos_port_palette_part(int first, int n)
{
    if (photo_mode) return;
    palette_ours_range(first, first + n);
}

void fos_port_palette()
{
    /* once: Canon's own entries, to check the colour conversion against */
    static int logged = 0;
    if (!logged && debug_log)
    {
        logged = 1;
        dbg("canon palette 1-7: %08x %08x %08x %08x %08x %08x %08x",
            LCD_Palette[3 * 1 + 2], LCD_Palette[3 * 2 + 2], LCD_Palette[3 * 3 + 2], LCD_Palette[3 * 4 + 2],
            LCD_Palette[3 * 5 + 2], LCD_Palette[3 * 6 + 2], LCD_Palette[3 * 7 + 2]);
        dbg("canon palette 8-15: %08x %08x %08x %08x %08x %08x %08x %08x",
            LCD_Palette[3 * 8 + 2], LCD_Palette[3 * 9 + 2], LCD_Palette[3 * 10 + 2], LCD_Palette[3 * 11 + 2],
            LCD_Palette[3 * 12 + 2], LCD_Palette[3 * 13 + 2], LCD_Palette[3 * 14 + 2], LCD_Palette[3 * 15 + 2]);
    }
    photo_mode = 0;
    palette_ours();
    ui_last_palette = get_ms_clock();
}

void fos_port_photo(const uint8_t (*pal)[3], int n, int k)
{
    photo_n = COERCE(n, 0, FOS_PRINT_COLOURS);
    memcpy(photo_pal, pal, photo_n * 3);
    photo_k = COERCE(k, 0, 256);
    photo_mode = 1;
    palette_ours();
    ui_last_palette = get_ms_clock();
}

void fos_port_log(const char *msg)
{
    dbg("%s", msg);
}

int fos_port_dev_cap()
{
    return 0;
}

/* ---- the card */

const char *fos_port_card()
{
    static char root[8];
    snprintf(root, sizeof(root), "%s:/", get_ml_card()->drive_letter);
    return root;
}

void *fos_port_open(const char *path, int write)
{
    return write ? FIO_CreateFile(path) : FIO_OpenFile(path, O_RDONLY | O_SYNC);
}

/* Canon reads files by DMA: into memory the CPU caches, it goes through
 * an uncached buffer */
#define BOUNCE (64 * 1024)

static int film_read_unlocked(void *f, void *buf, int size)
{
    if (buf != CACHEABLE(buf))
        return FIO_ReadFile(f, buf, size);
    static uint8_t *bounce = 0;
    if (!bounce) bounce = fio_malloc(BOUNCE);
    if (!bounce) return -1;
    int done = 0;
    while (done < size)
    {
        int n = MIN(size - done, BOUNCE);
        int got = FIO_ReadFile(f, UNCACHEABLE(bounce), n);
        if (got <= 0) break;
        memcpy((uint8_t *) buf + done, UNCACHEABLE(bounce), got);
        done += got;
        if (got < n) break;
    }
    return done;
}

static struct semaphore *film_read_lock;
int fos_port_read(void *f,void *buf,int size)
{
    if(!film_read_lock)film_read_lock=create_named_semaphore("film_read",SEM_CREATE_UNLOCKED);
    if(!film_read_lock || take_semaphore(film_read_lock,0)!=0)return -1;
    int result=film_read_unlocked(f,buf,size);
    give_semaphore(film_read_lock);return result;
}

int fos_port_write(void *f, const void *buf, int size)
{
    return FIO_WriteFile(f, buf, size);
}

int fos_port_seek(void *f, int offset)
{
    return FIO_SeekSkipFile(f, offset, SEEK_SET) == offset ? 0 : -1;
}

void fos_port_close(void *f)
{
    if (f) FIO_CloseFile(f);
}

int fos_port_list(const char *dir, int (*fn)(void *ctx, const char *name, int is_dir), void *ctx)
{
    char path[FIO_MAX_PATH_LENGTH];
    int len = snprintf(path, sizeof(path), "%s", dir);
    if (len > 0 && len < (int) sizeof(path) - 1 && path[len - 1] != '/') strcat(path, "/");
    struct fio_file file;
    struct fio_dirent *d = FIO_FindFirstEx(path, &file);
    if (IS_ERROR(d)) return -1;
    int n = 0;
    do
    {
        if (file.name[0] == '.') continue;
        n++;
        if (!fn(ctx, file.name, (file.mode & ATTR_DIRECTORY) != 0)) break;
    }
    while (FIO_FindNextEx(d, &file) == 0);
    FIO_FindClose(d);
    return n;
}

int fos_port_mkdir(const char *path)
{
    if (is_dir(path)) return 0;
    FIO_CreateDirectory(path);
    return is_dir(path) ? 0 : -1;
}

void *fos_port_alloc(int size)
{
    void *p = tmp_malloc(size);
    if (!p) dbg("alloc: %d bytes failed", size);
    return p;
}

/* Memory the CPU works in hard (LUT, strip, decoder pool): the ordinary cached heap first,
 * the temporary pool if that is too small. */
void *fos_port_alloc_hot(int size)
{
    void *p = malloc(size);
    if (!p) p = tmp_malloc(size);
    if (!p) dbg("alloc: %d bytes failed", size);
    return p;
}

/* is LiveView showing a picture under the film OS? (else the video layer is stale memory) */
int fos_port_live(void)
{
    return lv;
}

void fos_port_free(void *p)
{
    if (p) free(p);
}

void fos_port_roll(struct fos_roll *r)
{
    if (roll_loaded && !recent_valid)
    {
        update_log_path();
        read_back_log();
    }
    memset(r, 0, sizeof(*r));
    r->loaded = roll_loaded;
    r->number = roll_number;
    r->film = COERCE(roll_film, 0, fos_nfilms - 1);
    r->exposures = roll_exposures;
    r->shot = frames_shot;
    r->need_wind = need_wind;
    snprintf(r->loaded_at, sizeof(r->loaded_at), "%s", loaded_at);
    r->nrecent = nrecent;
    for (int i = 0; i < nrecent && i < FOS_RECENT; i++)
    {
        r->recent[i].no = recent[i].no;
        memcpy(r->recent[i].time, recent[i].time, sizeof(r->recent[i].time));
        memcpy(r->recent[i].fstop, recent[i].fstop, sizeof(r->recent[i].fstop));
    }
}

static int *setting_var(int s)
{
    switch (s)
    {
        case FS_HUD:        return &hud_enabled;
        case FS_LEDS:       return &leds_on_half;
        case FS_FOCUS:      return &focus_screen;
        case FS_WIND:       return &wind_mode;
        case FS_APKEYS:     return &aperture_keys;
        case FS_LENS_WIDE:  return &lens_widest;
        case FS_FSTOP:      return &manual_fstop;
        case FS_STARTUP:    return &startup_picker;
        case FS_PLAYLOCK:   return &play_lock;
        case FS_BOOTANIM:   return &boot_anim;
        case FS_DEBUG:      return &debug_log;
        case FS_FILM:       return &film_sel;
        case FS_EXP:        return &exp_sel;
        case FS_SCR_STD:    return &scr_std;
        case FS_SCR_SAT:    return &scr_sat;
        case FS_SCR_WARM:   return &scr_warm;
        case FS_SCR_BRIGHT: return &scr_bright;
        case FS_THEME:      return &ui_theme_cfg;
        case FS_PLATE:      return &plate_enabled;
        case FS_PORTRAIT:   return &portrait_cfg;
        case FS_GRAIN:      return &grain_cfg;
        case FS_HALATION:   return &halation_cfg;
    }
    return 0;
}

int fos_port_get(int s)
{
    int *v = setting_var(s);
    if (s == FS_FSTOP) return current_fstop();
    return v ? *v : 0;
}

void fos_port_set(int s, int value)
{
    int *v = setting_var(s);
    if (v) *v = value;
    hud_dirty = 1;
    /* screen colour: the whole film OS changes at once */
    if (s == FS_SCR_STD || s == FS_SCR_SAT || s == FS_SCR_WARM || s == FS_SCR_BRIGHT)
    {
        palette_ours();
        ui_last_palette = get_ms_clock();
    }
}


/* System > Self-test: how much memory the camera can give us and how fast its processor runs the kinds of code we have. It runs
 * in the film OS task (the screen waits, about ten seconds) and writes the numbers to the log whether or not Debug log is on. */
static int bench_largest(int tmp)
{
    int lo = 0, hi = 24 << 20;
    while (hi - lo > 64 * 1024)
    {
        int mid = (lo + hi) / 2;
        void *p = tmp ? tmp_malloc(mid) : malloc(mid);
        if (p) { if (tmp) tmp_free(p); else free(p); lo = mid; }
        else hi = mid;
    }
    return lo;
}


/* the bake's three costs, timed on a made-up picture with the same code the bake runs: JPEG encode, JPEG decode (whole and at
 * 1/4 scale) and the colour look (a 33 x 33 x 33 table) */
static void *prof_fp;
static unsigned prof_pos;
static size_t prof_read(JDEC *jd, uint8_t *dst, size_t n)
{
    (void) jd;
    if (!dst)
    {
        if (fos_port_seek(prof_fp, prof_pos + n) < 0) return 0;
        prof_pos += n;
        return n;
    }
    int got = fos_port_read(prof_fp, dst, (int) n);
    if (got <= 0) return 0;
    prof_pos += got;
    return (size_t) got;
}
static int prof_px;
static int prof_out(JDEC *jd, void *bitmap, JRECT *r)
{
    (void) jd; (void) bitmap;
    prof_px += (r->right - r->left + 1) * (r->bottom - r->top + 1);
    return 1;
}
static void bake_profile()
{
    const int W = 1280, H = 960;
    uint8_t *rgb = malloc(W * H * 3);
    if (!rgb) { dbg("selftest: bake profile: no memory for the test picture"); return; }
    unsigned v = 99u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            v = v * 1664525u + 1013904223u;
            int n = (int) (v >> 28) - 8;                                    /* a little noise, like a photograph */
            uint8_t *p = rgb + (y * W + x) * 3;
            p[0] = COERCE(128 + (x >> 3) - (y >> 4) + n, 0, 255);
            p[1] = COERCE(110 + (y >> 3) + n, 0, 255);
            p[2] = COERCE(90 + ((x + y) >> 4) - n, 0, 255);
        }
    const char *path = "ML/PROFILE.JPG";
    char full[64];
    snprintf(full, sizeof(full), "%s%s", fos_port_card(), path);
    int t = get_ms_clock();
    int ok = fos_jpeg_write(full, rgb, W * 3, W, H, 92, 0);
    int enc = get_ms_clock() - t;
    dbg("selftest: encode %d x %d (q92, 4:2:0): %d ms = %d.%d MP/s%s", W, H, enc, enc ? W * H / 1000 / enc : 0, enc ? (W * H / 100 / enc) % 10 : 0, ok ? "" : "  FAILED");
    free(rgb);
    if (ok)
    {
        uint8_t *pool = malloc(65536);
        for (int pass = 0; pass < 2 && pool; pass++)
        {
            const int scale = pass ? 2 : 0;
            prof_fp = fos_port_open(full, 0);
            prof_pos = 0;
            JDEC jd;
            if (prof_fp && jd_prepare(&jd, prof_read, pool, 65536, 0) == JDR_OK)
            {
                prof_px = 0;
                t = get_ms_clock();
                int rc = jd_decomp(&jd, prof_out, scale);
                int ms = get_ms_clock() - t;
                dbg("selftest: decode at 1/%d scale: %d ms = %d.%d MP/s of the full picture (rc %d)", 1 << scale, ms, ms ? W * H / 1000 / ms : 0, ms ? (W * H / 100 / ms) % 10 : 0, rc);
            }
            else dbg("selftest: decode at 1/%d: could not start", 1 << scale);
            if (prof_fp) fos_port_close(prof_fp);
        }
        free(pool);
        FIO_RemoveFile(full);
    }
    uint8_t *lut = malloc(33 * 33 * 33 * 3);
    if (lut)
    {
        for (int i = 0; i < 33 * 33 * 33; i++)
        {
            lut[3 * i] = (i % 33) * 255 / 32; lut[3 * i + 1] = (i / 33 % 33) * 255 / 32; lut[3 * i + 2] = (i / 1089) * 255 / 32;
        }
        uint8_t px[3], o[3];
        unsigned acc = 0;
        v = 7u;
        t = get_ms_clock();
        for (int i = 0; i < 1000000; i++)
        {
            v = v * 1664525u + 1013904223u;
            px[0] = v >> 8; px[1] = v >> 16; px[2] = v >> 24;
            fos_lut_pixel(lut, px, o);
            acc += o[0];
        }
        int ms = get_ms_clock() - t;
        dbg("selftest: colour look, 1M different pixels: %d ms = %d.%d MP/s (checksum %d)", ms, ms ? 1000 / ms : 0, ms ? (10000 / ms) % 10 : 0, (int) (acc & 255));
        /* the same table on smooth runs of pixels (like a photograph) */
        acc = 0;
        t = get_ms_clock();
        for (int i = 0; i < 1000000; i++)
        {
            px[0] = (i / 6) & 255; px[1] = (i / 9 + 40) & 255; px[2] = (i / 11 + 90) & 255;
            fos_lut_pixel(lut, px, o);
            acc += o[1];
        }
        ms = get_ms_clock() - t;
        dbg("selftest: colour look on smooth runs: %d ms = %d.%d MP/s (checksum %d)", ms, ms ? 1000 / ms : 0, ms ? (10000 / ms) % 10 : 0, (int) (acc & 255));
        free(lut);
    }
}

static void selftest()
{
    const int saved = debug_log;
    debug_log = 1;
    dbg("selftest: begin, firmware %s %s", __DATE__, __TIME__);
    dbg("selftest: largest single block: malloc %d KB, tmp_malloc %d KB", bench_largest(0) >> 10, bench_largest(1) >> 10);

    int t = get_ms_clock();
    volatile unsigned sink = fos_bench_alu(20u * 1000 * 1000);
    int alu = get_ms_clock() - t;
    dbg("selftest: 20M integer steps %d ms = %d M steps/s", alu, alu ? 20000 / alu : 0);

    const int size = 256 * 1024;
    for (int pass = 0; pass < 2; pass++)
    {
        const char *name = pass ? "tmp_malloc" : "malloc";
        uint8_t *a = pass ? tmp_malloc(size) : malloc(size), *b = pass ? tmp_malloc(size) : malloc(size);
        if (a && b)
        {
            fos_bench_fill(a, size);
            t = get_ms_clock();
            sink += fos_bench_copy(a, b, size, 64);
            int copy = get_ms_clock() - t;
            t = get_ms_clock();
            sink += fos_bench_stride(a, size, 64);
            int rd = get_ms_clock() - t;
            dbg("selftest: %s: copy 16 MB %d ms = %d MB/s; strided read %d ms", name, copy, copy ? 16000 / copy : 0, rd);
        }
        else dbg("selftest: %s: could not get 2 x 256 KB", name);
        if (pass) { if (a) tmp_free(a); if (b) tmp_free(b); }
        else { free(a); free(b); }
    }

    (void) sink;
    bake_profile();
    dbg("selftest: end");
    debug_log = saved;
}

void fos_port_action(int action, int a, int b)
{
    switch (action)
    {
        case FA_LOAD:
            film_sel = roll_film = COERCE(a,0,fos_nfilms-1);
            roll_loaded = 0;
            need_wind = 0;
            dbg("look selected: %d %s",film_sel,fos_films[film_sel].name);
            break;
        case FA_REWIND:
            rewind_roll();
            break;
        case FA_BUSY:
            if (a && !dev_busy) powersave_prohibit();
            if (!a && dev_busy) powersave_permit();
            dev_busy = a;
            dbg("darkroom: %s", a ? "developing" : "done");
            break;
        case FA_REAPPLY:
            if (roll_loaded) ui_after |= AFTER_APPLY;
            break;
        case FA_CANON_MENU:
            ui_after |= AFTER_CANON_MENU;
            break;
        case FA_SELFTEST:
            selftest();
            break;
    }
}

void fos_port_clock(char *buf, int size)
{
    /* screens ask every frame; the RTC is read once a second */
    static char cached[8];
    static int last = -100000;
    int now_ms = get_ms_clock();
    if (now_ms - last > 1000)
    {
        struct tm now;
        LoadCalendarFromRTC(&now);
        snprintf(cached, sizeof(cached), "%02d:%02d", now.tm_hour, now.tm_min);
        last = now_ms;
    }
    snprintf(buf, size, "%s", cached);
}

int fos_port_lens_manual()
{
    return lens_is_manual();
}

const char *fos_port_lens_name()
{
    return lens_info.name[0] ? lens_info.name : "Lens";
}

/* M-Film's version (VERSION at the top of the repository; build_fork.sh writes fos_version.h) */
#if defined(__has_include)
#if __has_include("fos_version.h")
#include "fos_version.h"
#endif
#endif
#ifndef MFILM_VERSION
#define MFILM_VERSION "dev"
#endif

const char *fos_port_model()
{
    /* "Canon EOS 650D": as the camera reports itself */
    return camera_model[0] ? camera_model : "Canon camera";
}

const char *fos_port_version()
{
    return MFILM_VERSION;
}


void fos_port_stat(const struct fos_stat *s)
{
    stat_all.frames += s->frames;
    stat_all.draw_ms += s->draw_ms;
    stat_all.present_ms += s->present_ms;
    stat_all.rest_ms += s->rest_ms;
    stat_all.cans_ms += s->cans_ms;
    if (s->worst_ms > stat_all.worst_ms) stat_all.worst_ms = s->worst_ms;
    if (s->open_ms) stat_all.open_ms = s->open_ms;
}

/* ---------------------------------------------------------------- viewfinder HUD */

static volatile int hud_showing = 0;    /* HUD drawn on the LiveView screen */

/* the back screen as a top plate: LiveView off, nothing else of ours on screen (and Canon's own screens left alone) */
static int plate_visible()
{
    return plate_enabled && !lv && DISPLAY_IS_ON && display_idle() && !is_movie_mode() && !ui_running;
}

static int hud_visible()
{
    return hud_enabled && roll_loaded && lv && display_idle() && !is_movie_mode() && !ui_running;
}

/* Magic Lantern's own LiveView overlays never draw: the film OS (or
 * Canon's clean screen) is all you see */
int filmroll_overlays_active()
{
    return 1;
}

/* the HUD draws with Canon's palette */
static struct fos_hud_palette hud_pal;

static void hud_palette_init()
{
    hud_pal.clear = COLOR_EMPTY;
    hud_pal.black = COLOR_BLACK;
    hud_pal.white = COLOR_WHITE;
    hud_pal.red = COLOR_RED;
    hud_pal.dark_red = COLOR_DARK_RED;
    for (int i = 0; i <= 100; i++)
        hud_pal.gray[i] = COLOR_GRAY(i);
}

/* the shutter speed as the LEDs show it: 1/125 is 125, slow ones keep the " */
static void shutter_led(char *buf, int size)
{
    if (!lens_info.raw_shutter)
    {
        snprintf(buf, size, "--");
        return;
    }
    const char *s = lens_format_shutter(lens_info.raw_shutter);
    int n = 0;
    for (; *s && n < size - 1; s++)
        if ((*s >= '0' && *s <= '9') || *s == '.' || *s == '"')
            buf[n++] = *s;
    buf[n] = 0;
}

static void hud_draw(int lit, int blink)
{
    struct fos_hud h;
    memset(&h, 0, sizeof(h));
    h.lit = lit;
    h.blink = blink;
    h.fstop = current_fstop();
    h.widest = lens_is_manual() ? lens_widest : 0;
    if (lit)
    {
        int ev8 = get_ae_value();               /* meter reading, 1/8 EV, + = too bright */
        h.meter = 1;
        h.want = h.fstop + (ev8 * 3 + (ev8 >= 0 ? 4 : -4)) / 8;
    }
    shutter_led(h.shutter, sizeof(h.shutter));
    h.frame = frames_shot + 1;
    h.exposures = roll_exposures;
    h.finished = ROLL_FINISHED;
    h.need_wind = need_wind;
    h.focus_screen = focus_screen;
    h.film = fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].name;
    h.grain = COERCE(grain_cfg, 0, 3);
    h.halation = COERCE(halation_cfg, 0, 3);
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_draw(vram, BMPPITCH, &h, &hud_pal);
}


static const char *wb_text(char *buf, int size)
{
    static const char *const names[] = { "AUTO", "SUNNY", "CLOUDY", "TUNGSTEN", "FLUOR", "FLASH", "CUSTOM", "?", "SHADE" };
    if (lens_info.wb_mode == WB_KELVIN) snprintf(buf, size, "%dK", lens_info.kelvin);
    else snprintf(buf, size, "%s", lens_info.wb_mode <= 8 ? names[lens_info.wb_mode] : "?");
    return buf;
}

static void plate_fill(struct fos_hud *h, int lit, int blink)
{
    memset(h, 0, sizeof(*h));
    h->lit = 1;                                 /* the plate's readouts are always lit; `lit` (half-press) only turns the meter on */
    h->blink = blink;
    h->fstop = current_fstop();
    {   /* the meter is always on: the pointer follows the last reading, which Canon keeps between half-presses */
        int ev8 = get_ae_value();
        h->meter = 1;
        h->want = h->fstop + (ev8 * 3 + (ev8 >= 0 ? 4 : -4)) / 8;
    }
    shutter_led(h->shutter, sizeof(h->shutter));
    h->frame = (int) bake_head;                 /* pictures taken since the camera was switched on */
    h->film = fos_films[COERCE(film_sel, 0, fos_nfilms - 1)].name;
    h->grain = COERCE(grain_cfg, 0, 3);
    h->halation = COERCE(halation_cfg, 0, 3);
    h->iso = lens_info.iso;
    struct fos_bake_info bi;
    fos_port_bake_info(&bi);
    h->bake_queued = bi.queued; h->bake_active = bi.active; h->bake_paused = bi.paused; h->bake_percent = bi.percent;
    fos_port_bake_status(h->status, sizeof(h->status));
    snprintf(h->mode, sizeof(h->mode), "%s", get_shootmode_name_short(shooting_mode));
    {   /* the picture quality, named the way ML names it (DIGIC 5 layout) */
        const int pq = pic_quality, raw = pq & 0x60000, jpg = pq & 0x10000, rawsize = pq & 0xF, js = (pq >> 8) & 0xFF;
        const char *rs = rawsize == 1 ? "mRAW" : rawsize == 2 ? "sRAW" : rawsize == 7 ? "sRAW1" : rawsize == 8 ? "sRAW2" : raw ? "RAW" : "";
        const char *js_ = !jpg ? "" : js == 0 ? "L" : js == 1 ? "M" : js == 2 ? "S" : js == 0x0e ? "S1" : js == 0x0f ? "S2" : js == 0x10 ? "S3" : "?";
        snprintf(h->quality, sizeof(h->quality), "%s%s%s", rs, raw && jpg ? "+" : "", js_);
        h->quality_fine = (pq >> 24) == 3;
    }
    h->focal = lens_info.focal_len;
    wb_text(h->wb, sizeof(h->wb));
    h->battery = GetBatteryLevel();
    h->shots_left = avail_shot > 0 ? avail_shot : -1;
    fos_port_clock(h->clock, sizeof(h->clock));
}

/* the plate's accent follows the colour theme, from Canon's own palette entries (red; its burnt orange, entry 4; green): Canon's
 * palette has nothing brighter, and the LiveView HUD stays red */
static uint8_t *plate_buf;
static void plate_draw(int lit, int blink)
{
    static const uint8_t accent[3][2] = { { COLOR_RED, COLOR_DARK_RED }, { 4, 4 }, { COLOR_GREEN2, COLOR_GREEN1 } };
    struct fos_hud h;
    plate_fill(&h, lit, blink);
    const int t = COERCE(ui_theme_cfg, 0, 2);
    hud_pal.red = accent[t][0];
    hud_pal.dark_red = accent[t][1];
    hud_pal.alert = COLOR_RED;
    /* drawn in a buffer of its own and copied across in one go: clearing and redrawing on the live screen showed as a flicker */
    uint8_t *vram = bmp_vram();
    if (vram)
    {
        if (!plate_buf) plate_buf = fos_port_alloc_hot(720 * 480);
        if (plate_buf)
        {
            fos_hud_plate(plate_buf, 720, &h, &hud_pal);
            for (int y = 0; y < 480; y++)
                memcpy(vram + y * BMPPITCH, plate_buf + y * 720, 720);
        }
        else
            fos_hud_plate(vram, BMPPITCH, &h, &hud_pal);
    }
    hud_pal.red = COLOR_RED;
    hud_pal.dark_red = COLOR_DARK_RED;
}

/* what the plate shows that the main signature does not cover, as one number (redraw when it changes) */
static int plate_sig()
{
    struct fos_hud h;
    plate_fill(&h, 0, 0);
    unsigned v = 2166136261u;
    const int parts[] = { pic_quality, h.bake_queued, h.bake_active, h.bake_paused, h.bake_percent, h.iso, h.focal, h.battery, h.shots_left,
                          shooting_mode, (int) lens_info.wb_mode, (int) lens_info.kelvin, h.clock[3] * 256 + h.clock[4], h.clock[1] };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) v = (v ^ (unsigned) parts[i]) * 16777619u;
    return (int) (v ^ (unsigned) h.status[0] << 3);
}

static void plate_clear()
{
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_plate_clear(vram, BMPPITCH, &hud_pal);
    fos_port_free(plate_buf);                   /* the buffer is only held while the plate is up */
    plate_buf = 0;
}

static void hud_clear()
{
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_clear(vram, BMPPITCH, &hud_pal);
}

/* one HUD refresh; called from the main loop */
static int bake_queued_now(void)
{
    struct fos_bake_info bi;
    fos_port_bake_info(&bi);
    return bi.queued;
}

static void hud_step()
{
    static int last_sig = -1, last_sig2 = -1;
    static int last_draw = 0;
    static int drawn = 0;
    static int was_visible = -1;
    static int plate_drawn = 0, canon_held = 0, gone_since = 0;

    const int plate = plate_visible();
    int visible = plate || hud_visible();
    if (visible != was_visible)
    {
        dbg("hud: visible %d plate %d (enabled %d roll %d lv %d movie %d ui %d)",
            visible, plate, hud_enabled, roll_loaded, lv, is_movie_mode(), ui_running);
        was_visible = visible;
    }
    hud_showing = visible;

    /* the plate covers Canon's own info screen, so Canon is kept from drawing it while the plate is up, and gets the screen back
     * some time after (the way M-Film does it) */
    {
        const int tnow = get_ms_clock();
        if (plate && !canon_held)
        {
            canon_gui_disable_front_buffer();
            clrscr();
            canon_held = 1;
            hud_dirty = 1;
            gone_since = 0;
        }
        else if (!plate && canon_held)
        {
            if (ui_running) { canon_held = 0; gone_since = 0; }                         /* the film OS has taken the screen itself */
            else if (!gone_since) gone_since = tnow;
            else if (tnow - gone_since > 700) { canon_gui_enable_front_buffer(1); canon_held = 0; gone_since = 0; }
        }
        else gone_since = 0;
    }
    if (plate_drawn && !plate && display_idle() && !ui_running)
    {
        plate_clear();
        plate_drawn = 0;
        last_sig = -1;
    }

    if (!visible)
    {
        /* HUD switched off or roll rewound: wipe it once we're back on the LiveView screen */
        if (drawn && (!hud_enabled || !roll_loaded) && lv && display_idle() && !ui_running)
        {
            hud_clear();
            drawn = 0;
        }
        last_sig = -1;
        return;
    }
    drawn = 1;

    int now = get_ms_clock();
    int lit = !leds_on_half || get_halfshutter_pressed() || now < leds_until;
    int blink = (now / 400) % 2;
    if (plate && !bake_queued_now()) blink = 0;           /* nothing on the plate pulses unless a bake is waiting */
    int ev8 = (lit || plate) ? get_ae_value() : 0;
    if (plate) ev8 = (ev8 * 3 + (ev8 >= 0 ? 4 : -4)) / 8;        /* the plate only needs the pointer's third of a stop: redraw when that moves, not on every tiny reading */

    int sig = lit | blink << 1 | need_wind << 2 | (frames_shot & 0xFF) << 3
            | lens_info.raw_shutter << 11 | (ev8 & 0xFF) << 19 | (current_fstop() & 0x1F) << 27;

    /* redraw on change, and every second to win over Canon's overlays */
    const int sig2 = plate ? plate_sig() : 0;
    if (sig != last_sig || sig2 != last_sig2 || hud_dirty || now - last_draw > 1000)
    {
        hud_dirty = 0;
        last_sig = sig;
        last_sig2 = sig2;
        last_draw = now;
        if (plate) { plate_draw(lit, blink); plate_drawn = 1; }
        else hud_draw(lit, blink);
    }
}

/* ---------------------------------------------------------------- keys */

/* while the film OS is up it gets the dial and the buttons; anything it
 * doesn't use goes on to Canon, like the ML menu does */
static int ui_key(struct event *event, int key)
{
    int dir = 0;
    switch (key)
    {
        case MODULE_KEY_PRESS_HALFSHUTTER:
        case MODULE_KEY_PRESS_FULLSHUTTER:
        case MODULE_KEY_REC:
            ui_abort = 1;               /* back to the camera, ready to shoot */
            return 1;
        case MODULE_KEY_UNPRESS_HALFSHUTTER:
        case MODULE_KEY_UNPRESS_FULLSHUTTER:
            return 1;                   /* Canon must see the shutter let go */

        case MODULE_KEY_WHEEL_LEFT:     dir = FK_WHEEL_L; break;
        case MODULE_KEY_WHEEL_RIGHT:    dir = FK_WHEEL_R; break;
        case MODULE_KEY_WHEEL_UP:       dir = FK_WHEEL_L; break;
        case MODULE_KEY_WHEEL_DOWN:     dir = FK_WHEEL_R; break;

        case MODULE_KEY_PRESS_LEFT:     keyq_push(FK_LEFT); return 0;
        case MODULE_KEY_PRESS_RIGHT:    keyq_push(FK_RIGHT); return 0;
        case MODULE_KEY_PRESS_UP:       keyq_push(FK_UP); return 0;
        case MODULE_KEY_PRESS_DOWN:     keyq_push(FK_DOWN); return 0;
        case MODULE_KEY_UNPRESS_UDLR:
            keyq_push(FK_RELEASE | FK_LEFT);
            keyq_push(FK_RELEASE | FK_RIGHT);
            keyq_push(FK_RELEASE | FK_UP);
            keyq_push(FK_RELEASE | FK_DOWN);
            return 0;

        case MODULE_KEY_PRESS_SET:
        case MODULE_KEY_Q:
        case MODULE_KEY_JOY_CENTER:     keyq_push(FK_SET); return 0;
        case MODULE_KEY_UNPRESS_SET:    return 0;
        case MODULE_KEY_MENU:           keyq_push(FK_MENU); return 0;
        case MODULE_KEY_INFO:           keyq_push(FK_INFO); return 0;
        case MODULE_KEY_PLAY:           keyq_push(FK_PLAY); return 0;
        case MODULE_KEY_TRASH:
        case MODULE_KEY_TOUCH_2_FINGER: keyq_push(FK_HOME); return 0;

        case 0:
            return 1;                   /* not a button: Canon's business */
        default:
            return 0;
    }

    /* newer firmware may group several wheel clicks into one event */
    int count = MAX(1, MIN(event->arg, 8));
    for (int i = 0; i < count; i++)
        keyq_push(dir);
    return 0;
}

static unsigned int lv_key(unsigned int key)
{
    if (dev_busy) return 1;
    /* what used to open the Magic Lantern menu opens home instead */
    if (key == MODULE_KEY_TRASH || key == MODULE_KEY_TOUCH_2_FINGER)
    {
        if (shooting_screen())
        {
            dbg("key: home");
            open_ui(FOS_OPEN_HOME);
        }
        return 0;
    }

    /* PLAY: the developed prints, never Canon's playback of what the
     * darkroom hasn't seen yet */
    if (key == MODULE_KEY_PLAY && shooting_screen())
    {
        dbg("key: PLAY -> prints");
        open_ui(FOS_OPEN_PRINTS);
        return 0;
    }

    /* only on the plain LiveView screen: never steal keys from Canon's menus or dialogs */
    if (!lv || !roll_loaded || !display_idle())
        return 1;

    if (key == MODULE_KEY_PRESS_HALFSHUTTER || key == MODULE_KEY_UNPRESS_HALFSHUTTER)
        hud_dirty = 1;

    /* aperture ring for manual lenses: the wheel in Av mode (where it has
     * nothing to adjust without a lens), or up/down if the user prefers */
    if (lens_is_manual() && !ROLL_FINISHED)
    {
        int dir = 0;
        if (aperture_keys == 0 && shooting_mode == SHOOTMODE_AV)
            dir = (key == MODULE_KEY_WHEEL_RIGHT) ? 1 : (key == MODULE_KEY_WHEEL_LEFT) ? -1 : 0;
        else if (aperture_keys == 1)
            dir = (key == MODULE_KEY_PRESS_UP) ? 1 : (key == MODULE_KEY_PRESS_DOWN) ? -1 : 0;
        if (dir)
        {
            step_manual_fstop(dir);
            leds_until = get_ms_clock() + 2000;
            hud_dirty = 1;
            return 0;
        }
    }

    if (ROLL_FINISHED)
    {
        if (key == MODULE_KEY_PRESS_SET)
        {
            open_ui(FOS_OPEN_END);
            return 0;
        }
        return 1;
    }

    if (need_wind)
    {
        if (key == MODULE_KEY_PRESS_SET)
        {
            need_wind = 0;
            leds_until = get_ms_clock() + 1500;
            hud_dirty = 1;
            return 0;
        }
        if (wind_mode == 2 && (key == MODULE_KEY_PRESS_HALFSHUTTER || key == MODULE_KEY_PRESS_FULLSHUTTER))
            return 0;
    }

    return 1;
}

int handle_filmroll_keys(struct event *event)
{
    int key = module_translate_key(event->param, MODULE_KEY_PORTABLE);
    if (ui_running)
    {
#ifdef BGMT_UNPRESS_LEFT
        /* the EOS M reports each direction's release on its own, and the
         * portable key codes have no name for those */
        switch (event->param)
        {
            case BGMT_UNPRESS_LEFT:  keyq_push(FK_RELEASE | FK_LEFT);  return 0;
            case BGMT_UNPRESS_RIGHT: keyq_push(FK_RELEASE | FK_RIGHT); return 0;
            case BGMT_UNPRESS_UP:    keyq_push(FK_RELEASE | FK_UP);    return 0;
            case BGMT_UNPRESS_DOWN:  keyq_push(FK_RELEASE | FK_DOWN);  return 0;
        }
#endif
        return ui_key(event, key);
    }

    if (!key)
        return 1;

    /* newer firmware may group several wheel clicks into one event */
    int count = 1;
    if (key == MODULE_KEY_WHEEL_LEFT || key == MODULE_KEY_WHEEL_RIGHT)
        count = MAX(1, event->arg);

    int pass = 1;
    for (int i = 0; i < count; i++)
        pass = lv_key(key);
    return pass;
}

/* ---------------------------------------------------------------- the ML menu, replaced */

void filmroll_menu_requested()
{
    if (shooting_screen())
        open_ui(FOS_OPEN_HOME);
}

static void filmroll_init()
{
    if (roll_loaded)
        update_log_path();
    fos_ui_init();
    hud_palette_init();
    dbg("init: M-Film " MFILM_VERSION ", fork build %s %s, roll %d loaded %d shot %d/%d, startup %d hud %d",
        __DATE__, __TIME__, roll_number, roll_loaded, frames_shot, roll_exposures,
        startup_picker, hud_enabled);
}

/* The film OS's fonts are a file on the card (see fos_gfx.h): kept in
 * memory taken after boot, not in autoexec.bin */
static void load_fonts()
{
    int size = 0;
    uint8_t *raw = read_entire_file("ML/DATA/" FOS_FONT_FILE, &size);
    if (!raw)
    {
        dbg("fonts: ML/DATA/%s not found", FOS_FONT_FILE);
        return;
    }
    uint8_t *blob = malloc(size);       /* cached memory, for drawing */
    if (blob)
    {
        memcpy(blob, raw, size);
        if (!fos_fonts_load(blob, size))
        {
            free(blob);
            blob = 0;
        }
    }
    fio_free(raw);
    dbg("fonts: %d bytes, %s", size, blob ? "loaded" : "not usable");
}

/* the films beyond the built-in ones, from the card; the file stays in
 * memory (the films point into it) */
static void load_films()
{
    int size = 0;
    uint8_t *raw = read_entire_file("ML/DATA/" FOS_FILM_FILE, &size);
    if (!raw)
    {
        dbg("films: ML/DATA/%s not found, the built-in ones only", FOS_FILM_FILE);
        return;
    }
    uint8_t *blob = malloc(size);
    if (blob)
    {
        memcpy(blob, raw, size);
        if (!fos_films_load(blob, size))
        {
            free(blob);
            blob = 0;
        }
    }
    fio_free(raw);
    dbg("films: %d (%d bytes, %s)", fos_nfilms, size, blob ? "loaded" : "not usable");
}

/* the film OS is the camera's start screen */
static void startup_task()
{
    /* wait (up to ~15 s) for ML to finish booting and LiveView to settle */
    for (int i = 0; i < 150; i++)
    {
        if (ml_started && shooting_screen())
            break;
        if (i % 10 == 0 && ml_started)
            DBG_STATE("startup waiting");
        msleep(100);
    }
    msleep(300);

    DBG_STATE("startup check");
    if (!ml_started || !shooting_screen())
    {
        dbg("startup: not on the shooting screen, film OS skipped");
        return;
    }
    int shelf = startup_picker == 2 || (startup_picker == 1 && !ROLL_IN_PROGRESS);
    if (boot_anim && startup_picker)
        open_ui(shelf ? FOS_OPEN_BOOT : FOS_OPEN_BOOT_LV);
    else if (shelf)
        open_ui(FOS_OPEN_SHELF);
}

/* Everything runs from one task that waits for ML to finish booting, so
 * nothing touches Canon's GUI state before it exists: start screen first,
 * then frame logging and the HUD. */
static void filmroll_task()
{
    while (!ml_started)
        msleep(100);
    /* Initialize shared I/O locks before capture jobs can be published. */
    if(!film_read_lock)film_read_lock=create_named_semaphore("film_read",SEM_CREATE_UNLOCKED);
    if(!film_log_lock)film_log_lock=create_named_semaphore("film_log",SEM_CREATE_UNLOCKED);
    dbg("main: started");

    load_fonts();
    load_films();
    roll_loaded = 0; wind_mode = 0; hud_enabled = 0; play_lock = 0;
    startup_task();
    film_capture_check();

    int last_alive = 0;
    TASK_LOOP
    {
        poll_frames();
        bake_overlay();
        if(review_pending && !ui_running && shooting_screen() && !get_halfshutter_pressed() && job_state_ready_to_take_pic()){
            review_pending=0;open_ui(FOS_OPEN_REVIEW);
        }

        /* the roll has just been finished: once the camera is quiet, ask
         * whether to develop it now or load a new one first */
        if (end_due && get_ms_clock() > end_due && ROLL_FINISHED)
        {
            if (!ui_running && shooting_screen() && !get_halfshutter_pressed() && job_state_ready_to_take_pic())
            {
                end_due = 0;
                dbg("roll %03d finished: asking", roll_number);
                open_ui(FOS_OPEN_END);
            }
        }
        else if (end_due && !ROLL_FINISHED) end_due = 0;
        if (k % 2 == 0)
            hud_step();                     /* every 100 ms */

        int now = get_ms_clock();
        if (now - last_alive > 60000)
        {
            dbg("main: alive, roll %d loaded %d shot %d/%d file %d",
                roll_number, roll_loaded, frames_shot, roll_exposures, last_file_number);
            last_alive = now;
        }
        msleep(50);
    }
}

INIT_FUNC("filmroll", filmroll_init);
TASK_CREATE("film_main", filmroll_task, 0, 0x1e, 0x2000);
TASK_CREATE("film_quick", quick_preview_task, 0, 0x1e, 0x4000);
TASK_CREATE("film_bake", background_bake_task, 0, 0x1f, 0x6000);
