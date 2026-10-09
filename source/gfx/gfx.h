/* gfx: the app's 2D renderer (README.md in this folder). A frame is
 * gfx_begin, then primitives, then gfx_end, which shows the frame and returns
 * when the RSX has drawn it. Positions are canvas units that gfx_viewport
 * maps to output pixels. gfx.c is the portable front end (batching, the
 * viewport, the vertex area); a back end draws: gfx_rsx.c on the console,
 * the preview's gfx_soft.c on the host. Plain C types only, so the preview
 * compiles this header too. */
#ifndef GFX_H
#define GFX_H

#include <ppu-types.h>

/* the primitive types, with the RSX's BEGIN_END values */
#define GFX_TRIANGLES      5
#define GFX_TRIANGLE_STRIP 6
#define GFX_TRIANGLE_FAN   7
#define GFX_QUADS          8
#define GFX_PRIM_MAX       256               /* the most vertices of one gfx_prim */

/* A vertex as the RSX reads it (20 bytes): the position (the RSX adds z = 0
 * and w = 1), the colour 0xRRGGBBAA, the texture coordinate. Write it with
 * gfx_put. */
typedef struct {
    float x, y;
    u32 rgba;
    float u, v;
} gfx_vtx;

/* An A4R4G4B4 texture in RSX memory, rows of pitch bytes, clamped to the
 * edge; linear: bilinear filtering, else nearest. gfx keeps the pointer that
 * gfx_texture gets until the frame ends, and compares textures by it. */
typedef struct {
    u32 offset;
    u16 w, h, pitch, linear;
} gfx_tex;

extern int gfx_w, gfx_h;                     /* the output in pixels, set by gfx_init */

int gfx_init(void);                          /* 0, or the number of the step that failed (gfx_be_init) */
/* RSX memory that is never given back, and its RSX offset when offset is not
 * NULL; NULL when it does not fit. After gfx_init only one thread at a time
 * may call it (the app: the RSX memory test). */
void *gfx_vram(u32 size, u32 align, u32 *offset);
void gfx_viewport(float tx, float ty, float sx, float sy);   /* pixel = canvas * s + t, at once */
void gfx_begin(void);
void gfx_texture(const gfx_tex *t);          /* for the next primitives; NULL: colour only */
/* Room for one primitive of n vertices (1 to GFX_PRIM_MAX); the caller
 * writes all n before its next gfx call. Outside a frame they are dropped. */
gfx_vtx *gfx_prim(int type, int n);
void gfx_end(void);
void gfx_exit(void);                         /* at exit: the RSX runs what is left (at most about 2 s) */

/* One vertex, and a pointer to the next. Each field is its own 4-byte store:
 * GCC 7.2 can merge plain stores into 8-byte ones, which RSX memory may not
 * take if the vertices ever move there. */
static inline gfx_vtx *gfx_put(gfx_vtx *p, float x, float y, u32 rgba, float u, float v)
{
    volatile gfx_vtx *q = p;
    q->x = x;
    q->y = y;
    q->rgba = rgba;
    q->u = u;
    q->v = v;
    return p + 1;
}

/* The back end, for gfx.c only. vp is tx, ty, sx, sy. A draw is n vertices
 * of the area from index first, t NULL for colour only. drain returns when
 * the RSX has read every vertex drawn so far. */
int gfx_be_init(u32 vtx_bytes, gfx_vtx **area);
void gfx_be_begin(const float *vp);
void gfx_be_viewport(const float *vp);
void gfx_be_draw(int type, u32 first, u32 n, const gfx_tex *t);
void gfx_be_drain(void);
void gfx_be_end(void);
void gfx_be_exit(void);

#endif
