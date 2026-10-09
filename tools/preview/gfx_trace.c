/* PV_GFXTRACE=file: every gfx call of the app, one line per call, for the
 * end-to-end check of the trace harness (the format is in README.md).
 * build.zsh compiles source/gfx/gfx.c with its API renamed to gfx_real_*,
 * and these wrappers write the trace and call it. The vertices of a gfx_prim
 * go out with its line at the app's next gfx call: then they are written.
 * gfx_vram (gfx_soft.c) writes its own line and may come from another thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include "gfx.h"
#include "pv.h"

int gfx_real_init(void);
void gfx_real_viewport(float tx, float ty, float sx, float sy);
void gfx_real_begin(void);
void gfx_real_texture(const gfx_tex *t);
gfx_vtx *gfx_real_prim(int type, int n);
void gfx_real_end(void);

static FILE *tf;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char *cur, *prev;                     /* this frame's lines, the last frame written */
static size_t cur_n, cur_cap, prev_n, prev_cap;
static long rep, frames, max_frames = -1;
static const gfx_vtx *pend;                  /* the last gfx_prim, not traced yet */
static int pend_type, pend_n;

static void put(const char *s, size_t n)
{
    if (cur_n + n > cur_cap) {
        cur_cap = (cur_n + n) * 2 + 4096;
        cur = realloc(cur, cur_cap);
        if (!cur) { fprintf(stderr, "gfxtrace: out of memory\n"); exit(4); }
    }
    memcpy(cur + cur_n, s, n);
    cur_n += n;
}

static void vline(const char *fmt, va_list ap)       /* the mutex is held */
{
    char line[256];
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    if (n < 0 || n > (int)sizeof line - 2) { fprintf(stderr, "gfxtrace: line too long\n"); exit(4); }
    line[n++] = '\n';
    put(line, (size_t)n);
}

static void line(const char *fmt, ...)               /* the mutex is held */
{
    va_list ap;
    va_start(ap, fmt);
    vline(fmt, ap);
    va_end(ap);
}

void pv_trace_line(const char *fmt, ...)
{
    if (!tf) return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mu);
    if (tf) vline(fmt, ap);
    pthread_mutex_unlock(&mu);
    va_end(ap);
}

static void flush_prim(void)                         /* the mutex is held */
{
    if (!pend_n) return;
    line("gfx_prim %d %d", pend_type, pend_n);
    for (int i = 0; pend && i < pend_n; i++)
        line("v %a %a 0x%08x %a %a", pend[i].x, pend[i].y, pend[i].rgba, pend[i].u, pend[i].v);
    pend_n = 0;
}

static void close_trace(void)                        /* the mutex is held */
{
    if (!tf) return;
    flush_prim();
    if (rep) fprintf(tf, "repeat %ld\n", rep);
    fwrite(cur, 1, cur_n, tf);
    fprintf(tf, "end %ld\n", frames);
    fclose(tf);
    tf = NULL;
}

static void at_exit(void)
{
    pthread_mutex_lock(&mu);
    close_trace();
    pthread_mutex_unlock(&mu);
}

/* A frame equal to the one before is written as "repeat N" (N such frames). */
static void frame_end(void)                          /* the mutex is held */
{
    if (prev && prev_n == cur_n && !memcmp(prev, cur, cur_n)) rep++;
    else {
        if (rep) fprintf(tf, "repeat %ld\n", rep);
        rep = 0;
        fwrite(cur, 1, cur_n, tf);
        char *b = prev;
        size_t c = prev_cap;
        prev = cur, prev_n = cur_n, prev_cap = cur_cap;
        cur = b, cur_cap = c;
    }
    cur_n = 0;
    if (++frames == max_frames) close_trace();
}

/* One API call: the pending primitive first, then this call's line. */
static void call(const char *fmt, ...)
{
    if (!tf) return;
    pthread_mutex_lock(&mu);
    if (tf) {
        flush_prim();
        va_list ap;
        va_start(ap, fmt);
        vline(fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&mu);
}

int gfx_init(void)
{
    const char *p = getenv("PV_GFXTRACE");
    if (p) {
        tf = fopen(p, "w");
        if (!tf) { fprintf(stderr, "gfxtrace: cannot write %s\n", p); exit(4); }
        if (getenv("PV_GFXTRACE_FRAMES")) max_frames = atol(getenv("PV_GFXTRACE_FRAMES"));
        atexit(at_exit);
    }
    int r = gfx_real_init();
    call("video 0x%x 0x%x", gfx_w, gfx_h);
    call("gfx_init = %d", r);
    return r;
}

void gfx_viewport(float tx, float ty, float sx, float sy)
{
    call("gfx_viewport %a %a %a %a", tx, ty, sx, sy);
    gfx_real_viewport(tx, ty, sx, sy);
}

void gfx_begin(void)
{
    call("gfx_begin");
    gfx_real_begin();
}

void gfx_texture(const gfx_tex *t)
{
    if (t) call("gfx_texture 0x%x 0x%x 0x%x 0x%x %d", t->offset, t->w, t->h, t->pitch, t->linear);
    else call("gfx_texture NULL");
    gfx_real_texture(t);
}

/* The last primitive goes out first: a drain in gfx_real_prim reuses its
 * vertices. */
gfx_vtx *gfx_prim(int type, int n)
{
    if (!tf) return gfx_real_prim(type, n);
    pthread_mutex_lock(&mu);
    if (tf) flush_prim();
    pthread_mutex_unlock(&mu);
    gfx_vtx *p = gfx_real_prim(type, n);
    pthread_mutex_lock(&mu);
    if (tf) pend = p, pend_type = type, pend_n = n;
    pthread_mutex_unlock(&mu);
    return p;
}

void gfx_end(void)
{
    if (tf) {
        pthread_mutex_lock(&mu);
        if (tf) {
            flush_prim();
            line("gfx_end");
            frame_end();
        }
        pthread_mutex_unlock(&mu);
    }
    gfx_real_end();                          /* the frame ends in pv_flip, which may exit */
}
