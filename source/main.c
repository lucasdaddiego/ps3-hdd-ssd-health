/* HDD/SSD Health: IDENTIFY + SMART of the PS3's internal drive on the TV, from a HEN app.
 * Drawing follows ErikPshat/GamePad-Test (tiny3d + libfont3d), with a 16x32
 * bitmap of JetBrains Mono built in (font.c, from art/make_font.py): no file
 * read before the first frame, and a monospace table. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-lv2.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>
#include <tiny3d.h>
#include <libfont.h>
#include "ata.h"
#include "font.h"

#define BTN_LEFT     0x8000
#define BTN_DOWN     0x4000
#define BTN_RIGHT    0x2000
#define BTN_UP       0x1000
#define BTN_START    0x0800
#define BTN_CROSS    0x0040
#define BTN_CIRCLE   0x0020
#define BTN_TRIANGLE 0x0010
#define BTN_R1       0x0008
#define BTN_L1       0x0004

#define WHITE  0xffffffff
#define GREY   0x9a9a9aff
#define GREEN  0x5ce05cff
#define YELLOW 0xffd23cff
#define RED    0xff5a5aff
#define BLUE   0x78c8ffff

#define ROWS 13

static drive_state D;
static char report_path[128];

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

static void title(void)
{
    text(30, 18, 30, WHITE, "HDD/SSD Health");
}

/* Shown during each call that can block; if it stays, the console froze. */
static void progress(const char *msg)
{
    for (int i = 0; i < 2; i++) {
        begin_frame();
        title();
        text(30, 120, 20, WHITE, "%s ...", msg);
        text(30, 200, 16, GREY, "If this screen stays for more than 2 minutes, the console froze.");
        text(30, 222, 16, GREY, "Hold the power button, then start the app again: it skips this step.");
        tiny3d_Flip();
    }
}

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

/* ---- screens -------------------------------------------------------------- */

static const char *gen_text(int g) { return g == 3 ? "6" : g == 2 ? "3" : g == 1 ? "1.5" : "?"; }

static u32 attr_color(const smart_attr *a)
{
    if (a->thresh && a->thresh < 0xFE && a->value <= a->thresh) return RED;
    if (a->thresh && a->thresh < 0xFE && a->worst <= a->thresh) return YELLOW;
    if (smart_counter(a->id) && (a->raw & 0xFFFFFFFFull))
        return YELLOW;
    return WHITE;
}

static void draw_probe_screen(void)
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
}

static void draw_main(int scroll, int confirm, int running, const char *st_msg, u32 st_color)
{
    const ata_identity *i = &D.id;
    const smart_data *s = &D.s;
    char type[16];
    if (i->rotation == 1) snprintf(type, sizeof type, "SSD");
    else if (i->rotation > 0x400 && i->rotation < 0xFFFF) snprintf(type, sizeof type, "HDD %d rpm", i->rotation);
    else snprintf(type, sizeof type, "drive");
    text(30, 62, 18, WHITE, "%s   firmware %s   serial %s", i->model, i->firmware, i->serial);
    float x = text(30, 86, 16, GREY, "%s   %.1f GB   SATA %s Gb/s (max %s)   TRIM %s   SMART %s", type,
                   (double)i->sectors * i->logical_size / 1e9, gen_text(i->sata_cur_gen), gen_text(i->sata_max_gen),
                   i->trim ? "yes" : "no", i->smart_enabled ? "on" : "off");
    if (D.have_smart && s->temperature >= 0) text(x + 12, 86, 16, GREY, "  %d C", s->temperature);
    if (!D.have_smart) {
        text(30, 120, 18, YELLOW, D.smart_frozen ? "SMART READ DATA froze the console before: skipped."
                                                 : "SMART READ DATA refused: rc 0x%08x", (unsigned)D.smart_rc);
        return;
    }
    char why[256];
    int h = smart_health(s, D.have_stlog ? &D.log : NULL, why, sizeof why);
    u32 hc = h == HEALTH_OK ? GREEN : h == HEALTH_WARN ? YELLOW : RED;
    x = text(30, 112, 20, hc, "%s", h == HEALTH_OK ? "Health OK" : h == HEALTH_WARN ? "Health WARN" : "Health FAIL");
    text(x + 16, 116, 14, hc, "%.90s", why);

    text(30, 142, 14, GREY, "ID");
    text(70, 142, 14, GREY, "Attribute");
    text(330, 142, 14, GREY, "Value");
    text(395, 142, 14, GREY, "Worst");
    text(460, 142, 14, GREY, "Thresh");
    text(535, 142, 14, GREY, "Raw");
    for (int r = 0; r < ROWS && scroll + r < s->count; r++) {
        const smart_attr *a = &s->a[scroll + r];
        float y = 162 + r * 20;
        u32 c = attr_color(a);
        text(30, y, 16, c, "%u", a->id);
        text(70, y, 16, c, "%s", smart_attr_name(a->id));
        text(338, y, 16, c, "%u", a->value);
        text(403, y, 16, c, "%u", a->worst);
        text(468, y, 16, c, "%u", a->thresh);
        if (a->id == 194 || a->id == 190) text(535, y, 16, c, "%u C", (unsigned)(a->raw & 0xff));
        else text(535, y, 16, c, "%llu", (unsigned long long)a->raw);
    }
    if (s->count > ROWS) text(780, 162 + (ROWS - 1) * 20, 14, GREY, "%d/%d", scroll + ROWS, s->count);

    float y = 430;
    if (confirm)
        text(30, y, 16, YELLOW, "Press TRIANGLE again to start the short self-test (about %u min). CROSS cancels.",
             s->short_minutes ? s->short_minutes : 2);
    else if (running)
        text(30, y, 16, BLUE, "Self-test running: %d%% left. You can leave the app; the drive continues.",
             (s->selftest & 0xF) * 10);
    else if (st_msg)
        text(30, y, 16, st_color, "%s", st_msg);
    else if (D.have_stlog && D.log.count)
        text(30, y, 16, WHITE, "Last self-test: %s, %s, at %u h", selftest_type_text(D.log.e[0].type),
             selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
    else
        text(30, y, 16, GREY, D.have_stlog ? "Self-test log: empty" : "Self-test log: not readable (rc 0x%08x)",
             (unsigned)D.stlog_rc);
    text(30, 455, 14, GREY, "UP/DOWN scroll   CIRCLE re-read   %sSTART exit",
         drive_can_selftest(&D) ? "TRIANGLE short self-test   " : "");
}

static void close_drive(void) { drive_close(&D); }

/* Before the drive is touched: the file self-test, and CROSS to go on. */
static int start_screen(void)
{
    int mk, op, wr;
    int fs_ok = fs_selftest(&mk, &op, &wr) == 0;
    while (1) {
        read_pad();
        if (pressed & BTN_START) return 0;
        if ((pressed & BTN_CROSS) && fs_ok) return 1;
        begin_frame();
        title();
        text(30, 70, 18, GREEN, "Graphics ok.");
        text(30, 96, 18, fs_ok ? GREEN : RED, "File test: mkdir rc 0x%08x, open rc 0x%08x, write rc 0x%08x",
             (unsigned)mk, (unsigned)op, (unsigned)wr);
        if (fs_ok) {
            text(30, 150, 18, WHITE, "CROSS: read the drive (IDENTIFY, then SMART; read-only).");
            text(30, 176, 16, GREY, "Each step is written to " APP_DIR "/journal.txt before it runs.");
            text(30, 198, 16, GREY, "If a step freezes the console, the next start skips it.");
        } else {
            text(30, 150, 18, RED, "The app cannot write " APP_DIR ".");
            text(30, 176, 16, GREY, "Without its journal it does not touch the drive.");
        }
        text(30, 455, 14, GREY, "%sSTART exit", fs_ok ? "CROSS read the drive   " : "");
        tiny3d_Flip();
    }
}

int main(void)
{
    ioPadInit(7);
    init_graph();
    atexit(close_drive);
    if (!start_screen()) { ioPadEnd(); return 0; }
    drive_probe(&D, progress);
    report_write(&D, report_path, sizeof report_path);

    int scroll = 0, confirm = 0, running = 0, seen_running = 0;
    s64 next_poll = 0, started = 0;
    char st_msg[128] = "";
    u32 st_color = WHITE;
    while (1) {
        read_pad();
        if (pressed & BTN_START) break;
        int max_scroll = D.s.count > ROWS ? D.s.count - ROWS : 0;
        if (pressed & BTN_DOWN) scroll++;
        if (pressed & BTN_UP) scroll--;
        if (pressed & BTN_R1) scroll += ROWS;
        if (pressed & BTN_L1) scroll -= ROWS;
        scroll = scroll < 0 ? 0 : scroll > max_scroll ? max_scroll : scroll;

        if ((pressed & BTN_CIRCLE) && !running && D.ata_ok) {
            progress("Reading SMART data");
            drive_read_smart(&D, 1);
            report_write(&D, report_path, sizeof report_path);
            st_msg[0] = 0;
        }
        if (pressed & BTN_CROSS) confirm = 0;
        if ((pressed & BTN_TRIANGLE) && !running && drive_can_selftest(&D)) {
            if (!confirm) confirm = 1;
            else {
                confirm = 0;
                progress("Starting the short self-test");
                if (drive_start_short_selftest(&D) == 0) {
                    running = 1;
                    seen_running = 0;
                    started = sysGetSystemTime();
                    next_poll = started + 3000000;
                } else {
                    snprintf(st_msg, sizeof st_msg, "Self-test start refused: rc 0x%08x", (unsigned)D.selftest_rc);
                    st_color = RED;
                }
            }
        }
        /* Poll SMART data every 5 s; byte 363 high nibble 0xF = still running. */
        if (running && sysGetSystemTime() >= next_poll) {
            next_poll = sysGetSystemTime() + 5000000;
            if (drive_read_smart(&D, 0) != 0) {
                running = 0;
                snprintf(st_msg, sizeof st_msg, "SMART read failed during the self-test: rc 0x%08x", (unsigned)D.smart_rc);
                st_color = RED;
            } else if ((D.s.selftest >> 4) == 0xF) {
                seen_running = 1;
            } else if (seen_running || sysGetSystemTime() - started > 15000000) {
                running = 0;
                drive_read_smart(&D, 1);
                report_write(&D, report_path, sizeof report_path);
                snprintf(st_msg, sizeof st_msg, "Self-test finished: %s", selftest_status_text(D.s.selftest));
                st_color = (D.s.selftest >> 4) == 0 ? GREEN : RED;
            }
        }

        begin_frame();
        title();
        if (D.ata_ok && D.have_identify) draw_main(scroll, confirm, running, st_msg[0] ? st_msg : NULL, st_color);
        else draw_probe_screen();
        text(30, 478, 12, GREY, "Report: %s", report_path);
        tiny3d_Flip();
    }
    drive_close(&D);
    ioPadEnd();
    return 0;
}
