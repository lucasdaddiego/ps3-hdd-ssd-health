/* Host stubs for the PS3 Health preview. The app's own sources compile on the
 * Mac against stub headers; this file gives them a world:
 *   - tiny3d: a small software rasterizer into a 1920x1080 float buffer
 *     (convex primitives, colour x A4R4G4B4 texel, alpha test, src-alpha blend,
 *     each pixel blended once per call);
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

#define FW 1920
#define FH 1080
videoResolution Video_Resolution = {1920, 1080};
static float fb[FH][FW][3];
static unsigned stamp[FH][FW], call_id;
static u8 *arena;
static u32 arena_used;
static float vpx, vpy, vsx = 1, vsy = 1;
static int frame;
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

static struct { u32 off, w, h, stride; int fmt, filter; } tex;
typedef struct { float x, y, u, v, r, g, b, a; } vtx;
static vtx vb[400000];
static int nv, ptype, ptex, pending;
static vtx cur;
static u32 ccol = 0xffffffff;
static float cu, cv;

int tiny3d_Init(u32 size)
{
    (void)size;
    const char *r = getenv("PV_RES");
    if (r && !strcmp(r, "720")) { Video_Resolution.width = 1280; Video_Resolution.height = 720; }
    if (r && !strcmp(r, "480")) { Video_Resolution.width = 720; Video_Resolution.height = 480; }
    arena = calloc(1, 160 << 20);
    script_load();
    return 0;
}
void tiny3d_Project2D(void) {}
void *tiny3d_AllocTexture(u32 size)
{
    arena_used = (arena_used + 127) & ~127u;
    if (arena_used + size > (160u << 20)) return NULL;
    void *p = arena + arena_used;
    arena_used += size;
    return p;
}
u32 tiny3d_TextureOffset(void *p) { return (u32)((u8 *)p - arena); }
void tiny3d_SetTextureWrap(int unit, u32 offset, u32 w, u32 h, u32 stride, int fmt, int wu, int wv, int filter)
{
    (void)unit; (void)wu; (void)wv;
    tex.off = offset; tex.w = w; tex.h = h; tex.stride = stride; tex.fmt = fmt; tex.filter = filter;
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
    cur.r = (ccol >> 24 & 255) / 255.0f;
    cur.g = (ccol >> 16 & 255) / 255.0f;
    cur.b = (ccol >> 8 & 255) / 255.0f;
    cur.a = (ccol & 255) / 255.0f;
    cur.u = cu;
    cur.v = cv;
    vb[nv++] = cur;
    pending = 0;
}

int tiny3d_SetPolygon(int type) { ptype = type; nv = 0; ptex = 0; pending = 0; call_id++; return 0; }
void tiny3d_VertexPos(float x, float y, float z) { (void)z; push(); cur.x = x * vsx + vpx; cur.y = y * vsy + vpy; pending = 1; }
void tiny3d_VertexColor(u32 c) { ccol = c; }
void tiny3d_VertexTexture(float u, float v) { cu = u; cv = v; ptex = 1; }

static void texel(int x, int y, float *o)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)tex.w) x = tex.w - 1;
    if (y >= (int)tex.h) y = tex.h - 1;
    u16 t = *(u16 *)(arena + tex.off + y * tex.stride + x * 2);
    o[0] = (t >> 8 & 15) / 15.0f; o[1] = (t >> 4 & 15) / 15.0f; o[2] = (t & 15) / 15.0f; o[3] = (t >> 12 & 15) / 15.0f;
}

static void sample(float u, float v, float *o)
{
    if (tex.filter == 0) { texel((int)floorf(u * tex.w), (int)floorf(v * tex.h), o); return; }
    float fx = u * tex.w - 0.5f, fy = v * tex.h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0, a[4], b[4], c[4], d[4];
    texel(x0, y0, a); texel(x0 + 1, y0, b); texel(x0, y0 + 1, c); texel(x0 + 1, y0 + 1, d);
    for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay;
}

static void raster(const vtx *a, const vtx *b, const vtx *c)
{
    if (!render_on()) return;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f) return;
    int W = Video_Resolution.width, H = Video_Resolution.height;
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (stamp[y][x] == call_id) continue;
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            float col[4] = {w0 * a->r + w1 * b->r + w2 * c->r, w0 * a->g + w1 * b->g + w2 * c->g, w0 * a->b + w1 * b->b + w2 * c->b,
                            w0 * a->a + w1 * b->a + w2 * c->a};
            if (ptex) {
                float t[4];
                sample(w0 * a->u + w1 * b->u + w2 * c->u, w0 * a->v + w1 * b->v + w2 * c->v, t);
                for (int k = 0; k < 4; k++) col[k] *= t[k];
            }
            if (col[3] < 16 / 255.0f) continue;          /* the alpha test */
            stamp[y][x] = call_id;
            for (int k = 0; k < 3; k++) fb[y][x][k] = col[k] * col[3] + fb[y][x][k] * (1 - col[3]);
        }
}

/* PV_VSTAT: tiny3d's vertex memory per frame (16 B position, 4 B colour,
 * 8 B texture per vertex, each polygon padded to 64 B; 1 MB per frame). */
static long vbytes, vmax, polys, pmax, vmax_frame;
static void vstat_exit(void) { if (getenv("PV_VSTAT")) fprintf(stderr, "vstat: max %ld bytes (%ld polygons) per frame, at frame %ld\n", vmax, pmax, vmax_frame); }
void tiny3d_End(void)
{
    push();
    vbytes = (vbytes + nv * (20 + (ptex ? 8 : 0)) + 63) & ~63L;
    polys++;
    switch (ptype) {
    case TINY3D_QUADS:
        for (int i = 0; i + 3 < nv; i += 4) { raster(&vb[i], &vb[i + 1], &vb[i + 2]); raster(&vb[i], &vb[i + 2], &vb[i + 3]); }
        break;
    case TINY3D_TRIANGLES:
        for (int i = 0; i + 2 < nv; i += 3) raster(&vb[i], &vb[i + 1], &vb[i + 2]);
        break;
    case TINY3D_TRIANGLE_FAN:
        for (int i = 1; i + 1 < nv; i++) raster(&vb[0], &vb[i], &vb[i + 1]);
        break;
    case TINY3D_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < nv; i++) raster(&vb[i], &vb[i + 1], &vb[i + 2]);
        break;
    default:
        break;
    }
    nv = 0;
}

void tiny3d_Clear(u32 color, int flags)
{
    (void)flags;
    if (!render_on()) return;
    float r = (color >> 16 & 255) / 255.0f, g = (color >> 8 & 255) / 255.0f, b = (color & 255) / 255.0f;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) { fb[y][x][0] = r; fb[y][x][1] = g; fb[y][x][2] = b; }
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
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++) {
                float v = fb[y][x][k];
                fputc(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255 + 0.5f), f);
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
s32 sysLv2FsRename(const char *a, const char *b) { char p[1024], q[1024]; return rename(map(a, p, sizeof p), map(b, q, sizeof q)) ? -1 : 0; }
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
