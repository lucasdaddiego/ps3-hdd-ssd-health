#ifndef PV_VIDEO_H
#define PV_VIDEO_H
#include "../ppu-types.h"
#define VIDEO_SCANMODE_INTERLACE 0
#define VIDEO_SCANMODE_PROGRESSIVE 1
#define VIDEO_ASPECT_AUTO 0
#define VIDEO_ASPECT_4_3 1
#define VIDEO_ASPECT_16_9 2
#define VIDEO_RESOLUTION_1080 1
#define VIDEO_RESOLUTION_720 2
#define VIDEO_RESOLUTION_480 4
#define VIDEO_RESOLUTION_576 5
#define VIDEO_RESOLUTION_1600x1080 10
#define VIDEO_RESOLUTION_1440x1080 11
#define VIDEO_RESOLUTION_1280x1080 12
#define VIDEO_RESOLUTION_960x1080 13
typedef struct { u16 width, height; } videoResolution;
typedef struct { u8 resolution, scanMode, conversion, aspect; u8 padding[2]; u16 refreshRates; } videoDisplayMode;
typedef struct { u8 state, colorSpace; u8 padding[6]; videoDisplayMode displayMode; } videoState;
s32 videoGetState(s32 out, s32 dev, videoState *s);
s32 videoGetResolution(s32 id, videoResolution *r);
#endif
