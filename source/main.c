/* HDD/SSD Health: IDENTIFY + SMART of the PS3's internal drive on the TV, from a HEN app.
 * Drawing follows ErikPshat/GamePad-Test (tiny3d + libfont3d), with a 16x32
 * bitmap of JetBrains Mono built in (font.c, from art/make_font.py): no file
 * read before the first frame, and a monospace table. Three pages: the
 * attribute table, a summary, and a QR code that opens a prefilled issue. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-lv2.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include <tiny3d.h>
#include <libfont.h>
#include "ata.h"
#include "font.h"
#include "vendor.h"
#include "compat.h"
#include "version.h"
#include "qrcodegen.h"

#define BTN_LEFT     0x8000
#define BTN_DOWN     0x4000
#define BTN_RIGHT    0x2000
#define BTN_UP       0x1000
#define BTN_START    0x0800
#define BTN_SELECT   0x0100
#define BTN_SQUARE   0x0080
#define BTN_CROSS    0x0040
#define BTN_CIRCLE   0x0020
#define BTN_TRIANGLE 0x0010
#define BTN_R1       0x0008
#define BTN_L1       0x0004
#define BTN_R2       0x0002
#define BTN_L2       0x0001

#define WHITE  0xffffffff
#define GREY   0x9a9a9aff
#define GREEN  0x5ce05cff
#define YELLOW 0xffd23cff
#define RED    0xff5a5aff
#define BLUE   0x78c8ffff
#define BLACK  0x000000ff
#define PANEL  0x182640ff            /* the attribute table, a step lighter than the background */

#define ROWS 13
#define PAGES 3
#define QR_MAX_VERSION 25

static drive_state D;
static char report_path[128];
static volatile int quit;               /* set by the system: Quit Game from the PS button menu */

/* the QR code of the compat report, rebuilt after each report_write */
static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static int qr_ok;
static char qr_body[1024];

static void sys_callback(u64 status, u64 param, void *usrdata)
{
    (void)param;
    (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) quit = 1;
}

/* ---- font and frame ------------------------------------------------------- */

static void init_graph(void)
{
    tiny3d_Init(1024 * 1024);
    tiny3d_Project2D();
    u32 *tex = tiny3d_AllocTexture(4 * 1024 * 1024);
    if (!tex) exit(0);
    ResetFont();
    AddFontFromBitmapArray(font, (u8 *)tex, 32, 255, 16, 32, 2, BIT0_FIRST_PIXEL);
    double sx = Video_Resolution.width, sy = Video_Resolution.height;
    tiny3d_UserViewport(1, 0, 0, sx / 848.0, sy / 512.0, sx / 1920.0, sy / 1080.0);
}

static void begin_frame(void)
{
    tiny3d_Clear(0xff101828, TINY3D_CLEAR_ALL);
    tiny3d_AlphaTest(1, 0x10, TINY3D_ALPHA_FUNC_GEQUAL);
    tiny3d_BlendFunc(1, TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
                     TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ZERO,
                     TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
}

static float text(float x, float y, int size, u32 color, const char *fmt, ...)
{
    char s[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    SetFontSize(size / 2, size);
    SetFontColor(color, 0);
    return DrawString(x, y, s);
}

static void rect(float x, float y, float w, float h, u32 rgba)
{
    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(x, y, 65535);
    tiny3d_VertexColor(rgba);
    tiny3d_VertexPos(x + w, y, 65535);
    tiny3d_VertexColor(rgba);
    tiny3d_VertexPos(x + w, y + h, 65535);
    tiny3d_VertexColor(rgba);
    tiny3d_VertexPos(x, y + h, 65535);
    tiny3d_VertexColor(rgba);
    tiny3d_End();
}

static void title(void)
{
    float x = text(30, 18, 30, WHITE, "HDD/SSD Health");
    x = text(x + 14, 30, 14, GREY, "v" APP_VERSION);
    if (D.demo) text(x + 14, 26, 18, YELLOW, "DEMO");
}

/* ---- blocking calls -------------------------------------------------------
 * A call that can freeze the console runs in a second thread while this one
 * draws a counter. A stopped counter is the freeze signal. */

static volatile int job_done;
static const char *volatile job_msg;        /* string literals only: no lifetime to manage */

static void progress(const char *msg) { job_msg = msg; }

static void job_thread(void *arg)
{
    ((void (*)(void))arg)();
    __sync_synchronize();
    job_done = 1;
    sysThreadExit(0);
}

static void draw_job(double sec)
{
    begin_frame();
    title();
    text(30, 120, 20, WHITE, "%s ...", job_msg);
    text(30, 165, 30, WHITE, "%c  %.1f s", "|/-\\"[(int)(sec * 6) & 3], sec);
    text(30, 220, 16, GREY, "The counter runs while the console works. If it stops, the console froze.");
    text(30, 242, 16, GREY, "Hold the power button, then start the app again: it skips the frozen step.");
    tiny3d_Flip();
}

static void run_job(const char *msg, void (*fn)(void))
{
    sys_ppu_thread_t t;
    u64 ret;
    job_msg = msg;
    job_done = 0;
    s64 t0 = sysGetSystemTime();
    if (sysThreadCreate(&t, job_thread, (void *)fn, 1500, 0x20000, THREAD_JOINABLE, "job") != 0) {
        draw_job(0);                         /* no thread: one frame, then the call in this thread */
        fn();
        return;
    }
    while (!job_done) draw_job((double)(sysGetSystemTime() - t0) / 1e6);
    sysThreadJoin(t, &ret);
}

static void job_probe(void) { drive_probe(&D, progress); }
static void job_console(void) { console_info(&D); }
static void job_smart(void) { drive_read_smart(&D, 1); }
static void job_speed(void) { drive_speed_test(&D); }
static int job_rc, st_type;                  /* the self-test being started: 1 short, 2 extended, 0 found running */
static void job_selftest(void) { job_rc = drive_start_selftest(&D, st_type); }
static void job_errlog(void) { drive_read_error_log(&D); }
static void job_gpl(void) { drive_read_gpl(&D); }


/* ---- pad ------------------------------------------------------------------ */

static unsigned held, pressed;

static void read_pad(void)
{
    padInfo info;
    padData pd;
    unsigned now = held;
    sysUtilCheckCallback();
    ioPadGetInfo(&info);
    for (int n = 0; n < MAX_PADS; n++)
        if (info.status[n]) {
            ioPadGetData(n, &pd);
            if (pd.len > 0) now = pd.button[2] << 8 | (pd.button[3] & 0xff);
            break;
        }
    pressed = now & ~held;
    held = now;
}

/* Two-press gestures (SQUARE clear journal, R2 speed test):
 * the first press arms, the same button fires, any other button disarms.
 * Returns the button that fired this frame, 0 otherwise. */
static unsigned armed;

static unsigned gesture(unsigned buttons)
{
    if (!pressed) return 0;
    if (pressed == armed) { armed = 0; return pressed; }
    armed = (pressed & buttons) && !(pressed & ~buttons) ? pressed : 0;
    return 0;
}

/* ---- first-run prompt -------------------------------------------------------
 * A command this console never ran is shown once: CROSS runs it (journaled
 * like the others), CIRCLE skips it and the journal remembers the skip until
 * SQUARE twice. Returns 1 to run, 0 to skip, -1 on exit. */
static int first_run_prompt(const char *title_s, const char *l1, const char *l2, const char *l3)
{
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CROSS) return 1;
        if (pressed & BTN_CIRCLE) return 0;
        begin_frame();
        title();
        text(30, 100, 20, YELLOW, "New on this console: %s", title_s);
        text(30, 150, 16, WHITE, "%s", l1);
        text(30, 172, 16, WHITE, "%s", l2);
        text(30, 194, 16, GREY, "%s", l3);
        text(30, 240, 16, GREY, "The call is written to the journal first. If it freezes the console, hold the power");
        text(30, 262, 16, GREY, "button and start the app again: it skips the call. A skip is kept until SQUARE twice.");
        text(30, 455, 14, GREY, "CROSS run it   CIRCLE skip it   START exit");
        tiny3d_Flip();
    }
}

typedef struct {
    const char *name;       /* the first journal entry the job writes: its state decides the prompt */
    const char *title, *l1, *l2, *l3, *progress;
    void (*job)(void);
    int (*wanted)(void);
} optional_step;

static int want_errlog(void) { return D.ata_ok && !D.demo; }
static int want_gpl(void) { return D.ata_ok && !D.demo && D.id.gpl; }

static const optional_step steps[] = {
    {"error_log", "SMART READ LOG 01h (the error log)",
     "Reads the drive's summary error log: how many command errors it recorded, and the last one.",
     "Same path and same kind of read as the self-test log, which this console already ran.",
     "Read-only. 512 bytes. Shown on the summary page and in the report.",
     "SMART READ LOG 01h (the error log)", job_errlog, want_errlog},
    {"gpl_devstat", "READ LOG EXT 2Fh (general purpose logs)",
     "Reads two logs: device statistics (04h: exact sectors written, endurance used,",
     "temperature extremes) and the SATA Phy counters (11h: CRC errors, link resets).",
     "Read-only. A 48-bit data-in command, the first on this path. Up to 5 reads of 512 bytes.",
     "READ LOG EXT (device statistics, Phy counters)", job_gpl, want_gpl},
};

/* Runs the optional steps: prompt on the first run, skip a declined one, run the rest. -1 on exit. */
static int optional_steps(void)
{
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        const optional_step *st = &steps[k];
        if (!st->wanted()) continue;
        int js = journal_state(st->name);
        if (js == J_SKIPPED) continue;
        if (js == J_NONE) {
            int r = first_run_prompt(st->title, st->l1, st->l2, st->l3);
            if (r < 0) return -1;
            if (r == 0) { journal_skip(st->name); continue; }
        }
        run_job(st->progress, st->job);
    }
    return 0;
}

/* ---- QR ------------------------------------------------------------------- */

static void build_qr(void)
{
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    static char url[2400];
    char title_s[96];
    compat_title(&D, title_s, sizeof title_s);
    compat_body(&D, qr_body, sizeof qr_body);
    /* A blank issue with title and body: the issue form (template=...) takes
     * field values on the web, but the GitHub mobile app, which catches the
     * link on a phone, fills only title and body. */
    const form_field f[] = {{"body", qr_body}};
    if (issue_form_url(NULL, title_s, f, 1, url, sizeof url) < 0) { qr_ok = 0; return; }
    qr_ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
}

static void write_report(void)
{
    report_write(&D, report_path, sizeof report_path);
    build_qr();
}

/* Dark modules as one quad per horizontal run, on a white field with the quiet zone. */
static void draw_qr(float x, float y, float box)
{
    if (!qr_ok) { text(x, y, 16, YELLOW, "The report is too long for a QR code."); return; }
    int n = qrcodegen_getSize(qr);
    float m = (float)(int)(box / (n + 8));
    if (m < 1) m = 1;
    float side = m * (n + 8);
    rect(x, y, side, side, WHITE);
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++) {
            if (!qrcodegen_getModule(qr, c, r)) continue;
            int c2 = c;
            while (c2 + 1 < n && qrcodegen_getModule(qr, c2 + 1, r)) c2++;
            rect(x + (c + 4) * m, y + (r + 4) * m, (c2 - c + 1) * m, m, BLACK);
            c = c2;
        }
}

/* ---- screens -------------------------------------------------------------- */

static const char *gen_text(int g) { return g == 3 ? "6" : g == 2 ? "3" : g == 1 ? "1.5" : "?"; }

static u32 attr_color(const smart_attr *a)
{
    if (a->thresh && a->thresh < 0xFE && a->value <= a->thresh) return RED;
    if (a->thresh && a->thresh < 0xFE && a->worst <= a->thresh) return YELLOW;
    if (smart_counter(a->id) && (a->raw & 0xFFFFFFFFull)) return YELLOW;
    long long dl;
    if (D.have_last && smart_delta(&D.s, &D.last, a->id, &dl) && dl) return BLUE;   /* changed since the previous read */
    return WHITE;
}

static void draw_probe_screen(const char *st_msg, u32 st_color)
{
    float y = 70;
    text(30, y, 16, D.info_rc ? YELLOW : WHITE, "609 device info  rc 0x%08x  %llu sectors", (unsigned)D.info_rc,
         (unsigned long long)D.info_sectors);
    y += 22;
    text(30, y, 16, D.opened ? WHITE : YELLOW, "600 open         rc 0x%08x", (unsigned)D.open_rc);
    y += 22;
    if (D.opened)
        text(30, y, 16, D.identify_frozen ? RED : YELLOW, "616 IDENTIFY     rc 0x%08x  %s", (unsigned)D.identify_rc,
             D.identify_note);
    y += 40;
    if (!D.opened)
        text(30, y, 18, YELLOW, "The system refused to open the drive (syscall 600).");
    else if (D.identify_frozen)
        text(30, y, 18, RED, "IDENTIFY froze this console before: the app does not retry it.");
    else
        text(30, y, 18, YELLOW, "This console refuses ATA commands through syscall 616: not supported.");
    text(30, y + 26, 16, GREY, "The report in " APP_DIR " has the details.");
    text(30, y + 70, 16, WHITE, "Please report this setup: the QR code opens a prefilled GitHub issue.");
    draw_qr(620, 60, 200);
    if (st_msg) text(30, 430, 16, st_color, "%s", st_msg);
    text(30, 455, 14, GREY, "SQUARE twice clear journal  %s%s  START exit", D.demo ? "" : "R2 twice speed test  ",
         D.demo ? "SELECT back to start" : "SELECT copy report to USB");
}

static void draw_header(int page)
{
    const ata_identity *i = &D.id;
    const smart_data *s = &D.s;
    static const char *names[PAGES] = {"attributes", "summary", "share"};
    char type[16];
    if (i->rotation == 1) snprintf(type, sizeof type, "SSD");
    else if (i->rotation > 0x400 && i->rotation < 0xFFFF) snprintf(type, sizeof type, "HDD %d rpm", i->rotation);
    else snprintf(type, sizeof type, "drive");
    text(600, 24, 14, GREY, "LEFT/RIGHT  page %d/%d %s", page + 1, PAGES, names[page]);
    text(30, 62, 18, WHITE, "%s   firmware %s   serial %s", i->model, i->firmware, i->serial);
    float x = text(30, 86, 16, GREY, "%s   %.1f GB   SATA %s Gb/s (max %s)   TRIM %s   SMART %s", type,
                   (double)i->sectors * i->logical_size / 1e9, gen_text(i->sata_cur_gen), gen_text(i->sata_max_gen),
                   i->trim ? "yes" : "no", i->smart_enabled ? "on" : "off");
    if (D.have_smart && s->temperature >= 0)
        text(x + 12, 86, 16, s->temperature >= D.sum.temp_limit ? YELLOW : GREY, "  %d C", s->temperature);
}

static void draw_health(void)
{
    char why[256];
    int h = smart_health(&D.s, D.have_stlog ? &D.log : NULL, D.sum.temp_limit, why, sizeof why);
    u32 hc = h == HEALTH_OK ? GREEN : h == HEALTH_WARN ? YELLOW : RED;
    float x = text(30, 112, 20, hc, "%s", h == HEALTH_OK ? "Health OK" : h == HEALTH_WARN ? "Health WARN" : "Health FAIL");
    text(x + 16, 116, 14, hc, "%.80s", why);
}

static void draw_attributes(int scroll)
{
    const smart_data *s = &D.s;
    rect(22, 136, 804, 24 + ROWS * 20 + 6, PANEL);
    text(30, 142, 14, GREY, "ID");
    text(70, 142, 14, GREY, "Attribute");
    text(330, 142, 14, GREY, "Value");
    text(395, 142, 14, GREY, "Worst");
    text(460, 142, 14, GREY, "Thresh");
    text(535, 142, 14, GREY, "Raw");
    text(680, 142, 14, GREY, D.have_prev ? "Since last" : "");
    for (int r = 0; r < ROWS && scroll + r < s->count; r++) {
        const smart_attr *a = &s->a[scroll + r];
        float y = 162 + r * 20;
        u32 c = attr_color(a);
        long long dl;
        text(30, y, 16, c, "%u", a->id);
        text(70, y, 16, c, "%.26s", vendor_attr_name(D.vendor, a->id));
        text(338, y, 16, c, "%u", a->value);
        text(403, y, 16, c, "%u", a->worst);
        text(468, y, 16, c, "%u", a->thresh);
        if (a->id == 194 || a->id == 190) text(535, y, 16, c, "%u C", (unsigned)(a->raw & 0xff));
        else text(535, y, 16, c, "%llu", (unsigned long long)a->raw);
        if (D.have_prev && smart_delta(s, &D.prev, a->id, &dl) && dl) text(680, y, 16, c, "%+lld", dl);
    }
    if (s->count > ROWS) {
        char pg[16];
        snprintf(pg, sizeof pg, "%d/%d", scroll + ROWS, s->count);
        text(818 - 8 * (float)strlen(pg), 162 + (ROWS - 1) * 20, 14, GREY, "%s", pg);
    }
}

static float heading(float y, const char *name)
{
    text(30, y, 14, GREY, "%s", name);
    return y + 20;
}

static void draw_summary(void)
{
    const smart_summary *m = &D.sum;
    const ata_identity *i = &D.id;
    char h[48], dtext[256];
    float y = 140;
    const float step = 21;
    const dev_stats *ds = &D.ds;

    y = heading(y, "DRIVE");
    text(120, y - 20, 14, GREY, "attribute names: %s table", vendor_get(D.vendor)->name);
    hours_text(m->hours, h, sizeof h);
    if (m->cycles >= 0) text(30, y, 16, WHITE, "Power-on %s, %d power cycles", h, m->cycles);
    else text(30, y, 16, WHITE, "Power-on %s", h);
    y += step;
    /* The endurance indicator (device statistics) is "percent used" by the
     * standard, but the Dahua reports 100 on a drive at 100% life: some
     * firmware stores the remaining percent. So it is shown as a number, never
     * as a verdict, and never in red. */
    int eu = ds->have_ssd ? ds->endurance_used : -1;
    if (m->life_left >= 0) {
        float x = text(30, y, 16, m->life_left <= 10 ? RED : WHITE, "Life left %d%% (attribute %d)", m->life_left, m->life_id);
        if (eu >= 0) text(x + 24, y, 16, GREY, "endurance indicator %d (device statistics)", eu);
    } else if (eu >= 0)
        text(30, y, 16, GREY, "Life: endurance indicator %d (device statistics; 0 = new, 100 = used up, some firmware inverts it)", eu);
    else text(30, y, 16, GREY, "Life left: no known attribute for this vendor");
    y += step;
    if (ds->have_general && ds->sectors_written >= 0)
        text(30, y, 16, WHITE, "Host writes %.2f TB (device statistics, %lld sectors)%s",
             (double)ds->sectors_written * i->logical_size / 1e12, ds->sectors_written,
             m->writes_id && m->tb_written >= 0 ? "" : "   (no vendor attribute)");
    else if (m->writes_id && m->tb_written >= 0)
        text(30, y, 16, WHITE, "Host writes %.2f TB (attribute %d, %s)", m->tb_written, m->writes_id, writes_unit_text(m->writes_unit));
    else if (m->writes_id)
        text(30, y, 16, GREY, "Host writes: attribute %d raw %llu (%s)", m->writes_id, (unsigned long long)m->writes_raw,
             writes_unit_text(m->writes_unit));
    else text(30, y, 16, GREY, "Host writes: not reported");
    y += step;
    if (i->rotation == 1) {
        /* the PS3 firmware predates TRIM and never sends it: the SSD's own
         * garbage collection does all the work, so spare area (over-provisioning) matters */
        text(30, y, 16, GREY, "TRIM: never sent by the PS3. Over-provisioning: set it on a PC before the install (README).");
        y += step;
    }
    y += 8;

    y = heading(y, "HEALTH");
    {
        char mx[32] = "";
        if (ds->have_temp && ds->temp_max != -999) snprintf(mx, sizeof mx, ", max ever %d", ds->temp_max);
        if (D.cpu_temp >= 0)
            text(30, y, 16, WHITE, "Temperature: drive %d C (warning at %d%s)   Cell %d C   RSX %d C", D.s.temperature,
                 m->temp_limit, mx, D.cpu_temp, D.rsx_temp);
        else
            text(30, y, 16, WHITE, "Temperature: drive %d C (warning at %d%s)   console: n/a (383 rc 0x%x)", D.s.temperature,
                 m->temp_limit, mx, (unsigned)D.temps_rc);
        y += step;
    }
    {
        float x;
        if (D.have_stlog && D.log.count)
            x = text(30, y, 16, WHITE, "Last self-test: %s, %s, at %u h", selftest_type_text(D.log.e[0].type),
                     selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
        else x = text(30, y, 16, GREY, D.have_stlog ? "Self-test log: empty" : "Self-test log: not readable");
        if (D.have_errlog) text(x + 24, y, 16, D.elog.error_count ? YELLOW : GREY, "ATA error log: %d", D.elog.error_count);
        y += step;
    }
    if (D.have_prev) {
        int j = 0, changed = 0;
        dtext[0] = 0;
        for (int k = 0; k < D.s.count; k++) {
            long long dl;
            if (!smart_delta(&D.s, &D.prev, D.s.a[k].id, &dl) || !dl) continue;
            changed++;
            if (j < (int)sizeof dtext) j += snprintf(dtext + j, sizeof dtext - j, "%s%u %+lld", j ? ", " : "", D.s.a[k].id, dl);
        }
        text(30, y, 16, changed ? BLUE : GREY, "Since %s: %.66s", D.prev_when, changed ? dtext : "no change");
    } else text(30, y, 16, GREY, "Since last report: no previous read on this console.");
    y += step + 8;

    y = heading(y, "SPEED");
    text(30, y, 16, WHITE, "SATA link %s Gb/s now, drive max %s Gb/s.%s", gen_text(i->sata_cur_gen), gen_text(i->sata_max_gen),
         i->sata_cur_gen == 1 ? " The PS3 port is 1.5 Gb/s: about 150 MB/s at most." : "");
    y += step;
    if (D.have_phy) {
        const phy_counters *p = &D.phy;
        long long crc = (p->icrc > 0 ? p->icrc : 0) + (p->crc_h2d > 0 ? p->crc_h2d : 0);
        text(30, y, 16, crc ? YELLOW : GREY, "Link counters: %lld CRC errors, %lld resets, %lld PhyRdy drops (SATA Phy log)", crc,
             p->comreset < 0 ? 0 : p->comreset, p->phy_nrdy < 0 ? 0 : p->phy_nrdy);
        y += step;
    }
    if (D.speed_done)
        text(30, y, 16, GREEN, "Speed test: write %.0f MB/s, read %.0f MB/s (%.0f MB file through the file system)",
             D.speed_mb / (D.speed_wsec > 0 ? D.speed_wsec : 1), D.speed_mb / (D.speed_rsec > 0 ? D.speed_rsec : 1), D.speed_mb);
    else if (D.speed_frozen) text(30, y, 16, RED, "Speed test: it froze this console before, skipped.");
    else if (D.speed_rc) text(30, y, 16, YELLOW, "Speed test: failed, rc 0x%08x", (unsigned)D.speed_rc);
    else if (D.demo) text(30, y, 16, GREY, "Speed test: not available in demo mode.");
    else text(30, y, 16, GREY, "Speed test: not run. R2 twice writes and reads a 64 MB file in the app folder.");
}

/* After the speed test: the two rates, until any button. */
static void speed_screen(void)
{
    while (1) {
        read_pad();
        if (quit || pressed) return;
        begin_frame();
        title();
        text(30, 100, 20, WHITE, "Speed test");
        if (D.speed_done) {
            text(30, 150, 30, GREEN, "Write %.0f MB/s", D.speed_mb / (D.speed_wsec > 0 ? D.speed_wsec : 1));
            text(30, 195, 30, GREEN, "Read  %.0f MB/s", D.speed_mb / (D.speed_rsec > 0 ? D.speed_rsec : 1));
            text(30, 260, 16, GREY, "%.0f MB file in " APP_DIR ": written in %.2f s, read in %.2f s, deleted.", D.speed_mb,
                 D.speed_wsec, D.speed_rsec);
            text(30, 282, 16, GREY, "Through the file system, the path games use. The PS3 port is 1.5 Gb/s: 150 MB/s at most.");
        } else if (D.speed_frozen) {
            text(30, 150, 20, RED, "The speed test froze this console before: skipped.");
        } else {
            text(30, 150, 20, YELLOW, "The speed test failed: rc 0x%08x", (unsigned)D.speed_rc);
            text(30, 180, 16, GREY, "The file goes to " APP_DIR ". A full drive gives ENOSPC (0x80010023).");
        }
        text(30, 455, 14, GREY, "Any button: back to the summary page");
        tiny3d_Flip();
    }
}

static void draw_share(void)
{
    draw_qr(30, 130, 300);
    float x = 340, y = 142;
    text(x, y, 16, WHITE, "Scan with a phone: it opens a new GitHub issue");
    text(x, y + 20, 16, WHITE, "with the text below. Nothing is sent until you");
    text(x, y + 40, 16, WHITE, "submit it. No serial number, no console id.");
    y += 72;
    /* the body, wrapped at a space to 58 columns: 8 px per glyph from x = 340 */
    const int cols = 58;
    const char *line = qr_body;
    while (*line && y < 420) {
        const char *nl = strchr(line, '\n');
        int len = nl ? (int)(nl - line) : (int)strlen(line);
        int take = len;
        if (take > cols) {
            take = cols;
            while (take > cols / 2 && line[take] != ' ') take--;
            if (line[take] != ' ') take = cols;
        }
        text(x, y, 14, GREY, "%.*s", take, line);
        y += 18;
        line += take;
        while (*line == ' ') line++;
        if (*line == '\n') line++;
    }
}

static int selftest_percent(void);
static int running, seen_running;        /* the self-test state, with its functions below */

static void draw_main(int page, int scroll, const char *st_msg, u32 st_color)
{
    draw_header(page);
    if (!D.have_smart) {
        text(30, 120, 18, YELLOW, D.smart_frozen ? "SMART READ DATA froze the console before: skipped."
                                                 : "SMART READ DATA refused: rc 0x%08x", (unsigned)D.smart_rc);
        return;
    }
    draw_health();
    if (page == 0) draw_attributes(scroll);
    else if (page == 1) draw_summary();
    else draw_share();

    float y = 430;
    if (armed == BTN_SQUARE)
        text(30, y, 16, YELLOW, "Press SQUARE again to clear the freeze journal. Any other button cancels.");
    else if (armed == BTN_R2)
        text(30, y, 16, YELLOW, "Press R2 again: the app writes a 64 MB file in its folder, reads it back, deletes it.");
    else if (running && selftest_percent() >= 0)
        text(30, y, 16, BLUE, "Self-test running: %d%% done. TRIANGLE shows the test screen.", selftest_percent());
    else if (running)
        text(30, y, 16, BLUE, "Self-test started, waiting for the first progress report. TRIANGLE shows the test screen.");
    else if (st_msg)
        text(30, y, 16, st_color, "%s", st_msg);
    else if (page == 0 && D.have_stlog && D.log.count)
        text(30, y, 16, WHITE, "Last self-test: %s, %s, at %u h", selftest_type_text(D.log.e[0].type),
             selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
    else if (page == 0)
        text(30, y, 16, GREY, D.have_stlog ? "Self-test log: empty" : "Self-test log: not readable (rc 0x%08x)",
             (unsigned)D.stlog_rc);
    /* 8 px per glyph at size 14: a footer must stay under 98 characters */
    const char *sel = D.demo ? "SELECT back to start" : "SELECT to USB";
    if (page == 0)
        text(30, 455, 14, GREY, "UP/DOWN scroll  CIRCLE re-read  %s%s%s%s",
             running ? "TRIANGLE test screen  " : drive_can_selftest(&D) ? "TRIANGLE self-test  " : "",
             D.demo ? "" : "R2 speed test  ", sel, running ? "" : "  START exit");
    else
        text(30, 455, 14, GREY, "CIRCLE re-read  SQUARE twice clear journal  %s%s%s", D.demo ? "" : "R2 speed test  ", sel,
             running ? "" : "  START exit");
}

static void close_drive(void) { drive_close(&D); }

/* Before the drive is touched: the file self-test, CROSS to go on, SELECT for demo mode. */
static int start_screen(int *demo)
{
    int mk, op, wr, cleared = 0;
    int fs_ok = fs_selftest(&mk, &op, &wr) == 0;
    *demo = 0;
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return 0;
        if ((pressed & BTN_CROSS) && fs_ok) return 1;
        if ((pressed & BTN_SELECT) && fs_ok) { *demo = 1; return 1; }
        if (fs_ok && gesture(BTN_SQUARE) == BTN_SQUARE) { journal_clear(); cleared = 1; }
        begin_frame();
        title();
        if (fs_ok) {
            text(30, 90, 18, WHITE, "CROSS: read the drive (IDENTIFY, then SMART; read-only).");
            text(30, 118, 16, GREY, "Each step is written to " APP_DIR "/journal.txt before it runs.");
            text(30, 140, 16, GREY, "If a step freezes the console, the next start skips it.");
            text(30, 184, 16, GREY, "SELECT: demo mode. It shows the sectors saved in " DEMO_DIR);
            text(30, 206, 16, GREY, "and never touches the drive. SELECT again goes back to this screen.");
        } else {
            text(30, 90, 18, RED, "The app cannot write " APP_DIR ".");
            text(30, 118, 16, GREY, "File test: mkdir rc 0x%08x, open rc 0x%08x, write rc 0x%08x", (unsigned)mk, (unsigned)op,
                 (unsigned)wr);
            text(30, 140, 16, GREY, "Without its journal it does not touch the drive.");
        }
        if (armed == BTN_SQUARE) text(30, 430, 16, YELLOW, "Press SQUARE again to clear the freeze journal. The next read runs every step.");
        else if (cleared) text(30, 430, 16, GREEN, "Freeze journal cleared: every drive step runs again.");
        text(30, 455, 14, GREY, "%sSTART exit", fs_ok ? "CROSS read the drive  SELECT demo mode  SQUARE twice clear journal  " : "");
        tiny3d_Flip();
    }
}

/* Every exit path: START, Quit Game from the PS button menu, and exit(). */
static void leave(void)
{
    drive_close(&D);
    ioPadEnd();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
}

/* The main screen until START (returns 0) or, in demo mode, SELECT (returns 1:
 * back to the start screen). */
/* ---- self-test ------------------------------------------------------------ */

static s64 next_poll, started, deadline;
static selftest_entry log_before;
static int log_before_count;
static char st_msg[160];
static u32 st_color = WHITE;

/* Poll SMART data every 5 s; byte 363 high nibble 0xF = still running.
 * The result comes only from a new entry in the self-test log: a drive
 * that accepts the command and never runs the test shows no green pass. */
static void selftest_poll(void)
{
    if (!running || sysGetSystemTime() < next_poll) return;
    s64 now = sysGetSystemTime();
    next_poll = now + 5000000;
    if (drive_read_smart(&D, 0) != 0) {
        running = 0;
        snprintf(st_msg, sizeof st_msg, "SMART read failed during the self-test: rc 0x%08x", (unsigned)D.smart_rc);
        st_color = RED;
    } else if ((D.s.selftest >> 4) == 0xF) {
        seen_running = 1;
        deadline = now + 60000000;   /* the log entry may come a little after the status byte */
    } else {
        drive_read_smart(&D, 1);
        int logged = D.have_stlog && D.log.count &&
                     (D.log.count != log_before_count || memcmp(&D.log.e[0], &log_before, sizeof log_before));
        if (logged) {
            running = 0;
            write_report();
            snprintf(st_msg, sizeof st_msg, "Self-test finished: %s (log entry at %u h)",
                     selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
            st_color = (D.log.e[0].status >> 4) == 0 ? GREEN : RED;
        } else if (now > deadline) {
            running = 0;
            write_report();
            snprintf(st_msg, sizeof st_msg, "Self-test: %s, and no new log entry (byte 363 = 0x%02x)",
                     seen_running ? "the drive stopped reporting progress" : "the drive reported no progress",
                     D.s.selftest);
            st_color = YELLOW;
        }
    }
}

/* Percent done from the status byte, -1 before the first progress report. */
static int selftest_percent(void)
{
    return seen_running ? 100 - (D.s.selftest & 0xF) * 10 : -1;
}

/* Short or extended: CROSS short, TRIANGLE extended, CIRCLE cancel. Returns 1, 2 or 0. */
static int selftest_choice(void)
{
    const smart_data *s = &D.s;
    while (1) {
        read_pad();
        if (quit || (pressed & (BTN_CIRCLE | BTN_START))) return 0;
        if (pressed & BTN_CROSS) return 1;
        if ((pressed & BTN_TRIANGLE) && !D.selftest_long_frozen) return 2;
        begin_frame();
        title();
        text(30, 100, 20, WHITE, "Which self-test?");
        text(30, 150, 18, GREEN, "CROSS: short self-test, about %u min.", s->short_minutes ? s->short_minutes : 2);
        text(30, 176, 16, GREY, "The drive checks its electronics and a small part of the surface.");
        if (D.selftest_long_frozen)
            text(30, 220, 18, RED, "Extended self-test: it froze this console before, so the app does not offer it.");
        else
            text(30, 220, 18, GREEN, "TRIANGLE: extended self-test, about %d min.", s->ext_minutes ? s->ext_minutes : 60);
        text(30, 246, 16, GREY, "The drive reads its whole surface. Nothing is written.");
        text(30, 290, 16, YELLOW, "The console is very slow while a test runs. Stay in the app until the test ends:");
        text(30, 312, 16, YELLOW, "START is blocked during a test. Neither test can be aborted from this app.");
        text(30, 455, 14, GREY, "CROSS short   TRIANGLE extended   CIRCLE cancel");
        tiny3d_Flip();
    }
}

static const char *selftest_title(void)
{
    return st_type == 2 ? "Extended self-test" : st_type == 1 ? "Short self-test" : "Self-test in progress (started earlier)";
}

/* The self-test as its own screen: progress bar, counter, then the result.
 * Any button goes back to the table; the drive continues the test. */
static void selftest_screen(void)
{
    while (1) {
        read_pad();
        if (quit) return;
        if (pressed & ~(running ? BTN_START : 0)) return;    /* START stays blocked during a test */
        selftest_poll();
        begin_frame();
        title();
        double sec = (double)(sysGetSystemTime() - started) / 1e6;
        if (running) {
            int pct = selftest_percent();
            text(30, 100, 20, WHITE, "%s", selftest_title());
            if (pct >= 0) text(30, 150, 30, GREEN, "%d%% done", pct);
            else text(30, 150, 30, GREEN, "Starting");
            rect(30, 200, 788, 18, PANEL);
            if (pct > 0) rect(30, 200, 788 * pct / 100.0f, 18, GREEN);
            text(30, 236, 20, WHITE, "%c  %.0f s", "|/-\\"[(int)(sec * 6) & 3], sec);
            text(30, 280, 16, GREY, "The drive runs the test by itself and reports tenths. About %d min.",
                 st_type == 2 ? (D.s.ext_minutes ? D.s.ext_minutes : 60) : (D.s.short_minutes ? D.s.short_minutes : 2));
            text(30, 302, 16, GREY, "The counter runs while the console works. If it stops, the console froze.");
            text(30, 324, 16, YELLOW, "START is blocked: the console is very slow while a test runs. Wait for the result.");
            text(30, 455, 14, GREY, "Any other button: back to the table, the drive continues the test");
        } else {
            text(30, 100, 20, WHITE, "%s", selftest_title());
            text(30, 150, 30, st_color, "%s", st_color == GREEN ? "Passed" : st_color == RED ? "Failed" : "No result");
            text(30, 200, 16, st_color, "%s", st_msg);
            text(30, 455, 14, GREY, "Any button: back to the table");
        }
        text(30, 478, 12, GREY, "Report: %s", report_path);
        tiny3d_Flip();
    }
}

static int run(void)
{
    int scroll = 0, page = 0;
    running = seen_running = 0;
    log_before_count = 0;
    st_msg[0] = 0;
    st_color = WHITE;
    memset(&log_before, 0, sizeof log_before);
    if (D.have_smart && !D.demo && (D.s.selftest >> 4) == 0xF) {    /* a test from an earlier session still runs */
        running = seen_running = 1;
        st_type = 0;
        log_before_count = D.have_stlog ? D.log.count : 0;
        if (log_before_count) log_before = D.log.e[0];
        started = sysGetSystemTime();
        next_poll = started + 3000000;
        deadline = started + 60000000;
    }
    while (1) {
        read_pad();
        if (quit) return 0;
        if ((pressed & BTN_START) && !running) return 0;
        if (D.demo && (pressed & BTN_SELECT)) return 1;
        unsigned gb = BTN_SQUARE;              /* only gestures that can fire now arm */
        if (!running && !D.demo) gb |= BTN_R2;
        unsigned fired = gesture(gb);
        if (pressed) st_msg[0] = 0;
        if ((pressed & BTN_START) && running) {        /* a test in the background makes the console very slow */
            snprintf(st_msg, sizeof st_msg, "START is blocked while the self-test runs: the console is very slow until it ends.");
            st_color = YELLOW;
        }
        if (running && (pressed & BTN_TRIANGLE)) selftest_screen();   /* back to the running test */
        if (fired == BTN_SQUARE) {
            journal_clear();
            snprintf(st_msg, sizeof st_msg, "Freeze journal cleared. Start the app again to run the skipped steps.");
            st_color = GREEN;
        }
        int max_scroll = D.s.count > ROWS ? D.s.count - ROWS : 0;
        if (pressed & BTN_RIGHT) page = (page + 1) % PAGES;
        if (pressed & BTN_LEFT) page = (page + PAGES - 1) % PAGES;
        if (page == 0) {
            if (pressed & BTN_DOWN) scroll++;
            if (pressed & BTN_UP) scroll--;
            if (pressed & BTN_R1) scroll += ROWS;
            if (pressed & BTN_L1) scroll -= ROWS;
            scroll = scroll < 0 ? 0 : scroll > max_scroll ? max_scroll : scroll;
        }

        if ((pressed & BTN_CIRCLE) && !running && D.ata_ok && !D.demo) {
            run_job("Reading SMART data", job_smart);
            write_report();
        }
        if ((pressed & BTN_SELECT) && !D.demo) {
            char dst[128];
            int rc = report_copy_usb(report_path, dst, sizeof dst);
            if (rc == 0) snprintf(st_msg, sizeof st_msg, "Report copied to %s", dst);
            else if (rc == -1) snprintf(st_msg, sizeof st_msg, "No USB stick found (/dev_usb000 to 007).");
            else snprintf(st_msg, sizeof st_msg, "Could not write %s (%d)", dst, rc);
            st_color = rc == 0 ? GREEN : YELLOW;
        }
        if (fired == BTN_R2 && !running && !D.demo) {
            run_job("Speed test: writing and reading a 64 MB file", job_speed);
            write_report();
            speed_screen();             /* the result; the summary page keeps its own line */
            page = 1;
        }
        if ((pressed & BTN_TRIANGLE) && !running && drive_can_selftest(&D) && (st_type = selftest_choice()) != 0) {
            log_before_count = D.have_stlog ? D.log.count : 0;
            if (log_before_count) log_before = D.log.e[0];
            run_job(st_type == 2 ? "Starting the extended self-test" : "Starting the short self-test", job_selftest);
            if (job_rc == 0) {
                running = 1;
                seen_running = 0;
                started = sysGetSystemTime();
                next_poll = started + 3000000;
                deadline = started + 60000000;
                selftest_screen();
            } else {
                snprintf(st_msg, sizeof st_msg, "Self-test start refused: rc 0x%08x", (unsigned)D.selftest_rc);
                st_color = RED;
            }
        }
        selftest_poll();

        begin_frame();
        title();
        if (D.ata_ok && D.have_identify) draw_main(page, scroll, st_msg[0] ? st_msg : NULL, st_color);
        else draw_probe_screen(st_msg[0] ? st_msg : NULL, st_color);
        text(30, 478, 12, GREY, "Report: %s", report_path);
        tiny3d_Flip();
    }
}

int main(void)
{
    int demo;
    ioPadInit(7);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
    init_graph();
    atexit(close_drive);
    while (1) {
        memset(&D, 0, sizeof D);
        armed = 0;
        if (!start_screen(&demo)) break;
        armed = 0;
        if (demo) {
            if (drive_demo_load(&D)) {
                memset(&D, 0, sizeof D);
                D.demo = 1;
                snprintf(D.identify_note, sizeof D.identify_note, "no demo sectors in " DEMO_DIR);
            }
        } else {
            drive_load_prev(&D);
            run_job("Reading the drive", job_probe);
            if (optional_steps() < 0) break;
        }
        run_job("Reading the console firmware and temperatures", job_console);
        write_report();
        int back = run();
        drive_close(&D);
        if (!back) break;
    }
    leave();
    return 0;
}
