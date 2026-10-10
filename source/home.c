/* The home screen: six tiles with the last result of each module, from
 * APP_DIR/state.txt. It sends no command to the hardware. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/systime.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"

#define STATE_FILE APP_DIR "/state.txt"

const module modules[MODULE_COUNT] = {
    {"Drive", "drive", "The internal drive: SMART, the self-tests and a speed test.", mod_drive_open,
     mod_drive_report},
    {"Cooling", "cooling", "Temperatures, the fan duty, and a load test with a curve.", mod_cool_open,
     mod_cool_report},
    {"Controller", "pad", "Stick drift, buttons, the sensors and the rumble.", mod_pad_open,
     mod_pad_report},
    {"Display", "display", "The output mode, your TV's visible area, test patterns.", mod_display_open,
     mod_display_report},
    {"Transfer", "transfer", "The speed of a USB stick, the internet and the LAN.", mod_net_open,
     mod_net_report},
    {"Memory", "memory", "Pattern tests on the XDR and the RSX memory.", mod_mem_open,
     mod_mem_report},
};

static tile_state tiles[MODULE_COUNT];

static int tile_index(const char *key)
{
    for (int k = 0; k < MODULE_COUNT; k++)
        if (!strcmp(modules[k].key, key)) return k;
    return -1;
}

/* key|dot|date|line 1|line 2 */
void state_load(void)
{
    static char t[2048];
    int n = fs_read_file(STATE_FILE, t, sizeof t - 1);
    memset(tiles, 0, sizeof tiles);
    if (n <= 0) return;
    t[n] = 0;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[5];
        int nf = 0;
        for (char *c = line; nf < 5; nf++) {
            f[nf] = c;
            char *bar = strchr(c, '|');
            if (!bar) { nf++; break; }
            *bar = 0;
            c = bar + 1;
        }
        if (nf < 5) continue;
        int k = tile_index(f[0]);
        if (k < 0) continue;
        tiles[k].dot = f[1][0] - '0';
        if (tiles[k].dot < 0 || tiles[k].dot > DOT_BAD) tiles[k].dot = DOT_NONE;
        snprintf(tiles[k].when, sizeof tiles[k].when, "%s", f[2]);
        snprintf(tiles[k].l1, sizeof tiles[k].l1, "%s", f[3]);
        snprintf(tiles[k].l2, sizeof tiles[k].l2, "%s", f[4]);
    }
}

/* Written when the text changed since the last good write: every return to
 * the home screen sets a tile, and an fsync'd write of the same bytes would
 * hold the UI thread. A failed write is tried again. */
static void state_save(void)
{
    static char t[2048], last[sizeof t];
    static int last_len = -1, last_rc;
    int j = 0;
    for (int k = 0; k < MODULE_COUNT; k++) {
        if (!tiles[k].dot && !tiles[k].l1[0]) continue;
        j += snprintf(t + j, sizeof t - j, "%s|%d|%s|%s|%s\n", modules[k].key, tiles[k].dot, tiles[k].when, tiles[k].l1,
                      tiles[k].l2);
        if (j >= (int)sizeof t - 1) break;
    }
    if (j > (int)sizeof t - 1) j = (int)sizeof t - 1;
    if (j == last_len && !last_rc && !memcmp(t, last, (size_t)j)) return;
    last_rc = fs_write_file(STATE_FILE, t, j, 0);
    memcpy(last, t, (size_t)j);
    last_len = j;
}

void state_set(const char *key, int dot, const char *l1, const char *l2)
{
    int k = tile_index(key);
    if (k < 0) return;
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    time_t tt = (time_t)sec;
    struct tm tm;
    gmtime_r(&tt, &tm);
    tiles[k].dot = dot;
    snprintf(tiles[k].when, sizeof tiles[k].when, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(tiles[k].l1, sizeof tiles[k].l1, "%.44s", l1 ? l1 : "");
    snprintf(tiles[k].l2, sizeof tiles[k].l2, "%.44s", l2 ? l2 : "");
    for (int i = 0; i < 2; i++) {         /* a bar would break the file */
        char *s = i ? tiles[k].l2 : tiles[k].l1;
        for (; *s; s++) if (*s == '|' || *s == '\n') *s = ' ';
    }
    state_save();
}

static u32 dot_color(int dot) { return dot == DOT_OK ? GREEN : dot == DOT_WARN ? YELLOW : dot == DOT_BAD ? RED : GREY; }

/* A line icon per module, in a 72 px box. */
static void icon(int k, float x, float y, u32 c)
{
    switch (k) {
    case 0:                                  /* a drive: case, platter, arm */
        round_frame(x + 2, y + 12, 68, 48, 8, 4, c);
        ring(x + 26, y + 36, 13, 4, c);
        disc(x + 26, y + 36, 3, c);
        line(x + 54, y + 50, x + 34, y + 30, 4, c);
        disc(x + 54, y + 50, 4, c);
        break;
    case 1:                                  /* a thermometer */
        round_frame(x + 26, y + 2, 20, 50, 10, 4, c);
        disc(x + 36, y + 58, 13, c);
        rect(x + 33, y + 24, 6, 30, c);
        for (int i = 0; i < 3; i++) rect(x + 52, y + 10 + i * 10, 10, 3, c);
        break;
    case 2:                                  /* a controller: body, D-pad, face buttons */
        round_frame(x, y + 16, 72, 40, 18, 4, c);
        line(x + 12, y + 36, x + 28, y + 36, 4, c);
        line(x + 20, y + 28, x + 20, y + 44, 4, c);
        disc(x + 52, y + 28, 3.5f, c);
        disc(x + 52, y + 44, 3.5f, c);
        disc(x + 44, y + 36, 3.5f, c);
        disc(x + 60, y + 36, 3.5f, c);
        break;
    case 3:                                  /* a screen on a stand */
        round_frame(x + 2, y + 6, 68, 44, 6, 4, c);
        rect(x + 32, y + 50, 8, 10, c);
        round_rect(x + 18, y + 59, 36, 6, 3, c);
        break;
    case 4:                                  /* up and down arrows */
        line(x + 22, y + 66, x + 22, y + 22, 5, c);
        tri(x + 22, y + 6, x + 9, y + 26, x + 35, y + 26, c);
        line(x + 50, y + 6, x + 50, y + 50, 5, c);
        tri(x + 50, y + 66, x + 37, y + 46, x + 63, y + 46, c);
        break;
    default:                                 /* a memory chip with its pins */
        round_frame(x + 14, y + 14, 44, 44, 5, 4, c);
        rect(x + 28, y + 28, 16, 16, c);
        for (int i = 0; i < 3; i++) {
            float o = 22 + i * 12;
            rect(x + 3, y + o, 11, 4, c);
            rect(x + 58, y + o, 11, 4, c);
            rect(x + o, y + 3, 4, 11, c);
            rect(x + o, y + 58, 4, 11, c);
        }
        break;
    }
}

static void tile_box(int k, float *x, float *y, float *w, float *h)
{
    const float gap = 24;
    *w = (SW - 2 * MG - 2 * gap) / 3;
    *h = (BOTTOM - 8 - TOP - gap) / 2;
    *x = MG + (k % 3) * (*w + gap);
    *y = TOP + (k / 3) * (*h + gap);
}

static void draw_tile(int k, int selected)
{
    float x, y, w, h;
    tile_box(k, &x, &y, &w, &h);
    const tile_state *t = &tiles[k];
    round_rect(x, y, w, h, 18, selected ? PANEL2 : PANEL);
    if (selected) round_frame(x - 4, y - 4, w + 8, h + 8, 22, 3, WHITE);
    icon(k, x + 28, y + 26, selected ? WHITE : BLUE);
    text(x + 124, y + 26, F_MED, WHITE, "%s", modules[k].name);
    if (t->dot) disc(x + w - 42, y + 46, 11, dot_color(t->dot));
    else ring(x + w - 42, y + 46, 9, 3, DIM);    /* hollow: not run yet */
    text_wrap(x + 124, y + 74, F_SMALL, selected ? GREY : DIM, w - 152, 3, modules[k].desc);   /* beside the icon */
    float tw = w - 56, ty = y + h - 124;     /* the result sits at the foot of the tile */
    if (t->dot || t->l1[0]) {
        text_fit(x + 28, ty, F_BODY, WHITE, tw, "%s", t->l1);
        text_fit(x + 28, ty + 40, F_BODY, GREY, tw, "%s", t->l2);
        text(x + 28, y + h - 44, F_SMALL, DIM, "Last run %s", t->when);
    } else text(x + 28, ty + 40, F_BODY, GREY, "Not run yet");
}

static void draw_verdict(void)
{
    char bad[96] = "", warn[96] = "", none[96] = "", msg[200];
    int nb = 0, nw = 0, nn = 0;                  /* counts */
    for (int k = 0; k < MODULE_COUNT; k++) {
        const char *nm = modules[k].name;
        char *list = tiles[k].dot == DOT_BAD ? bad : tiles[k].dot == DOT_WARN ? warn : tiles[k].dot == DOT_NONE ? none : NULL;
        int *count = tiles[k].dot == DOT_BAD ? &nb : tiles[k].dot == DOT_WARN ? &nw : &nn;
        if (!list) continue;
        size_t len = strlen(list);
        snprintf(list + len, sizeof bad - len, "%s%s", len ? ", " : "", nm);
        (*count)++;
    }
    if (nb) snprintf(msg, sizeof msg, "Problem found: %s. Open the module for the details.", bad);
    else if (nw) snprintf(msg, sizeof msg, "Check: %s. Open the module for the details.", warn);
    else if (nn == 0) snprintf(msg, sizeof msg, "No problem found in the six modules.");
    else if (nn == MODULE_COUNT) snprintf(msg, sizeof msg, "Open a module with CROSS. Each one explains what it does before it runs.");
    else snprintf(msg, sizeof msg, "No problem found. Not run yet: %s.", none);
    status_line(msg, nb ? RED : nw ? YELLOW : nn == MODULE_COUNT ? GREY : GREEN);
}

int home_screen(int sel)
{
    char msg[160] = "";
    u32 msg_color = WHITE;
    if (sel < 0 || sel >= MODULE_COUNT) sel = 0;
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CROSS) return sel;
        if ((pressed & BTN_TRIANGLE) && help_screen() < 0) return -1;
        if (pressed & BTN_RIGHT) sel = sel % 3 == 2 ? sel - 2 : sel + 1;
        if (pressed & BTN_LEFT) sel = sel % 3 == 0 ? sel + 2 : sel - 1;
        if (pressed & (BTN_UP | BTN_DOWN)) sel = (sel + 3) % MODULE_COUNT;
        if (fs_ok && gesture(BTN_SQUARE) == BTN_SQUARE) {   /* no writable folder: nothing to clear */
            int rc = journal_clear();
            if (rc == 0) snprintf(msg, sizeof msg, "Freeze journal cleared. Skipped and frozen calls run again. The Drive steps run again after a restart.");
            else snprintf(msg, sizeof msg, "Could not clear the freeze journal: rc 0x%08x", (unsigned)rc);
            msg_color = rc == 0 ? GREEN : RED;
        } else if (pressed & ~BTN_SQUARE) msg[0] = 0;
        if (pressed & BTN_SELECT) msg_color = report_to_usb(msg, sizeof msg);
        begin_frame();
        title();
        if (console_fw[0]) title_right("Firmware %s", console_fw);
        for (int k = 0; k < MODULE_COUNT; k++) draw_tile(k, k == sel);
        if (!fs_ok) {                        /* the file test codes take the line of the report path */
            if (msg[0]) status_line(msg, msg_color);
            else status_line("The app cannot write " APP_DIR ": no journal, so Drive and Cooling stay closed.", RED);
            text_fit(MG, Y_NOTE, F_SMALL, GREY, SW - 2 * MG, "File test: mkdir rc 0x%08x, open rc 0x%08x, write rc 0x%08x",
                     (unsigned)fs_mk, (unsigned)fs_op, (unsigned)fs_wr);
        } else {
            if (armed == BTN_SQUARE) status_line("Press SQUARE again to clear the freeze journal. Any other button cancels.", YELLOW);
            else if (msg[0]) status_line(msg, msg_color);
            else draw_verdict();
            report_footnote();
        }
        footer(fs_ok ? "D-pad choose  CROSS open  TRIANGLE help  SELECT report to USB  SQUARE twice clear journal  START exit"
                     : "D-pad choose  CROSS open  TRIANGLE help  SELECT report to USB  START exit");
        ui_flip();
    }
}
