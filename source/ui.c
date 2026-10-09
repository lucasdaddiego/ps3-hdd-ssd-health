/* The frame: gfx (source/gfx), a 1920x1080 space drawn 1:1 at 1080p into the
 * visible area of the TV, and text from Inter bitmap atlases (data/fonts.bin,
 * made by art/make_font.py), one quad per glyph, so every glyph pixel is
 * exact at 1080p. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ppu-lv2.h>
#include <sys/file.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include "gfx.h"
#include "ui.h"
#include "fonts_bin.h"
#include "ata.h"
#include "fs.h"
#include "version.h"

#define PI_F 3.14159265f

volatile int quit;
unsigned held, pressed, armed;
padData pad_last;
int pad_port = -1;
const char *ui_module;
int ui_demo;
int safe_l = SAFE_DEF_L, safe_t = SAFE_DEF_T, safe_r = SAFE_DEF_R, safe_b = SAFE_DEF_B;
int SW = SAFE_DEF_R - SAFE_DEF_L, SH = SAFE_DEF_B - SAFE_DEF_T;
static int org_x = SAFE_DEF_L, org_y = SAFE_DEF_T;   /* the canvas origin on the screen */

static void sys_callback(u64 status, u64 param, void *usrdata)
{
    (void)param;
    (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) quit = 1;
}

/* ---- fonts ---------------------------------------------------------------- */

typedef struct {
    int cw, ch, base, pad, asc, line, aw, ah;
    unsigned char adv[112];
    gfx_tex tex;                             /* the atlas in RSX memory */
} font_t;

static font_t fonts[F_COUNT];
static int font_first = 32, font_count = 106, font_cols = 16;
static int text_linear = 1;

static unsigned rd16(const u8 *p) { return (unsigned)p[0] << 8 | p[1]; }
static const u8 *align4(const u8 *p) { return p + (4 - (p - fonts_bin) % 4) % 4; }

/* fonts.bin into RSX textures: 4-bit alpha to A4R4G4B4, white. */
static int fonts_init(void)
{
    const u8 *p = fonts_bin;
    if (fonts_bin_size < 12 || memcmp(p, "PHF1", 4)) return -1;
    int n = rd16(p + 4);
    font_first = rd16(p + 6);
    font_count = rd16(p + 8);
    font_cols = rd16(p + 10);
    if (n < F_COUNT || font_count > 112 || font_first + font_count < 138 || !font_cols) return -1;
    p += 12;
    for (int k = 0; k < F_COUNT; k++) {
        font_t *f = &fonts[k];
        f->cw = rd16(p);
        f->ch = rd16(p + 2);
        f->base = rd16(p + 4);
        f->pad = rd16(p + 6);
        f->asc = rd16(p + 8);
        f->line = rd16(p + 10);
        f->aw = rd16(p + 12);
        f->ah = rd16(p + 14);
        p += 16;
        memcpy(f->adv, p, font_count);
        p = align4(p + font_count);
        int npx = f->aw * f->ah;
        u16 *t = gfx_vram(npx * 2, 128, &f->tex.offset);
        if (!t) return -1;
        for (int i = 0; i < npx; i += 2) {
            unsigned a0 = p[i / 2] >> 4, a1 = p[i / 2] & 15;
            t[i] = a0 ? (u16)(a0 << 12 | 0x0fff) : 0;
            t[i + 1] = a1 ? (u16)(a1 << 12 | 0x0fff) : 0;
        }
        p = align4(p + npx / 2);
        f->tex.w = f->aw;
        f->tex.h = f->ah;
        f->tex.pitch = f->aw * 2;
        f->tex.linear = text_linear;
    }
    return 0;
}

/* The atlas slot of a character; with TNUM, digits take their tabular slot (128 + d). */
static int glyph(int font, unsigned char c)
{
    if ((font & TNUM) && c >= '0' && c <= '9') c = 128 + c - '0';
    int k = c - font_first;
    return k < 0 || k >= font_count ? '?' - font_first : k;
}

static int adv(int font, unsigned char c) { return fonts[font & 0xff].adv[glyph(font, c)]; }

static int width_n(int font, const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n && s[i]; i++) w += adv(font, s[i]);
    return w;
}

int text_w(int font, const char *s) { return width_n(font, s, (int)strlen(s)); }
int line_h(int font) { return fonts[font & 0xff].line; }
int base_dy(int big, int small) { return fonts[big & 0xff].asc - fonts[small & 0xff].asc; }

/* n characters, one quad each from the font's atlas, which is bound once per
 * run; each cell lands on whole pixels. */
static float run(float x, float y, int font, u32 color, const char *s, int n)
{
    const font_t *f = &fonts[font & 0xff];
    float px = floorf(x + 0.5f), top = floorf(y + 0.5f) + f->asc - f->base;
    float du = (float)f->cw / f->aw, dv = (float)f->ch / f->ah;
    gfx_texture(&f->tex);
    for (int i = 0; i < n; i++) {
        int k = glyph(font, s[i]);
        if (s[i] != ' ') {
            float u = (k % font_cols) * du, v = (k / font_cols) * dv;
            float x0 = px - f->pad, x1 = x0 + f->cw, y1 = top + f->ch;
            gfx_vtx *q = gfx_prim(GFX_QUADS, 4);
            q = gfx_put(q, x0, top, color, u, v);
            q = gfx_put(q, x1, top, color, u + du, v);
            q = gfx_put(q, x1, y1, color, u + du, v + dv);
            gfx_put(q, x0, y1, color, u, v + dv);
        }
        px += f->adv[k];
    }
    return px;
}

#define FORMAT(buf, fmt)                     \
    char buf[320];                           \
    do {                                     \
        va_list ap;                          \
        va_start(ap, fmt);                   \
        vsnprintf(buf, sizeof buf, fmt, ap); \
        va_end(ap);                          \
    } while (0)

float text(float x, float y, int font, u32 color, const char *fmt, ...)
{
    FORMAT(s, fmt);
    return run(x, y, font, color, s, (int)strlen(s));
}

float text_r(float xr, float y, int font, u32 color, const char *fmt, ...)
{
    FORMAT(s, fmt);
    float x = xr - text_w(font, s);
    run(x, y, font, color, s, (int)strlen(s));
    return x;
}

float text_c(float xc, float y, int font, u32 color, const char *fmt, ...)
{
    FORMAT(s, fmt);
    return run(xc - text_w(font, s) / 2, y, font, color, s, (int)strlen(s));
}

/* Cut with "..." when the text is wider than maxw. */
float text_fit(float x, float y, int font, u32 color, float maxw, const char *fmt, ...)
{
    FORMAT(s, fmt);
    int len = (int)strlen(s);
    if (text_w(font, s) <= maxw) return run(x, y, font, color, s, len);
    int ell = text_w(font, "..."), n = 0, w = 0;
    while (n < len && w + adv(font, s[n]) + ell <= maxw) w += adv(font, s[n++]);
    while (n > 0 && s[n - 1] == ' ') n--;
    return run(run(x, y, font, color, s, n), y, font, color, "...", 3);
}

/* Greedy word wrap to maxw, also at '\n'. */
int text_wrap(float x, float y, int font, u32 color, float maxw, int maxlines, const char *s)
{
    int lines = 0, step = line_h(font) + line_h(font) / 6;
    while (*s && lines < maxlines) {
        int n = 0, w = 0, brk = -1;
        while (s[n] && s[n] != '\n') {
            int a = adv(font, s[n]);
            if (w + a > maxw) break;
            w += a;
            if (s[n] == ' ') brk = n;
            n++;
        }
        int take = n;
        if (s[n] && s[n] != '\n' && brk > 0) take = brk;
        if (take == 0 && s[n] && s[n] != '\n') take = 1;      /* never stall on a too-wide character */
        run(x, y + lines * step, font, color, s, take);
        lines++;
        s += take;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return lines;
}

/* ---- drawing kit ---------------------------------------------------------- */

/* A shape: n vertices of one primitive, colour only. */
static gfx_vtx *shape(int type, int n)
{
    gfx_texture(NULL);
    return gfx_prim(type, n);
}

/* A batch: many quads of one colour. gfx joins consecutive quads into one
 * draw, so batch_end has nothing left to do. */
static u32 batch_c;

void batch_begin(u32 c) { batch_c = c; }

static void batch_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    gfx_vtx *q = shape(GFX_QUADS, 4);
    q = gfx_put(q, x0, y0, batch_c, 0, 0);
    q = gfx_put(q, x1, y1, batch_c, 0, 0);
    q = gfx_put(q, x2, y2, batch_c, 0, 0);
    gfx_put(q, x3, y3, batch_c, 0, 0);
}

void batch_rect(float x, float y, float w, float h) { batch_quad(x, y, x + w, y, x + w, y + h, x, y + h); }

void batch_line(float x0, float y0, float x1, float y1, float w)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    batch_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny);
}

void batch_end(void) {}

void rect(float x, float y, float w, float h, u32 c)
{
    batch_begin(c);
    batch_rect(x, y, w, h);
    batch_end();
}

void rect_v(float x, float y, float w, float h, u32 top, u32 bottom)
{
    gfx_vtx *q = shape(GFX_QUADS, 4);
    q = gfx_put(q, x, y, top, 0, 0);
    q = gfx_put(q, x + w, y, top, 0, 0);
    q = gfx_put(q, x + w, y + h, bottom, 0, 0);
    gfx_put(q, x, y + h, bottom, 0, 0);
}

void frame(float x, float y, float w, float h, float t, u32 c)
{
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y + t, t, h - 2 * t, c);
    rect(x + w - t, y + t, t, h - 2 * t, c);
}

/* The outline of a rounded rectangle, clockwise from the top-left corner. */
static int round_pts(float x, float y, float w, float h, float r, int seg, float *p)
{
    const float cx[4] = {x + r, x + w - r, x + w - r, x + r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
    int n = 0;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= seg; i++) {
            float a = PI_F * (1 + k * 0.5f) + PI_F / 2 * i / seg;
            p[n++] = cx[k] + cosf(a) * r;
            p[n++] = cy[k] + sinf(a) * r;
        }
    return n / 2;
}

static float clamp_r(float r, float w, float h)
{
    float m = (w < h ? w : h) / 2;
    return r > m ? m : r < 0 ? 0 : r;
}

void round_rect(float x, float y, float w, float h, float r, u32 c)
{
    float p[4 * 9 * 2];
    r = clamp_r(r, w, h);
    if (r < 1) { rect(x, y, w, h, c); return; }
    int n = round_pts(x, y, w, h, r, r < 8 ? 4 : 8, p);
    poly_fan(x + w / 2, y + h / 2, p, n, c);
}

void round_frame(float x, float y, float w, float h, float r, float t, u32 c)
{
    float o[4 * 9 * 2], in[4 * 9 * 2];
    r = clamp_r(r, w, h);
    if (r < t) r = t;
    int seg = r < 8 ? 4 : 8;
    int n = round_pts(x, y, w, h, r, seg, o);
    round_pts(x + t, y + t, w - 2 * t, h - 2 * t, r - t, seg, in);
    gfx_vtx *q = shape(GFX_TRIANGLE_STRIP, 2 * (n + 1));
    for (int k = 0; k <= n; k++) {
        int i = k % n;
        q = gfx_put(q, o[i * 2], o[i * 2 + 1], c, 0, 0);
        q = gfx_put(q, in[i * 2], in[i * 2 + 1], c, 0, 0);
    }
}

void line(float x0, float y0, float x1, float y1, float w, u32 c)
{
    batch_begin(c);
    batch_line(x0, y0, x1, y1, w);
    batch_end();
}

void ring(float cx, float cy, float r, float w, u32 c)
{
    float ro = r + w / 2, ri = r - w / 2;
    gfx_vtx *q = shape(GFX_TRIANGLE_STRIP, 2 * 49);
    for (int k = 0; k <= 48; k++) {
        float a = k * 2 * PI_F / 48, ca = cosf(a), sa = sinf(a);
        q = gfx_put(q, cx + ca * ro, cy + sa * ro, c, 0, 0);
        q = gfx_put(q, cx + ca * ri, cy + sa * ri, c, 0, 0);
    }
}

void disc(float cx, float cy, float r, u32 c)
{
    float p[32 * 2];
    for (int k = 0; k < 32; k++) {
        p[k * 2] = cx + cosf(k * 2 * PI_F / 32) * r;
        p[k * 2 + 1] = cy + sinf(k * 2 * PI_F / 32) * r;
    }
    poly_fan(cx, cy, p, 32, c);
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
    gfx_vtx *q = shape(GFX_TRIANGLES, 3);
    q = gfx_put(q, x0, y0, c, 0, 0);
    q = gfx_put(q, x1, y1, c, 0, 0);
    gfx_put(q, x2, y2, c, 0, 0);
}

void poly_fan(float cx, float cy, const float *pts, int n, u32 c)
{
    gfx_vtx *q = shape(GFX_TRIANGLE_FAN, n + 2);
    q = gfx_put(q, cx, cy, c, 0, 0);
    for (int k = 0; k <= n; k++) q = gfx_put(q, pts[(k % n) * 2], pts[(k % n) * 2 + 1], c, 0, 0);
}

void card(float x, float y, float w, float h) { round_rect(x, y, w, h, 16, PANEL); }

void bar(float x, float y, float w, float h, float frac, u32 c)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    round_rect(x, y, w, h, h / 2, LINE);
    if (frac > 0) round_rect(x, y, w * frac < h ? h : w * frac, h, h / 2, c);
}

float heading(float x, float y, const char *s)
{
    text(x, y, F_SMALL, BLUE, "%s", s);
    return y + line_h(F_SMALL) + 8;
}

/* A face button: a dark cap with the symbol in its colour, as on the controller. */
void button_icon(float cx, float cy, float size, unsigned btn)
{
    float r = size / 2, s = r * 0.5f, t = size * 0.11f;
    if (t < 2) t = 2;
    disc(cx, cy, r, 0x0b1424ff);
    ring(cx, cy, r - 1, 2, 0x3a4d72ff);
    switch (btn) {
    case BTN_CROSS:
        line(cx - s, cy - s, cx + s, cy + s, t, C_CROSS);
        line(cx - s, cy + s, cx + s, cy - s, t, C_CROSS);
        break;
    case BTN_CIRCLE:
        ring(cx, cy, s * 1.05f, t, C_CIRCLE);
        break;
    case BTN_SQUARE:
        frame(cx - s * 0.95f, cy - s * 0.95f, s * 1.9f, s * 1.9f, t, C_SQUARE);
        break;
    case BTN_TRIANGLE: {
        float R = s * 1.2f, ty = cy + s * 0.15f;
        float ax = cx, ay = ty - R, bx = cx + R * 0.866f, by = ty + R * 0.5f, qx = cx - R * 0.866f;
        line(ax, ay, bx, by, t, C_TRIANGLE);
        line(bx, by, qx, by, t, C_TRIANGLE);
        line(qx, by, ax, ay, t, C_TRIANGLE);
        break;
    }
    }
}

/* Twelve dots, the brightest one turning: it moves only while the console works. */
void spinner(float cx, float cy, float r, double sec)
{
    int head = (int)(sec * 12) % 12;
    for (int i = 0; i < 12; i++) {
        int age = (head - i + 12) % 12;
        u32 a = age > 10 ? 40 : 255 - age * 20;
        float ang = i * PI_F / 6;
        disc(cx + cosf(ang) * r, cy + sinf(ang) * r, r * 0.17f, (WHITE & 0xffffff00) | a);
    }
}

void background(void) { rect_v(-org_x, -org_y, 1920, 1080, BG_TOP, BG_BOT); }

static void card_head(float x, float y, float w, unsigned btn, int on, const char *title_s, const char *desc, int lines)
{
    button_icon(x + 52, y + 54, 52, btn);
    text_fit(x + 100, y + 34, F_MED, on ? WHITE : DIM, w - 128, "%s", title_s);
    text_wrap(x + 32, y + 110, F_BODY, on ? GREY : DIM, w - 64, lines, desc);
}

void option_card(float x, float y, float w, float h, unsigned btn, int on, const char *title_s, const char *desc)
{
    card(x, y, w, h);
    card_head(x, y, w, btn, on, title_s, desc, (int)((h - 130) / (line_h(F_BODY) + line_h(F_BODY) / 6)));
}

void test_card(float x, float y, float w, float h, unsigned btn, const char *title_s, const char *desc)
{
    card(x, y, w, h);
    card_head(x, y, w, btn, 1, title_s, desc, 4);
    rect(x + 32, y + TEST_RESULT_Y - 24, w - 64, 2, LINE);
}

/* ---- a screen ------------------------------------------------------------- */

/* No clear: the opaque background covers the whole output in every frame. */
void begin_frame(void)
{
    gfx_begin();
    background();
}

/* Quit Game from the PS button menu reaches sys_callback here, in every frame,
 * also while a job runs: the job ends before the app does. gfx_end shows the
 * frame and returns when the RSX has drawn it. */
void ui_flip(void)
{
    sysUtilCheckCallback();
    gfx_end();
}

#define TITLE_Y 24

void title(void)
{
    float x = text(MG, TITLE_Y, F_TITLE, WHITE, "PS3 Health");
    x = text(x + 14, TITLE_Y + base_dy(F_TITLE, F_SMALL), F_SMALL, DIM, "v" APP_VERSION);
    if (ui_module) {
        disc(x + 22, TITLE_Y + fonts[F_TITLE].asc - 10, 4, DIM);
        x = text(x + 40, TITLE_Y + base_dy(F_TITLE, F_MED), F_MED, BLUE, "%s", ui_module);
    }
    if (ui_demo) {
        float y = TITLE_Y + base_dy(F_TITLE, F_SMALL) - 3;
        round_rect(x + 18, y, text_w(F_SMALL, "DEMO") + 24, line_h(F_SMALL) + 6, 8, 0x4a3c0cff);
        text(x + 30, y + 3, F_SMALL, YELLOW, "DEMO");
    }
    rect(MG, 98, SW - 2 * MG, 2, LINE);
}

void title_right(const char *fmt, ...)
{
    FORMAT(s, fmt);
    text_r(SW - MG, TITLE_Y + base_dy(F_TITLE, F_BODY), F_BODY, GREY, "%s", s);
}

void tabs(const char *const *names, int n, int sel)
{
    const int gap = 40;
    int total = 0;
    for (int i = 0; i < n; i++) total += text_w(F_BODY, names[i]) + (i ? gap : 0);
    float x = SW - MG - total, y = TITLE_Y + base_dy(F_TITLE, F_BODY);
    for (int i = 0; i < n; i++) {
        int w = text_w(F_BODY, names[i]);
        text(x, y, F_BODY, i == sel ? WHITE : DIM, "%s", names[i]);
        if (i == sel) round_rect(x, 91, w, 5, 2, BLUE);
        x += w + gap;
    }
}

static const struct { const char *name; unsigned btn; } button_names[] = {
    {"CROSS", BTN_CROSS}, {"CIRCLE", BTN_CIRCLE}, {"TRIANGLE", BTN_TRIANGLE}, {"SQUARE", BTN_SQUARE},
    {"START", 0}, {"SELECT", 0}, {"R2", 0}, {"UP/DOWN", 0}, {"LEFT/RIGHT", 0}, {"D-pad", 0},
};

/* The button line: each item starts with a button name, drawn as the face
 * button or as a label, and its action follows in grey. */
void footer(const char *s) { footer_at(MG, Y_FOOTER, s); }

void footer_at(float x, float y, const char *s)
{
    const float ty = y + 4;                  /* the small font, centred on the 36 px row */
    while (*s && x < SW - MG) {
        const char *e = strstr(s, "  ");
        int n = e ? (int)(e - s) : (int)strlen(s);
        char item[96];
        snprintf(item, sizeof item, "%.*s", n, s);
        const char *rest = item;
        char *sp = strchr(item, ' ');
        int tl = sp ? (int)(sp - item) : (int)strlen(item);
        for (unsigned k = 0; k < sizeof button_names / sizeof button_names[0]; k++) {
            if ((int)strlen(button_names[k].name) != tl || strncmp(item, button_names[k].name, tl)) continue;
            if (button_names[k].btn) {
                button_icon(x + 17, y + 18, 34, button_names[k].btn);
                x += 44;
            } else {
                int w = width_n(F_SMALL, item, tl) + 20;
                round_rect(x, y + 2, w, 32, 8, PANEL2);
                run(x + 10, ty, F_SMALL, WHITE, item, tl);
                x += w + 10;
            }
            rest = sp ? sp + 1 : item + tl;
            break;
        }
        x = text(x, ty, F_SMALL, GREY, "%s", rest) + 36;
        s += n;
        while (*s == ' ') s++;
    }
}

void status_line(const char *msg, u32 color) { text_fit(MG, Y_STATUS, F_BODY, color, SW - 2 * MG, "%s", msg); }

/* ---- the visible area ----------------------------------------------------- */

static void viewport(int l, int t, int r, int b)
{
    float sx = gfx_w / 1920.0f, sy = gfx_h / 1080.0f;
    org_x = l;
    org_y = t;
    SW = r - l;
    SH = b - t;
    gfx_viewport(l * sx, t * sy, sx, sy);
}

void safe_apply(void) { viewport(safe_l, safe_t, safe_r, safe_b); }

void safe_override(int full)
{
    if (full) viewport(0, 0, 1920, 1080);
    else safe_apply();
}

int safe_valid(int l, int t, int r, int b)
{
    return l >= 0 && t >= 0 && r <= 1920 && b <= 1080 && r - l >= SAFE_MIN_W && b - t >= SAFE_MIN_H;
}

int safe_load(void)
{
    char t[64];
    int l, tp, r, b;
    int n = fs_read_file(APP_DIR "/safe_area.txt", t, sizeof t - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    if (sscanf(t, "%d %d %d %d", &l, &tp, &r, &b) != 4 || !safe_valid(l, tp, r, b))
        return 0;
    safe_l = l;
    safe_t = tp;
    safe_r = r;
    safe_b = b;
    safe_apply();
    return 1;
}

void safe_set(int l, int t, int r, int b)
{
    char s[64];
    safe_l = l;
    safe_t = t;
    safe_r = r;
    safe_b = b;
    snprintf(s, sizeof s, "%d %d %d %d\n", l, t, r, b);
    fs_write_file(APP_DIR "/safe_area.txt", s, strlen(s), 0);
    safe_apply();
}

void ui_init(void)
{
    ioPadInit(7);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
    int step = gfx_init();
    if (step) {                              /* no picture: the failed step goes to a file, for FTP */
        char s[40];
        sysLv2FsMkdir(APP_DIR, 0777);
        fs_write_file(APP_DIR "/gfx_init.txt", s, snprintf(s, sizeof s, "gfx_init step %d\n", step), 0);
        exit(0);
    }
    /* 1:1 at 1080p: nearest sampling keeps every glyph pixel exact; any other
     * output scales the canvas, and linear sampling smooths it */
    text_linear = !(gfx_w == 1920 && gfx_h == 1080);
    if (fonts_init()) exit(0);
    safe_apply();
}

void ui_end(void)
{
    gfx_exit();                              /* the RSX idle, with nothing left in the ring */
    ioPadEnd();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
}

/* ---- pad ------------------------------------------------------------------ */

void read_pad(void)
{
    padInfo info;
    padData pd;
    unsigned now = held;
    sysUtilCheckCallback();
    ioPadGetInfo(&info);
    for (int n = 0; n < MAX_PADS; n++)
        if (info.status[n]) {
            ioPadGetData(n, &pd);
            if (pd.len > 0) {
                now = pd.button[2] << 8 | (pd.button[3] & 0xff);
                pad_last = pd;
                pad_port = n;
            }
            break;
        }
    pressed = now & ~held;
    held = now;
}

int ui_leave(void)
{
    if (quit || (pressed & BTN_START)) return -1;
    return (pressed & BTN_CIRCLE) ? 0 : 1;
}

/* Two-press gestures: the first press arms, the same button fires, any other
 * button disarms. Returns the button that fired this frame, 0 otherwise. */
unsigned gesture(unsigned buttons)
{
    if (!pressed) return 0;
    if (pressed == armed) { armed = 0; return pressed; }
    armed = (pressed & buttons) && !(pressed & ~buttons) ? pressed : 0;
    return 0;
}

/* ---- blocking calls ------------------------------------------------------- */

volatile int job_cancel;
static volatile int job_done, job_pct = -1;
static const char *volatile job_msg;        /* string literals only: no lifetime to manage */

void progress(const char *msg) { job_msg = msg; }
void job_set_percent(int pct) { job_pct = pct; }

static void job_thread(void *arg)
{
    ((void (*)(void))arg)();
    __sync_synchronize();
    job_done = 1;
    sysThreadExit(0);
}

static void draw_job(double sec, int cancelable)
{
    begin_frame();
    title();
    float y = TOP + 30;
    text_fit(MG, y, F_MED, WHITE, SW - 2 * MG, "%s ...", job_msg);
    y += 96;
    spinner(MG + 34, y + 32, 28, sec);
    text(MG + 92, y, F_LARGE | TNUM, WHITE, "%.1f s", sec);
    y += 116;
    if (job_pct >= 0) {
        int p = job_pct > 100 ? 100 : job_pct;
        bar(MG, y, SW - 2 * MG, 16, p / 100.0f, GREEN);
        text(MG, y + 30, F_BODY | TNUM, GREY, "%d%%", p);
        y += 96;
    }
    text_wrap(MG, y, F_BODY, GREY, SW - 2 * MG, 2, "The counter runs while the console works. If it stops, the console froze.");
    if (cancelable) footer(job_cancel ? "Stopping ..." : "CIRCLE stop");
    else text_wrap(MG, y + 44, F_BODY, GREY, SW - 2 * MG, 2,
                   "Hold the power button, then start the app again: it skips the frozen step.");
    ui_flip();
}

static void job_run(const char *msg, void (*fn)(void), int cancelable)
{
    sys_ppu_thread_t t;
    u64 ret;
    job_msg = msg;
    job_done = 0;
    job_cancel = 0;
    job_pct = -1;
    s64 t0 = sysGetSystemTime();
    if (sysThreadCreate(&t, job_thread, (void *)fn, 1500, 0x20000, THREAD_JOINABLE, "job") != 0) {
        draw_job(0, 0);                      /* no thread: one frame, then the call in this thread */
        fn();
        job_pct = -1;
        return;
    }
    while (!job_done) {
        if (cancelable) {
            read_pad();
            if (quit || (pressed & BTN_CIRCLE)) job_cancel = 1;
        }
        draw_job((double)(sysGetSystemTime() - t0) / 1e6, cancelable);
    }
    sysThreadJoin(t, &ret);
    job_pct = -1;
    pressed = 0;                             /* a button still down is not a new press */
}

void run_job(const char *msg, void (*fn)(void)) { job_run(msg, fn, 0); }
void run_job_cancelable(const char *msg, void (*fn)(void)) { job_run(msg, fn, 1); }

/* ---- first-run prompt -------------------------------------------------------
 * A command this console never ran is shown once: CROSS runs it (journaled
 * like the others), CIRCLE skips it and the journal remembers the skip until
 * SQUARE twice. */
int first_run_prompt(const char *title_s, const char *l1, const char *l2, const char *l3)
{
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CROSS) return 1;
        if (pressed & BTN_CIRCLE) return 0;
        begin_frame();
        title();
        float x = MG + 40, w = SW - 2 * MG - 80, y = TOP + 10;
        card(MG, y, SW - 2 * MG, 500);
        round_rect(MG, y, 8, 500, 4, YELLOW);
        text_fit(x, y + 34, F_MED, YELLOW, w, "New on this console: %s", title_s);
        text_fit(x, y + 112, F_BODY, WHITE, w, "%s", l1);
        text_fit(x, y + 156, F_BODY, WHITE, w, "%s", l2);
        text_fit(x, y + 200, F_BODY, GREY, w, "%s", l3);
        text_wrap(x, y + 290, F_BODY, GREY, w, 4,
                  "The call is written to the journal first. If it freezes the console, hold the power button and "
                  "start the app again: it skips the call. A skip is kept until SQUARE twice on the home screen.");
        footer("CROSS run it  CIRCLE skip it  START exit");
        ui_flip();
    }
}
