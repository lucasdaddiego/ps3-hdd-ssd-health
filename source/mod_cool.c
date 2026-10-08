/* The Cooling module: the Cell and RSX temperatures (syscall 383), the fan
 * duty (syscall 409, read-only, behind the first-run prompt), and a timed load
 * on the PPU and the RSX with a temperature curve. Nothing is written to the
 * fan: the system keeps its own policy. */
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include "app.h"
#include "ui.h"
#include "report.h"

#define MAX_SAMPLES 400                      /* every 2 s: 13 min */

static int cpu_now = -1, rsx_now = -1, temps_rc, read_once;   /* read_once: the module read once, so the report has its section and the rc */
static int fan_pct = -1, fan_mode = -1, fan_rc;
static struct {
    int done, n, seconds, cancelled;
    int cpu0, rsx0, fan0, cpu_max, rsx_max, fan_end;
    short cpu[MAX_SAMPLES], rsx[MAX_SAMPLES], fan[MAX_SAMPLES];   /* -1 = not read */
} load;

static void read_now(void)
{
    temps_rc = console_temps(&cpu_now, &rsx_now);
    read_once = 1;
    fan_rc = fan_policy_read(&fan_pct, &fan_mode);     /* -1 and no value when skipped or frozen */
}

/* ---- the load ------------------------------------------------------------- */

static volatile int load_on;

static void load_thread(void *arg)
{
    (void)arg;
    volatile double x = 1.0;
    while (load_on) {
        for (int i = 0; i < 200000; i++) x = x * 1.0000001 + 0.5;
    }
    sysThreadExit(0);
}

/* Fill-rate work before anything else is drawn: the opaque background,
 * again and again, so the picture does not change. (A layer under the alpha
 * test's 0x10 would be discarded and cost the RSX nothing.) */
static void rsx_load(void)
{
    for (int k = 0; k < 80; k++) background();
}

static u32 temp_color(int t) { return t < 0 ? GREY : t < 72 ? GREEN : t < 80 ? YELLOW : RED; }

/* The y of a temperature on a graph from 30 to 90 C; outside that range it stays on the edge. */
static float graph_y(int t, float gy, float gh)
{
    t = t < 30 ? 30 : t > 90 ? 90 : t;
    return gy + gh - (t - 30) * gh / 60;
}

/* The curve: 30 to 90 C, the Cell in white, the RSX in blue, the verdict
 * limits as thin lines; span is the time the x axis covers. */
static void draw_graph(float x, float y, float w, float h, int n, int span)
{
    card(x, y, w, h);
    float gx = x + 76, gw = w - 100, gy = y + 20, gh = h - 60;
    for (int t = 40; t <= 90; t += gh < 240 ? 20 : 10) {     /* a short graph: every 20 C */
        float ly = graph_y(t, gy, gh);
        rect(gx, ly, gw, 1, LINE);
        text_r(gx - 12, ly - line_h(F_SMALL) / 2, F_SMALL, DIM, "%d", t);
    }
    rect(gx, graph_y(72, gy, gh), gw, 2, 0xffcc4060);           /* warm */
    rect(gx, graph_y(80, gy, gh), gw, 2, 0xff646470);           /* hot */
    if (span < 2) span = 2;
    for (int t = 0; t <= span; t += span > 180 ? 60 : 30)
        text_c(gx + gw * t / span, gy + gh + 8, F_SMALL, DIM, "%d:%02d", t / 60, t % 60);
    float sx = gw / (span / 2.0f);
    for (int t = 0; t < 2; t++) {           /* one batch per curve, the Cell over the RSX */
        const short *v = t ? load.cpu : load.rsx;
        batch_begin(t ? WHITE : BLUE);
        for (int k = 1; k < n; k++)
            if (v[k] >= 0 && v[k - 1] >= 0)  /* a failed read leaves a gap */
                batch_line(gx + (k - 1) * sx, graph_y(v[k - 1], gy, gh), gx + k * sx, graph_y(v[k], gy, gh), 4);
        batch_end();
    }
    text_r(x + w - 24, y + 16, F_SMALL, WHITE, "Cell");
    text_r(x + w - 90, y + 16, F_SMALL, BLUE, "RSX");
}

static void load_screen(int seconds)
{
    sys_ppu_thread_t th[2];
    int ok[2] = {0, 0};
    u64 ret;
    memset(&load, 0, sizeof load);
    load.seconds = seconds;
    read_now();
    load.cpu0 = cpu_now;
    load.rsx0 = rsx_now;
    load.fan0 = fan_pct;
    load_on = 1;
    int started = 0;
    for (int k = 0; k < 2; k++)
        if (sysThreadCreate(&th[k], load_thread, NULL, 2500, 0x4000, THREAD_JOINABLE, "load") == 0) ok[k] = 1, started++;
    s64 t0 = sysGetSystemTime(), next = t0;
    while (1) {
        read_pad();
        s64 now = sysGetSystemTime();
        if (quit || (pressed & BTN_CIRCLE)) { load.cancelled = 1; break; }
        if (now - t0 >= (s64)seconds * 1000000) break;
        if (now >= next && load.n < MAX_SAMPLES) {
            read_now();
            load.cpu[load.n] = (short)cpu_now;
            load.rsx[load.n] = (short)rsx_now;
            load.fan[load.n] = (short)fan_pct;
            load.n++;
            next += 2000000;
        }
        begin_frame();
        rsx_load();
        title();
        double sec = (double)(now - t0) / 1e6;
        float x = text(MG, TOP, F_MED | TNUM, WHITE, "Load test  %d:%02d of %d:%02d", (int)sec / 60, (int)sec % 60, seconds / 60, seconds % 60);
        spinner(x + 40, TOP + 20, 16, sec);
        float rx = SW - MG;
        if (fan_pct >= 0) rx = text_r(rx, TOP, F_MED | TNUM, WHITE, "fan %d%%", fan_pct) - 40;
        rx = text_r(rx, TOP, F_MED | TNUM, temp_color(rsx_now), "RSX %d C", rsx_now) - 40;
        text_r(rx, TOP, F_MED | TNUM, temp_color(cpu_now), "Cell %d C", cpu_now);
        bar(MG, TOP + 56, SW - 2 * MG, 10, (float)(sec / seconds), GREEN);
        draw_graph(MG, TOP + 90, SW - 2 * MG, BOTTOM - TOP - 140, load.n, seconds);
        text_fit(MG, BOTTOM - 36, F_BODY, GREY, SW - 2 * MG, "%d PPU threads and the RSX fill rate at full load. Started at Cell %d C, RSX %d C.",
                 started, load.cpu0, load.rsx0);
        status_line("The counter runs while the console works. CIRCLE stops the test and keeps the curve.", GREY);
        footer("CIRCLE stop");
        ui_flip();
    }
    load_on = 0;
    for (int k = 0; k < 2; k++) if (ok[k]) sysThreadJoin(th[k], &ret);
    load.cpu_max = load.rsx_max = -1;
    for (int k = 0; k < load.n; k++) {
        if (load.cpu[k] > load.cpu_max) load.cpu_max = load.cpu[k];
        if (load.rsx[k] > load.rsx_max) load.rsx_max = load.rsx[k];
    }
    load.fan_end = fan_pct;
    load.seconds = (int)((sysGetSystemTime() - t0) / 1000000);
    load.done = 1;
    pressed = 0;                             /* a button still down is not a new press */
    write_report();
}

/* No readings or a short test give no verdict, a yellow one; 80 C is too hot even in a short test. */
static int verdict_dot(void)
{
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    return m < 0 ? DOT_WARN : m >= 80 ? DOT_BAD : load.n < 10 || m >= 72 ? DOT_WARN : DOT_OK;
}

static const char *verdict_text(void)
{
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    if (m < 0) return "No temperature readings during the test: no verdict.";
    if (m >= 80) return "Hotter than the usual range under load. Clean the dust, check the fan, consider new thermal paste.";
    if (load.n < 10) return "Too short for a verdict: run at least two minutes.";
    if (m >= 72) return "Warm under load. The console copes, but dust or dry paste would push it further: clean it.";
    return "Within the usual range under load.";
}

static void cool_tile(void)
{
    char l1[40], l2[40];
    if (load.done) {
        snprintf(l1, sizeof l1, "Cell %d>%d C, RSX %d>%d C", load.cpu0, load.cpu_max, load.rsx0, load.rsx_max);
        if (load.fan0 >= 0) snprintf(l2, sizeof l2, "fan %d>%d%%, %d:%02d load", load.fan0, load.fan_end, load.seconds / 60, load.seconds % 60);
        else snprintf(l2, sizeof l2, "%d:%02d load test", load.seconds / 60, load.seconds % 60);
        state_set("cooling", verdict_dot(), l1, l2);
    } else if (cpu_now >= 0) {
        snprintf(l1, sizeof l1, "Cell %d C, RSX %d C idle", cpu_now, rsx_now);
        if (fan_pct >= 0) snprintf(l2, sizeof l2, "fan %d%%, no load test yet", fan_pct);
        else snprintf(l2, sizeof l2, "no load test yet");
        state_set("cooling", cpu_now >= 70 || rsx_now >= 70 ? DOT_WARN : DOT_OK, l1, l2);
    }
}

static void job_read(void) { read_now(); }

int mod_cool_open(void)
{
    if (!fs_ok) return 0;                    /* no journal, no system call: the home screen says why */
    if (journal_state("fan_policy") == J_NONE) {   /* the first time, and again after SQUARE twice */
        int r = first_run_prompt("sys_sm_get_fan_policy (syscall 409)", "Reads the fan policy: the duty the system set, 0 to 100 %.",
                                 "The read half of what webMAN uses for its fan control. Nothing is written.",
                                 "Read-only. Without it the module shows temperatures only.");
        if (r < 0) return -1;
        if (r == 0) journal_skip("fan_policy");
    }
    run_job("Reading the temperatures and the fan policy", job_read);   /* the journaled calls, with the counter */
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { cool_tile(); return back; }
        if ((pressed & BTN_CROSS) && cpu_now >= 0) load_screen(120);
        if ((pressed & BTN_TRIANGLE) && cpu_now >= 0) load_screen(300);
        if (pressed & BTN_SQUARE) read_now();
        begin_frame();
        title();
        float w = SW - 2 * MG, y = TOP, x = MG + 32;
        card(MG, y, w, 176);
        heading(x, y + 22, "NOW");
        if (cpu_now >= 0) {
            text(x, y + 62, F_SMALL, GREY, "Cell");
            text(x, y + 88, F_LARGE, temp_color(cpu_now), "%d C", cpu_now);
            text(x + 250, y + 62, F_SMALL, GREY, "RSX");
            text(x + 250, y + 88, F_LARGE, temp_color(rsx_now), "%d C", rsx_now);
        } else text_wrap(x, y + 70, F_MED, YELLOW, 460, 2, "Temperatures not readable: no load test.");
        text(x + 500, y + 62, F_SMALL, GREY, "Fan");
        if (fan_pct >= 0) text(x + 500, y + 88, F_LARGE, WHITE, "%d%%", fan_pct);
        else text(x + 500, y + 100, F_BODY, journal_state("fan_policy") == J_FROZEN ? RED : GREY, "%s",
                  journal_state("fan_policy") == J_SKIPPED ? "read skipped" : journal_state("fan_policy") == J_FROZEN ? "read froze before"
                                                                                                          : "not read");
        if (w > 1180)
            text_wrap(x + 760, y + 66, F_BODY, GREY, w - 820, 3,
                      cpu_now >= 0 ? "Idle values under 60 C are usual for a PS3 with clean cooling. SQUARE reads again."
                                   : "Syscall 383 did not answer; the report has its return code.");
        y += 200;
        float ow = (w - 32) / 2;
        option_card(MG, y, ow, 220, BTN_CROSS, cpu_now >= 0, "Load test, 2 minutes",
                    "Two PPU threads and the RSX fill rate at full load, the temperatures every 2 s on a curve.");
        option_card(MG + ow + 32, y, ow, 220, BTN_TRIANGLE, cpu_now >= 0, "Load test, 5 minutes",
                    "Long enough for a steady temperature. The console shuts itself down near 85 C.");
        y += 244;
        float h = BOTTOM - y;
        card(MG, y, w, h);
        heading(x, y + 22, "LAST RESULT");
        if (load.done) {
            float tw = w * 0.45f;
            text_fit(x, y + 62, F_BODY, WHITE, tw, "%d:%02d%s: Cell %d to %d C, RSX %d to %d C", load.seconds / 60, load.seconds % 60,
                     load.cancelled ? " (stopped)" : "", load.cpu0, load.cpu_max, load.rsx0, load.rsx_max);
            if (load.fan0 >= 0)
                text_fit(x, y + 104, F_BODY, WHITE, tw, "Fan %d%% to %d%%%s", load.fan0, load.fan_end, load.fan_end > load.fan0 ? ", it responded" : "");
            else text(x, y + 104, F_BODY, GREY, "Fan not read");
            text_wrap(x, y + 146, F_BODY, verdict_dot() == DOT_OK ? GREEN : verdict_dot() == DOT_WARN ? YELLOW : RED, tw,
                      (int)((h - 160) / 38), verdict_text());
            if (h >= 160) draw_graph(MG + w * 0.5f, y + 16, w * 0.5f - 16, h - 32, load.n, load.n * 2);
        } else text(x, y + 62, F_BODY, GREY, "No load test in this session.");
        footer("CROSS 2 min load  TRIANGLE 5 min load  SQUARE read again  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

void mod_cool_report(void)
{
    if (!read_once) return;
    rep_out("--- cooling ---\n");
    if (cpu_now >= 0) rep_out("now: Cell %d C, RSX %d C (383 rc 0x%08x)", cpu_now, rsx_now, (unsigned)temps_rc);
    else rep_out("now: temperatures not read (383 rc 0x%08x)", (unsigned)temps_rc);
    if (fan_pct >= 0) rep_out(", fan %d%% (policy mode %d)", fan_pct, fan_mode);
    else rep_out(", fan not read (409 rc 0x%08x)", (unsigned)fan_rc);
    rep_out("\n");
    if (!load.done) return;
    rep_out("load test %d s%s: Cell %d -> max %d C, RSX %d -> max %d C, fan %d -> %d%%\n", load.seconds,
            load.cancelled ? " (stopped)" : "", load.cpu0, load.cpu_max, load.rsx0, load.rsx_max, load.fan0, load.fan_end);
    rep_out("verdict: %s\n", verdict_text());
    rep_out("samples (every 2 s): t Cell RSX fan\n");
    for (int k = 0; k < load.n; k += 5)
        rep_out("  %4d %3d %3d %3d\n", k * 2, load.cpu[k], load.rsx[k], load.fan[k]);   /* -1 = not read */
}
