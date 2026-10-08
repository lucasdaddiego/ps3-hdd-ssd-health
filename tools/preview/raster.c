/* The preview's software RSX. It draws the app's 2D primitives as the console
 * does, so that the pictures of two renderers compare pixel for pixel:
 *   - a primitive splits into triangles: QUADS into (0,1,2) and (0,2,3) per
 *     quad, a FAN into (0,i,i+1), a STRIP into (i,i+1,i+2), every second
 *     strip triangle turned to (i+1,i,i+2);
 *   - each triangle is drawn once, in order, with no culling. It covers a
 *     pixel whose centre is inside it, and a centre on an edge only for a top
 *     or a left edge (the top-left fill rule): two triangles that share an
 *     edge never cover a pixel twice;
 *   - the colour interpolates across the triangle, times the texel when a
 *     texture is bound (tiny3D's two fragment programs);
 *   - the alpha test GEQUAL 16/255, then source-alpha blending in 8-bit math
 *     into the 8-bit XRGB buffer: dst = (src * a + dst * (255 - a) + 127) / 255.
 * Model choices, not console facts: positions snap to 1/256 pixel, pixel
 * centres are at +0.5, the interpolation and the texture sampling are float,
 * the alpha test compares the alpha before its 8-bit conversion, and the
 * 8-bit conversions round to nearest. */
#include <math.h>
#include <stdlib.h>
#include "raster.h"

#define SUB 256                              /* snap steps per pixel */
#define LIM (1 << 20)                        /* a triangle with a vertex this far out is not drawn */

static u32 *fb;
static int fw, fh;

void raster_init(int w, int h)
{
    free(fb);
    fb = calloc((size_t)w * h, sizeof *fb);
    fw = w;
    fh = h;
}

void raster_clear(u32 argb)
{
    for (int i = 0; i < fw * fh; i++) fb[i] = argb & 0xffffff;
}

u32 raster_pixel(int x, int y) { return fb[y * fw + x]; }

/* ---- texture -------------------------------------------------------------- */

static void texel(const raster_tex *t, int x, int y, float *o)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)t->w) x = t->w - 1;
    if (y >= (int)t->h) y = t->h - 1;
    u16 c = *(const u16 *)(t->texels + y * t->stride + x * 2);
    o[0] = (c >> 8 & 15) / 15.0f; o[1] = (c >> 4 & 15) / 15.0f; o[2] = (c & 15) / 15.0f; o[3] = (c >> 12 & 15) / 15.0f;
}

static void sample(const raster_tex *t, float u, float v, float *o)
{
    if (!t->linear) { texel(t, (int)floorf(u * t->w), (int)floorf(v * t->h), o); return; }
    float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0, a[4], b[4], c[4], d[4];
    texel(t, x0, y0, a); texel(t, x0 + 1, y0, b); texel(t, x0, y0 + 1, c); texel(t, x0 + 1, y0 + 1, d);
    for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay;
}

/* ---- triangles ------------------------------------------------------------ */

static int snap(float f, s64 *o)
{
    if (!(f > -LIM && f < LIM)) return -1;   /* also NaN */
    *o = (s64)floor(f * (double)SUB + 0.5);
    return 0;
}

static s64 min3(s64 a, s64 b, s64 c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static s64 max3(s64 a, s64 b, s64 c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
static s64 ceil_div(s64 a, s64 b) { return a >= 0 ? (a + b - 1) / b : -(-a / b); }
static s64 floor_div(s64 a, s64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* 0 for a top or a left edge (dx, dy from its start to its end, the triangle
 * on the right on the screen), -1 for the others: a centre on them is out */
static s64 bias(s64 dx, s64 dy) { return dy < 0 || (dy == 0 && dx > 0) ? 0 : -1; }

static u32 to8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (u32)(f * 255 + 0.5f); }

static void rgba(u32 c, float *o)
{
    o[0] = (c >> 24) / 255.0f; o[1] = (c >> 16 & 255) / 255.0f; o[2] = (c >> 8 & 255) / 255.0f; o[3] = (c & 255) / 255.0f;
}

/* One pixel: the colour from the weights of a, b and c, the alpha test, the blend. */
static void shade(u32 *px, float w0, float w1, float w2, const raster_vtx *a, const raster_vtx *b, const raster_vtx *c,
                  const float *ca, const float *cb, const float *cc, const raster_tex *t)
{
    float col[4];
    for (int k = 0; k < 4; k++) col[k] = w0 * ca[k] + w1 * cb[k] + w2 * cc[k];
    if (t) {
        float s[4];
        sample(t, w0 * a->u + w1 * b->u + w2 * c->u, w0 * a->v + w1 * b->v + w2 * c->v, s);
        for (int k = 0; k < 4; k++) col[k] *= s[k];
    }
    if (col[3] < 16 / 255.0f) return;        /* the alpha test */
    u32 al = to8(col[3]), d = *px, o = 0;
    for (int k = 0; k < 3; k++) {
        int sh = 16 - 8 * k;
        o |= (to8(col[k]) * al + (d >> sh & 255) * (255 - al) + 127) / 255 << sh;
    }
    *px = o;
}

static void tri(const raster_vtx *a, const raster_vtx *b, const raster_vtx *c, const raster_tex *t)
{
    s64 ax, ay, bx, by, cx, cy;
    if (snap(a->x, &ax) || snap(a->y, &ay) || snap(b->x, &bx) || snap(b->y, &by) || snap(c->x, &cx) || snap(c->y, &cy)) return;
    s64 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (!area) return;
    if (area < 0) {                          /* the other winding: b and c change places */
        const raster_vtx *v = b;
        b = c;
        c = v;
        s64 sx = bx, sy = by;
        bx = cx; by = cy; cx = sx; cy = sy;
        area = -area;
    }
    /* the pixels whose centres the bounding box holds, inside the buffer */
    s64 x0 = ceil_div(min3(ax, bx, cx) - SUB / 2, SUB), x1 = floor_div(max3(ax, bx, cx) - SUB / 2, SUB);
    s64 y0 = ceil_div(min3(ay, by, cy) - SUB / 2, SUB), y1 = floor_div(max3(ay, by, cy) - SUB / 2, SUB);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw - 1) x1 = fw - 1;
    if (y1 > fh - 1) y1 = fh - 1;
    if (x0 > x1 || y0 > y1) return;
    /* the edge values at the centre of pixel (x0, y0): each is 0 on its edge
     * and area at the opposite vertex (e0 for a, e1 for b, e2 for c) */
    s64 px = x0 * SUB + SUB / 2, py = y0 * SUB + SUB / 2;
    s64 e0 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
    s64 e1 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
    s64 e2 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    s64 b0 = bias(cx - bx, cy - by), b1 = bias(ax - cx, ay - cy), b2 = bias(bx - ax, by - ay);
    float ca[4], cb[4], cc[4];
    rgba(a->rgba, ca);
    rgba(b->rgba, cb);
    rgba(c->rgba, cc);
    double inv = 1.0 / area;
    for (s64 y = y0; y <= y1; y++) {
        s64 r0 = e0, r1 = e1, r2 = e2;
        for (s64 x = x0; x <= x1; x++) {
            if (r0 + b0 >= 0 && r1 + b1 >= 0 && r2 + b2 >= 0)
                shade(&fb[y * fw + x], (float)(r0 * inv), (float)(r1 * inv), (float)(r2 * inv), a, b, c, ca, cb, cc, t);
            r0 -= (cy - by) * SUB;
            r1 -= (ay - cy) * SUB;
            r2 -= (by - ay) * SUB;
        }
        e0 += (cx - bx) * SUB;
        e1 += (ax - cx) * SUB;
        e2 += (bx - ax) * SUB;
    }
}

void raster_draw(int type, const raster_vtx *v, int n, const raster_tex *tex)
{
    switch (type) {
    case RASTER_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            tri(&v[i], &v[i + 1], &v[i + 2], tex);
            tri(&v[i], &v[i + 2], &v[i + 3], tex);
        }
        break;
    case RASTER_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) tri(&v[i], &v[i + 1], &v[i + 2], tex);
        break;
    case RASTER_TRIANGLE_FAN:
        for (int i = 1; i + 1 < n; i++) tri(&v[0], &v[i], &v[i + 1], tex);
        break;
    case RASTER_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) tri(&v[i + 1], &v[i], &v[i + 2], tex);
            else tri(&v[i], &v[i + 1], &v[i + 2], tex);
        }
        break;
    }
}
