#ifndef PV_TINY3D_H
#define PV_TINY3D_H
#include "ppu-types.h"
#include "sysutil/video.h"
typedef enum { TINY3D_POINTS = 1, TINY3D_LINES, TINY3D_LINE_LOOP, TINY3D_LINE_STRIP, TINY3D_TRIANGLES, TINY3D_TRIANGLE_STRIP,
               TINY3D_TRIANGLE_FAN, TINY3D_QUADS, TINY3D_QUAD_STRIP, TINY3D_POLYGON } type_polygon;
typedef enum { TINY3D_TEX_FORMAT_L8 = 0x100, TINY3D_TEX_FORMAT_A1R5G5B5 = 0x200, TINY3D_TEX_FORMAT_A4R4G4B4 = 0x300,
               TINY3D_TEX_FORMAT_R5G6B5 = 0x400, TINY3D_TEX_FORMAT_A8R8G8B8 = 0x500 } text_format;
#define TEXTURE_NEAREST 0
#define TEXTURE_LINEAR 1
#define TEXTWRAP_REPEAT 0
#define TEXTWRAP_CLAMP 1
#define TINY3D_CLEAR_ALL 7
#define TINY3D_CLEAR_COLOR 1
#define TINY3D_ALPHA_FUNC_GEQUAL 6
#define TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA 1
#define TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA 2
#define TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA 4
#define TINY3D_BLEND_FUNC_DST_ALPHA_ZERO 8
#define TINY3D_BLEND_RGB_FUNC_ADD 16
#define TINY3D_BLEND_ALPHA_FUNC_ADD 32
extern videoResolution Video_Resolution;
int tiny3d_Init(u32 size); void tiny3d_Project2D(void); void *tiny3d_AllocTexture(u32 size); u32 tiny3d_TextureOffset(void *p);
void tiny3d_SetTextureWrap(int unit, u32 offset, u32 w, u32 h, u32 stride, int fmt, int wrap_u, int wrap_v, int filter);
int tiny3d_SetPolygon(int type); void tiny3d_VertexPos(float x, float y, float z); void tiny3d_VertexColor(u32 rgba);
void tiny3d_VertexTexture(float u, float v); void tiny3d_End(void); void tiny3d_Flip(void);
void tiny3d_Clear(u32 color, int flags); void tiny3d_AlphaTest(int enable, u8 ref, int func); void tiny3d_BlendFunc(int enable, int src, int dst, int func);
void tiny3d_UserViewport(int on, float px, float py, float sx, float sy, float sx3, float sy3);
#endif
