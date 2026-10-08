/* The Memory module: pattern tests on the user memory (XDR) the app can get,
 * and on the RSX memory (GDDR3) through its mapping. The app writes only to
 * memory it allocated itself. RSX memory comes from tiny3d's allocator, a
 * bump pointer over the local memory that gives nothing back: the blocks
 * stay with the app until it exits, and a second test reuses them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <tiny3d.h>
#include "app.h"
#include "ui.h"
#include "report.h"

#define BLOCK (1024 * 1024)
#define MAX_BLOCKS 256
#define MAX_ERR 8

typedef struct {
    int done, blocks, passes, cancelled;
    long errors;
    uintptr_t err_addr[MAX_ERR];
    uint32_t err_want[MAX_ERR], err_got[MAX_ERR];
} mem_result;

static mem_result xdr, vram;
static void *blk[MAX_BLOCKS];

static void note_error(mem_result *r, uintptr_t addr, uint32_t want, uint32_t got)
{
    if (r->errors < MAX_ERR) {
        r->err_addr[r->errors] = addr;
        r->err_want[r->errors] = want;
        r->err_got[r->errors] = got;
    }
    r->errors++;
}

/* Pattern p of a word at index i in block b: address based, its inverse, an
 * alternating word, and a linear congruential sequence. */
static uint32_t pattern(int p, int b, unsigned i)
{
    uint32_t a = ((uint32_t)b << 20) + i * 4;
    switch (p) {
    case 0: return a ^ 0xA5A5A5A5u;
    case 1: return ~(a ^ 0xA5A5A5A5u);
    case 2: return (i & 1) ? 0x55555555u : 0xAAAAAAAAu;
    default: return (a + 0x9E3779B9u) * 2654435761u;
    }
}

/* The user memory is written and compared in place. The RSX mapping goes
 * through 64 KB chunks: a pattern chunk is built in user memory and copied
 * in, and the block is copied out before the compare, because one word at a
 * time on that mapping is far slower than a memcpy. */
#define CHUNK 65536
static uint32_t chunk[CHUNK / 4] __attribute__((aligned(128)));
static uint32_t readback[CHUNK / 4] __attribute__((aligned(128)));

static void write_block(int p, int b, int direct)
{
    if (direct) {
        uint32_t *w = blk[b];
        for (unsigned i = 0; i < BLOCK / 4; i++) w[i] = pattern(p, b, i);
        return;
    }
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        for (unsigned i = 0; i < CHUNK / 4; i++) chunk[i] = pattern(p, b, off / 4 + i);
        memcpy((uint8_t *)blk[b] + off, chunk, CHUNK);
    }
}

static void check_block(mem_result *r, int p, int b, int direct)
{
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        const uint32_t *w = direct ? (const uint32_t *)((uint8_t *)blk[b] + off) : readback;
        if (!direct) memcpy(readback, (uint8_t *)blk[b] + off, CHUNK);
        for (unsigned i = 0; i < CHUNK / 4; i++) {
            uint32_t want = pattern(p, b, off / 4 + i);
            if (w[i] != want) note_error(r, (uintptr_t)blk[b] + off + i * 4, want, w[i]);
        }
    }
}

static void run_passes(mem_result *r, int passes, int direct)
{
    int total = passes * r->blocks * 2, step = 0;
    for (int p = 0; p < passes && !job_cancel; p++) {
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            write_block(p, b, direct);
            job_set_percent(step * 100 / total);
        }
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            check_block(r, p, b, direct);
            job_set_percent(step * 100 / total);
        }
        if (!job_cancel) r->passes = p + 1;
    }
    r->cancelled = job_cancel;
}

static void job_xdr(void)
{
    memset(&xdr, 0, sizeof xdr);
    progress("Allocating the user memory in 1 MB blocks");
    int n = 0;
    while (n < MAX_BLOCKS && (blk[n] = malloc(BLOCK)) != NULL) n++;
    for (int k = 0; k < 8 && n > 0; k++) free(blk[--n]);     /* headroom for the app itself */
    xdr.blocks = n;
    progress("Testing the user memory (XDR): 4 patterns, each written then verified");
    run_passes(&xdr, 4, 1);
    for (int b = 0; b < n; b++) free(blk[b]);
    xdr.done = 1;
}

static void *vblk[MAX_BLOCKS];
static int vn = -1;

static void job_vram(void)
{
    memset(&vram, 0, sizeof vram);
    progress("Taking the free RSX memory in 1 MB blocks (kept until the app exits)");
    if (vn < 0) {
        vn = 0;
        while (vn < MAX_BLOCKS && (vblk[vn] = tiny3d_AllocTexture(BLOCK)) != NULL) vn++;
    }
    memcpy(blk, vblk, sizeof blk);
    vram.blocks = vn;
    progress("Testing the RSX memory (GDDR3) through its mapping: 2 patterns, slow by design");
    run_passes(&vram, 2, 0);
    vram.done = 1;
}

/* One test: its button, what it does, and its result. */
static void mem_card(float x, float y, float w, float h, unsigned btn, const char *name, const char *desc, const mem_result *r)
{
    test_card(x, y, w, h, btn, name, desc);
    float ry = y + TEST_RESULT_Y;
    if (!r->done) { text(x + 32, ry, F_MED, DIM, "Not tested"); return; }
    if (r->errors) text(x + 32, ry, F_MED, RED, "%ld error%s", r->errors, r->errors == 1 ? "" : "s");
    else text(x + 32, ry, F_MED, r->cancelled ? YELLOW : GREEN, "%s", r->cancelled ? "Stopped, no error so far" : "No errors");
    text(x + 32, ry + 52, F_BODY, GREY, "%d MB in 1 MB blocks, %d pass%s", r->blocks, r->passes, r->passes == 1 ? "" : "es");
    for (int k = 0; k < r->errors && k < MAX_ERR && k < 3 && ry + 96 + k * 32 < y + h - 30; k++)
        text_fit(x + 32, ry + 96 + k * 32, F_SMALL, RED, w - 64, "at 0x%08lx: wrote 0x%08x, read 0x%08x", (unsigned long)r->err_addr[k],
                 r->err_want[k], r->err_got[k]);
}

static void tile_line(char *out, int n, const char *name, const mem_result *r)
{
    if (!r->done) snprintf(out, n, "%s not tested", name);
    else snprintf(out, n, "%s %d MB: %s%ld errors", name, r->blocks, r->cancelled ? "stopped, " : "", r->errors);
}

/* An error is red; a stopped test is no pass, yellow. */
static void mem_tile(void)
{
    char l1[48], l2[48];
    if (!xdr.done && !vram.done) return;
    long errs = (xdr.done ? xdr.errors : 0) + (vram.done ? vram.errors : 0);
    int stopped = (xdr.done && xdr.cancelled) || (vram.done && vram.cancelled);
    tile_line(l1, sizeof l1, "XDR", &xdr);
    tile_line(l2, sizeof l2, "RSX", &vram);
    state_set("memory", errs ? DOT_BAD : stopped ? DOT_WARN : DOT_OK, l1, l2);
}

int mod_mem_open(void)
{
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { mem_tile(); return back; }
        if (pressed & BTN_CROSS) run_job_cancelable("Memory test", job_xdr);
        if (pressed & BTN_TRIANGLE) run_job_cancelable("Memory test", job_vram);
        begin_frame();
        title();
        float w = (SW - 2 * MG - 32) / 2, h = BOTTOM - TOP - 56 > 520 ? 520 : BOTTOM - TOP - 56;
        mem_card(MG, TOP, w, h, BTN_CROSS, "User memory (XDR)",
                 "Takes all the user memory the app can get, about 200 MB of the 256, writes four patterns and reads each "
                 "back. About a minute. The rest belongs to the system.", &xdr);
        mem_card(MG + w + 32, TOP, w, h, BTN_TRIANGLE, "RSX memory (GDDR3)",
                 "The same on the free graphics memory through its slow mapping, two patterns. A few minutes. The blocks "
                 "stay taken until the app exits.", &vram);
        text_fit(MG, TOP + h + 24, F_BODY, GREY, SW - 2 * MG, "%s",
                 "Both tests write only to memory the app allocated. A wrong word is a memory error. CIRCLE during a test stops it.");
        footer("CROSS test the user memory  TRIANGLE test the RSX memory  CIRCLE home  START exit");
        ui_flip();
    }
}

static void report_result(const mem_result *r, const char *name)
{
    if (!r->done) return;
    rep_out("%s: %d MB in 1 MB blocks, %d passes%s, %ld errors\n", name, r->blocks, r->passes, r->cancelled ? " (stopped)" : "",
            r->errors);
    for (int k = 0; k < r->errors && k < MAX_ERR; k++)
        rep_out("  error at 0x%08lx: wrote 0x%08x, read 0x%08x\n", (unsigned long)r->err_addr[k], r->err_want[k], r->err_got[k]);
}

void mod_mem_report(void)
{
    if (!xdr.done && !vram.done) return;
    rep_out("--- memory ---\n");
    report_result(&xdr, "user memory (XDR)");
    report_result(&vram, "RSX memory (GDDR3)");
}
