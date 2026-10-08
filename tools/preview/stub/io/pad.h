#ifndef PV_PAD_H
#define PV_PAD_H
#include "../ppu-types.h"
#define MAX_PADS 127
#define MAX_PORT_NUM 7
typedef struct { u32 max, connected, info; u16 vendor_id[MAX_PADS]; u16 product_id[MAX_PADS]; u8 status[MAX_PADS]; } padInfo;
typedef struct {
    s32 len;
    union {
        u16 button[64];
        struct {
            u16 zeroes, b1, b2, b3, ANA_R_H, ANA_R_V, ANA_L_H, ANA_L_V;
            u16 PRE_RIGHT, PRE_LEFT, PRE_UP, PRE_DOWN, PRE_TRIANGLE, PRE_CIRCLE, PRE_CROSS, PRE_SQUARE, PRE_L1, PRE_R1, PRE_L2, PRE_R2;
            u16 SENSOR_X, SENSOR_Y, SENSOR_Z, SENSOR_G;
        };
    };
} padData;
typedef struct { u32 info[32]; } padCapabilityInfo;
typedef struct { union { u8 motor[2]; struct { u8 small_motor, large_motor; }; }; u8 reserved[6]; } padActParam;
s32 ioPadInit(u32 max); s32 ioPadEnd(void); s32 ioPadGetInfo(padInfo *i); s32 ioPadGetData(u32 port, padData *d);
s32 ioPadSetPressMode(u32 port, u32 mode); s32 ioPadSetSensorMode(u32 port, u32 mode);
s32 ioPadGetCapabilityInfo(u32 port, padCapabilityInfo *c); u32 ioPadSetActDirect(u32 port, padActParam *a);
#endif
