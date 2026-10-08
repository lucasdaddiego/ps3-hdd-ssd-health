/* The Controller module: sticks (rest offset, circle coverage), every button
 * with its pressure, the sixaxis sensors and the rumble motors. Pure pad
 * input: no system call. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <io/pad.h>
#include <lv2/systime.h>
#include "app.h"
#include "ui.h"
#include "report.h"

static int tested;                       /* a rest test ran */
static float rest[4];                    /* mean offset from centre: LX LY RX RY, in -127..127 */
static int jitter[4];                    /* max - min while at rest */
static int circle_done;
static float cover[2];                   /* circle coverage 0..1, left and right */
static unsigned char maxr[2][36];        /* the largest radius seen per 10-degree sector */
static padCapabilityInfo cap;
static int have_cap;

/* Sony's capability flags in info[0]; the header's bitfields start at the top
 * bit on this big-endian CPU, so the raw word is read instead. */
enum { CAP_PS3 = 0x1, CAP_PRESSURE = 0x2, CAP_SENSORS = 0x4, CAP_HPS = 0x8, CAP_RUMBLE = 0x10 };
static int cap_bit(unsigned bit) { return (cap.info[0] & bit) != 0; }

#define LX ((int)pad_last.ANA_L_H - 128)
#define LY ((int)pad_last.ANA_L_V - 128)
#define RX ((int)pad_last.ANA_R_H - 128)
#define RY ((int)pad_last.ANA_R_V - 128)
#define PI_F 3.14159265f

static float drift_pct(int k) { return fabsf(rest[k]) * 100.0f / 127.0f; }
static float worst_drift(int stick) { return fmaxf(drift_pct(stick * 2), drift_pct(stick * 2 + 1)); }
static u32 drift_color(float pct) { return pct < 4 ? GREEN : pct < 8 ? YELLOW : RED; }

/* A stick: its range circle, the reached shape, the cross hair and the dot. */
static void stick(float cx, float cy, float r, int s, int show_shape)
{
    int v[4] = {LX, LY, RX, RY};
    disc(cx, cy, r, 0x0e1830ff);
    if (show_shape) {
        float pts[72];
        for (int k = 0; k < 36; k++) {
            float a = (k + 0.5f) * PI_F / 18.0f - PI_F;
            pts[k * 2] = cx + cosf(a) * maxr[s][k] * r / 127.0f;
            pts[k * 2 + 1] = cy + sinf(a) * maxr[s][k] * r / 127.0f;
        }
        poly_fan(cx, cy, pts, 36, 0x4fd98250);
    }
    ring(cx, cy, r, 3, LINE);
    rect(cx - r, cy - 1, 2 * r, 2, LINE);
    rect(cx - 1, cy - r, 2, 2 * r, LINE);
    float dx = cx + v[s * 2] * r / 127.0f, dy = cy + v[s * 2 + 1] * r / 127.0f;
    line(cx, cy, dx, dy, 3, 0x6cb8ff90);
    disc(dx, dy, 11, WHITE);
}

/* Three seconds without touching the sticks: the mean offset and the noise. */
static void rest_test(void)
{
    long sum[4] = {0, 0, 0, 0};
    int mn[4] = {999, 999, 999, 999}, mx[4] = {-999, -999, -999, -999}, n = 0;
    s64 t0 = sysGetSystemTime();
    while (sysGetSystemTime() - t0 < 3000000) {
        read_pad();
        if (quit) return;
        int v[4] = {LX, LY, RX, RY};
        for (int k = 0; k < 4; k++) {
            sum[k] += v[k];
            if (v[k] < mn[k]) mn[k] = v[k];
            if (v[k] > mx[k]) mx[k] = v[k];
        }
        n++;
        double left = 3.0 - (double)(sysGetSystemTime() - t0) / 1e6;
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Rest test: let go of the sticks");
        text(MG, TOP + 70, F_LARGE | TNUM, GREEN, "%.1f s", left);
        bar(MG, TOP + 160, SW - 2 * MG, 14, (float)(1 - left / 3.0), GREEN);
        float r = 110, cy = TOP + 230 + r;
        stick(SW / 2 - 220, cy, r, 0, 0);
        stick(SW / 2 + 220, cy, r, 1, 0);
        text_c(SW / 2 - 220, cy + r + 24, F_BODY | TNUM, GREY, "L %+4d %+4d", v[0], v[1]);
        text_c(SW / 2 + 220, cy + r + 24, F_BODY | TNUM, GREY, "R %+4d %+4d", v[2], v[3]);
        ui_flip();
    }
    if (!n) return;
    for (int k = 0; k < 4; k++) {
        rest[k] = (float)sum[k] / n;
        jitter[k] = mx[k] - mn[k];
    }
    tested = 1;
    pressed = 0;                             /* a button still down is not a new press */
}

static void circle_screen(void)
{
    memset(maxr, 0, sizeof maxr);
    circle_done = 0;                         /* a cancelled test leaves no old result behind */
    s64 t0 = sysGetSystemTime();
    while (sysGetSystemTime() - t0 < 8000000) {
        read_pad();
        if (quit || (pressed & BTN_CIRCLE)) return;
        int v[4] = {LX, LY, RX, RY};
        for (int s = 0; s < 2; s++) {
            float dx = v[s * 2], dy = v[s * 2 + 1];
            float r = hypotf(dx, dy);
            if (r > 127) r = 127;
            int sec = ((int)floorf((atan2f(dy, dx) + PI_F) * 18.0f / PI_F)) % 36;
            if (sec < 0) sec += 36;
            if (r > maxr[s][sec]) maxr[s][sec] = (unsigned char)r;
        }
        double left = 8.0 - (double)(sysGetSystemTime() - t0) / 1e6;
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Circle test: turn both sticks slowly around their full edge");
        text(MG, TOP + 50, F_BODY | TNUM, GREY, "%.0f s left. The green shape is where each stick reached.", left);
        float r = (BOTTOM - TOP - 140) / 2;
        if (r > 200) r = 200;
        float cy = TOP + 120 + r;
        stick(SW / 2 - r - 80, cy, r, 0, 1);
        stick(SW / 2 + r + 80, cy, r, 1, 1);
        footer("CIRCLE cancel");
        ui_flip();
    }
    for (int s = 0; s < 2; s++) {
        int sum = 0;
        for (int k = 0; k < 36; k++) sum += maxr[s][k];
        cover[s] = sum / 36.0f / 127.0f;
    }
    circle_done = 1;
    pressed = 0;                             /* a button still down is not a new press */
}

static void rumble(int port)
{
    padActParam act;
    memset(&act, 0, sizeof act);
    for (int phase = 0; phase < 3; phase++) {
        act.small_motor = phase == 0;
        act.large_motor = phase == 1 ? 255 : 0;
        ioPadSetActDirect(port, &act);
        s64 t0 = sysGetSystemTime();
        while (sysGetSystemTime() - t0 < (phase == 2 ? 100000 : 1000000)) {
            read_pad();
            if (quit) break;
            begin_frame();
            title();
            text(MG, TOP, F_MED, WHITE, "Rumble");
            text(MG, TOP + 70, F_LARGE, phase < 2 ? GREEN : GREY, "%s",
                 phase == 0 ? "Small motor" : phase == 1 ? "Large motor" : "Off");
            text(MG, TOP + 160, F_BODY, GREY, "%s", phase == 0 ? "High frequency, a buzz." : phase == 1 ? "Low frequency, a strong shake." : "");
            ui_flip();
        }
    }
    pressed = 0;                             /* a button still down is not a new press */
}

/* A D-pad direction: a key with an arrow, lit while held, the pressure under it. */
static void dkey(float cx, float cy, float dx, float dy, int down, int pre)
{
    round_rect(cx - 30, cy - 30, 60, 60, 12, down ? 0x2c6a48ff : PANEL2);
    float tx = cx + dx * 14, ty = cy + dy * 14, px = -dy * 12, py = dx * 12;
    tri(tx, ty, cx - dx * 8 + px, cy - dy * 8 + py, cx - dx * 8 - px, cy - dy * 8 - py, down ? WHITE : GREY);
    if (pre > 0) text_c(cx + dx * 66, cy + dy * 66 - line_h(F_SMALL) / 2, F_SMALL | TNUM, WHITE, "%d", pre);
}

/* A face button, ringed while held, its pressure beside it. */
static void fkey(float cx, float cy, float dx, float dy, unsigned btn, u32 c, int pre)
{
    if (held & btn) ring(cx, cy, 36, 5, c);
    button_icon(cx, cy, 58, btn);
    if (pre > 0) text_c(cx + dx * 70, cy + dy * 70 - line_h(F_SMALL) / 2, F_SMALL | TNUM, WHITE, "%d", pre);
}

/* A shoulder or trigger: its name, a pressure bar, the value. */
static void shoulder(float x, float y, float w, const char *name, unsigned btn, int pre)
{
    int down = (held & btn) != 0;
    round_rect(x, y, 56, 36, 8, down ? 0x2c6a48ff : PANEL2);
    text_c(x + 28, y + 4, F_SMALL, down ? WHITE : GREY, "%s", name);
    bar(x + 72, y + 12, w - 72 - 64, 12, pre / 255.0f, BLUE);
    text_r(x + w, y + 3, F_SMALL | TNUM, pre ? WHITE : DIM, "%d", pre);
}

static void pill(float x, float y, const char *name, int down)
{
    int w = text_w(F_SMALL, name) + 28;
    round_rect(x, y, w, 38, 19, down ? 0x2c6a48ff : PANEL2);
    text(x + 14, y + 5, F_SMALL, down ? WHITE : GREY, "%s", name);
}

static void draw_live(int port)
{
    const padData *p = &pad_last;
    float H = BOTTOM - TOP, wl = (SW - 2 * MG - 24) * 0.46f, wr = SW - 2 * MG - 24 - wl;
    float xl = MG, xr = MG + wl + 24;
    card(xl, TOP, wl, H);
    card(xr, TOP, wr, H);
    int v[4] = {LX, LY, RX, RY};

    /* sticks */
    heading(xl + 32, TOP + 22, "STICKS");
    float r = (wl - 160) / 4;
    if (r > 130) r = 130;
    if (r > (H - 330) / 2) r = (H - 330) / 2;
    float cy = TOP + 70 + r;
    for (int s = 0; s < 2; s++) {
        float cx = xl + wl * (s ? 0.74f : 0.26f), ty = cy + r + 26;
        stick(cx, cy, r, s, circle_done);
        text_c(cx, ty, F_BODY | TNUM, WHITE, "%s %+4d %+4d", s ? "R" : "L", v[s * 2], v[s * 2 + 1]);
        if (tested) {
            float d = worst_drift(s);
            int noise = jitter[s * 2] > jitter[s * 2 + 1] ? jitter[s * 2] : jitter[s * 2 + 1];
            text_c(cx, ty + 42, F_BODY, drift_color(d), "drift %.1f%%", d);
            text_c(cx, ty + 80, F_SMALL, GREY, "rest %+.1f %+.1f, noise %d", rest[s * 2], rest[s * 2 + 1], noise);
        } else text_c(cx, ty + 42, F_BODY, DIM, "rest: not tested");
        if (circle_done) text_c(cx, ty + 116, F_SMALL, cover[s] >= 0.85f ? GREEN : YELLOW, "circle %.0f%%", cover[s] * 100);
    }
    text_wrap(xl + 32, TOP + H - 74, F_SMALL, GREY, wl - 64, 2,
              "Drift is the rest offset as a share of the full travel: under 4 % is normal, over 8 % games see it.");

    /* buttons */
    heading(xr + 32, TOP + 22, "BUTTONS AND PRESSURE");
    float y = TOP + 62, half = (wr - 96) / 2;
    shoulder(xr + 32, y, half, "L2", BTN_L2, p->PRE_L2);
    shoulder(xr + 64 + half, y, half, "R2", BTN_R2, p->PRE_R2);
    shoulder(xr + 32, y + 48, half, "L1", BTN_L1, p->PRE_L1);
    shoulder(xr + 64 + half, y + 48, half, "R1", BTN_R1, p->PRE_R1);
    float dc = xr + wr * 0.26f, fc = xr + wr * 0.74f, kc = y + 244;
    dkey(dc, kc - 66, 0, -1, held & BTN_UP, p->PRE_UP);
    dkey(dc, kc + 66, 0, 1, held & BTN_DOWN, p->PRE_DOWN);
    dkey(dc - 66, kc, -1, 0, held & BTN_LEFT, p->PRE_LEFT);
    dkey(dc + 66, kc, 1, 0, held & BTN_RIGHT, p->PRE_RIGHT);
    fkey(fc, kc - 66, 0, -1, BTN_TRIANGLE, C_TRIANGLE, p->PRE_TRIANGLE);
    fkey(fc + 66, kc, 1, 0, BTN_CIRCLE, C_CIRCLE, p->PRE_CIRCLE);
    fkey(fc, kc + 66, 0, 1, BTN_CROSS, C_CROSS, p->PRE_CROSS);
    fkey(fc - 66, kc, -1, 0, BTN_SQUARE, C_SQUARE, p->PRE_SQUARE);
    float my = kc + 154, mx = xr + wr / 2;
    pill(mx - 190, my, "SELECT", held & BTN_SELECT);
    pill(mx - 60, my, "L3", held & BTN_L3);
    pill(mx + 14, my, "R3", held & BTN_R3);
    pill(mx + 90, my, "START", held & BTN_START);

    /* sensors */
    y = my + 64;
    if (y + 90 <= TOP + H) {
        heading(xr + 32, y, "SIXAXIS");
        text(xr + 32, y + 36, F_BODY | TNUM, WHITE, "X %4u   Y %4u   Z %4u   gyro %4u", p->SENSOR_X, p->SENSOR_Y, p->SENSOR_Z, p->SENSOR_G);
        float bx = xr + wr - 90, by = y + 40;
        ring(bx, by, 34, 3, LINE);
        disc(bx + ((int)p->SENSOR_X - 512) / 10.0f, by + ((int)p->SENSOR_Z - 512) / 10.0f, 7, WHITE);
        text_fit(xr + 32, y + 78, F_SMALL, GREY, wr - 200, "port %d   pressure %s   sensors %s   rumble %s", port,
                 have_cap ? (cap_bit(CAP_PRESSURE) ? "yes" : "no") : "?", have_cap ? (cap_bit(CAP_SENSORS) ? "yes" : "no") : "?",
                 have_cap ? (cap_bit(CAP_RUMBLE) ? "yes" : "no") : "?");
    }
}

static void pad_tile(void)
{
    char l1[48], l2[48];
    if (!tested) return;
    float d = fmaxf(worst_drift(0), worst_drift(1));
    snprintf(l1, sizeof l1, "Drift L %.1f%%, R %.1f%%", worst_drift(0), worst_drift(1));
    if (circle_done) snprintf(l2, sizeof l2, "Circle L %.0f%%, R %.0f%%", cover[0] * 100, cover[1] * 100);
    else snprintf(l2, sizeof l2, "Circle test not run");
    state_set("pad", d < 4 ? DOT_OK : d < 8 ? DOT_WARN : DOT_BAD, l1, l2);
}

int mod_pad_open(void)
{
    int port = pad_port < 0 ? 0 : pad_port;
    ioPadSetPressMode(port, 1);
    ioPadSetSensorMode(port, 1);
    have_cap = ioPadGetCapabilityInfo(port, &cap) == 0;
    /* A button tester: every button only lights up. The actions need SELECT
     * held, so testing START or CIRCLE does not leave the screen. */
    while (1) {
        read_pad();
        int mod = (held & BTN_SELECT) != 0;
        if (quit || (mod && (pressed & BTN_START))) { pad_tile(); return -1; }
        if (mod && (pressed & BTN_CIRCLE)) { pad_tile(); return 0; }
        if (mod && (pressed & BTN_CROSS)) rest_test();
        if (mod && (pressed & BTN_TRIANGLE)) circle_screen();
        if (mod && (pressed & BTN_SQUARE)) rumble(port);
        begin_frame();
        title();
        draw_live(port);
        footer("SELECT hold, then:  CROSS rest test  TRIANGLE circle test  SQUARE rumble  CIRCLE home  START exit");
        ui_flip();
    }
}

void mod_pad_report(void)
{
    if (!tested && !circle_done) return;
    rep_out("--- controller ---\n");
    if (have_cap)
        rep_out("capabilities 0x%08x: ps3 spec %d, pressure %d, sensors %d, high precision sticks %d, rumble %d\n", cap.info[0],
                cap_bit(CAP_PS3), cap_bit(CAP_PRESSURE), cap_bit(CAP_SENSORS), cap_bit(CAP_HPS), cap_bit(CAP_RUMBLE));
    if (tested)
        rep_out("rest offset (of 127): left %+.1f %+.1f (noise %d %d), right %+.1f %+.1f (noise %d %d); drift L %.1f%% R %.1f%%\n",
                rest[0], rest[1], jitter[0], jitter[1], rest[2], rest[3], jitter[2], jitter[3], worst_drift(0), worst_drift(1));
    if (circle_done) rep_out("circle coverage: left %.0f%%, right %.0f%%\n", cover[0] * 100, cover[1] * 100);
}
