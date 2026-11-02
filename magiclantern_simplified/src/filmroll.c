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
#include "filmroll.h"
#include "fos.h"
#include "fos_dev.h"
#include "fos_bake.h"
#include "fos_preview.h"
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
static CONFIG_INT("filmroll.theme",     ui_theme_cfg,     1);
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
void fos_port_bake_progress(int done,int total){bake_done=done;bake_total=total;}
void fos_port_bake_status(char *out,int size){
 unsigned queued=bake_head-bake_tail;
 if(!queued){out[0]=0;return;}
 if(!bake_active){snprintf(out,size,"Preparing preview  |  %d queued",queued);return;}
 int pct=bake_total?bake_done*100/bake_total:0;
 if(bake_paused){snprintf(out,size,"Bake paused for capture  |  %d queued",queued);return;}
 int elapsed=MAX(0,get_ms_clock()-bake_start-bake_paused_ms);
 if(pct>=5&&bake_done>0){
  int remain=(int)((int64_t)elapsed*(bake_total-bake_done)/bake_done/1000);
  snprintf(out,size,"Bake %d%%  ~%ds left  |  %d queued",pct,remain,queued);
 }else snprintf(out,size,"Bake %d%%  estimating...  |  %d queued",pct,queued);
}
void fos_port_bake_info(struct fos_bake_info *info){
 memset(info,0,sizeof(*info));info->remaining=-1;
 info->completed=bake_completed;info->failed=bake_failed;
 unsigned tail=bake_tail,head=bake_head;
 if(head<=tail)return;
 unsigned count=head-tail;if(count>BAKE_QUEUE)count=BAKE_QUEUE;
 info->queued=count;info->active=bake_active;info->paused=bake_paused;
 if(info->active&&bake_total>0){
  int done=bake_done,total=bake_total;info->percent=COERCE(done*100/total,0,100);
  if(info->percent>=5&&done>0&&!info->paused){
   int elapsed=MAX(0,get_ms_clock()-bake_start-bake_paused_ms);
   info->remaining=(int)((int64_t)elapsed*MAX(0,total-done)/done/1000);
  }
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
   job->quick_ok=fos_quick_preview(job->source,job->dest,job->film);
   dbg("quick preview: %s result %d %d ms (queued %d ms before it started)",job->source,job->quick_ok,get_ms_clock()-start,start-job->created_ms);
   if(job->quick_ok){const char *name=strrchr(job->dest,'/');snprintf(review_file,sizeof(review_file),"%s",name?name+1:job->dest);review_pending=1;}
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
  job->state=3;bake_active=1;bake_done=0;bake_total=1;bake_paused_ms=0;bake_start=get_ms_clock();
  powersave_prohibit();int ok=fos_bake(job->source,job->dest,job->film);powersave_permit();
  dbg("background bake: %s -> %s result %d error %s; %d ms from shutter to done, %d ms of it paused for Canon/half-press",job->source,job->dest,ok,fos_bake_error(),get_ms_clock()-job->created_ms,bake_paused_ms);
  if(!ok&&!ml_shutdown_requested)NotifyBox(5000,"Bake: %s",fos_bake_error());
  if(ok&&!job->quick_ok){const char *name=strrchr(job->dest,'/');snprintf(review_file,sizeof(review_file),"%s",name?name+1:job->dest);review_pending=1;}
  if(ok)bake_completed++;else bake_failed++;
  bake_active=0;job->state=4;bake_revision++;bake_tail++;msleep(10);
 }
}
static void bake_overlay(void){
 static int last,last_bar,visible,fill16;int now=get_ms_clock();
 if(ui_running||!shooting_screen()||get_halfshutter_pressed()||!job_state_ready_to_take_pic())return;
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

static void film_logo_draw()
{
    logo_load();
    uint8_t *v = bmp_vram_real();
    if (!v) return;
    for (int y = 0; y < 480; y++) memset(v + y * BMPPITCH, COLOR_BLACK, 720);
    if (!logo_bits) return;
    int row = (logo_w + 7) / 8, ox = (720 - logo_w) / 2, oy = (480 - logo_h) / 2;
    for (int y = 0; y < logo_h; y++)
        for (int x = 0; x < logo_w; x++)
            if (logo_bits[y * row + (x >> 3)] & (0x80 >> (x & 7)))
                v[(oy + y) * BMPPITCH + ox + x] = COLOR_WHITE;
}

static void film_ui_open(int open)
{
    /* the logo only when the main menu opens from Canon's screen, not for a shortcut into Images or the shelf */
    const int show_logo = open == FOS_OPEN_HOME;
    NotifyBoxHide();
    if (show_logo && !lv && DISPLAY_IS_ON) film_logo_draw();     /* cover Canon's screen while the mode changes */
    if (lv) bmp_off();
    SetGUIRequestMode(GUIMODE_ML_MENU);
    msleep(200);
    canon_gui_disable_front_buffer();
    CancelDateTimer();
    clrscr();
    bmp_on();
    if (show_logo)
    {
        film_logo_draw();                           /* and again once the bitmap layer is ours */
        msleep(450);                                /* a moment to read it before the shutter opens */
    }
    ui_last_mode_poke = get_ms_clock();
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
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_draw(vram, BMPPITCH, &h, &hud_pal);
}

static void hud_clear()
{
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_clear(vram, BMPPITCH, &hud_pal);
}

/* one HUD refresh; called from the main loop */
static void hud_step()
{
    static int last_sig = -1;
    static int last_draw = 0;
    static int drawn = 0;
    static int was_visible = -1;

    int visible = hud_visible();
    if (visible != was_visible)
    {
        dbg("hud: visible %d (enabled %d roll %d lv %d movie %d ui %d)",
            visible, hud_enabled, roll_loaded, lv, is_movie_mode(), ui_running);
        was_visible = visible;
    }
    hud_showing = visible;

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
    int ev8 = lit ? get_ae_value() : 0;

    int sig = lit | blink << 1 | need_wind << 2 | (frames_shot & 0xFF) << 3
            | lens_info.raw_shutter << 11 | (ev8 & 0xFF) << 19 | (current_fstop() & 0x1F) << 27;

    /* redraw on change, and every second to win over Canon's overlays */
    if (sig != last_sig || hud_dirty || now - last_draw > 1000)
    {
        hud_dirty = 0;
        last_sig = sig;
        last_draw = now;
        hud_draw(lit, blink);
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
