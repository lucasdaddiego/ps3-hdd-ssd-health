/* The Display module: the video output, the visible area of the TV (measured
 * at the first start, then every screen of the app is drawn 1:1 inside it),
 * and full-screen test patterns. No system call beyond the video state read. */
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <lv2/systime.h>
#include "app.h"
#include "ui.h"
#include "report.h"

static videoState vs;
static videoResolution vr;
static int have_vs;
static int visited;

static const char *res_name(int id)
{
    switch (id) {
    case VIDEO_RESOLUTION_1080: return "1080";
    case VIDEO_RESOLUTION_720: return "720";
    case VIDEO_RESOLUTION_480: return "480";
    case VIDEO_RESOLUTION_576: return "576";
    case VIDEO_RESOLUTION_1600x1080: return "1600x1080";
    case VIDEO_RESOLUTION_1440x1080: return "1440x1080";
    case VIDEO_RESOLUTION_1280x1080: return "1280x1080";
    case VIDEO_RESOLUTION_960x1080: return "960x1080";
    default: return "?";
    }
}

static const char *color_space(int c) { return c == 1 ? "RGB" : c == 2 ? "YUV" : c == 4 ? "xvYCC" : "?"; }

static void refresh_text(unsigned bits, char *out, int n)
{
    int j = 0;
    out[0] = 0;
    if (bits & 1) j += snprintf(out + j, n - j, "%s59.94", j ? "/" : "");
    if (bits & 2) j += snprintf(out + j, n - j, "%s50", j ? "/" : "");
    if (bits & 4) j += snprintf(out + j, n - j, "%s60", j ? "/" : "");
    if (bits & 8) j += snprintf(out + j, n - j, "%s30", j ? "/" : "");
    if (!j) snprintf(out, n, "?");
}

static void read_video(void)
{
    memset(&vs, 0, sizeof vs);
    memset(&vr, 0, sizeof vr);
    have_vs = videoGetState(0, 0, &vs) == 0;
    if (have_vs) videoGetResolution(vs.displayMode.resolution, &vr);
}

static void mode_line(char *out, int n)
{
    char rr[32];
    if (!have_vs) { snprintf(out, n, "video state not readable"); return; }
    refresh_text(vs.displayMode.refreshRates, rr, sizeof rr);
    snprintf(out, n, "%ux%u %s%s, %s, %s, %s Hz", vr.width, vr.height, res_name(vs.displayMode.resolution),
             vs.displayMode.scanMode == VIDEO_SCANMODE_PROGRESSIVE ? "p" : "i",
             vs.displayMode.aspect == VIDEO_ASPECT_16_9 ? "16:9" : vs.displayMode.aspect == VIDEO_ASPECT_4_3 ? "4:3" : "aspect auto",
             color_space(vs.colorSpace), rr);
}

static int at_1080(void) { return have_vs && vr.width == 1920 && vr.height == 1080; }

/* ---- the visible area ----------------------------------------------------- */

/* An arrow whose tip touches the edge, pointing at it. */
static void arrow(float tx, float ty, float dx, float dy, u32 c)
{
    float bx = tx - dx * 46, by = ty - dy * 46, px = -dy * 26, py = dx * 26;
    tri(tx, ty, bx + px, by + py, bx - px, by - py, c);
    line(bx, by, bx - dx * 40, by - dy * 40, 10, c);
}

/* The edges on the full screen. D-pad moves both sides together; SQUARE
 * switches to one edge at a time (L1/R1 choose it). CROSS saves, TRIANGLE
 * takes the whole screen. CIRCLE leaves; at the first start it keeps the
 * default and saves it, so the screen does not come back. */
void mod_display_calibrate(int first)
{
    int e[4] = {safe_l, safe_t, safe_r, safe_b};
    int single = 0, sel = 0;
    s64 since = 0, last = 0;
    static const char *const names[4] = {"left", "top", "right", "bottom"};
    safe_override(1);
    while (!quit) {
        read_pad();
        if (pressed & BTN_CIRCLE) {
            if (first) safe_set(safe_l, safe_t, safe_r, safe_b);
            break;
        }
        if (pressed & BTN_CROSS) {
            safe_set(e[0], e[1], e[2], e[3]);
            break;
        }
        if (pressed & BTN_TRIANGLE) { e[0] = 0; e[1] = 0; e[2] = 1920; e[3] = 1080; }
        if (pressed & BTN_SQUARE) single = !single;
        if (single && (pressed & BTN_R1)) sel = (sel + 1) % 4;
        if (single && (pressed & BTN_L1)) sel = (sel + 3) % 4;
        unsigned dir = held & (BTN_LEFT | BTN_RIGHT | BTN_UP | BTN_DOWN);
        s64 now = sysGetSystemTime();
        int step = 0;
        if (pressed & dir) { step = 1; since = last = now; }
        else if (dir && now - since > 350000 && now - last > 30000) { step = 1; last = now; }
        if (step) {
            int n[4] = {e[0], e[1], e[2], e[3]};
            if (!single) {
                int h = (dir & BTN_RIGHT) ? 2 : (dir & BTN_LEFT) ? -2 : 0, v = (dir & BTN_UP) ? 2 : (dir & BTN_DOWN) ? -2 : 0;
                n[0] -= h; n[2] += h; n[1] -= v; n[3] += v;
                if (n[0] < 0) n[0] = 0;              /* one side at the screen edge: the other still moves */
                if (n[2] > 1920) n[2] = 1920;
                if (n[1] < 0) n[1] = 0;
                if (n[3] > 1080) n[3] = 1080;
            } else if (sel == 0 || sel == 2) n[sel] += (dir & BTN_RIGHT) ? 2 : (dir & BTN_LEFT) ? -2 : 0;
            else n[sel] += (dir & BTN_DOWN) ? 2 : (dir & BTN_UP) ? -2 : 0;
            if (safe_valid(n[0], n[1], n[2], n[3])) memcpy(e, n, sizeof e);   /* the same check as the next start */
        }

        begin_frame();
        u32 cut = 0x3c1018ff;
        rect(0, 0, 1920, e[1], cut);
        rect(0, e[3], 1920, 1080 - e[3], cut);
        rect(0, e[1], e[0], e[3] - e[1], cut);
        rect(e[2], e[1], 1920 - e[2], e[3] - e[1], cut);
        for (int k = 0; k < 4; k++) {
            u32 c = single && k == sel ? YELLOW : single ? 0x4fd98280 : GREEN;
            if (k == 0) rect(e[0], e[1], 4, e[3] - e[1], c);
            if (k == 1) rect(e[0], e[1], e[2] - e[0], 4, c);
            if (k == 2) rect(e[2] - 4, e[1], 4, e[3] - e[1], c);
            if (k == 3) rect(e[0], e[3] - 4, e[2] - e[0], 4, c);
        }
        float cx = (e[0] + e[2]) / 2.0f, cy = (e[1] + e[3]) / 2.0f;
        arrow(e[0], cy, -1, 0, single && sel == 0 ? YELLOW : GREEN);
        arrow(cx, e[1], 0, -1, single && sel == 1 ? YELLOW : GREEN);
        arrow(e[2], cy, 1, 0, single && sel == 2 ? YELLOW : GREEN);
        arrow(cx, e[3], 0, 1, single && sel == 3 ? YELLOW : GREEN);

        float w = 1180, h = 420, x = cx - w / 2, y = cy - h / 2;
        card(x, y, w, h);
        text(x + 40, y + 32, F_MED, WHITE, first ? "Your TV: the visible area" : "Visible area");
        text_wrap(x + 40, y + 96, F_BODY, GREY, w - 80, 2,
                  "Move the edges until the four arrow tips touch the edges of your TV picture. The app then draws "
                  "every screen inside them, pixel for pixel.");
        if (single) text(x + 40, y + 190, F_BODY, YELLOW, "One edge at a time: the %s edge. L1/R1 choose the edge, the D-pad moves it.", names[sel]);
        else text(x + 40, y + 190, F_BODY, WHITE, "D-pad: LEFT/RIGHT narrower or wider, UP/DOWN shorter or taller.");
        text(x + 40, y + 236, F_SMALL | TNUM, GREY, "left %d   top %d   right %d   bottom %d   =   %d x %d of 1920 x 1080", e[0], e[1], e[2],
             e[3], e[2] - e[0], e[3] - e[1]);
        footer_at(x + 40, y + 300, single ? "CROSS save  SQUARE both sides  TRIANGLE whole screen" : "CROSS save  SQUARE one edge  TRIANGLE whole screen");
        footer_at(x + 40, y + 350, first ? "CIRCLE keep the default" : "CIRCLE cancel");
        ui_flip();
    }
    safe_override(0);
    pressed = 0;                             /* a button still down is not a new press */
}

/* ---- test patterns -------------------------------------------------------- */

static const char *const pattern_names[] = {"white", "black", "red", "green", "blue", "gray ramp", "crosshatch and circle",
                                            "1-pixel stripes", "lag timer"};
static const char *const pattern_notes[] = {
    "A dark dot is a dead pixel. Also look for uneven brightness at the edges.",
    "A lit dot is a stuck pixel. Also look for light bleeding in from the edges.",
    "A dot in another colour is a stuck sub-pixel.",
    "A dot in another colour is a stuck sub-pixel.",
    "A dot in another colour is a stuck sub-pixel.",
    "16 steps from black to white: each step should look different from its neighbours.",
    "Straight lines, even squares and a round circle: the TV keeps the geometry and the aspect.",
    "",
    "Film the TV next to this screen's source: the timer difference is the lag. CROSS flashes one frame.",
};
#define NPAT ((int)(sizeof pattern_names / sizeof pattern_names[0]))

static void draw_pattern(int p, s64 t0, int frame, int flash)
{
    switch (p) {
    case 0: rect(0, 0, 1920, 1080, WHITE); break;
    case 1: rect(0, 0, 1920, 1080, BLACK); break;
    case 2: rect(0, 0, 1920, 1080, 0xff0000ff); break;
    case 3: rect(0, 0, 1920, 1080, 0x00ff00ff); break;
    case 4: rect(0, 0, 1920, 1080, 0x0000ffff); break;
    case 5:
        for (int k = 0; k < 16; k++) {
            u32 v = k * 17;
            rect(k * 120, 0, 120, 1080, (v << 24) | (v << 16) | (v << 8) | 0xff);
            text_c(k * 120 + 60, 540, F_SMALL, k < 8 ? WHITE : BLACK, "%u", v);
        }
        break;
    case 6:
        rect(0, 0, 1920, 1080, 0x202020ff);
        for (int x = 0; x <= 1920; x += 120) rect(x - 1, 0, 2, 1080, WHITE);
        for (int y = 0; y <= 1080; y += 108) rect(0, y - 1, 1920, 2, WHITE);
        rect(959, 0, 2, 1080, YELLOW);
        rect(0, 539, 1920, 2, YELLOW);
        ring(960, 540, 432, 3, YELLOW);      /* round on a 16:9 picture shown at the right aspect */
        break;
    case 7:
        /* every other pixel: even stripes mean the TV shows each pixel 1:1;
         * moire or soft bands mean it scales the picture (overscan) */
        rect(0, 0, 1920, 1080, BLACK);
        batch_begin(WHITE);                  /* 1020 stripes: one polygon */
        for (int x = 0; x < 960; x += 2) batch_rect(x, 0, 1, 1080);
        for (int y = 0; y < 1080; y += 2) batch_rect(960, y, 960, 1);
        batch_end();
        break;
    case 8: {
        double ms = (double)(sysGetSystemTime() - t0) / 1e3;
        if (flash) rect(0, 0, 1920, 1080, WHITE);
        float x = (frame * 8) % 1920;
        rect(x, 160, 12, 760, WHITE);
        for (int k = 0; k <= 16; k++) rect(k * 120 - 1, 120, 2, 24, GREY);
        text(safe_l + 60, safe_t + 220, F_LARGE | TNUM, GREEN, "%.0f ms", ms);
        text(safe_l + 60, safe_t + 300, F_MED | TNUM, WHITE, "frame %d", frame);
        break;
    }
    }
}

static void patterns_screen(void)
{
    int p = 0, frame = 0, flash = 0;
    s64 t0 = sysGetSystemTime(), shown = t0;
    safe_override(1);
    while (!quit) {
        read_pad();
        if (pressed & BTN_CIRCLE) break;
        if (pressed & BTN_RIGHT) { p = (p + 1) % NPAT; shown = t0 = sysGetSystemTime(); frame = 0; }
        if (pressed & BTN_LEFT) { p = (p + NPAT - 1) % NPAT; shown = t0 = sysGetSystemTime(); frame = 0; }
        flash = (pressed & BTN_CROSS) != 0;
        begin_frame();
        draw_pattern(p, t0, frame, flash);
        if (sysGetSystemTime() - shown < 3000000 || p >= 7) {     /* the legend, inside the visible area */
            float x = safe_l + 40, y = safe_b - 190, w = safe_r - safe_l - 80;
            round_rect(x, y, w, 150, 16, 0x0b1424e0);
            text(x + 32, y + 22, F_MED, WHITE, "%d/%d  %s", p + 1, NPAT, pattern_names[p]);
            if (p == 7)
                text_fit(x + 32, y + 66, F_BODY, at_1080() ? GREY : YELLOW, w - 64, "%s",
                         at_1080() ? "Even stripes: the TV shows every pixel. Moire bands: it scales the picture (overscan on)."
                                   : "Only meaningful at 1080p output: this output scales the picture.");
            else text_fit(x + 32, y + 66, F_BODY, GREY, w - 64, "%s", pattern_notes[p]);
            footer_at(x + 32, y + 104, "LEFT/RIGHT pattern  CIRCLE back");
        }
        ui_flip();
        frame++;
    }
    safe_override(0);
    pressed = 0;
}

/* ---- module --------------------------------------------------------------- */

static int measured(void) { return safe_l != SAFE_DEF_L || safe_t != SAFE_DEF_T || safe_r != SAFE_DEF_R || safe_b != SAFE_DEF_B; }

static void display_tile(void)
{
    char l1[48], l2[48];
    if (have_vs)
        snprintf(l1, sizeof l1, "%ux%u%s %s", vr.width, vr.height, vs.displayMode.scanMode == VIDEO_SCANMODE_PROGRESSIVE ? "p" : "i",
                 vs.displayMode.aspect == VIDEO_ASPECT_16_9 ? "16:9" : vs.displayMode.aspect == VIDEO_ASPECT_4_3 ? "4:3" : "");
    else snprintf(l1, sizeof l1, "Video state not readable");
    snprintf(l2, sizeof l2, "Visible area %d x %d", safe_r - safe_l, safe_b - safe_t);
    state_set("display", DOT_OK, l1, l2);
}

int mod_display_open(void)
{
    char mode[96];
    read_video();
    visited = 1;
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { display_tile(); return back; }
        if (pressed & BTN_CROSS) mod_display_calibrate(0);
        if (pressed & BTN_TRIANGLE) patterns_screen();
        mode_line(mode, sizeof mode);
        begin_frame();
        title();
        float y = TOP, w = SW - 2 * MG;
        card(MG, y, w, 168);
        heading(MG + 32, y + 22, "OUTPUT");
        text(MG + 32, y + 62, F_MED, WHITE, "%s", mode);
        text_fit(MG + 32, y + 112, F_BODY, GREY, w - 64, "%s",
                 at_1080() ? "The app draws 1:1 at this output: every pixel of its 1920 x 1080 space is a pixel on the TV."
                           : "The app draws in a 1920 x 1080 space; the console scales it to this output.");
        y += 196;
        card(MG, y, w, 168);
        heading(MG + 32, y + 22, "VISIBLE AREA");
        text(MG + 32, y + 62, F_MED, measured() ? GREEN : WHITE, "%d x %d  %s", safe_r - safe_l, safe_b - safe_t,
             measured() ? "measured on this TV" : "the default, 93 % of the screen");
        text_fit(MG + 32, y + 112, F_BODY, GREY, w - 64, "left %d, top %d, right %d, bottom %d.  CROSS adjusts it.", safe_l, safe_t,
                 safe_r, safe_b);
        y += 196;
        if (y + 168 <= BOTTOM) {
            card(MG, y, w, 168);
            heading(MG + 32, y + 22, "TEST PATTERNS");
            text_wrap(MG + 32, y + 62, F_BODY, WHITE, w - 64, 3,
                      "TRIANGLE: plain colours for dead pixels, a gray ramp, a crosshatch with a circle for geometry and "
                      "aspect, 1-pixel stripes that show whether the TV scales the picture, and a lag timer.");
        }
        footer("CROSS adjust the visible area  TRIANGLE test patterns  CIRCLE home  START exit");
        ui_flip();
    }
}

void mod_display_report(void)
{
    if (!visited) return;
    char mode[96];
    mode_line(mode, sizeof mode);
    rep_out("--- display ---\noutput %s (resolution id %u, color space %u, refresh bits 0x%x)\n", mode,
            vs.displayMode.resolution, vs.colorSpace, vs.displayMode.refreshRates);
    rep_out("visible area %s: left %d top %d right %d bottom %d of 1920 x 1080\n", measured() ? "measured" : "default",
            safe_l, safe_t, safe_r, safe_b);
}
