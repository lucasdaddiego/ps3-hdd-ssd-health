/* The portable front end of gfx: the vertex area, the batching and the
 * viewport. The console (gfx_rsx.c) and the host preview build this file. */
#include <stddef.h>
#include "gfx.h"

#ifndef GFX_VTX_BYTES
#define GFX_VTX_BYTES (960 * 1024)           /* the vertex area; the preview tests smaller ones */
#endif

int gfx_w = 1920, gfx_h = 1080;

static gfx_vtx *area;                        /* the vertices of the frame, from index 0 */
static u32 cap, used;                        /* the area in vertices, and the part in use */
static float vp[4] = {0, 0, 1, 1};           /* the viewport: tx, ty, sx, sy */
static const gfx_tex *tex;                   /* the texture of the next primitives */
static int in_frame;

/* The draw that the next primitives can still join. */
static struct {
    int type;
    const gfx_tex *tex;
    u32 first, n;
} open;

int gfx_init(void)
{
    cap = GFX_VTX_BYTES / sizeof(gfx_vtx);
    if (cap < GFX_PRIM_MAX) return -1;       /* a build error: no room for one primitive */
    return gfx_be_init(GFX_VTX_BYTES, &area);
}

/* Sends the open draw to the back end. */
static void close_draw(void)
{
    if (open.n) gfx_be_draw(open.type, open.first, open.n, open.tex);
    open.n = 0;
}

void gfx_viewport(float tx, float ty, float sx, float sy)
{
    close_draw();                            /* the vertices so far keep the old viewport */
    vp[0] = tx;
    vp[1] = ty;
    vp[2] = sx;
    vp[3] = sy;
    if (in_frame) gfx_be_viewport(vp);
}

/* The last frame's gfx_end waited for the RSX, so the vertices start again at
 * index 0. */
void gfx_begin(void)
{
    if (in_frame) return;
    in_frame = 1;
    used = 0;
    gfx_be_begin(vp);
}

void gfx_texture(const gfx_tex *t) { tex = t; }

/* Consecutive QUADS or TRIANGLES with the same texture join one draw. A
 * strip or a fan is a draw of its own: one more vertex would continue it. */
gfx_vtx *gfx_prim(int type, int n)
{
    if (n < 1 || n > GFX_PRIM_MAX) return NULL;      /* a caller error: a crash, not a write past the area */
    if (!in_frame) return area;
    if (used + n > cap) {                    /* the area is full: wait until the RSX read it, then reuse it */
        close_draw();
        gfx_be_drain();
        used = 0;
    }
    if (!open.n || open.type != type || open.tex != tex || (type != GFX_QUADS && type != GFX_TRIANGLES)) {
        close_draw();
        open.type = type;
        open.tex = tex;
        open.first = used;
    }
    open.n += n;
    used += n;
    return area + used - n;
}

void gfx_end(void)
{
    if (!in_frame) return;
    close_draw();
    in_frame = 0;
    gfx_be_end();
}

/* At exit, as PSL1GHT's samples do: the RSX runs the words after the last
 * flush, and gfx waits until it has. */
void gfx_exit(void)
{
    gfx_end();
    gfx_be_exit();
}
