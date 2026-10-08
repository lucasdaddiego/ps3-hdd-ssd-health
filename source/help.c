/* Help, from TRIANGLE on the home screen: the QR code of the compatibility
 * report, which opens a prefilled GitHub issue on a phone, the text it
 * carries, and a short guide to the report, the freeze journal and the repo. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "app.h"
#include "ui.h"
#include "report.h"
#include "compat.h"
#include "version.h"
#include "qrcodegen.h"

#define QR_MAX_VERSION 25
#define QR_SIDE (QR_MAX_VERSION * 4 + 17)    /* modules per side */

/* the QR code of the compat report, rebuilt when Help opens and the link changed */
static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static int qr_ok, qr_side, qr_nruns;
static char qr_body[1024];
static struct { uint8_t r, c, len; } qr_run[QR_SIDE * (QR_SIDE + 1) / 2];   /* the dark runs of each row */

/* ---- QR ------------------------------------------------------------------- */

/* Encodes the link and keeps the code as runs of dark modules: the encode
 * (eight masks tried) and the module scan run once per new link, not per frame. */
static void build_qr(void)
{
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    static char url[2400], last_url[2400];
    char title_s[96];
    compat_title(&D, title_s, sizeof title_s);
    compat_body(&D, qr_body, sizeof qr_body);
    /* A blank issue with title and body: the issue form (template=...) takes
     * field values on the web, but the GitHub mobile app, which catches the
     * link on a phone, fills only title and body. */
    const form_field f[] = {{"body", qr_body}};
    if (issue_form_url(NULL, title_s, f, 1, url, sizeof url) < 0) { qr_ok = 0; last_url[0] = 0; return; }
    if (qr_ok && !strcmp(url, last_url)) return;
    snprintf(last_url, sizeof last_url, "%s", url);
    qr_ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
    qr_nruns = 0;
    qr_side = qr_ok ? qrcodegen_getSize(qr) : 0;
    for (int r = 0; r < qr_side; r++)
        for (int c = 0; c < qr_side; c++) {
            if (!qrcodegen_getModule(qr, c, r)) continue;
            int c2 = c;
            while (c2 + 1 < qr_side && qrcodegen_getModule(qr, c2 + 1, r)) c2++;
            qr_run[qr_nruns].r = (uint8_t)r;
            qr_run[qr_nruns].c = (uint8_t)c;
            qr_run[qr_nruns++].len = (uint8_t)(c2 - c + 1);
            c = c2;
        }
}

/* The dark runs as one batch of quads, on a white field with the quiet
 * zone; whole pixels per module. Returns the side drawn. */
static float draw_qr(float x, float y, float box)
{
    if (!qr_ok) { text_wrap(x, y, F_BODY, YELLOW, box, 3, "The report is too long for a QR code."); return box; }
    float m = (float)(int)(box / (qr_side + 8));
    if (m < 1) m = 1;
    float side = m * (qr_side + 8);
    round_rect(x, y, side, side, 10, WHITE);
    batch_begin(BLACK);
    for (int k = 0; k < qr_nruns; k++)
        batch_rect(x + (qr_run[k].c + 4) * m, y + (qr_run[k].r + 4) * m, qr_run[k].len * m, m);
    batch_end();
    return side;
}

/* ---- screen --------------------------------------------------------------- */

#define LINE_MAX 1200                        /* longer lines are hard to read on a TV, and reach its edges */

static float note(float x, float y, float w, const char *key, const char *s)
{
    float tw = w - 260 < LINE_MAX ? w - 260 : LINE_MAX;
    text(x, y, F_BODY, WHITE, "%s", key);
    int n = text_wrap(x + 260, y, F_BODY, GREY, tw, 3, s);
    return y + (n > 1 ? n : 1) * (line_h(F_BODY) + line_h(F_BODY) / 6) + 10;
}

int help_screen(void)
{
    char msg[160] = "";
    u32 msg_color = WHITE;
    build_qr();                              /* the drive data of this moment; no new encode for the same link */
    ui_module = "Help";
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { ui_module = NULL; pressed = 0; return back; }   /* the press that leaves acts nowhere else */
        if (pressed) msg[0] = 0;
        if (pressed & BTN_SELECT) msg_color = report_to_usb(msg, sizeof msg);
        begin_frame();
        title();
        /* the code and its text on top, the guide in a card under them: the
         * card gets the height of its notes (two of two lines, one of one) */
        int step = line_h(F_BODY) + line_h(F_BODY) / 6;
        float card_h = 22 + line_h(F_SMALL) + 8 + 2 * (2 * step + 10) + step + 18;
        float box = BOTTOM - TOP - 32 - card_h;
        if (box > 480) box = 480;
        float side = draw_qr(MG, TOP, box);
        float x = MG + side + 48, w = SW - MG - x, y = TOP;
        if (w > LINE_MAX) w = LINE_MAX;
        y = heading(x, y, "REPORT YOUR SETUP");
        y += text_wrap(x, y, F_BODY, WHITE, w, 3,
                       "Scan with a phone: it opens a new GitHub issue with the text below. Nothing is sent until you "
                       "submit it. No serial number, no console id.") * step + 12;
        if (!mod_drive_probed()) {
            text_fit(x, y, F_BODY, YELLOW, w, "%s", "The drive is not read yet: open Drive first, then come back.");
            y += step + 8;
        }
        float cy = BOTTOM - card_h, cx = MG + 32, cw = SW - 2 * MG - 64;   /* the card sits at the foot */
        int sstep = line_h(F_SMALL) + line_h(F_SMALL) / 6;
        if (cy - 24 > y + sstep) text_wrap(x, y, F_SMALL, GREY, w, (int)((cy - 24 - y) / sstep), qr_body);

        card(MG, cy, SW - 2 * MG, card_h);
        cy = heading(cx, cy + 22, "GOOD TO KNOW");
        cy = note(cx, cy, cw, "Report", "Every module writes it in " APP_DIR ". SELECT copies it and the freeze journal to "
                                        "a USB stick, into ps3_health/.");
        cy = note(cx, cy, cw, "Freeze journal", "Each risky call is written down before it runs. If one freezes the console, "
                                                "hold the power button: the next start skips it. SQUARE twice on the home "
                                                "screen clears the journal.");
        note(cx, cy, cw, "Source", &APP_REPO[sizeof "https://" - 1]);
        if (msg[0]) status_line(msg, msg_color);
        footer("SELECT copy report to USB  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}
