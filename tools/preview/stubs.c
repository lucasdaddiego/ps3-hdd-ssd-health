/* Host stubs for the PS3 Health preview. The app's own sources compile on the
 * Mac against stub headers; this file gives them a world:
 *   - tiny3d: the primitives go to raster.c, a software RSX (each triangle
 *     once, the top-left fill rule, colour x A4R4G4B4 texel, the alpha test,
 *     source-alpha blending in 8-bit math, an 8-bit XRGB buffer), and the
 *     textures to the free RSX memory that the console has;
 *   - the pad: a script of frames and buttons (PV_SCRIPT);
 *   - time: 1/60 s per flip;
 *   - files: /dev_* paths inside a sandbox (PV_ROOT);
 *   - syscalls 600/601/609/616/383/409: an emulated drive that answers from the
 *     console's sector dumps (PV_DRIVE), temperatures, a fan duty.
 * Snapshots: "frame:SNAP=name" writes PV_OUT/name.ppm. README.md lists the
 * other PV_* variables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include "tiny3d.h"
#include "io/pad.h"
#include "sysutil/sysutil.h"
#include "sysutil/video.h"
#include "lv2/systime.h"
#include "sys/systime.h"
#include "sys/thread.h"
#include "sys/file.h"
#include "net/net.h"
#include "net/netctl.h"
#include "arpa/inet.h"
#include "sysmodule/sysmodule.h"
#include "ppu-lv2.h"
#include "raster.h"

videoResolution Video_Resolution = {1920, 1080};
static float vpx, vpy, vsx = 1, vsy = 1;
static int frame, drawing;
static u64 sim_us = 1000000;

/* ---- script --------------------------------------------------------------- */

#define MAXEV 512
static struct { int f0, f1; unsigned btn; char snap[64]; int exit_; int spin; } ev[MAXEV];
static int nev, maxframes = 20000;

/* Only frames that get a snapshot are rasterized. */
static int render_on(void)
{
    for (int i = 0; i < nev; i++)
        if (ev[i].f0 == frame && ev[i].snap[0]) return 1;
    return 0;
}

static unsigned btn_of(const char *s)
{
    static const struct { const char *n; unsigned b; } t[] = {
        {"LEFT", 0x8000}, {"DOWN", 0x4000}, {"RIGHT", 0x2000}, {"UP", 0x1000}, {"START", 0x0800}, {"R3", 0x0400},
        {"L3", 0x0200}, {"SELECT", 0x0100}, {"SQUARE", 0x80}, {"CROSS", 0x40}, {"CIRCLE", 0x20}, {"TRIANGLE", 0x10},
        {"R1", 8}, {"L1", 4}, {"R2", 2}, {"L2", 1}};
    unsigned b = 0;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", s);
    char *sv = NULL;
    for (char *tok = strtok_r(tmp, "+", &sv); tok; tok = strtok_r(NULL, "+", &sv))
        for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++)
            if (!strcmp(tok, t[k].n)) b |= t[k].b;
    return b;
}

static void script_load(void)
{
    const char *s = getenv("PV_SCRIPT");
    if (getenv("PV_MAXFRAMES")) maxframes = atoi(getenv("PV_MAXFRAMES"));
    if (!s) return;
    char *copy = strdup(s);
    char *sv = NULL;
    for (char *e = strtok_r(copy, " ;\n", &sv); e && nev < MAXEV; e = strtok_r(NULL, " ;\n", &sv)) {
        char *colon = strchr(e, ':');
        if (!colon) continue;
        *colon = 0;
        int f0 = atoi(e), f1 = f0;
        char *dash = strchr(e, '-');
        if (dash) f1 = atoi(dash + 1);
        const char *a = colon + 1;
        ev[nev].f0 = f0;
        ev[nev].f1 = f1;
        if (!strncmp(a, "SNAP=", 5)) snprintf(ev[nev].snap, sizeof ev[nev].snap, "%s", a + 5);
        else if (!strcmp(a, "EXIT")) ev[nev].exit_ = 1;
        else if (!strcmp(a, "SPIN")) ev[nev].spin = 1;
        else ev[nev].btn = btn_of(a);
        nev++;
    }
    drawing = render_on();
}

static unsigned buttons_now(int *spin)
{
    unsigned b = 0;
    *spin = 0;
    for (int i = 0; i < nev; i++)
        if (frame >= ev[i].f0 && frame <= ev[i].f1) { b |= ev[i].btn; if (ev[i].spin) *spin = 1; }
    return b;
}

/* ---- tiny3d --------------------------------------------------------------- */

/* RSX local memory: tiny3d_AllocTexture gets what tiny3D leaves free on the
 * console. That is the 249 MB of gcmGetConfiguration less what tiny3d_Init
 * takes first, in its order and alignment (rsxutil.c, tiny3d.c): two colour
 * buffers, the depth buffer (at least 1920 x 1088), two 256-byte fragment
 * programs and the 1 MB vertex buffer. So the RSX memory test counts what the
 * console counts: 222 MB at 1080p, 230 at 720p, 235 at 480p. */
#define RSX_LOCAL 0x0F900000u
static u8 *arena;                            /* the free RSX memory, from local offset rsx_base */
static u32 rsx_base, rsx_next;

static u32 align_up(u32 p, u32 a) { return (p + a - 1) & ~(a - 1); }

static void rsx_layout(u32 w, u32 h)
{
    u32 pitch = 4 * ((w + 15) / 16 * 16), zpitch = 4 * (w > 1920 ? (w + 15) / 16 * 16 : 1920);
    u32 zrows = h > 1088 ? (h + 15) / 16 * 16 : 1088, p = 0;
    p = align_up(p, 64) + pitch * h;         /* the colour buffers */
    p = align_up(p, 64) + pitch * h;
    p = align_up(p, 64) + zpitch * zrows;    /* the depth buffer */
    p = align_up(p, 256) + 256;              /* the fragment programs */
    p = align_up(p, 256) + 256;
    p = align_up(p, 64) + 1024 * 1024;       /* the vertex buffer */
    rsx_base = rsx_next = p;
    arena = calloc(1, RSX_LOCAL - p);
}

_Static_assert(TINY3D_TRIANGLES == RASTER_TRIANGLES && TINY3D_TRIANGLE_STRIP == RASTER_TRIANGLE_STRIP &&
               TINY3D_TRIANGLE_FAN == RASTER_TRIANGLE_FAN && TINY3D_QUADS == RASTER_QUADS, "the primitive types pass through");

static raster_tex tex;
static raster_vtx vb[400000], cur;
static int nv, ptype, ptex, pending;
static u32 ccol = 0xffffffff;
static float cu, cv;

int tiny3d_Init(u32 size)
{
    (void)size;
    const char *r = getenv("PV_RES");
    if (r && !strcmp(r, "720")) { Video_Resolution.width = 1280; Video_Resolution.height = 720; }
    if (r && !strcmp(r, "480")) { Video_Resolution.width = 720; Video_Resolution.height = 480; }
    rsx_layout(Video_Resolution.width, Video_Resolution.height);
    raster_init(Video_Resolution.width, Video_Resolution.height);
    script_load();
    return 0;
}
void tiny3d_Project2D(void) {}
void *tiny3d_AllocTexture(u32 size)
{
    u32 p = align_up(rsx_next, 128);
    if (size > RSX_LOCAL - p) return NULL;
    rsx_next = p + size;
    return arena + (p - rsx_base);
}
u32 tiny3d_TextureOffset(void *p) { return (u32)((u8 *)p - arena); }
void tiny3d_SetTextureWrap(int unit, u32 offset, u32 w, u32 h, u32 stride, int fmt, int wu, int wv, int filter)
{
    (void)unit; (void)fmt; (void)wu; (void)wv;
    tex.texels = arena + offset; tex.w = w; tex.h = h; tex.stride = stride; tex.linear = filter != TEXTURE_NEAREST;
}
void tiny3d_UserViewport(int on, float px, float py, float sx, float sy, float a, float b)
{
    (void)a; (void)b;
    if (!on) { vpx = vpy = 0; vsx = vsy = 1; return; }
    vpx = px; vpy = py; vsx = sx; vsy = sy;
}
void tiny3d_AlphaTest(int e, u8 r, int f) { (void)e; (void)r; (void)f; }
void tiny3d_BlendFunc(int e, int s, int d, int f) { (void)e; (void)s; (void)d; (void)f; }

static void push(void)
{
    if (!pending || nv >= (int)(sizeof vb / sizeof vb[0])) return;
    cur.rgba = ccol;
    cur.u = cu;
    cur.v = cv;
    vb[nv++] = cur;
    pending = 0;
}

int tiny3d_SetPolygon(int type) { ptype = type; nv = 0; ptex = 0; pending = 0; return 0; }
void tiny3d_VertexPos(float x, float y, float z) { (void)z; push(); cur.x = x * vsx + vpx; cur.y = y * vsy + vpy; pending = 1; }
void tiny3d_VertexColor(u32 c) { ccol = c; }
void tiny3d_VertexTexture(float u, float v) { cu = u; cv = v; ptex = 1; }

/* PV_VSTAT: tiny3d's vertex memory per frame (16 B position, 4 B colour,
 * 8 B texture per vertex, each polygon padded to 64 B; 1 MB per frame). */
static long vbytes, vmax, polys, pmax, vmax_frame;
static void vstat_exit(void) { if (getenv("PV_VSTAT")) fprintf(stderr, "vstat: max %ld bytes (%ld polygons) per frame, at frame %ld\n", vmax, pmax, vmax_frame); }
void tiny3d_End(void)
{
    push();
    vbytes = (vbytes + nv * (20 + (ptex ? 8 : 0)) + 63) & ~63L;
    polys++;
    if (drawing) raster_draw(ptype, vb, nv, ptex ? &tex : NULL);
    nv = 0;
}

void tiny3d_Clear(u32 color, int flags)
{
    (void)flags;
    if (drawing) raster_clear(color);
}

static void snap(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s.ppm", getenv("PV_OUT") ? getenv("PV_OUT") : ".", name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    int W = Video_Resolution.width, H = Video_Resolution.height;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u32 c = raster_pixel(x, y);
            fputc(c >> 16 & 255, f);
            fputc(c >> 8 & 255, f);
            fputc(c & 255, f);
        }
    fclose(f);
    fprintf(stderr, "snap %s at frame %d\n", name, frame);
}

void tiny3d_Flip(void)
{
    int quit_now = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].f0 != frame) continue;
        if (ev[i].snap[0]) snap(ev[i].snap);
        if (ev[i].exit_) quit_now = 1;
    }
    static int vstat_on;
    if (!vstat_on) { vstat_on = 1; atexit(vstat_exit); }
    if (vbytes > vmax) { vmax = vbytes; pmax = polys; vmax_frame = frame; }
    vbytes = polys = 0;
    frame++;
    drawing = render_on();
    sim_us += 16667;
    usleep(1000);                            /* a real millisecond per frame: job threads keep pace */
    if (quit_now) exit(0);
    if (frame > maxframes) { fprintf(stderr, "frame limit\n"); exit(3); }
}

/* ---- pad ------------------------------------------------------------------ */

s32 ioPadInit(u32 m) { (void)m; return 0; }
s32 ioPadEnd(void) { return 0; }
s32 ioPadGetInfo(padInfo *i) { memset(i, 0, sizeof *i); i->max = 7; i->connected = 1; i->status[0] = 1; return 0; }
s32 ioPadGetData(u32 port, padData *d)
{
    (void)port;
    int spin;
    unsigned b = buttons_now(&spin);
    memset(d, 0, sizeof *d);
    d->len = 24;
    d->button[2] = b >> 8 & 0xff;
    d->button[3] = b & 0xff;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    const char *st = getenv("PV_STICKS");
    if (st) sscanf(st, "%d,%d,%d,%d", &lx, &ly, &rx, &ry);
    if (spin) {
        float a = frame * 0.09f;
        lx = (int)(cosf(a) * 120); ly = (int)(sinf(a) * 120);
        rx = (int)(cosf(-a) * 110); ry = (int)(sinf(-a) * 125);
    }
    d->ANA_L_H = 128 + lx; d->ANA_L_V = 128 + ly; d->ANA_R_H = 128 + rx; d->ANA_R_V = 128 + ry;
    const struct { unsigned bit; u16 *f; } p[] = {
        {0x2000, &d->PRE_RIGHT}, {0x8000, &d->PRE_LEFT}, {0x1000, &d->PRE_UP}, {0x4000, &d->PRE_DOWN}, {0x10, &d->PRE_TRIANGLE},
        {0x20, &d->PRE_CIRCLE}, {0x40, &d->PRE_CROSS}, {0x80, &d->PRE_SQUARE}, {4, &d->PRE_L1}, {8, &d->PRE_R1}, {1, &d->PRE_L2},
        {2, &d->PRE_R2}};
    for (unsigned k = 0; k < sizeof p / sizeof p[0]; k++) *p[k].f = (b & p[k].bit) ? 187 : 0;
    d->SENSOR_X = 520; d->SENSOR_Y = 400; d->SENSOR_Z = 498; d->SENSOR_G = 512;
    return 0;
}
s32 ioPadSetPressMode(u32 p, u32 m) { (void)p; (void)m; return 0; }
s32 ioPadSetSensorMode(u32 p, u32 m) { (void)p; (void)m; return 0; }
s32 ioPadGetCapabilityInfo(u32 p, padCapabilityInfo *c) { (void)p; memset(c, 0, sizeof *c); c->info[0] = 0x17; return 0; }
u32 ioPadSetActDirect(u32 p, padActParam *a) { (void)p; (void)a; return 0; }

/* ---- system --------------------------------------------------------------- */

s32 sysUtilRegisterCallback(s32 s, sysutilCallback cb, void *u) { (void)s; (void)cb; (void)u; return 0; }
s32 sysUtilUnregisterCallback(s32 s) { (void)s; return 0; }
s32 sysUtilCheckCallback(void) { return 0; }
u64 sysGetSystemTime(void) { return sim_us; }
s32 sysGetCurrentTime(u64 *sec, u64 *nsec) { *sec = 1791460800ull + sim_us / 1000000; *nsec = 0; return 0; }
s32 sysUsleep(u32 us) { (void)us; usleep(1000); return 0; }

typedef struct { void (*fn)(void *); void *arg; } tstart;
static void *trampoline(void *p) { tstart t = *(tstart *)p; free(p); t.fn(t.arg); return NULL; }
s32 sysThreadCreate(sys_ppu_thread_t *t, void (*entry)(void *), void *arg, s32 prio, u64 stack, u64 flags, char *name)
{
    (void)prio; (void)stack; (void)flags; (void)name;
    tstart *s = malloc(sizeof *s);
    s->fn = entry;
    s->arg = arg;
    return pthread_create(t, NULL, trampoline, s) ? -1 : 0;
}
s32 sysThreadJoin(sys_ppu_thread_t t, u64 *ret) { (void)ret; return pthread_join(t, NULL); }
void sysThreadExit(u64 v) { (void)v; pthread_exit(NULL); }

s32 videoGetState(s32 o, s32 d, videoState *s)
{
    (void)o; (void)d;
    memset(s, 0, sizeof *s);
    s->state = 1;
    s->colorSpace = 1;
    s->displayMode.resolution = Video_Resolution.height == 1080 ? VIDEO_RESOLUTION_1080 : Video_Resolution.height == 720 ? VIDEO_RESOLUTION_720 : VIDEO_RESOLUTION_480;
    s->displayMode.scanMode = VIDEO_SCANMODE_PROGRESSIVE;
    s->displayMode.aspect = VIDEO_ASPECT_16_9;
    s->displayMode.refreshRates = 1 | 4;
    return 0;
}
s32 videoGetResolution(s32 id, videoResolution *r) { (void)id; *r = Video_Resolution; return 0; }

/* ---- files ---------------------------------------------------------------- */

static const char *map(const char *p, char *out, int n)
{
    snprintf(out, n, "%s%s", getenv("PV_ROOT") ? getenv("PV_ROOT") : "/tmp/pv", p);
    return out;
}
s32 sysLv2FsOpen(const char *path, s32 of, s32 *fd, u32 mode, const void *arg, u64 sz)
{
    (void)mode; (void)arg; (void)sz;
    char p[1024];
    int f = (of & 3) == SYS_O_WRONLY ? O_WRONLY : (of & 3) == SYS_O_RDWR ? O_RDWR : O_RDONLY;
    if (of & SYS_O_CREAT) f |= O_CREAT;
    if (of & SYS_O_TRUNC) f |= O_TRUNC;
    if (of & SYS_O_APPEND) f |= O_APPEND;
    int r = open(map(path, p, sizeof p), f, 0666);
    if (r < 0) return (s32)0x80010006;
    *fd = r;
    return 0;
}
s32 sysLv2FsClose(s32 fd) { return close(fd); }
s32 sysLv2FsRead(s32 fd, void *b, u64 l, u64 *n) { ssize_t r = read(fd, b, l); *n = r > 0 ? (u64)r : 0; return r < 0 ? -1 : 0; }
s32 sysLv2FsWrite(s32 fd, const void *b, u64 l, u64 *n) { ssize_t r = write(fd, b, l); *n = r > 0 ? (u64)r : 0; return r < 0 ? -1 : 0; }
s32 sysLv2FsLSeek64(s32 fd, s64 off, s32 wh, u64 *pos) { off_t r = lseek(fd, off, wh); if (r < 0) return -1; *pos = (u64)r; return 0; }
s32 sysLv2FsFsync(s32 fd) { (void)fd; return 0; }
s32 sysLv2FsStat(const char *path, sysFSStat *st) { char p[1024]; struct stat s; (void)st; return stat(map(path, p, sizeof p), &s) ? (s32)0x80010006 : 0; }
s32 sysLv2FsMkdir(const char *path, s32 mode)
{
    char p[1024];
    (void)mode;
    if (mkdir(map(path, p, sizeof p), 0777) == 0) return 0;
    return errno == EEXIST ? (s32)0x80010014 : (s32)0x80010006;
}
s32 sysLv2FsUnlink(const char *path) { char p[1024]; return unlink(map(path, p, sizeof p)) ? -1 : 0; }

/* ---- network: offline ----------------------------------------------------- */

s32 netInitialize(void) { return 0; }
s32 netDeinitialize(void) { return 0; }
s32 netSocket(s32 d, s32 t, s32 p) { (void)d; (void)t; (void)p; return -1; }
s32 netConnect(s32 s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return -1; }
s32 netClose(s32 s) { (void)s; return 0; }
s32 netBind(s32 s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return -1; }
s32 netListen(s32 s, s32 b) { (void)s; (void)b; return -1; }
s32 netAccept(s32 s, const struct sockaddr *a, socklen_t *l) { (void)s; (void)a; (void)l; return -1; }
s32 netSetSockOpt(s32 s, s32 lv, s32 o, const void *v, socklen_t l) { (void)s; (void)lv; (void)o; (void)v; (void)l; return 0; }
ssize_t netSend(s32 s, const void *b, size_t l, s32 f) { (void)s; (void)b; (void)l; (void)f; return -1; }
ssize_t netRecv(s32 s, void *b, size_t l, s32 f) { (void)s; (void)b; (void)l; (void)f; return -1; }
struct net_hostent *netGetHostByName(const char *n) { (void)n; return NULL; }
s32 netCtlInit(void) { return 0; }
void netCtlTerm(void) {}
s32 netCtlGetInfo(s32 code, union net_ctl_info *i) { (void)code; snprintf(i->ip_address, sizeof i->ip_address, "192.168.1.20"); return 0; }
s32 sysModuleLoad(u32 id) { (void)id; return 0; }
in_addr_t inet_addr(const char *cp) { unsigned a, b, c, d; return sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? (a << 24 | b << 16 | c << 8 | d) : (in_addr_t)-1; }

/* ---- syscalls: the emulated drive ----------------------------------------- */

typedef struct __attribute__((packed)) {
    u16 features, sector_count, lba_low, lba_mid, lba_high;
    u8 device, command;
    u32 is_ext, proto, in_out, size, pad;
    u64 buffer;
    u32 arglen, pad2;
} ata_block;

static int load(const char *name, u8 *out)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", getenv("PV_DRIVE") ? getenv("PV_DRIVE") : ".", name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, 512, f);
    fclose(f);
    return n == 512 ? 0 : -1;
}

static u64 ata(ata_block *b, u8 *out)
{
    switch (b->command) {
    case 0xEC: return load("identify.bin", out) ? 0x80010003 : 0;
    case 0xB0:
        if (b->features == 0xD0) {
            if (load("smart.bin", out)) return 0x80010003;
            if (getenv("PV_ST")) out[363] = (u8)strtol(getenv("PV_ST"), NULL, 16);
            return 0;
        }
        if (b->features == 0xD1) return load("thresh.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD5 && b->lba_low == 6) return load("selftest.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD5 && b->lba_low == 1) return load("errlog.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD4) return 0;
        return 0x80010003;
    case 0x2F:
        if (b->lba_low == 4 && b->lba_mid == 0) { memset(out, 0, 512); out[0] = 1; out[8] = 1; out[9] = 1; return 0; }
        if (b->lba_low == 4 && b->lba_mid == 1) return load("devstat.bin", out) ? 0x80010003 : 0;
        if (b->lba_low == 0x11) return load("phy.bin", out) ? 0x80010003 : 0;
        return 0x80010003;
    }
    return 0x80010003;
}

u64 pv_syscall(int n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g)
{
    (void)d; (void)f; (void)g;
    switch (n) {
    case 600: *(u32 *)(uintptr_t)c = 1; return 0;
    case 601: return 0;
    case 609: {
        u8 *info = (u8 *)(uintptr_t)b;
        u64 sectors = 2000409264ull;
        u32 ss = 512;
        memcpy(info + 0x28, &sectors, 8);
        memcpy(info + 0x30, &ss, 4);
        return 0;
    }
    case 616: return ata((ata_block *)(uintptr_t)c, (u8 *)(uintptr_t)e);
    case 383: {
        if (getenv("PV_383_RC")) return (s32)strtoul(getenv("PV_383_RC"), NULL, 16);   /* a refused call */
        double t = (sim_us - 1000000) / 1e6;
        int rise = getenv("PV_RISE") ? (int)(20 * (1 - exp(-t / 70))) : 0;
        int off = getenv("PV_TEMP_OFF") ? atoi(getenv("PV_TEMP_OFF")) : 0;            /* readings off the 30..90 C graph */
        *(u32 *)(uintptr_t)b = (u32)((a ? 55 + rise : 51 + rise) + off) << 24;
        return 0;
    }
    case 409:
        *(u8 *)(uintptr_t)b = 0;
        *(u8 *)(uintptr_t)c = 1;
        *(u8 *)(uintptr_t)d = getenv("PV_RISE") ? (u8)(0x55 + (sim_us / 1000000) / 3) : 0x55;
        *(u8 *)(uintptr_t)e = 0;
        return 0;
    }
    return 0x80010003;
}

#include "rsx/gcm_sys.h"
static gcmControlRegister ctrl_reg;     /* the stub draws synchronously: GET is always PUT */
gcmControlRegister *gcmGetControlRegister(void) { return &ctrl_reg; }
