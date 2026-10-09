/* The host world of the preview, for every app on the frame. The app's own
 * sources compile on the Mac against stub headers; this file gives them a
 * world:
 *   - the video output (PV_RES) and the end of each frame (pv_flip), for
 *     gfx_soft.c, the preview's back end of the app's renderer;
 *   - the pad: a script of frames and buttons (PV_SCRIPT);
 *   - time: 1/60 s per flip;
 *   - files: /dev_* paths inside a sandbox (PV_ROOT);
 *   - the network, offline, and the LV2 syscalls, refused: both weak, so that
 *     an app's own file replaces them (Health: pv_app.c, the drive from a
 *     console's sector dumps, 383 and 409).
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
#include "pv.h"

static videoResolution res = {1920, 1080};
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

/* ---- frames --------------------------------------------------------------- */

void pv_start(void)
{
    const char *r = getenv("PV_RES");
    if (r && !strcmp(r, "720")) { res.width = 1280; res.height = 720; }
    if (r && !strcmp(r, "480")) { res.width = 720; res.height = 480; }
    script_load();
}

int pv_drawing(void) { return drawing; }

static void snap(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s.ppm", getenv("PV_OUT") ? getenv("PV_OUT") : ".", name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    int W = res.width, H = res.height;
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

void pv_flip(void)
{
    int quit_now = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].f0 != frame) continue;
        if (ev[i].snap[0]) snap(ev[i].snap);
        if (ev[i].exit_) quit_now = 1;
    }
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
    s->displayMode.resolution = res.height == 1080 ? VIDEO_RESOLUTION_1080 : res.height == 720 ? VIDEO_RESOLUTION_720 : VIDEO_RESOLUTION_480;
    s->displayMode.scanMode = VIDEO_SCANMODE_PROGRESSIVE;
    s->displayMode.aspect = VIDEO_ASPECT_16_9;
    s->displayMode.refreshRates = 1 | 4;
    return 0;
}
s32 videoGetResolution(s32 id, videoResolution *r) { (void)id; *r = res; return 0; }

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

/* ---- network: offline, weak: an app's own file can replace it ------------ */

__attribute__((weak)) s32 netInitialize(void) { return 0; }
__attribute__((weak)) s32 netDeinitialize(void) { return 0; }
__attribute__((weak)) s32 netSocket(s32 d, s32 t, s32 p) { (void)d; (void)t; (void)p; return -1; }
__attribute__((weak)) s32 netConnect(s32 s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return -1; }
__attribute__((weak)) s32 netClose(s32 s) { (void)s; return 0; }
__attribute__((weak)) s32 netBind(s32 s, const struct sockaddr *a, socklen_t l) { (void)s; (void)a; (void)l; return -1; }
__attribute__((weak)) s32 netListen(s32 s, s32 b) { (void)s; (void)b; return -1; }
__attribute__((weak)) s32 netAccept(s32 s, const struct sockaddr *a, socklen_t *l) { (void)s; (void)a; (void)l; return -1; }
__attribute__((weak)) s32 netSetSockOpt(s32 s, s32 lv, s32 o, const void *v, socklen_t l) { (void)s; (void)lv; (void)o; (void)v; (void)l; return 0; }
__attribute__((weak)) ssize_t netSend(s32 s, const void *b, size_t l, s32 f) { (void)s; (void)b; (void)l; (void)f; return -1; }
__attribute__((weak)) ssize_t netRecv(s32 s, void *b, size_t l, s32 f) { (void)s; (void)b; (void)l; (void)f; return -1; }
__attribute__((weak)) struct net_hostent *netGetHostByName(const char *n) { (void)n; return NULL; }
__attribute__((weak)) s32 netCtlInit(void) { return 0; }
__attribute__((weak)) void netCtlTerm(void) {}
__attribute__((weak)) s32 netCtlGetInfo(s32 code, union net_ctl_info *i) { (void)code; snprintf(i->ip_address, sizeof i->ip_address, "192.168.1.20"); return 0; }
s32 sysModuleLoad(u32 id) { (void)id; return 0; }
in_addr_t inet_addr(const char *cp) { unsigned a, b, c, d; return sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? (a << 24 | b << 16 | c << 8 | d) : (in_addr_t)-1; }

/* ---- syscalls: refused ---------------------------------------------------- */

/* Every LV2 syscall the app makes lands here (stub/ppu-lv2.h). This default
 * refuses them all, so an app with no syscalls still links; an app's own file
 * defines the strong one (Health: pv_app.c, the drive, 383 and 409). */
__attribute__((weak)) u64 pv_syscall(int n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g)
{
    (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 0x80010003;                       /* not supported */
}
