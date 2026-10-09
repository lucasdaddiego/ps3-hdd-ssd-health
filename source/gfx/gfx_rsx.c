/* The console back end of gfx: it writes the RSX command words itself, with
 * no librsx. The method names are PSL1GHT's (rsx/nv40.h), and README.md in
 * this folder gives the source of each word. */
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <ppu-asm.h>
#include <ppu_intrinsics.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>
#include "gfx.h"

/* The IO area, main memory that the RSX reads: libgcm's first 4 KB, the
 * command ring, then the vertices from 64 KB to 1 MB. */
#define IO_BYTES 0x100000
#define RING_AT  0x1000
#define VTX_AT   0x10000
#ifndef GFX_RING_BYTES
#define GFX_RING_BYTES (VTX_AT - RING_AT)    /* the harness tests smaller rings, of 4 KB or more */
#endif
#define LABEL 255                            /* the backend label of the fence */
#define POLLS 10000                          /* a wait: at most 10,000 sleeps of 200 us, about 2 s */

static gcmContextData *ctx;
static gcmControlRegister *ctl;
static u8 *io;
static u32 io_off;                           /* the RSX offset of the IO area */
static u32 *ring, *wp;                       /* the ring start, and where the next word goes */
static volatile u32 *label;
static u32 seq;                              /* the last label number */
static int inval;                            /* the next draw invalidates the vertex cache first */
static int ready;                            /* the init has completed */
static int back = 1;                         /* the colour buffer of the frame; 0 may be on the TV */
static u32 pitch, rt_format, fb_off[2], fp_off[2], vtx_buf;
static int prog;                             /* the fragment program in use: 0 colour, 1 texture x colour */
static const gfx_tex *bound;                 /* the texture on unit 0 in this frame */
static u8 *vram_base;                        /* RSX local memory, for gfx_vram */
static u32 vram_size, vram_next;

/* The fragment programs as cgcomp -a writes them, with the halfwords swapped
 * as the RSX reads them (programs.zsh rebuilds and checks them). */
static const u32 fp_words[2][8] = {
    {0x3e810100, 0xc8011c9d, 0xc8000001, 0xc8000001},   /* MOV o[COLH], f[COL0] */
    {0x9e001780, 0xc8011c9d, 0xc8000001, 0xc8003fe1,    /* TEXX R0, f[TEX0], texture[0], 2D */
     0x3e810280, 0xc8001c9d, 0xc8010001, 0xc8000001},   /* MULX o[COLH], R0, f[COL0] */
};
static const int fp_len[2] = {4, 8};

/* The vertex program (cgcomp -a -v): MOV o[HPOS], v[0]; MOV o[COL0], v[3];
 * MOV o[TEX0], v[8]. The masks are the ones in the header that cgcomp writes:
 * the inputs v0, v3, v8, and the outputs COL0 (front and back) and TEX0. */
static const u32 vp_words[12] = {
    0x401f9c6c, 0x0040000d, 0x8106c083, 0x6041ff80,
    0x401f9c6c, 0x0040030d, 0x8106c083, 0x6041ff84,
    0x401f9c6c, 0x0040080d, 0x8106c083, 0x6041ff9d,
};
#define VP_INPUTS  0x0109
#define VP_OUTPUTS 0x4005

/* A firmware call only when the offset is asked for: the RSX memory test
 * allocates from its job thread while the main thread flips. */
void *gfx_vram(u32 size, u32 align, u32 *offset)
{
    u64 p = ((u64)vram_next + align - 1) & ~((u64)align - 1);
    if (!align || (align & (align - 1)) || p > vram_size || size > vram_size - p) return NULL;
    if (offset && gcmAddressToOffset(vram_base + p, offset)) return NULL;
    vram_next = (u32)(p + size);
    return vram_base + p;
}

/* ---- the ring --------------------------------------------------------------- */

/* A method with one, two or four data words: the header is count << 18 | method. */
static void w1(u32 m, u32 a)
{
    wp[0] = 1 << 18 | m;
    wp[1] = a;
    wp += 2;
}

static void w2(u32 m, u32 a, u32 b)
{
    wp[0] = 2 << 18 | m;
    wp[1] = a;
    wp[2] = b;
    wp += 3;
}

static void w4(u32 m, u32 a, u32 b, u32 c, u32 d)
{
    wp[0] = 4 << 18 | m;
    wp[1] = a;
    wp[2] = b;
    wp[3] = c;
    wp[4] = d;
    wp += 5;
}

static u32 fbits(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static u32 off_of(const void *p) { return io_off + (u32)((const u8 *)p - io); }

/* PUT to the write pointer: the RSX reads up to there. After every PUT move
 * the next draw invalidates the vertex cache. */
static void flush(void)
{
    __sync();
    ctl->put = off_of(wp);
    inval = 1;
}

/* A bounded wait: a hung RSX costs time, never the app. */
static void wait_for(volatile u32 *p, u32 v)
{
    for (int i = 0; *p != v && i < POLLS; i++) usleep(200);
}

/* The ring is full: the RSX reads to the end, a JUMP takes it back to the
 * start, and gfx waits until it got there. It waits for the RSX to be idle
 * first, because GET at the ring start can also mean "nothing read yet".
 * This is also the context callback, for gcmSetFlip and gcmSetWaitFlip;
 * need(64) before them keeps the firmware from calling it. */
static s32 wrap(gcmContextData *c, u32 count)
{
    (void)count;
    wp = c->current;
    flush();
    wait_for(&ctl->get, off_of(wp));
    *wp = 0x20000000 | off_of(ring);         /* JUMP */
    __sync();
    ctl->put = off_of(ring);
    wait_for(&ctl->get, off_of(ring));
    c->current = wp = ring;
    return 0;
}

static void need(u32 n)
{
    if (wp + n > ctx->end) {
        ctx->current = wp;
        wrap(ctx, n);
    }
}

/* Backend label 255 after the commands so far: the RSX writes it when it has
 * finished them. Byte 0 of each number equals byte 2, so the swap of the two
 * bytes that librsx makes for this method changes nothing. */
static u32 mark(void)
{
    seq = seq % 0xffff + 1;
    u32 v = seq << 16 | seq;
    need(4);
    w1(0x1d6c, LABEL * 0x10);                /* SEMAPHORE_OFFSET */
    w1(0x1d70, v);                           /* SEMAPHORE_BACKENDWRITE_RELEASE */
    return v;
}

/* ---- the state -------------------------------------------------------------- */

/* pixel = canvas * scale + translate. The clip z of every vertex is 0, which
 * the default depth range maps to 0.5. */
static void viewport(const float *v)
{
    w4(0x0a20, fbits(v[0]), fbits(v[1]), 0x3f000000, 0);                 /* VIEWPORT_TRANSLATE: z 0.5 */
    w4(0x0a30, fbits(v[2]), fbits(v[3]), 0x3f000000, 0x3f800000);       /* VIEWPORT_SCALE: z 0.5 */
}

/* The state of a frame that draws into colour buffer b, 119 words, after
 * every flip (the flip can reset the registers). Only colour target 0 is on,
 * and the depth test is off, so the zeta surface can sit on the same buffer:
 * nothing reads or writes it. */
static void state(int b, const float *v)
{
    need(119);
    w1(0x0194, 0xfeed0000);                  /* DMA_COLOR0: local memory */
    w1(0x0210, fb_off[b]);                   /* COLOR0_OFFSET */
    w1(0x020c, pitch);                       /* COLOR0_PITCH */
    w1(0x0198, 0xfeed0000);                  /* DMA_ZETA */
    w1(0x0214, fb_off[b]);                   /* ZETA_OFFSET */
    w1(0x022c, pitch);                       /* ZETA_PITCH */
    w1(0x0208, rt_format);                   /* RT_FORMAT: A8R8G8B8, Z24S8, linear */
    w1(0x0220, 1);                           /* RT_ENABLE: colour 0 */
    w1(0x02b8, 0);                           /* VIEWPORT_TX_ORIGIN */
    w1(0x1d88, 0x1000 | gfx_h);              /* COORD_CONVENTIONS */
    w2(0x0200, gfx_w << 16, gfx_h << 16);    /* RT_HORIZ, RT_VERT */
    viewport(v);
    w2(0x0a00, gfx_w << 16, gfx_h << 16);    /* VIEWPORT_HORIZ, VIEWPORT_VERT */
    w2(0x08c0, gfx_w << 16, gfx_h << 16);    /* SCISSOR_HORIZ, SCISSOR_VERT */
    for (int i = 0; i < 8; i++) w2(0x02c0 + i * 8, (gfx_w - 1) << 16, (gfx_h - 1) << 16);   /* VIEWPORT_CLIP */
    w1(0x1d78, 0x110);                       /* DEPTH_CONTROL: no near/far clip, clamp */
    w1(0x0a70, 0);                           /* DEPTH_WRITE_ENABLE */
    w1(0x0a74, 0);                           /* DEPTH_TEST_ENABLE */
    w1(0x0308, 0x206);                       /* ALPHA_FUNC: GEQUAL */
    w1(0x030c, 0x10);                        /* ALPHA_REF */
    w1(0x0304, 1);                           /* ALPHA_ENABLE */
    w2(0x0314, 0x03020302, 0x0303);          /* BLEND_FUNC: SRC_ALPHA, ONE_MINUS_SRC_ALPHA, alpha ZERO */
    w1(0x0320, 0x80068006);                  /* BLEND_EQUATION: ADD */
    w1(0x0310, 1);                           /* BLEND_ENABLE */
    w2(0x1e9c, 0, 0);                        /* VP_UPLOAD_FROM_ID, VP_START_FROM_ID */
    for (int i = 0; i < 12; i += 4) w4(0x0b80, vp_words[i], vp_words[i + 1], vp_words[i + 2], vp_words[i + 3]);
    w2(0x1ff0, VP_INPUTS, VP_OUTPUTS);       /* VP_ATTRIB_EN, VP_RESULT_EN */
    w1(0x08e4, fp_off[0] | 1);               /* FP_ACTIVE_PROGRAM: colour, in local memory */
    w1(0x1d60, 0x02000000);                  /* FP_CONTROL: 2 registers */
    w1(0x1740, 0x1422);                      /* VTXFMT0: position, 2 floats, stride 20 */
    w1(0x174c, 0x1444);                      /* VTXFMT3: colour, 4 bytes */
    w1(0x1760, 0x1422);                      /* VTXFMT8: texture coordinate, 2 floats */
    w1(0x1680, vtx_buf);                     /* VTXBUF0 */
    w1(0x168c, vtx_buf + 8);                 /* VTXBUF3 */
    w1(0x16a0, vtx_buf + 12);                /* VTXBUF8 */
    prog = 0;
    bound = NULL;                            /* the flip can turn texture unit 0 off */
}

/* Texture unit 0: A4R4G4B4, linear rows, 2D, clamped to the edge, so the RSX
 * never reads the border colour (0). */
static void bind(const gfx_tex *t)
{
    need(13);
    w1(0x1fd8, 1);                           /* TEX_CACHE_CTL: invalidate the texture cache */
    wp[0] = 8 << 18 | 0x1a00;                /* TEX_OFFSET0 to TEX_BORDER_COLOR0 */
    wp[1] = t->offset;
    wp[2] = 0x0001a329;                      /* format, one mipmap, local memory */
    wp[3] = 0x00010303;                      /* wrap */
    wp[4] = 0x80000000;                      /* enable */
    wp[5] = 0x0000aae4;                      /* swizzle */
    wp[6] = t->linear ? 0x02023fd6 : 0x01013fd6;   /* filter */
    wp[7] = (u32)t->w << 16 | t->h;
    wp[8] = 0;
    wp += 9;
    w1(0x1840, 0x00100000 | t->pitch);       /* TEX_SIZE1 */
    bound = t;
}

/* ---- the frame -------------------------------------------------------------- */

void gfx_be_begin(const float *vp) { state(back, vp); }

void gfx_be_viewport(const float *vp)
{
    need(10);
    viewport(vp);
}

/* A draw: BEGIN_END, one VB_VERTEX_BATCH per 256 vertices, BEGIN_END 0. */
void gfx_be_draw(int type, u32 first, u32 n, const gfx_tex *t)
{
    if (t && t != bound) bind(t);
    if (prog != (t != NULL)) {
        prog = t != NULL;
        need(4);
        w1(0x08e4, fp_off[prog] | 1);        /* FP_ACTIVE_PROGRAM */
        w1(0x1d60, 0x02000000);              /* FP_CONTROL */
    }
    need(8 + 4 + 2 * ((n + 255) / 256));
    if (inval) {                             /* the vertex cache invalidate of librsx */
        w1(0x1710, 0);
        w1(0x1714, 0);
        w1(0x1714, 0);
        w1(0x1714, 0);
        inval = 0;
    }
    w1(0x1808, type);
    while (n) {
        u32 k = n > 256 ? 256 : n;
        w1(0x1814, (k - 1) << 24 | first);
        first += k;
        n -= k;
    }
    w1(0x1808, 0);
}

void gfx_be_drain(void)
{
    u32 v = mark();
    flush();
    wait_for(label, v);
}

/* The label, then the flip and the wait for it in the order of PSL1GHT's
 * samples, then the waits: when this returns the frame is on the TV and the
 * RSX has read every vertex. The flush before gcmSetFlip puts PUT where its
 * words go; RPCS3's gcmSetFlip moves PUT past them itself. */
void gfx_be_end(void)
{
    u32 v = mark();
    need(64);
    flush();
    gcmResetFlipStatus();
    ctx->current = wp;
    gcmSetFlip(ctx, back);
    wp = ctx->current;
    flush();
    need(64);
    ctx->current = wp;
    gcmSetWaitFlip(ctx);                     /* a later flush sends it */
    wp = ctx->current;
    for (int i = 0; gcmGetFlipStatus() != 0 && i < POLLS; i++) usleep(200);   /* 0: flipped */
    wait_for(label, v);
    back ^= 1;
}

/* At exit: the wait for the last flip and a label go out, and gfx waits for
 * the label, as rsxFinish does in PSL1GHT's samples. Then the RSX is idle and
 * the context holds no word that it has not read. */
void gfx_be_exit(void)
{
    if (!ready) return;
    gfx_be_drain();
    ctx->current = wp;
}

/* ---- init ------------------------------------------------------------------- */

/* 0, or the number of the step that failed: with no picture, the app writes
 * it to a file. */
int gfx_be_init(u32 vtx_bytes, gfx_vtx **area)
{
    static const float vp0[4] = {0, 0, 1, 1};
    s32 (*cb)(gcmContextData *, u32) = wrap; /* a variable: __get_opd32 tests the address, and GCC warns on wrap's own */
    videoState st;
    videoResolution res;
    videoConfiguration vc;
    gcmConfiguration cfg;
    if (vtx_bytes > IO_BYTES - VTX_AT || GFX_RING_BYTES > VTX_AT - RING_AT || GFX_RING_BYTES < 0x1000) return 1;
    io = memalign(IO_BYTES, IO_BYTES);
    if (!io) return 2;
    if (gcmInitBody(&ctx, 0x10000, IO_BYTES, io)) return 3;
    if (gcmAddressToOffset(io, &io_off)) return 4;
    if (videoGetState(0, 0, &st) || videoGetResolution(st.displayMode.resolution, &res)) return 5;
    gfx_w = res.width;
    gfx_h = res.height;
    pitch = 4 * ((gfx_w + 15) / 16 * 16);
    rt_format = 0x148 | (31 - __builtin_clz(gfx_w)) << 16 | (31 - __builtin_clz(gfx_h)) << 24;
    memset(&vc, 0, sizeof vc);
    vc.resolution = st.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.aspect = st.displayMode.aspect;
    vc.pitch = pitch;
    if (videoConfigure(0, &vc, NULL, 0)) return 6;
    memset(&cfg, 0, sizeof cfg);
    gcmGetConfiguration(&cfg);               /* void in the firmware, so no result to test (PSL1GHT declares s32) */
    if (!cfg.localAddress || !cfg.localSize) return 7;
    vram_base = cfg.localAddress;
    vram_size = cfg.localSize;
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    for (int b = 0; b < 2; b++) {
        if (!gfx_vram(pitch * gfx_h, 64, &fb_off[b])) return 8;
        if (gcmSetDisplayBuffer(b, fb_off[b], pitch, gfx_w, gfx_h)) return 9;
    }
    for (int p = 0; p < 2; p++) {            /* each in its own 256-byte block */
        volatile u32 *m = gfx_vram(256, 256, &fp_off[p]);
        if (!m) return 10;
        for (int i = 0; i < fp_len[p]; i++) m[i] = fp_words[p][i];
    }
    vtx_buf = 0x80000000 | (io_off + VTX_AT);   /* bit 31: main memory */
    label = gcmGetLabelAddress(LABEL);
    ctl = gcmGetControlRegister();
    if (!label || !ctl) return 11;
    *label = 0;
    ring = (u32 *)(io + RING_AT);
    wp = ctx->current;
    ctx->begin = ring;
    ctx->end = ring + GFX_RING_BYTES / 4 - 1;   /* the last word is for the JUMP */
    if (wp < ring || wp >= ctx->end) return 12;
    ctx->callback = (gcmContextCallback)__get_opd32(cb);
    gcmResetFlipStatus();
    for (int b = 0; b < 2; b++) {            /* both buffers dark before a flip can show one */
        state(b, vp0);
        need(6);
        w1(0x1d90, 0xff0a1222);              /* CLEAR_COLOR_VALUE */
        w1(0x1d94, 0xf0);                    /* CLEAR_BUFFERS: colour only */
        w1(0x0100, 0);                       /* NOP */
    }
    gfx_be_drain();
    *area = (gfx_vtx *)(io + VTX_AT);
    ready = 1;
    return 0;
}
