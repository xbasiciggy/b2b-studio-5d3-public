/* filmroll: shoot the EOS M like a film camera.
 *
 * Pick a film stock, load a roll and shoot a fixed number of frames.
 * Loading a roll sets picture style, contrast / saturation / colour tone,
 * white balance and ISO for that stock, and starts a roll log next to the
 * images (DCIM/xxxCANON/ROLL_nnn.TXT) that the PC film lab reads
 * (lab/roll.py). Film keys must match lab/films.py.
 *
 * v2 adds:
 *  - a full-screen film picker (runs on top of the ML menu, like Arkanoid)
 *  - an SLR-viewfinder HUD in LiveView: exposure needle, red LED readout
 *    that lights on half-shutter, frame counter, wind reminder
 *  - PLAY locked until the roll is finished
 *
 * Shots are detected by watching the file number, so every frame is
 * logged even in burst mode.
 */

#include <module.h>
#include <dryos.h>
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

#include <stdarg.h>

extern int menu_redraw_blocked;
extern int ml_started;

/* raw PROP_PICTURE_STYLE values */
#define PS_STANDARD   0x81
#define PS_LANDSCAPE  0x83
#define PS_MONO       0x86

struct film
{
    const char *key;        /* must match lab/films.py */
    const char *brand;
    const char *name;
    const char *type;
    const char *desc;
    int band, ink;          /* canister label colours (ML palette) */
    int rated_iso;          /* box speed */
    int iso;                /* what the camera is set to (EOS M has no ISO 50) */
    int picstyle;
    int contrast, saturation, color_tone, sharpness;
    int kelvin;
    int wbs_gm;             /* + green, - magenta (-9..9) */
    int wbs_ba;             /* + amber, - blue    (-9..9) */
};

/* Picture style values are starting points for the in-camera JPEG
 * preview only; the real look comes from the lab. Mono stocks leave
 * saturation / tone alone because the Monochrome style uses those slots
 * for filter and toning effects. */
static const struct film films[] =
{
    { "portra400", "KODAK", "Portra 400", "COLOR NEG  C-41", "Soft warm skin, gentle highlights",
      COLOR_ORANGE, COLOR_BLACK, 400, 400, PS_STANDARD, -1, -1, 1, 2, 5500, 0, 1 },
    { "cinestill800t", "CINESTILL", "800T", "COLOR NEG  C-41", "Tungsten cine film, night goes blue",
      COLOR_RED, COLOR_WHITE, 800, 800, PS_STANDARD, 1, 1, -1, 2, 3200, 0, 0 },
    { "ektar100", "KODAK", "Ektar 100", "COLOR NEG  C-41", "Punchy colour, ultra fine grain",
      COLOR_DARK_RED, COLOR_WHITE, 100, 100, PS_LANDSCAPE, 1, 2, 0, 3, 5200, 0, 0 },
    { "gold200", "KODAK", "Gold 200", "COLOR NEG  C-41", "Golden hour, all day long",
      COLOR_YELLOW, COLOR_BLACK, 200, 200, PS_STANDARD, 1, 1, 2, 2, 5600, 0, 2 },
    { "superia400", "FUJIFILM", "Superia 400", "COLOR NEG  C-41", "Green shadows, everyday snaps",
      COLOR_GREEN2, COLOR_WHITE, 400, 400, PS_STANDARD, 1, 1, 0, 2, 5200, 1, 0 },
    { "velvia50", "FUJIFILM", "Velvia 50", "SLIDE  E-6", "Vivid slide film, hard contrast",
      COLOR_MAGENTA, COLOR_WHITE, 50, 100, PS_LANDSCAPE, 3, 4, 0, 3, 5200, 0, 0 },
    { "hp5", "ILFORD", "HP5 Plus", "B&W", "Classic B&W, forgiving latitude",
      COLOR_ALMOST_BLACK, COLOR_WHITE, 400, 400, PS_MONO, 1, 0, 0, 2, 5200, 0, 0 },
    { "trix400", "KODAK", "Tri-X 400", "B&W", "Gritty contrast, street classic",
      COLOR_DARK_ORANGE_MOD, COLOR_WHITE, 400, 400, PS_MONO, 3, 0, 0, 2, 5200, 0, 0 },
};
#define NUM_FILMS ((int) COUNT(films))

static const int exposure_choices[] = { 36, 24, 12 };
#define NUM_EXPOSURE_CHOICES ((int) COUNT(exposure_choices))

/* f-numbers x100 in 1/3 stops; index i sits (i - 1) thirds above f/1
 * (f/0.95 is close enough to f/1 to share the bottom of the scale) */
static const int fstops[] = {
     95,  100,  110,  120,  140,  160,  180,  200,  220,  250,
    280,  320,  350,  400,  450,  500,  560,  630,  710,  800,
    900, 1000, 1100, 1300, 1400, 1600, 1800, 2000, 2200,
};
#define NUM_FSTOPS ((int) COUNT(fstops))
#define FSTOP_CHOICES CHOICES("0.95", "1.0", "1.1", "1.2", "1.4", "1.6", "1.8", "2.0", "2.2", "2.5", \
                              "2.8", "3.2", "3.5", "4", "4.5", "5.0", "5.6", "6.3", "7.1", "8", \
                              "9", "10", "11", "13", "14", "16", "18", "20", "22")

/* user settings */
static CONFIG_INT("filmroll.film",      film_sel,         0);
static CONFIG_INT("filmroll.exp",       exp_sel,          0);
static CONFIG_INT("filmroll.hud",       hud_enabled,      1);
static CONFIG_INT("filmroll.focus",     focus_screen,     1);
static CONFIG_INT("filmroll.leds",      leds_on_half,     1);
static CONFIG_INT("filmroll.wind",      wind_mode,        1);   /* 0 off, 1 reminder, 2 block shutter */
static CONFIG_INT("filmroll.playlock",  play_lock,        1);
static CONFIG_INT("filmroll.startup",   startup_picker,   2);   /* 0 off, 1 when no film, 2 always */
static CONFIG_INT("filmroll.debug",     debug_log,        1);
static CONFIG_INT("filmroll.lens_wide", lens_widest,      6);   /* index into fstops: f/1.8 */
static CONFIG_INT("filmroll.fstop",     manual_fstop,     6);
static CONFIG_INT("filmroll.ap_keys",   aperture_keys,    0);   /* 0 wheel in Av, 1 up/down, 2 menu only */

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

/* ---------------------------------------------------------------- debug log */

/* field-test trace in ML/FILMROLL.LOG; turn off in the menu once things work */
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

    FILE *f = FIO_CreateFileOrAppend("ML/FILMROLL.LOG");
    if (!f)
        return;
    my_fprintf(f, "%s\n", buf);
    FIO_CloseFile(f);
}

#define DBG_STATE(what) dbg("%s: ml=%d lv=%d idle=%d menu=%d movie=%d mode=%d", what, \
                            ml_started, lv, display_idle(), gui_menu_shown(), is_movie_mode(), shooting_mode)

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
             note, roll_number, films[roll_film].key, roll_exposures, when);
    log_append(header);
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
    const struct film *f = &films[index];

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

/* ---------------------------------------------------------------- aperture */

/* manual (adapted) lenses report no aperture; the user dials it in instead */
static int lens_is_manual()
{
    return !lens_info.raw_aperture;
}

/* index into fstops for the aperture in use */
static int current_fstop()
{
    if (lens_is_manual())
        return COERCE(manual_fstop, lens_widest, NUM_FSTOPS - 1);

    int f100 = lens_info.aperture * 10;         /* lens_info.aperture is f x10 */
    int best = 0;
    for (int i = 1; i < NUM_FSTOPS; i++)
        if (ABS(fstops[i] - f100) < ABS(fstops[best] - f100))
            best = i;
    return best;
}

static void format_fstop(char *buf, int size, int index)
{
    int v = fstops[COERCE(index, 0, NUM_FSTOPS - 1)];
    if (v < 100)
        snprintf(buf, size, "0.%02d", v);
    else if (v % 100 == 0 || v >= 1000)
        snprintf(buf, size, "%d", v / 100);
    else
        snprintf(buf, size, "%d.%d", v / 100, (v / 10) % 10);
}

static void step_manual_fstop(int dir)
{
    manual_fstop = COERCE(current_fstop() + dir, lens_widest, NUM_FSTOPS - 1);
}

/* ---------------------------------------------------------------- rolls */

static void load_roll(int film, int exposures)
{
    roll_number = roll_number % 999 + 1;
    roll_film = film;
    roll_exposures = exposures;
    roll_folder = get_shooting_card()->folder_number;
    frames_shot = 0;
    need_wind = 0;
    last_file_number = get_shooting_card()->file_number;
    roll_loaded = 1;
    update_log_path();

    apply_film(roll_film);
    write_header("");

    hud_dirty = 1;
    beep();
    NotifyBox(3000, "ROLL %03d loaded: %s %s, %d exp", roll_number,
              films[roll_film].brand, films[roll_film].name, roll_exposures);
}

static void load_roll_task(int unused)
{
    load_roll(film_sel, exposure_choices[exp_sel]);
}

static void reapply_task(int unused)
{
    if (!roll_loaded) return;
    apply_film(roll_film);
    NotifyBox(2000, "%s settings applied", films[roll_film].name);
}

static void record_frame(int file_number)
{
    if (frames_shot >= roll_exposures)
    {
        NotifyBox(3000, "END OF ROLL - press SET for a new roll (not logged)");
        beep();
        return;
    }

    frames_shot++;

    char when[32];
    char aperture[8];
    char line[112];
    format_now(when, sizeof(when));
    format_fstop(aperture, sizeof(aperture), current_fstop());
    snprintf(line, sizeof(line), "%d  %s%04d.CR2  %s  f/%s\n",
             frames_shot, get_file_prefix(), file_number, when, aperture);
    log_append(line);

    leds_until = get_ms_clock() + 2500;
    hud_dirty = 1;

    if (frames_shot == roll_exposures)
    {
        need_wind = 0;
        NotifyBox(4000, "END OF ROLL  %d/%d  - SET: load a new roll", frames_shot, roll_exposures);
        beep_times(2);
        return;
    }

    if (wind_mode)
        need_wind = 1;
    if (!hud_enabled)
        NotifyBox(1500, "%s  %d/%d", films[roll_film].name, frames_shot, roll_exposures);
}

/* polled from the shoot task: log every new file number since the last poll */
static unsigned int filmroll_poll(unsigned int ctx)
{
    if (!roll_loaded)
        return CBR_RET_CONTINUE;

    int now = get_shooting_card()->file_number;
    if (now == last_file_number)
        return CBR_RET_CONTINUE;

    int prev = last_file_number;
    last_file_number = now;

    int count = now - prev;
    if (count < 0)
        count += 9999;                  /* IMG_9999 -> IMG_0001 */
    if (prev < 0 || count > 50)
        return CBR_RET_CONTINUE;        /* card swap or counter reset: just resync */

    if (get_shooting_card()->folder_number != roll_folder)
    {
        /* Canon started a new folder; start a log there too so each folder
         * develops on its own (frame numbers keep counting) */
        roll_folder = get_shooting_card()->folder_number;
        update_log_path();
        write_header(" - continued from the previous DCIM folder");
    }

    for (int i = 1; i <= count; i++)
        record_frame((prev + i - 1) % 9999 + 1);

    return CBR_RET_CONTINUE;
}

/* ---------------------------------------------------------------- drawing helpers */

#define SCREEN_W 720
#define SCREEN_H 480

#define FONT_BIG(fg, bg)    FONT(FONT_LARGE, fg, bg)
#define FONT_MID(fg, bg)    FONT(FONT_MED, fg, bg)
#define FONT_TINY(fg, bg)   FONT(FONT_SMALL, fg, bg)

static void print_centered(uint32_t font, int cx, int y, const char *text)
{
    bmp_printf(font, cx - bmp_string_width(font, text) / 2, y, "%s", text);
}

/* film canister; w = body width, height is 1.5 w */
static void draw_can(const struct film *f, int x, int y, int w, int labels)
{
    int h = w * 3 / 2;
    int cap_w = w * 2 / 5;
    int cap_x = x + (w - cap_w) / 2;
    int cap_h = MAX(3, h / 16);
    int body = COLOR_ALMOST_WHITE;
    int band_y = y + h * 2 / 5;
    int band_h = h / 4;

    bmp_fill(COLOR_GRAY(60), cap_x, y, cap_w, cap_h);
    bmp_fill(body, x, y + cap_h, w, h - 2 * cap_h);
    bmp_fill(f->band, x, band_y, w, band_h);
    bmp_fill(COLOR_GRAY(60), cap_x, y + h - cap_h, cap_w, cap_h);

    if (!labels)
        return;

    char iso[8];
    snprintf(iso, sizeof(iso), "%d", f->rated_iso);
    print_centered(FONT_TINY(COLOR_GRAY(30), body), x + w / 2, y + h / 5, f->brand);
    print_centered(FONT_BIG(f->ink, f->band), x + w / 2, band_y + (band_h - 32) / 2, iso);
    print_centered(FONT_TINY(COLOR_GRAY(40), body), x + w / 2, y + h * 3 / 4, "35mm");
}

/* ---------------------------------------------------------------- film picker */

enum { PICK_NONE, PICK_LOAD, PICK_EXIT };

static volatile int picker_running = 0;
static volatile int picker_film = 0;
static volatile int picker_exp = 0;
static volatile int picker_action = PICK_NONE;
static volatile int picker_dirty = 0;
static volatile int picker_confirm = 0;
static volatile int picker_home = 0;

static const char *picstyle_name(int ps)
{
    switch (ps)
    {
        case PS_STANDARD:  return "Standard";
        case PS_LANDSCAPE: return "Landscape";
        case PS_MONO:      return "Mono";
    }
    return "?";
}

static void picker_draw()
{
    const struct film *f = &films[picker_film];
    const int bg = COLOR_BLACK;

    bmp_fill(bg, 0, 0, SCREEN_W, SCREEN_H);

    /* header */
    int x = bmp_printf(FONT_BIG(COLOR_ORANGE, bg), 24, 12, "FILM");
    bmp_printf(FONT_MID(COLOR_GRAY(60), bg), 24 + x + 20, 18, "choose a roll");
    bmp_fill(COLOR_GRAY(30), 24, 56, SCREEN_W - 48, 1);

    /* big canister */
    draw_can(f, 60, 82, 130, 1);

    /* details */
    int tx = 240;
    bmp_printf(FONT_TINY(COLOR_ORANGE, bg), tx, 84, "%s", f->type);
    bmp_printf(FONT_MID(COLOR_GRAY(60), bg), tx, 104, "%s", f->brand);
    bmp_printf(FONT_BIG(COLOR_WHITE, bg), tx, 130, "%s", f->name);
    if (f->iso != f->rated_iso)
        bmp_printf(FONT_MID(COLOR_WHITE, bg), tx, 172, "ISO %d (camera %d)   %d exp",
                   f->rated_iso, f->iso, exposure_choices[picker_exp]);
    else
        bmp_printf(FONT_MID(COLOR_WHITE, bg), tx, 172, "ISO %d   %d exp",
                   f->rated_iso, exposure_choices[picker_exp]);
    bmp_printf(FONT_MID(COLOR_GRAY(70), bg), tx, 206, "%s", f->desc);
    if (f->picstyle == PS_MONO)
        bmp_printf(FONT_TINY(COLOR_GRAY(50), bg), tx, 244, "%s C%+d  %dK",
                   picstyle_name(f->picstyle), f->contrast, f->kelvin);
    else
        bmp_printf(FONT_TINY(COLOR_GRAY(50), bg), tx, 244, "%s C%+d S%+d T%+d  %dK",
                   picstyle_name(f->picstyle), f->contrast, f->saturation, f->color_tone, f->kelvin);

    /* strip of canisters */
    int can_w = 30, gap = 52;
    int strip_w = NUM_FILMS * can_w + (NUM_FILMS - 1) * gap;
    int sx = (SCREEN_W - strip_w) / 2;
    for (int i = 0; i < NUM_FILMS; i++)
    {
        int cx = sx + i * (can_w + gap);
        int lift = (i == picker_film) ? 10 : 0;
        draw_can(&films[i], cx, 330 - lift, can_w, 0);
        if (i == picker_film)
            bmp_fill(COLOR_ORANGE, cx - 4, 330 + can_w * 3 / 2 + 4, can_w + 8, 4);
    }

    /* film in the camera, top right */
    if (ROLL_IN_PROGRESS)
    {
        char status[48];
        snprintf(status, sizeof(status), "IN CAMERA: %s  %d/%d",
                 films[roll_film].name, frames_shot, roll_exposures);
        bmp_printf(FONT_MID(COLOR_ORANGE, bg),
                   SCREEN_W - 24 - bmp_string_width(FONT_MID(COLOR_ORANGE, bg), status), 18, "%s", status);
    }

    /* loading over a roll in progress needs a second SET */
    if (picker_confirm)
    {
        bmp_fill(COLOR_DARK_RED, 0, SCREEN_H - 96, SCREEN_W, 48);
        bmp_printf(FONT_MID(COLOR_WHITE, COLOR_DARK_RED), 24, SCREEN_H - 84,
                   "Roll %03d has %d left. SET again to rewind it.",
                   roll_number, roll_exposures - frames_shot);
    }

    /* footer */
    int fb = COLOR_GRAY(15);
    bmp_fill(fb, 0, SCREEN_H - 44, SCREEN_W, 44);
    bmp_printf(FONT_MID(COLOR_WHITE, fb), 24, SCREEN_H - 36,
               picker_home ? "< > film   INFO: exp   SET: load   MENU: camera"
                           : "< > film   INFO: exp   SET: load   MENU: back");
}

/* home: opened from LiveView (startup, PLAY, end of roll) rather than
 * from the ML menu, so leaving it goes straight back to the camera */
static void picker_run(int home)
{
    picker_film = ROLL_IN_PROGRESS ? roll_film : film_sel;
    picker_exp = exp_sel;
    picker_action = PICK_NONE;
    picker_confirm = 0;
    picker_home = home;
    picker_dirty = 1;
    picker_running = 1;
    menu_redraw_blocked = 1;
    dbg("picker: start (home %d)", home);

    while (gui_menu_shown() && picker_action == PICK_NONE)
    {
        if (picker_dirty)
        {
            picker_dirty = 0;
            picker_draw();
        }
        msleep(40);
    }

    int action = picker_action;
    picker_running = 0;
    menu_redraw_blocked = 0;
    dbg("picker: end (action %d, menu still shown %d)", action, gui_menu_shown());

    if (action == PICK_LOAD)
    {
        film_sel = picker_film;
        exp_sel = picker_exp;
        clrscr();
        gui_stop_menu();
        msleep(300);
        load_roll(film_sel, exposure_choices[exp_sel]);
    }
    else if (gui_menu_shown())
    {
        if (home)
        {
            clrscr();
            gui_stop_menu();
        }
        else
        {
            menu_redraw();
        }
    }
    hud_dirty = 1;
}

static void picker_task(void *unused)
{
    picker_run(0);
}

/* from LiveView: open the ML menu and cover it with the picker */
static void picker_home_task(int unused)
{
    if (picker_running)
        return;

    /* keep the ML menu from drawing itself, so the picker appears directly */
    menu_redraw_blocked = 1;
    gui_open_menu();
    for (int i = 0; i < 20 && !gui_menu_shown(); i++)
        msleep(50);
    if (!gui_menu_shown())
    {
        menu_redraw_blocked = 0;
        DBG_STATE("picker: ML menu did not open");
        return;
    }
    picker_run(1);
}

/* the picker is the camera's start screen */
static void startup_task(void *unused)
{
    /* wait (up to ~15 s) for ML to finish booting and LiveView to settle */
    for (int i = 0; i < 150; i++)
    {
        if (ml_started && lv && display_idle() && !gui_menu_shown())
            break;
        if (i % 10 == 0 && ml_started)
            DBG_STATE("startup waiting");
        msleep(100);
    }
    msleep(800);

    DBG_STATE("startup check");
    if (!ml_started || !lv || !display_idle() || gui_menu_shown() || is_movie_mode())
    {
        dbg("startup: not on the LiveView screen, picker skipped");
        return;
    }
    if (startup_picker == 2 || (startup_picker == 1 && !ROLL_IN_PROGRESS))
        picker_home_task(0);
    else
        dbg("startup: picker not wanted (setting %d, roll in progress %d)", startup_picker, ROLL_IN_PROGRESS);
}

static unsigned int picker_key(unsigned int key)
{
    /* anything but a second SET cancels a pending "rewind this roll?" */
    if (picker_confirm && key != MODULE_KEY_PRESS_SET && key != MODULE_KEY_UNPRESS_SET)
    {
        picker_confirm = 0;
        picker_dirty = 1;
    }

    switch (key)
    {
        case MODULE_KEY_WHEEL_LEFT:
        case MODULE_KEY_PRESS_LEFT:
            picker_film = (picker_film + NUM_FILMS - 1) % NUM_FILMS;
            picker_dirty = 1;
            return 0;

        case MODULE_KEY_WHEEL_RIGHT:
        case MODULE_KEY_PRESS_RIGHT:
            picker_film = (picker_film + 1) % NUM_FILMS;
            picker_dirty = 1;
            return 0;

        case MODULE_KEY_INFO:
        case MODULE_KEY_PRESS_UP:
        case MODULE_KEY_PRESS_DOWN:
            picker_exp = (picker_exp + 1) % NUM_EXPOSURE_CHOICES;
            picker_dirty = 1;
            return 0;

        case MODULE_KEY_PRESS_SET:
            if (ROLL_IN_PROGRESS && !picker_confirm)
            {
                picker_confirm = 1;
                picker_dirty = 1;
            }
            else
            {
                picker_action = PICK_LOAD;
            }
            return 0;

        case MODULE_KEY_MENU:
        case MODULE_KEY_TRASH:
            picker_action = PICK_EXIT;
            return 0;

        /* swallow the rest of the navigation so the menu underneath stays put */
        case MODULE_KEY_UNPRESS_SET:
        case MODULE_KEY_UNPRESS_UDLR:
        case MODULE_KEY_JOY_CENTER:
        case MODULE_KEY_PLAY:
        case MODULE_KEY_Q:
            return 0;
    }
    return 1;   /* e.g. half-shutter closes the menu, which ends the picker */
}

/* ---------------------------------------------------------------- viewfinder HUD */

#define STRIP_W   64                    /* meter strip on the right */
#define BAR_H     42                    /* LED readout at the bottom */
#define VIEW_W    (SCREEN_W - STRIP_W)
#define VIEW_H    (SCREEN_H - BAR_H)
#define THIRD_PX  14                    /* scale spacing per 1/3 stop */
#define SCALE_TOP 22                    /* y of f/22; f/1 sits 27 thirds below */

/* y on the aperture scale for fstops[index] (f/22 at the top, like an AE-1) */
static int fstop_y(int index)
{
    return SCALE_TOP + (NUM_FSTOPS - 1 - index) * THIRD_PX;
}

static int hud_visible()
{
    return hud_enabled && roll_loaded && lv && !gui_menu_shown() && display_idle()
        && !is_movie_mode() && !picker_running;
}

static void draw_needle(int y, int color)
{
    int x0 = SCREEN_W - STRIP_W + 4;
    for (int r = -7; r <= 7; r++)
        bmp_fill(color, x0, y + r, 16 - 2 * ABS(r), 1);
}

static void hud_draw(int lit, int blink)
{
    const int on = COLOR_RED;
    const int off = COLOR_GRAY(10);
    const int bg = COLOR_BLACK;

    /* aperture scale: f/22 at the top, full stops labelled, red zone above
     * f/22 (too much light) and below the lens' widest stop (too little) */
    int sx = SCREEN_W - STRIP_W;
    int cur = current_fstop();
    int widest = lens_is_manual() ? lens_widest : 0;
    char label[8];

    bmp_fill(bg, sx, 0, STRIP_W, VIEW_H);
    bmp_fill(COLOR_DARK_RED, sx + 26, 2, 5, SCALE_TOP - 8);
    bmp_fill(COLOR_DARK_RED, sx + 26, fstop_y(widest) + 8, 5, 18);

    for (int i = 1; i < NUM_FSTOPS; i += 3)     /* f/1, 1.4, 2 ... 22 */
    {
        int y = fstop_y(i);
        int usable = i >= widest;
        format_fstop(label, sizeof(label), i);
        bmp_fill(COLOR_GRAY(usable ? 50 : 20), sx + 24, y, 8, 1);
        bmp_printf(FONT_TINY(i == cur ? COLOR_WHITE : COLOR_GRAY(usable ? 60 : 25), bg),
                   sx + 35, y - 6, "%s", label);
    }

    /* the aperture you set: white tick */
    bmp_fill(COLOR_WHITE, sx + 20, fstop_y(cur) - 1, 14, 3);

    /* the needle points at the aperture the meter asks for */
    if (lit)
    {
        int ev8 = get_ae_value();               /* meter reading, 1/8 EV, + = too bright */
        int want = cur + (ev8 * 3 + (ev8 >= 0 ? 4 : -4)) / 8;
        int y;
        int out_of_range = want > NUM_FSTOPS - 1 || want < widest;
        if (want > NUM_FSTOPS - 1)
            y = SCALE_TOP - 10;
        else if (want < widest)
            y = fstop_y(widest) + 16;
        else
            y = fstop_y(want);
        if (!out_of_range || blink)
            draw_needle(y, on);
    }

    /* LED readout */
    int by = VIEW_H;
    bmp_fill(bg, 0, by, SCREEN_W, BAR_H);
    int ty = by + (BAR_H - 32) / 2;

    /* the wind reminder blinks whether or not the meter is on */
    int wind_col = !need_wind ? off : (blink ? on : COLOR_DARK_RED);
    bmp_printf(FONT_BIG(wind_col, bg), 16, ty, "WIND");

    bmp_printf(FONT_BIG(lit ? on : off, bg), 170, ty, "%s",
               lens_info.raw_shutter ? lens_format_shutter(lens_info.raw_shutter) : "--");
    format_fstop(label, sizeof(label), cur);
    bmp_printf(FONT_BIG(lit ? on : off, bg), 330, ty, "f%s", label);
    fill_circle(480, by + BAR_H / 2, 7, lit ? on : off);

    int fc = (lit || ROLL_FINISHED) ? on : off;
    if (ROLL_FINISHED)
        bmp_printf(FONT_BIG(blink ? fc : off, bg), 560, ty, "END");
    else
        bmp_printf(FONT_BIG(fc, bg), 560, ty, "No.%d", frames_shot + 1);

    /* focusing screen: microprism collar and split-image circle */
    if (focus_screen)
    {
        int cx = VIEW_W / 2, cy = VIEW_H / 2;
        draw_circle(cx, cy, 62, COLOR_GRAY(70));
        draw_circle(cx, cy, 28, COLOR_WHITE);
        draw_line(cx - 28, cy, cx + 28, cy, COLOR_WHITE);
    }
}

/* make our strips transparent again so LiveView shows through */
static void hud_clear()
{
    bmp_fill(COLOR_EMPTY, SCREEN_W - STRIP_W, 0, STRIP_W, VIEW_H);
    bmp_fill(COLOR_EMPTY, 0, VIEW_H, SCREEN_W, BAR_H);
    bmp_fill(COLOR_EMPTY, VIEW_W / 2 - 64, VIEW_H / 2 - 64, 129, 129);
}

static void hud_task(void *unused)
{
    int last_sig = -1;
    int last_draw = 0;
    int drawn = 0;
    int was_visible = -1;

    while (1)
    {
        msleep(100);

        int visible = hud_visible();
        if (visible != was_visible)
        {
            dbg("hud: visible %d (enabled %d roll %d lv %d menu %d idle %d movie %d picker %d)",
                visible, hud_enabled, roll_loaded, lv, gui_menu_shown(), display_idle(),
                is_movie_mode(), picker_running);
            was_visible = visible;
        }

        if (!visible)
        {
            /* HUD switched off: wipe it once we're back on the LiveView screen */
            if (drawn && !hud_enabled && lv && !gui_menu_shown() && display_idle())
            {
                hud_clear();
                drawn = 0;
            }
            last_sig = -1;
            continue;
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
}

/* ---------------------------------------------------------------- keys */

static unsigned int filmroll_keypress(unsigned int key)
{
    if (picker_running)
        return picker_key(key);

    /* only on the plain LiveView screen: never steal keys from Canon's menus or dialogs */
    if (gui_menu_shown() || !lv || !roll_loaded || !display_idle())
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
            run_in_separate_task(picker_home_task, 0);
            return 0;
        }
        return 1;
    }

    /* no peeking during a roll: PLAY brings up the film picker instead */
    if (key == MODULE_KEY_PLAY && play_lock)
    {
        dbg("key: PLAY -> picker");
        run_in_separate_task(picker_home_task, 0);
        return 0;
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
        {
            NotifyBox(1500, "Wind on first (SET)");
            return 0;
        }
    }

    return 1;
}

/* ---------------------------------------------------------------- menu */

static MENU_UPDATE_FUNC(filmroll_main_upd)
{
    if (roll_loaded)
        MENU_SET_VALUE("%s %d/%d", films[roll_film].name, frames_shot, roll_exposures);
    else
        MENU_SET_VALUE("no roll");
}

static MENU_UPDATE_FUNC(filmroll_status_upd)
{
    if (!roll_loaded)
    {
        MENU_SET_VALUE("no roll loaded");
        return;
    }
    MENU_SET_VALUE("ROLL %03d", roll_number);
    MENU_SET_RINFO("%d left", roll_exposures - frames_shot);
    MENU_SET_HELP("Log: %s", log_path);
}

static MENU_UPDATE_FUNC(lens_widest_upd)
{
    manual_fstop = MAX(manual_fstop, lens_widest);
    if (!lens_is_manual())
        MENU_SET_WARNING(MENU_WARN_INFO, "Electronic lens attached: its own aperture is used.");
}

static MENU_UPDATE_FUNC(manual_fstop_upd)
{
    manual_fstop = MAX(manual_fstop, lens_widest);
    if (!lens_is_manual())
        MENU_SET_WARNING(MENU_WARN_INFO, "Electronic lens attached: its own aperture is used.");
}

static MENU_SELECT_FUNC(picker_select)
{
    if (!picker_running)
        task_create("film_pick", 0x1c, 0x2000, picker_task, 0);
}

static MENU_SELECT_FUNC(load_roll_select)
{
    run_in_separate_task(load_roll_task, 0);
}

static MENU_SELECT_FUNC(reapply_select)
{
    run_in_separate_task(reapply_task, 0);
}

static struct menu_entry filmroll_menu[] =
{
    {
        .name           = "Film roll",
        .select         = menu_open_submenu,
        .update         = filmroll_main_upd,
        .help           = "Shoot like a film camera: pick a stock, load a roll, shoot it out.",
        .help2          = "Set RAW (or RAW+JPEG) so the PC lab can develop the roll.",
        .submenu_width  = 720,
        .children       = (struct menu_entry[]) {
            {
                .name       = "Choose film...",
                .select     = picker_select,
                .icon_type  = IT_ACTION,
                .help       = "Full-screen film picker. SET loads the roll.",
            },
            {
                .name       = "Film stock",
                .priv       = &film_sel,
                .max        = NUM_FILMS - 1,
                .choices    = CHOICES("Portra 400", "CineStill 800T", "Ektar 100", "Gold 200",
                                      "Superia 400", "Velvia 50", "HP5 Plus 400", "Tri-X 400"),
                .help       = "Film for the next roll. Takes effect when you load a roll.",
            },
            {
                .name       = "Exposures",
                .priv       = &exp_sel,
                .max        = NUM_EXPOSURE_CHOICES - 1,
                .choices    = CHOICES("36", "24", "12"),
                .help       = "Frames on the next roll.",
            },
            {
                .name       = "Load new roll",
                .select     = load_roll_select,
                .icon_type  = IT_ACTION,
                .help       = "Applies the film settings and starts a new roll log.",
                .help2      = "Any roll in progress ends here.",
            },
            {
                .name       = "Viewfinder HUD",
                .priv       = &hud_enabled,
                .max        = 1,
                .help       = "SLR-style meter needle and red LED readout in LiveView.",
                .help2      = "Works best with Canon's clean display (INFO) and Global Draw off.",
            },
            {
                .name       = "LEDs on half-press",
                .priv       = &leds_on_half,
                .max        = 1,
                .help       = "Like a real SLR: the readout lights only while metering.",
            },
            {
                .name       = "Focusing screen",
                .priv       = &focus_screen,
                .max        = 1,
                .help       = "Draws a split-image circle and microprism collar (decoration only).",
            },
            {
                .name       = "Lens widest aperture",
                .priv       = &lens_widest,
                .max        = NUM_FSTOPS - 1,
                .choices    = FSTOP_CHOICES,
                .update     = lens_widest_upd,
                .help       = "Manual lenses only: the widest f-stop of your lens (e.g. 1.8).",
                .help2      = "The aperture ring won't go wider than this.",
            },
            {
                .name       = "Manual aperture",
                .priv       = &manual_fstop,
                .max        = NUM_FSTOPS - 1,
                .choices    = FSTOP_CHOICES,
                .update     = manual_fstop_upd,
                .help       = "Manual lenses only: the f-stop set on the lens ring.",
                .help2      = "Shown in the HUD and written to the roll log for the lab.",
            },
            {
                .name       = "Aperture ring",
                .priv       = &aperture_keys,
                .max        = 2,
                .choices    = CHOICES("Wheel in Av", "Up / Down", "Menu only"),
                .help       = "How to change Manual aperture in LiveView.",
                .help2      = "Wheel in Av: in Av mode the wheel is free with a manual lens.",
            },
            {
                .name       = "Wind lever",
                .priv       = &wind_mode,
                .max        = 2,
                .choices    = CHOICES("OFF", "Reminder", "Block shutter"),
                .help       = "After each shot press SET to wind on.",
                .help2      = "Block shutter is experimental: it may not stop a full press.",
            },
            {
                .name       = "Picker at startup",
                .priv       = &startup_picker,
                .max        = 2,
                .choices    = CHOICES("OFF", "No film loaded", "Always"),
                .help       = "Show the film picker when the camera turns on.",
                .help2      = "MENU or half-shutter leaves it and keeps the film in the camera.",
            },
            {
                .name       = "Lock PLAY",
                .priv       = &play_lock,
                .max        = 1,
                .help       = "No peeking: during a roll PLAY opens the film picker instead.",
                .help2      = "Playback works again once the roll is finished.",
            },
            {
                .name       = "Debug log",
                .priv       = &debug_log,
                .max        = 1,
                .help       = "Writes ML/FILMROLL.LOG for troubleshooting. Turn off when all works.",
            },
            {
                .name       = "Re-apply film settings",
                .select     = reapply_select,
                .icon_type  = IT_ACTION,
                .help       = "Use after changing picture style, WB or ISO by hand.",
            },
            {
                .name       = "Current roll",
                .update     = filmroll_status_upd,
                .icon_type  = IT_ALWAYS_ON,
            },
            MENU_EOL,
        },
    },
};

static unsigned int filmroll_init()
{
    if (roll_loaded)
        update_log_path();
    menu_add("Shoot", filmroll_menu, COUNT(filmroll_menu));
    task_create("film_hud", 0x1e, 0x1000, hud_task, 0);
    if (startup_picker)
        task_create("film_start", 0x1e, 0x1000, startup_task, 0);
    dbg("init: v2 build %s %s, roll %d loaded %d shot %d/%d, startup %d hud %d",
        __DATE__, __TIME__, roll_number, roll_loaded, frames_shot, roll_exposures,
        startup_picker, hud_enabled);
    return 0;
}

static unsigned int filmroll_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(filmroll_init)
    MODULE_DEINIT(filmroll_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, filmroll_poll, 0)
    MODULE_CBR(CBR_KEYPRESS, filmroll_keypress, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(film_sel)
    MODULE_CONFIG(exp_sel)
    MODULE_CONFIG(hud_enabled)
    MODULE_CONFIG(focus_screen)
    MODULE_CONFIG(leds_on_half)
    MODULE_CONFIG(wind_mode)
    MODULE_CONFIG(play_lock)
    MODULE_CONFIG(startup_picker)
    MODULE_CONFIG(debug_log)
    MODULE_CONFIG(lens_widest)
    MODULE_CONFIG(manual_fstop)
    MODULE_CONFIG(aperture_keys)
    MODULE_CONFIG(roll_loaded)
    MODULE_CONFIG(roll_number)
    MODULE_CONFIG(roll_film)
    MODULE_CONFIG(roll_exposures)
    MODULE_CONFIG(frames_shot)
    MODULE_CONFIG(last_file_number)
    MODULE_CONFIG(roll_folder)
    MODULE_CONFIG(need_wind)
MODULE_CONFIGS_END()
