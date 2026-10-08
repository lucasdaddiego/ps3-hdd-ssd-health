/* The preview's software RSX: the app's 2D primitives into an 8-bit XRGB
 * colour buffer, drawn as the console draws them (raster.c). */
#ifndef PV_RASTER_H
#define PV_RASTER_H
#include "ppu-types.h"

/* the primitive types, with the values of the NV40 BEGIN_END method */
enum { RASTER_TRIANGLES = 5, RASTER_TRIANGLE_STRIP = 6, RASTER_TRIANGLE_FAN = 7, RASTER_QUADS = 8 };

/* A vertex: the position in output pixels, the texture coordinates, the
 * colour as 0xRRGGBBAA. */
typedef struct { float x, y, u, v; u32 rgba; } raster_vtx;

/* A texture: A4R4G4B4 texels, rows of stride bytes, clamped to the edge. */
typedef struct { const u8 *texels; u32 w, h, stride; int linear; } raster_tex;

void raster_init(int w, int h);              /* a new w x h colour buffer */
void raster_clear(u32 argb);
/* n vertices of one primitive type; tex NULL = colour only */
void raster_draw(int type, const raster_vtx *v, int n, const raster_tex *tex);
u32 raster_pixel(int x, int y);              /* 0x00RRGGBB */

#endif
