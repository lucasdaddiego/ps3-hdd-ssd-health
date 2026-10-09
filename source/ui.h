/* The frame every module draws in. The app draws in a 1920x1080 space, 1:1 at
 * 1080p output, into the part of the screen the TV shows (the canvas, SW x SH:
 * the visible area the Display module measures). Text comes from Inter bitmap
 * atlases at their native size (data/fonts.bin, art/make_font.py), one quad
 * per glyph. Also here: a small drawing kit, the pad, two-press
 * gestures, a blocking call in a second thread with a counter on screen, and
 * the first-run prompt. */
#ifndef UI_H
#define UI_H

#include <ppu-types.h>
#include <io/pad.h>

#define BTN_LEFT     0x8000
#define BTN_DOWN     0x4000
#define BTN_RIGHT    0x2000
#define BTN_UP       0x1000
#define BTN_START    0x0800
#define BTN_R3       0x0400
#define BTN_L3       0x0200
#define BTN_SELECT   0x0100
#define BTN_SQUARE   0x0080
#define BTN_CROSS    0x0040
#define BTN_CIRCLE   0x0020
#define BTN_TRIANGLE 0x0010
#define BTN_R1       0x0008
#define BTN_L1       0x0004
#define BTN_R2       0x0002
#define BTN_L2       0x0001

/* colours, RGBA */
#define WHITE   0xf2f5faff
#define GREY    0x9ba7baff
#define DIM     0x67738bff
#define GREEN   0x4fd982ff
#define YELLOW  0xffcc40ff
#define RED     0xff6464ff
#define BLUE    0x6cb8ffff
#define BLACK   0x000000ff
#define PANEL   0x152238ff           /* cards and tables */
#define PANEL2  0x1f3256ff           /* the selected card, table stripes, button labels */
#define LINE    0x2b3d60ff           /* separators and empty bars */
#define BG_TOP  0x0a1222ff           /* the background gradient */
#define BG_BOT  0x13264cff
/* the face-button colours of the controller */
#define C_TRIANGLE 0x40d8a8ff
#define C_CIRCLE   0xff6b6bff
#define C_CROSS    0x80a8ffff
#define C_SQUARE   0xea90dcff

/* the fonts, in the order of art/make_font.py's STYLES; | TNUM: tabular digits
 * (one width), for tables and counters, like OpenType's tnum */
enum { F_SMALL, F_BODY, F_MED, F_LARGE, F_TITLE, F_COUNT };
#define TNUM 0x100

/* the canvas: everything is drawn in 0..SW x 0..SH */
extern int SW, SH;
#define MG       40                  /* the margin inside the canvas */
#define TOP      126                 /* content starts under the title bar */
#define BOTTOM   (SH - 150)          /* content ends above the status line */
#define Y_STATUS (SH - 130)
#define Y_FOOTER (SH - 86)
#define Y_NOTE   (SH - 40)

extern volatile int quit;            /* Quit Game from the PS button menu */
extern unsigned held, pressed;       /* the pad after read_pad(): buttons down, buttons that went down this frame */
extern padData pad_last;             /* the last full pad packet: sticks, pressure, sensors */
extern int pad_port;                 /* the port it came from, -1 before the first packet */
extern unsigned armed;               /* the first press of a two-press gesture */
extern const char *ui_module;        /* the module name in the title bar, NULL on the home screen */
extern int ui_demo;                  /* DEMO in the title bar */

/* The app as the frame needs it; ui_init keeps the three pointers. */
typedef struct {
    const char *name;        /* the title bar */
    const char *version;     /* drawn as v<version> */
    const char *dir;         /* the app folder: safe_area.txt, gfx_init.txt */
} ui_app;
void ui_init(const ui_app *a);
void ui_end(void);

/* text: y is the top of the line box; each call returns the x after the text */
float text(float x, float y, int font, u32 color, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
float text_r(float xr, float y, int font, u32 color, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
float text_c(float xc, float y, int font, u32 color, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
float text_fit(float x, float y, int font, u32 color, float maxw, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
int text_wrap(float x, float y, int font, u32 color, float maxw, int maxlines, const char *s);   /* the lines drawn */
int text_w(int font, const char *s);
int line_h(int font);
int base_dy(int big, int small);     /* the y offset that puts a smaller font on a bigger one's baseline */

/* drawing kit */
void rect(float x, float y, float w, float h, u32 c);
void rect_v(float x, float y, float w, float h, u32 top, u32 bottom);     /* vertical gradient */
void round_rect(float x, float y, float w, float h, float r, u32 c);
void round_frame(float x, float y, float w, float h, float r, float t, u32 c);
void frame(float x, float y, float w, float h, float t, u32 c);
void line(float x0, float y0, float x1, float y1, float w, u32 c);
/* Many quads of one colour: batch_begin, then batch_rect or batch_line for
 * each, then batch_end. */
void batch_begin(u32 c);
void batch_rect(float x, float y, float w, float h);
void batch_line(float x0, float y0, float x1, float y1, float w);
void batch_end(void);
void ring(float cx, float cy, float r, float w, u32 c);
void disc(float cx, float cy, float r, u32 c);
void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);
void poly_fan(float cx, float cy, const float *pts, int n, u32 c);      /* a filled shape around a centre */
void card(float x, float y, float w, float h);
void bar(float x, float y, float w, float h, float frac, u32 c);        /* a track and its filled part */
float heading(float x, float y, const char *s);                         /* a section label; the y under it */
void button_icon(float cx, float cy, float size, unsigned btn);         /* a face button as on the controller */
void spinner(float cx, float cy, float r, double sec);
void background(void);
/* A choice: a card with its button, a title and a wrapped description. Grey when off. */
void option_card(float x, float y, float w, float h, unsigned btn, int on, const char *title_s, const char *desc);
/* A test: the same head with at most four lines of description, then a rule;
 * the caller draws the result from y + TEST_RESULT_Y. */
#define TEST_RESULT_Y 330
void test_card(float x, float y, float w, float h, unsigned btn, const char *title_s, const char *desc);

/* a screen */
void begin_frame(void);
void title(void);
void title_right(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tabs(const char *const *names, int n, int sel);                    /* pages, right of the title */
void footer(const char *s);          /* "CROSS read  CIRCLE home": items apart by two spaces, buttons drawn */
void footer_at(float x, float y, const char *s);
void status_line(const char *msg, u32 color);
void ui_flip(void);

void read_pad(void);
unsigned gesture(unsigned buttons);  /* returns the button that fired, 0 otherwise */
int ui_leave(void);                  /* after read_pad: -1 exit (START or Quit Game), 0 home (CIRCLE), 1 stay */

/* A blocking call in a second thread while this one draws a counter: a stopped
 * counter is the freeze signal. progress() changes the text, job_set_percent()
 * adds a bar. With cancel on, CIRCLE sets job_cancel for the call to see. */
extern volatile int job_cancel;
void progress(const char *msg);
void job_set_percent(int pct);       /* -1 = no bar */
void run_job(const char *msg, void (*fn)(void));
void run_job_cancelable(const char *msg, void (*fn)(void));

/* Returns 1 to run, 0 to skip, -1 on exit. */
int first_run_prompt(const char *title_s, const char *l1, const char *l2, const char *l3);

/* The visible area in 1920x1080 units, kept in safe_area.txt in the app
 * folder (ui_app.dir). The canvas is drawn 1:1 inside it. 93 % of the screen
 * until it is measured. */
extern int safe_l, safe_t, safe_r, safe_b;
#define SAFE_DEF_L 67                 /* the default: 93 % of the screen */
#define SAFE_DEF_T 38
#define SAFE_DEF_R 1853
#define SAFE_DEF_B 1042
#define SAFE_MIN_W 1600
#define SAFE_MIN_H 900
int safe_valid(int l, int t, int r, int b);   /* inside the 1920x1080 screen and at least SAFE_MIN_W x SAFE_MIN_H */
int safe_load(void);                 /* 1 when a measured area was read */
void safe_set(int l, int t, int r, int b);   /* writes the file and applies it */
void safe_apply(void);
void safe_override(int full);        /* 1: the canvas is the whole screen (the measure and the patterns) */

#endif
