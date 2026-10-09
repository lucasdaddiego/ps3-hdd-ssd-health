/* The preview's back end of gfx (source/gfx/gfx.h): on the host, what
 * gfx_rsx.c does on the console.
 *   - RSX local memory is an arena of the console's size. The blocks of
 *     gfx_rsx.c's init come first, in its order and alignment, so the RSX
 *     memory test counts what the console counts.
 *   - The draws wait in a list until a drain or the end of the frame, as the
 *     RSX reads the vertices only after gfx moves PUT. raster.c then draws
 *     them, and the vertices they used become NaN: a draw that reads a vertex
 *     from before the last drain draws nothing, and the snapshot shows it.
 * PV_STAT=1 prints the vertices, draws and drains per frame at exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "raster.h"
#include "pv.h"
#include "sysutil/video.h"

#define LOCAL_SIZE 0x0F900000u               /* gcmGetConfiguration's localSize on the console: 249 MB */

_Static_assert(GFX_TRIANGLES == RASTER_TRIANGLES && GFX_TRIANGLE_STRIP == RASTER_TRIANGLE_STRIP &&
               GFX_TRIANGLE_FAN == RASTER_TRIANGLE_FAN && GFX_QUADS == RASTER_QUADS, "the primitive types pass through");

static u8 *arena;
static u32 arena_next;
static gfx_vtx *area;
static raster_vtx *rv;                       /* one draw in output pixels */
static float vp[4];                          /* the viewport: tx, ty, sx, sy */
static struct draw {
    int type;
    u32 first, n;
    const gfx_tex *t;
    float vp[4];
} *list;
static int nlist, list_cap;
static long f_vtx, f_draws, f_drains, frames, max_vtx, max_at, max_draws, drains, frames_drained;

static void *alloc(u32 size, u32 align, u32 *offset)
{
    u64 p = ((u64)arena_next + align - 1) & ~((u64)align - 1);
    if (!align || (align & (align - 1)) || p > LOCAL_SIZE || size > LOCAL_SIZE - p) return NULL;
    arena_next = (u32)(p + size);
    if (offset) *offset = (u32)p;
    return arena + p;
}

void *gfx_vram(u32 size, u32 align, u32 *offset)
{
    u32 off = 0;
    void *p = alloc(size, align, &off);
    if (p) pv_trace_line("gfx_vram 0x%x 0x%x = 0x%x", size, align, off);
    else pv_trace_line("gfx_vram 0x%x 0x%x = NULL", size, align);
    if (p && offset) *offset = off;
    return p;
}

static void stat_exit(void)
{
    if (getenv("PV_STAT"))
        fprintf(stderr, "stat: %ld frames; at most %ld vertices (%ld bytes, frame %ld) and %ld draws in a frame; %ld drains in %ld frames\n",
                frames, max_vtx, max_vtx * (long)sizeof(gfx_vtx), max_at, max_draws, drains, frames_drained);
}

int gfx_be_init(u32 vtx_bytes, gfx_vtx **out)
{
    videoState st;
    videoResolution r;
    pv_start();
    if (videoGetState(0, 0, &st) || videoGetResolution(st.displayMode.resolution, &r)) return -1;
    gfx_w = r.width;
    gfx_h = r.height;
    u32 pitch = 4 * ((gfx_w + 15) / 16 * 16);
    arena = calloc(1, LOCAL_SIZE);
    area = malloc(vtx_bytes);
    rv = malloc(vtx_bytes / sizeof(gfx_vtx) * sizeof *rv);
    if (!arena || !area || !rv) return -1;
    /* gfx_rsx.c's blocks: the two colour buffers, the two fragment programs */
    if (!alloc(pitch * gfx_h, 64, NULL) || !alloc(pitch * gfx_h, 64, NULL) || !alloc(256, 256, NULL) || !alloc(256, 256, NULL))
        return -1;
    raster_init(gfx_w, gfx_h);
    raster_clear(0xff0a1222);
    atexit(stat_exit);
    *out = area;
    return 0;
}

void gfx_be_begin(const float *v) { memcpy(vp, v, sizeof vp); }
void gfx_be_viewport(const float *v) { memcpy(vp, v, sizeof vp); }

void gfx_be_draw(int type, u32 first, u32 n, const gfx_tex *t)
{
    if (nlist == list_cap) {
        list_cap = list_cap * 2 + 64;
        list = realloc(list, list_cap * sizeof *list);
        if (!list) { fprintf(stderr, "gfx_soft: out of memory\n"); exit(4); }
    }
    struct draw *d = &list[nlist++];
    d->type = type;
    d->first = first;
    d->n = n;
    d->t = t;
    memcpy(d->vp, vp, sizeof vp);
    f_vtx += n;
    f_draws++;
}

/* The RSX reads the listed draws; their vertices are free afterwards. The
 * position is x * s + t, in float, as the RSX viewport computes it. */
static void fence(void)
{
    u32 end = 0;
    for (int i = 0; i < nlist; i++) {
        const struct draw *d = &list[i];
        if (d->first + d->n > end) end = d->first + d->n;
        if (!pv_drawing()) continue;
        for (u32 k = 0; k < d->n; k++) {
            const gfx_vtx *g = &area[d->first + k];
            rv[k].x = g->x * d->vp[2] + d->vp[0];
            rv[k].y = g->y * d->vp[3] + d->vp[1];
            rv[k].u = g->u;
            rv[k].v = g->v;
            rv[k].rgba = g->rgba;
        }
        raster_tex tx = {0};
        if (d->t) {
            tx.texels = arena + d->t->offset;
            tx.w = d->t->w;
            tx.h = d->t->h;
            tx.stride = d->t->pitch;
            tx.linear = d->t->linear;
        }
        raster_draw(d->type, rv, (int)d->n, d->t ? &tx : NULL);
    }
    memset(area, 0xff, end * sizeof *area);
    nlist = 0;
}

void gfx_be_drain(void)
{
    fence();
    f_drains++;
}

void gfx_be_end(void)
{
    fence();
    frames++;
    if (f_vtx > max_vtx) max_vtx = f_vtx, max_at = frames - 1;
    if (f_draws > max_draws) max_draws = f_draws;
    if (f_drains) frames_drained++;
    drains += f_drains;
    f_vtx = f_draws = f_drains = 0;
    pv_flip();
}

void gfx_be_exit(void) {}                            /* gfx_be_end left nothing to draw */
