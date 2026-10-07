/* Internal-drive access through the LV2 storage syscalls (numbers from RPCS3's
 * lv2.cpp): 600 open, 601 close, 609 get_device_info and 616
 * execute_device_command with LV1 command 2 (SEND_ATA_COMMAND). Files use the
 * sysLv2Fs* syscalls: the sysFs* calls are stubs for the cellFs module, which
 * an app must load first.
 *
 * Found on HFW 4.93 + PS3HEN 3.6.0: syscall 604 with LV1 command 0x22 (the
 * HV-built IDENTIFY) froze the whole console; 616 with the 32-byte block below
 * works for IDENTIFY, SMART reads and the short self-test. Other firmware may
 * behave differently, so every call that reaches the drive is journaled first:
 * journal.txt gets "start X" (synced) before the call and "done X" after it. A
 * "start" without a "done" means the console froze inside X; the next start
 * writes "froze X" and never runs X again. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ppu-lv2.h>
#include <sys/file.h>
#include <sys/systime.h>
#include "ata.h"
#include "vendor.h"
#include "version.h"

#define ATA_HDD        0x0101000000000007ULL
#define LV1_ATA        2        /* LV1_STORAGE_SEND_ATA_COMMAND */
#define PROTO_NON_DATA 0
#define PROTO_PIO_IN   1
#define DIR_READ       1
#define JOURNAL        APP_DIR "/journal.txt"

/* Linux drivers/block/ps3disk.c struct lv1_ata_cmnd_block. Only the first 32
 * bytes go to LV2, the same cut as the BD drive's 56-byte ATAPI block: buffer
 * and arglen stay for LV2 to fill with its own bounce buffer. */
typedef struct {
    uint16_t features, sector_count, lba_low, lba_mid, lba_high;
    uint8_t device, command;
    uint32_t is_ext, proto, in_out, size;
    uint32_t pad;
    uint64_t buffer;
    uint32_t arglen, pad2;
} __attribute__((packed)) ata_block;

static uint8_t buf[512] __attribute__((aligned(128)));

static s32 storage_open(u64 dev, u32 *handle) { lv2syscall4(600, dev, 0, (u64)handle, 0); return_to_user_prog(s32); }
static s32 storage_close(u32 handle) { lv2syscall1(601, handle); return_to_user_prog(s32); }
static s32 storage_get_device_info(u64 dev, void *info) { lv2syscall2(609, dev, (u64)info); return_to_user_prog(s32); }
static s32 storage_execute_device_command(u32 h, u64 cmd, void *in, u64 inlen, void *out, u64 outlen, u64 *status)
{
    lv2syscall7(616, h, cmd, (u64)in, inlen, (u64)out, outlen, (u64)status);
    return_to_user_prog(s32);
}
/* 383 sys_game_get_temperature: 0 = Cell, 1 = RSX; degrees in the top byte. */
static s32 get_temperature(u32 id, u32 *t) { lv2syscall2(383, id, (u64)t); return_to_user_prog(s32); }

/* ---- journal ------------------------------------------------------------ */

static struct { char name[24]; int state; } jst[32];
static int jn, jloaded;

static int *jstate(const char *name)
{
    for (int i = 0; i < jn; i++)
        if (!strcmp(jst[i].name, name)) return &jst[i].state;
    if (jn == 32) return &jst[31].state;
    snprintf(jst[jn].name, sizeof jst[jn].name, "%s", name);
    jst[jn].state = J_NONE;
    return &jst[jn++].state;
}

static void write_file(const char *path, const void *data, u64 len, int append)
{
    s32 fd;
    u64 w;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (append ? SYS_O_APPEND : SYS_O_TRUNC), &fd, 0666, NULL, 0)) return;
    sysLv2FsWrite(fd, data, len, &w);
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
}

/* Reads up to n bytes; returns the count, -1 when the file is missing. */
static int read_file(const char *path, void *data, int n)
{
    s32 fd;
    u64 r = 0;
    if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0)) return -1;
    sysLv2FsRead(fd, data, n, &r);
    sysLv2FsClose(fd);
    return (int)r;
}

static void jappend(const char *fmt, const char *name, s32 rc, int ok)
{
    char line[96];
    snprintf(line, sizeof line, fmt, name, (unsigned)rc, ok);
    write_file(JOURNAL, line, strlen(line), 1);
}

/* Reads the tail of the journal (the last 32 KB), so a long session cannot
 * push the newest "start"/"done" pair past the end of the buffer; the cut
 * line at the head of the tail is dropped. */
static void journal_load(void)
{
    static char text[32768];
    s32 fd;
    u64 n = 0, size = 0, pos = 0;
    char *first = text;
    jn = 0;
    jloaded = 1;
    if (sysLv2FsOpen(JOURNAL, SYS_O_RDONLY, &fd, 0, NULL, 0)) return;
    if (sysLv2FsLSeek64(fd, 0, SEEK_END, &size) == 0 && size > sizeof text - 1) {
        sysLv2FsLSeek64(fd, size - (sizeof text - 1), SEEK_SET, &pos);
        first = NULL;                    /* the first line of the tail is cut */
    } else {
        sysLv2FsLSeek64(fd, 0, SEEK_SET, &pos);
    }
    sysLv2FsRead(fd, text, sizeof text - 1, &n);
    sysLv2FsClose(fd);
    text[n] = 0;
    if (!first) {
        first = strchr(text, '\n');
        first = first ? first + 1 : text + n;
    }
    for (char *line = strtok(first, "\n"); line; line = strtok(NULL, "\n")) {
        char verb[8], name[24];
        int ok = 0;
        if (sscanf(line, "%7s %23s", verb, name) != 2) continue;
        char *okp = strstr(line, "ok=");
        if (okp) ok = okp[3] == '1';
        int *s = jstate(name);
        if (!strcmp(verb, "start")) *s = J_PENDING;
        else if (!strcmp(verb, "done") && *s != J_FROZEN) *s = ok ? J_OK : J_BAD;
        else if (!strcmp(verb, "froze")) *s = J_FROZEN;
        else if (!strcmp(verb, "skip")) *s = J_SKIPPED;
    }
    for (int i = 0; i < jn; i++)
        if (jst[i].state == J_PENDING) {
            jst[i].state = J_FROZEN;
            jappend("froze %s\n", jst[i].name, 0, 0);
        }
    if (size > sizeof text / 2) {    /* compact: one line per call, the frozen ones kept */
        char line[64];
        text[0] = 0;
        for (int i = 0; i < jn; i++) {
            if (jst[i].state == J_FROZEN) snprintf(line, sizeof line, "froze %s\n", jst[i].name);
            else if (jst[i].state == J_SKIPPED) snprintf(line, sizeof line, "skip %s\n", jst[i].name);
            else snprintf(line, sizeof line, "done %s rc=0x00000000 ok=%d\n", jst[i].name, jst[i].state == J_OK);
            strcat(text, line);
        }
        write_file(JOURNAL, text, strlen(text), 0);
    }
}

/* The clear gesture: an empty journal, so the next start runs every call again. */
void journal_clear(void)
{
    write_file(JOURNAL, "", 0, 0);
    jn = 0;
}

int journal_state(const char *name)
{
    if (!jloaded) journal_load();
    return *jstate(name);
}

/* The first-run prompt was declined: remembered until the journal is cleared. */
void journal_skip(const char *name)
{
    jappend("skip %s\n", name, 0, 0);
    *jstate(name) = J_SKIPPED;
}

static int jstart(const char *name)
{
    int *s = jstate(name);
    if (*s == J_FROZEN || *s == J_SKIPPED) return 0;
    jappend("start %s\n", name, 0, 0);
    *s = J_PENDING;
    return 1;
}

static void jdone(const char *name, s32 rc, int ok)
{
    jappend("done %s rc=0x%08x ok=%d\n", name, rc, ok);
    *jstate(name) = ok ? J_OK : J_BAD;
}

/* File self-test before anything reaches the drive: the journal is the only
 * record of a freeze, so drive_probe must not run when writes fail. */
int fs_selftest(int *mkdir_rc, int *open_rc, int *write_rc)
{
    s32 fd;
    u64 w = 0;
    *mkdir_rc = sysLv2FsMkdir(APP_DIR, 0777);
    *write_rc = -1;
    *open_rc = sysLv2FsOpen(APP_DIR "/starts.txt", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0);
    if (*open_rc) return -1;
    *write_rc = sysLv2FsWrite(fd, "start\n", 6, &w);
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
    return *write_rc == 0 && w == 6 ? 0 : -1;
}

/* ---- drive calls ---------------------------------------------------------- */

/* One ATA command: syscall 616, LV1 command 2, the 32-byte block. */
static s32 ata_command(drive_state *d, uint16_t features, uint16_t count, uint16_t lba_low,
                       uint8_t command, uint32_t proto, uint32_t len)
{
    ata_block blk;
    memset(&blk, 0, sizeof blk);
    blk.features = features;
    blk.sector_count = count;
    blk.lba_low = lba_low;
    if (command == 0xB0) { blk.lba_mid = 0x4F; blk.lba_high = 0xC2; }   /* SMART key */
    blk.command = command;
    blk.proto = proto;
    blk.in_out = DIR_READ;
    blk.size = blk.arglen = len;
    u64 status = 0;
    memset(buf, 0, sizeof buf);
    return storage_execute_device_command(d->handle, LV1_ATA, &blk, 32, buf, len, &status);
}

/* READ LOG EXT (2Fh), one 512-byte page of a general purpose log. 48-bit: the
 * page number's low byte goes in LBA 15:8, its high byte in LBA 47:40 (the
 * HOB byte of lba_high, if the block's 16-bit fields map current|previous). */
static s32 read_log_ext(drive_state *d, uint8_t log, uint16_t page)
{
    ata_block blk;
    memset(&blk, 0, sizeof blk);
    blk.sector_count = 1;
    blk.lba_low = log;
    blk.lba_mid = page & 0xff;
    blk.lba_high = (uint16_t)((page >> 8) << 8);
    blk.device = 0x40;
    blk.command = 0x2F;
    blk.is_ext = 1;
    blk.proto = PROTO_PIO_IN;
    blk.in_out = DIR_READ;
    blk.size = blk.arglen = 512;
    u64 status = 0;
    memset(buf, 0, sizeof buf);
    s32 rc = storage_execute_device_command(d->handle, LV1_ATA, &blk, 32, buf, 512, &status);
    if (rc == 0 && d->id.swapped) ata_unswap(buf, 512);
    return rc;
}

/* A valid IDENTIFY: data, a printable model, and a good checksum when the drive signs it. */
static int identify_ok(drive_state *d)
{
    int nonzero = 0;
    for (int i = 0; i < 512; i++) nonzero |= buf[i];
    if (!nonzero) { snprintf(d->identify_note, sizeof d->identify_note, "returned 0 but no data"); return 0; }
    ata_identity id;
    if (ata_parse_identify(buf, NULL, &id)) { snprintf(d->identify_note, sizeof d->identify_note, "no model string"); return 0; }
    if (id.checksum == 0) { snprintf(d->identify_note, sizeof d->identify_note, "IDENTIFY checksum bad"); return 0; }
    snprintf(d->identify_note, sizeof d->identify_note, "IDENTIFY ok");
    return 1;
}

static void do_devinfo(drive_state *d)
{
    static uint8_t info[128] __attribute__((aligned(16)));
    memset(info, 0, sizeof info);
    d->info_rc = storage_get_device_info(ATA_HDD, info);
    memcpy(d->info_raw, info, sizeof d->info_raw);
    if (d->info_rc == 0) {
        memcpy(&d->info_sectors, info + 0x28, 8);        /* RPCS3 StorageDeviceInfo, big-endian = native */
        memcpy(&d->info_sector_size, info + 0x30, 4);
    }
}

void drive_probe(drive_state *d, void (*progress)(const char *msg))
{
    int have_prev = d->have_prev;
    smart_data prev = d->prev;
    char prev_when[sizeof d->prev_when], prev_model[sizeof d->prev_model];
    memcpy(prev_when, d->prev_when, sizeof prev_when);
    memcpy(prev_model, d->prev_model, sizeof prev_model);
    memset(d, 0, sizeof *d);
    d->have_prev = have_prev;
    d->prev = prev;
    memcpy(d->prev_when, prev_when, sizeof prev_when);
    memcpy(d->prev_model, prev_model, sizeof prev_model);
    d->cpu_temp = d->rsx_temp = -1;
    journal_load();
    d->identify_frozen = *jstate("identify") == J_FROZEN;
    d->smart_frozen = *jstate("smart_data") == J_FROZEN;
    d->selftest_frozen = *jstate("selftest_start") == J_FROZEN;
    d->selftest_long_frozen = *jstate("selftest_long_start") == J_FROZEN;
    d->speed_frozen = *jstate("speed_file") == J_FROZEN;

    progress("Reading the drive size (syscall 609)");
    if (jstart("devinfo")) { do_devinfo(d); jdone("devinfo", d->info_rc, d->info_rc == 0); }
    progress("Opening the drive (syscall 600)");
    if (!jstart("open")) { d->open_rc = 0x7FFFFFFF; return; }
    d->open_rc = storage_open(ATA_HDD, &d->handle);
    d->opened = d->open_rc == 0;
    jdone("open", d->open_rc, d->opened);
    if (!d->opened) return;

    if (d->identify_frozen) {
        snprintf(d->identify_note, sizeof d->identify_note, "froze the console before: skipped");
        return;
    }
    progress("IDENTIFY DEVICE (syscall 616)");
    jstart("identify");
    d->identify_rc = ata_command(d, 0, 1, 0, 0xEC, PROTO_PIO_IN, 512);
    d->ata_ok = d->identify_rc == 0 && identify_ok(d);
    if (d->identify_rc) snprintf(d->identify_note, sizeof d->identify_note, "refused");
    jdone("identify", d->identify_rc, d->ata_ok);
    if (!d->ata_ok) return;
    memcpy(d->identify, buf, 512);
    d->have_identify = 1;
    ata_parse_identify(d->identify, NULL, &d->id);
    d->vendor = vendor_detect(d->id.model);
    if (d->have_prev && d->prev_model[0] && !model_match(d->prev_model, d->id.model))
        d->have_prev = 0;                    /* another drive: its old counters mean nothing here */
    progress("Reading SMART data");
    drive_read_smart(d, 1);
}

/* The last read saved before this start: the baseline of the delta column.
 * Runs before drive_probe, which keeps these fields. */
void drive_load_prev(drive_state *d)
{
    static uint8_t prev[512];
    char when[96];
    int n;
    d->have_prev = 0;
    d->prev_model[0] = 0;
    if (read_file(APP_DIR "/smart.bin", prev, 512) != 512) return;
    if (smart_parse(prev, NULL, &d->prev)) return;
    n = read_file(APP_DIR "/smart.when", when, sizeof when - 1);
    if (n < 0) n = 0;
    when[n] = 0;
    for (char *c = when; *c; c++) if (*c == '\n' || *c == '\r') *c = 0;
    char *sep = strstr(when, " | ");
    if (sep) { *sep = 0; snprintf(d->prev_model, sizeof d->prev_model, "%s", sep + 3); }
    snprintf(d->prev_when, sizeof d->prev_when, "%s", when[0] ? when : "an earlier run");
    d->have_prev = 1;
}

/* Demo mode: the four sectors from DEMO_DIR stand in for the drive. */
int drive_demo_load(drive_state *d)
{
    memset(d, 0, sizeof *d);
    d->cpu_temp = d->rsx_temp = -1;
    d->demo = 1;
    if (read_file(DEMO_DIR "/identify.bin", d->identify, 512) != 512) return -1;
    if (read_file(DEMO_DIR "/smart.bin", d->smart, 512) != 512) return -1;
    d->have_thresh = read_file(DEMO_DIR "/thresh.bin", d->thresh, 512) == 512;
    d->have_stlog = read_file(DEMO_DIR "/selftest.bin", d->stlog, 512) == 512;
    d->have_errlog = read_file(DEMO_DIR "/errlog.bin", d->errlog, 512) == 512;
    if (ata_parse_identify(d->identify, NULL, &d->id)) return -1;
    d->have_identify = d->have_smart = d->ata_ok = 1;
    d->vendor = vendor_detect(d->id.model);
    snprintf(d->identify_note, sizeof d->identify_note, "DEMO: sectors from %s", DEMO_DIR);
    smart_parse(d->smart, d->have_thresh ? d->thresh : NULL, &d->s);
    if (d->have_stlog) selftest_parse(d->stlog, &d->log);
    if (d->have_errlog) errorlog_parse(d->errlog, &d->elog);
    if (read_file(DEMO_DIR "/devstat.bin", d->devstat, 512) == 512) { d->have_devstat = 1; devstat_general(d->devstat, &d->ds); }
    if (read_file(DEMO_DIR "/phy.bin", d->phylog, 512) == 512) d->have_phy = phy_parse(d->phylog, &d->phy) == 0;
    smart_summarize(&d->s, d->id.rotation == 1, d->vendor, &d->sum);
    return 0;
}

/* Firmware from /dev_flash/vsh/etc/version.txt ("release:04.9300:") and the
 * Cell and RSX temperatures (syscall 383, journaled like the drive commands).
 * (The IDPS, syscall 870, for the region: EPERM on HEN, so not used.) */
void console_info(drive_state *d)
{
    char text[256];
    int n = read_file("/dev_flash/vsh/etc/version.txt", text, sizeof text - 1);
    if (!jloaded) journal_load();            /* demo mode comes here without drive_probe */
    d->firmware[0] = 0;
    d->cpu_temp = d->rsx_temp = -1;
    if (n > 0) {
        text[n] = 0;
        char *r = strstr(text, "release:");
        unsigned major = 0;
        char minor[8] = "";
        if (r && sscanf(r + 8, "%u.%4[0-9]", &major, minor) == 2)
            snprintf(d->firmware, sizeof d->firmware, "%u.%.2s", major, minor);
    }
    if (jstart("temps")) {
        u32 t = 0;
        d->temps_rc = get_temperature(0, &t);
        if (d->temps_rc == 0) d->cpu_temp = (int)(t >> 24);
        if (d->temps_rc == 0 && get_temperature(1, &t) == 0) d->rsx_temp = (int)(t >> 24);
        jdone("temps", d->temps_rc, d->temps_rc == 0);
    }
}

/* Writes a 64 MB file (64 000 000 bytes) in APP_DIR in 1 MB pieces, reads it
 * back, deletes it.
 * Both go through the LV2 file system, the path games use for their data, so
 * no new syscall. (Raw sector reads, syscall 602, come back ENXIO on HEN.) */
int drive_speed_test(drive_state *d)
{
    static uint8_t sbuf[1000000] __attribute__((aligned(128)));   /* 64 x 1 MB = 64 MB, as shown */
    static const char *p = APP_DIR "/speed.tmp";
    const int pieces = 64;
    if (d->demo) return -1;
    d->speed_frozen = *jstate("speed_file") == J_FROZEN;
    if (d->speed_frozen) return -1;
    if (!jstart("speed_file")) return -1;
    for (unsigned i = 0; i < sizeof sbuf; i += 4) *(uint32_t *)(sbuf + i) = i * 2654435761u;
    s32 fd = -1, rc;
    u64 n = 0, t0 = 0, n0 = 0, t1 = 0, n1 = 0, t2 = 0, n2 = 0;
    d->speed_done = 0;
    d->speed_mb = (double)pieces * sizeof sbuf / 1e6;
    sysGetCurrentTime(&t0, &n0);
    rc = sysLv2FsOpen(p, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0);
    for (int k = 0; rc == 0 && k < pieces; k++) {
        rc = sysLv2FsWrite(fd, sbuf, sizeof sbuf, &n);
        if (rc == 0 && n != sizeof sbuf) rc = -1;
    }
    if (fd >= 0) { sysLv2FsFsync(fd); sysLv2FsClose(fd); fd = -1; }
    sysGetCurrentTime(&t1, &n1);
    if (rc == 0) rc = sysLv2FsOpen(p, SYS_O_RDONLY, &fd, 0, NULL, 0);
    for (int k = 0; rc == 0 && k < pieces; k++) {
        rc = sysLv2FsRead(fd, sbuf, sizeof sbuf, &n);
        if (rc == 0 && n != sizeof sbuf) rc = -1;
    }
    if (fd >= 0) sysLv2FsClose(fd);
    sysGetCurrentTime(&t2, &n2);
    sysLv2FsUnlink(p);
    d->speed_wsec = (double)(t1 - t0) + (double)((s64)n1 - (s64)n0) / 1e9;
    d->speed_rsec = (double)(t2 - t1) + (double)((s64)n2 - (s64)n1) / 1e9;
    d->speed_rc = rc;
    d->speed_done = rc == 0;
    jdone("speed_file", rc, d->speed_done);
    return rc;
}

static int smart_read(drive_state *d, const char *name, uint8_t features, uint16_t lba_low, uint8_t *dst, int *have)
{
    if (!jstart(name)) return 0x7FFFFFFF;
    s32 rc = ata_command(d, features, 1, lba_low, 0xB0, PROTO_PIO_IN, 512);
    jdone(name, rc, rc == 0);
    if (rc == 0) {
        memcpy(dst, buf, 512);
        if (d->id.swapped) ata_unswap(dst, 512);
        *have = 1;
    }
    return rc;
}

/* SMART READ LOG 01h, the summary error log. Same path as the self-test log. */
int drive_read_error_log(drive_state *d)
{
    if (!d->ata_ok || d->demo) return -1;
    d->errlog_rc = smart_read(d, "error_log", 0xD5, 0x01, d->errlog, &d->have_errlog);
    if (d->have_errlog) errorlog_parse(d->errlog, &d->elog);
    return d->errlog_rc;
}

/* The general purpose logs, behind the first-run prompt. Device statistics:
 * page 0 lists the pages, then 1 (general), 5 (temperature), 7 (SSD) when
 * listed; page 1 is kept as devstat.bin. Then the Phy counters log 11h. */
int drive_read_gpl(drive_state *d)
{
    if (!d->ata_ok || d->demo || !d->id.gpl) return -1;
    memset(&d->ds, 0, sizeof d->ds);
    if (jstart("gpl_devstat")) {
        uint8_t pages[32];
        int n = 0;
        d->gpl_rc = read_log_ext(d, 0x04, 0);
        if (d->gpl_rc == 0) n = devstat_pages(buf, pages, 32);
        for (int i = 0; i < n && d->gpl_rc == 0; i++) {
            if (pages[i] != 1 && pages[i] != 5 && pages[i] != 7) continue;
            d->gpl_rc = read_log_ext(d, 0x04, pages[i]);
            if (d->gpl_rc) break;
            if (pages[i] == 1) { memcpy(d->devstat, buf, 512); d->have_devstat = 1; devstat_general(buf, &d->ds); }
            else if (pages[i] == 5) devstat_temperature(buf, &d->ds);
            else devstat_ssd(buf, &d->ds);
        }
        jdone("gpl_devstat", d->gpl_rc, d->gpl_rc == 0 && d->have_devstat);
    }
    if (jstart("gpl_phy")) {
        d->phy_rc = read_log_ext(d, 0x11, 0);
        if (d->phy_rc == 0) {
            memcpy(d->phylog, buf, 512);
            d->have_phy = phy_parse(buf, &d->phy) == 0;
        }
        jdone("gpl_phy", d->phy_rc, d->have_phy);
    }
    return d->gpl_rc ? d->gpl_rc : d->phy_rc;
}

/* full = 0: SMART data only (self-test progress); 1: also thresholds and the self-test log. */
int drive_read_smart(drive_state *d, int full)
{
    if (!d->ata_ok || d->demo) return -1;
    d->smart_frozen = *jstate("smart_data") == J_FROZEN;
    if (d->smart_frozen) return -1;
    if (d->have_smart) { d->last = d->s; d->have_last = 1; }
    d->smart_rc = smart_read(d, "smart_data", 0xD0, 0, d->smart, &d->have_smart);
    if (full) {
        d->thresh_rc = smart_read(d, "smart_thresh", 0xD1, 0, d->thresh, &d->have_thresh);
        d->stlog_rc = smart_read(d, "selftest_log", 0xD5, 0x06, d->stlog, &d->have_stlog);
    }
    if (d->have_smart) {
        smart_parse(d->smart, d->have_thresh ? d->thresh : NULL, &d->s);
        smart_summarize(&d->s, d->id.rotation == 1, d->vendor, &d->sum);
    }
    if (d->have_stlog) selftest_parse(d->stlog, &d->log);
    return d->smart_rc;
}

int drive_can_selftest(const drive_state *d)
{
    return d->ata_ok && !d->demo && d->have_smart && !d->selftest_frozen &&
           ((d->s.offline_caps & 0x10) || d->id.selftest_supported);
}

/* SMART EXECUTE OFF-LINE IMMEDIATE, subcommand 1 = short, 2 = extended
 * self-test, both in off-line mode: the command returns at once and the drive
 * tests in the background. Never the captive forms (0x81, 0x82), which hold
 * the drive until the test ends. Each type has its own journal entry. */
int drive_start_selftest(drive_state *d, int type)
{
    const char *name = type == 2 ? "selftest_long_start" : "selftest_start";
    if (!drive_can_selftest(d)) return -1;
    if (type == 2 && d->selftest_long_frozen) return -1;
    if (!jstart(name)) { if (type == 2) d->selftest_long_frozen = 1; else d->selftest_frozen = 1; return -1; }
    d->selftest_rc = ata_command(d, 0xD4, 0, type == 2 ? 0x02 : 0x01, 0xB0, PROTO_NON_DATA, 0);
    jdone(name, d->selftest_rc, d->selftest_rc == 0);
    return d->selftest_rc;
}

void drive_close(drive_state *d)
{
    if (d->opened) storage_close(d->handle);
    d->opened = 0;
}

/* ---- report --------------------------------------------------------------- */

static char rep[32768];
static int rlen;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rep + rlen, sizeof rep - rlen, fmt, ap);
    va_end(ap);
    if (n > 0) rlen += n < (int)(sizeof rep - rlen) ? n : (int)(sizeof rep - rlen) - 1;
}

static const char *yesno(int v) { return v ? "yes" : "no"; }

const char *writes_unit_text(int unit)
{
    return unit == UNIT_LBA ? "512-byte LBAs" : unit == UNIT_GIB ? "GiB" : unit == UNIT_MIB32 ? "32 MiB units" : "unit unknown for this vendor";
}

void hours_text(int hours, char *out, int n)
{
    if (hours < 0) snprintf(out, n, "not reported");
    else if (hours >= 8760) snprintf(out, n, "%d h (%d y %d d)", hours, hours / 8760, hours % 8760 / 24);
    else snprintf(out, n, "%d h (%d d)", hours, hours / 24);
}

static const char *health_text(int h) { return h == HEALTH_OK ? "OK" : h == HEALTH_WARN ? "WARN" : "FAIL"; }

/* The changed attributes since prev: "9 +3, 12 +2". 0 when nothing changed. */
static int delta_text(const smart_data *now, const smart_data *prev, char *out, int n)
{
    int j = 0, changed = 0;
    out[0] = 0;
    for (int i = 0; i < now->count; i++) {
        long long dl;
        if (!smart_delta(now, prev, now->a[i].id, &dl) || !dl) continue;
        changed++;
        if (j < n) j += snprintf(out + j, n - j, "%s%u %+lld", j ? ", " : "", now->a[i].id, dl);
    }
    return changed;
}

int compat_title(const drive_state *d, char *out, int n)
{
    return snprintf(out, n, "Compat: %s on %s", d->have_identify ? d->id.model : "drive not read",
                    d->firmware[0] ? d->firmware : "unknown firmware");
}

static int app(char *out, int n, int j, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int app(char *out, int n, int j, const char *fmt, ...)
{
    if (j >= n - 1) return n - 1;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + j, n - j, fmt, ap);
    va_end(ap);
    if (k < 0) return j;
    return j + k < n ? j + k : n - 1;
}

static const char *drive_type_text(const ata_identity *i)
{
    return i->rotation == 1 ? "SSD" : i->rotation > 0x400 && i->rotation < 0xFFFF ? "HDD" : "type ?";
}

/* One line of the compatibility report: firmware, drive, result or health
 * (the same ids as the fields of .github/ISSUE_TEMPLATE/compat-report.yml).
 * Nothing that identifies the console or the drive: no serial number. */
static int compat_field(const drive_state *d, const char *id, char *out, int n)
{
    int j = 0;
    out[0] = 0;
    if (!strcmp(id, "firmware")) {
        j = app(out, n, j, "%s", d->firmware[0] ? d->firmware : "unknown");
        j = app(out, n, j, ", app %s%s", APP_VERSION, d->demo ? " demo mode" : "");
    } else if (!strcmp(id, "drive")) {
        if (!d->have_identify) return app(out, n, j, "not read");
        const ata_identity *i = &d->id;
        j = app(out, n, j, "%s, fw %s, %.0f GB, %s, SATA %d (max %d)", i->model, i->firmware,
                (double)i->sectors * i->logical_size / 1e9, drive_type_text(i), i->sata_cur_gen, i->sata_max_gen);
    } else if (!strcmp(id, "result")) {
        j = app(out, n, j, "609 rc 0x%x, 600 rc 0x%x, IDENTIFY %s, SMART %s, log %s", (unsigned)d->info_rc,
                (unsigned)d->open_rc, d->identify_frozen ? "froze" : d->ata_ok ? "ok" : "refused",
                d->smart_frozen ? "froze" : d->have_smart ? "ok" : "no", d->have_stlog ? "ok" : "no");
        if (d->have_stlog && d->log.count)
            j = app(out, n, j, ", last self-test %s at %u h", selftest_status_text(d->log.e[0].status), d->log.e[0].hours);
        if (d->speed_done)
            j = app(out, n, j, ", file write %.0f / read %.0f MB/s", d->speed_mb / (d->speed_wsec > 0 ? d->speed_wsec : 1),
                    d->speed_mb / (d->speed_rsec > 0 ? d->speed_rsec : 1));
        else if (d->speed_frozen) j = app(out, n, j, ", speed test froze");
        else if (d->speed_rc) j = app(out, n, j, ", speed test rc 0x%x", (unsigned)d->speed_rc);
        if (d->have_errlog) j = app(out, n, j, ", error log %d", d->elog.error_count);
        else if (d->errlog_rc) j = app(out, n, j, ", error log rc 0x%x", (unsigned)d->errlog_rc);
        if (d->have_devstat || d->have_phy) j = app(out, n, j, ", GPL logs %s/%s", d->have_devstat ? "stats ok" : "stats no",
                                                     d->have_phy ? "phy ok" : "phy no");
        else if (d->gpl_rc) j = app(out, n, j, ", GPL rc 0x%x", (unsigned)d->gpl_rc);
    } else if (!strcmp(id, "health")) {
        if (!d->have_smart) return app(out, n, j, "no SMART data");
        char why[256], h[48];
        int hl = smart_health(&d->s, d->have_stlog ? &d->log : NULL, d->sum.temp_limit, why, sizeof why);
        hours_text(d->sum.hours, h, sizeof h);
        j = app(out, n, j, "%s (%s)\nhours %s, cycles %d, temp %d C\n", health_text(hl), why, h, d->sum.cycles,
                d->s.temperature);
        static const uint8_t ids[] = {5, 187, 196, 197, 198, 199};
        for (unsigned k = 0; k < sizeof ids; k++) {
            const smart_attr *a = smart_find(&d->s, ids[k]);
            if (a) j = app(out, n, j, "%u=%llu ", ids[k], (unsigned long long)(a->raw & 0xFFFFFFFFull));
        }
    }
    return j;
}

/* The same four fields as text, for the report and the share page. */
int compat_body(const drive_state *d, char *out, int n)
{
    static const char *ids[] = {"firmware", "drive", "result", "health"};
    char v[512];
    int j = 0;
    j = app(out, n, j, "console: CECH-____, HEN/CFW ____ (you fill these in)\n");
    for (unsigned k = 0; k < 4; k++) {
        compat_field(d, ids[k], v, sizeof v);
        j = app(out, n, j, "%s: %s\n", ids[k], v);
    }
    return j;
}

int report_write(drive_state *d, char *path, int len)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    time_t t = (time_t)sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char when[32];
    snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
    snprintf(path, len, APP_DIR "/%sreport-%04d%02d%02d-%02d%02d%02d.txt", d->demo ? "demo-" : "", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    rlen = 0;
    out("HDD/SSD Health %s report, %04d-%02d-%02d %02d:%02d:%02d UTC%s\n\n", APP_VERSION, tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, d->demo ? " (DEMO MODE: sectors from files, not from the drive)" : "");
    out("console: firmware %s, Cell %d C, RSX %d C (383 rc 0x%08x)\n", d->firmware[0] ? d->firmware : "unknown",
        d->cpu_temp, d->rsx_temp, (unsigned)d->temps_rc);
    out("609 device info: rc=0x%08x sectors=%llu sector size=%u\n", (unsigned)d->info_rc,
        (unsigned long long)d->info_sectors, d->info_sector_size);
    out("600 open:        rc=0x%08x\n", (unsigned)d->open_rc);
    out("616 IDENTIFY:    rc=0x%08x %s\n", (unsigned)d->identify_rc, d->identify_note);
    if (d->have_identify) {
        const ata_identity *i = &d->id;
        char serial[sizeof i->serial];
        serial_mask(i->serial, serial, sizeof serial);
        out("\nmodel %s\nserial %s (masked; identify.bin has it in full)\nfirmware %s\n", i->model, serial, i->firmware);
        out("capacity %.1f GB (%llu sectors of %u bytes, LBA48 %s)\n",
            (double)i->sectors * i->logical_size / 1e9, (unsigned long long)i->sectors, i->logical_size, yesno(i->lba48));
        if (i->rotation == 1) out("type SSD\n");
        else if (i->rotation > 0x400 && i->rotation < 0xFFFF) out("type HDD, %d rpm\n", i->rotation);
        else out("type not reported\n");
        out("SATA gen max %d, now %d (1 = 1.5, 2 = 3, 3 = 6 Gb/s)\n", i->sata_max_gen, i->sata_cur_gen);
        out("TRIM %s (max DSM blocks %d), DRAT %s, RZAT %s\n", yesno(i->trim), i->dsm_max_blocks, yesno(i->drat), yesno(i->rzat));
        out("SMART supported %s, enabled %s, self-test %s\n", yesno(i->smart_supported), yesno(i->smart_enabled),
            yesno(i->selftest_supported));
        out("IDENTIFY checksum %d, word order %s\n", i->checksum, i->swapped ? "swapped" : "raw");
        out("vendor table: %s\n", vendor_get(d->vendor)->name);
        out("\nSMART READ DATA rc=0x%08x, THRESHOLDS rc=0x%08x, SELF-TEST LOG rc=0x%08x%s\n", (unsigned)d->smart_rc,
            (unsigned)d->thresh_rc, (unsigned)d->stlog_rc, d->smart_frozen ? " (froze before: skipped)" : "");
    }
    if (d->have_smart) {
        const smart_data *s = &d->s;
        out("checksums: data %d, thresholds %d\n\n", s->checksum, s->thresh_checksum);
        out("  ID  attribute                   value worst thresh  raw                          since last report\n");
        for (int k = 0; k < s->count; k++) {
            const smart_attr *a = &s->a[k];
            long long dl = 0;
            char ds[24] = "";
            if (d->have_prev && smart_delta(s, &d->prev, a->id, &dl) && dl) snprintf(ds, sizeof ds, "%+lld", dl);
            out("  %3u %-26s  %3u   %3u   %3u    %-14llu (0x%012llx) %s\n", a->id, vendor_attr_name(d->vendor, a->id),
                a->value, a->worst, a->thresh, (unsigned long long)a->raw, (unsigned long long)a->raw, ds);
        }
        char ht[48], dtext[512];
        hours_text(d->sum.hours, ht, sizeof ht);
        out("\npower-on %s, power cycles %d\n", ht, d->sum.cycles);
        if (d->sum.life_left >= 0) out("life left %d%% (attribute %d)\n", d->sum.life_left, d->sum.life_id);
        else out("life left: no known attribute for this vendor\n");
        if (d->sum.writes_id && d->sum.tb_written >= 0)
            out("host writes %.2f TB (attribute %d raw %llu, %s)\n", d->sum.tb_written, d->sum.writes_id,
                (unsigned long long)d->sum.writes_raw, writes_unit_text(d->sum.writes_unit));
        else if (d->sum.writes_id)
            out("host writes: attribute %d raw %llu (%s)\n", d->sum.writes_id, (unsigned long long)d->sum.writes_raw,
                writes_unit_text(d->sum.writes_unit));
        else out("host writes: not reported\n");
        if (d->have_prev) {
            int changed = delta_text(s, &d->prev, dtext, sizeof dtext);
            out("since %s: %s\n", d->prev_when, changed ? dtext : "no change");
        } else out("since last report: no previous read on this console\n");
        if (d->speed_done)
            out("speed test: write %.1f MB/s, read %.1f MB/s (%.0f MB file in " APP_DIR ", %.2f s + %.2f s)\n",
                d->speed_mb / (d->speed_wsec > 0 ? d->speed_wsec : 1), d->speed_mb / (d->speed_rsec > 0 ? d->speed_rsec : 1),
                d->speed_mb, d->speed_wsec, d->speed_rsec);
        else if (d->speed_rc) out("speed test: failed, rc 0x%08x\n", (unsigned)d->speed_rc);
        out("\ntemperature %d C (warning at %d)\n", s->temperature, d->sum.temp_limit);
        if ((s->selftest >> 4) == 0xF)
            out("self-test now: running, %d%% done (byte 363 = 0x%02x)\n", 100 - (s->selftest & 0xF) * 10, s->selftest);
        else out("self-test now: %s (byte 363 = 0x%02x)\n", selftest_status_text(s->selftest), s->selftest);
        out("short self-test about %u min, extended about %d min; off-line capabilities 0x%02x\n", s->short_minutes,
            s->ext_minutes, s->offline_caps);
        char why[256];
        int h = smart_health(s, d->have_stlog ? &d->log : NULL, d->sum.temp_limit, why, sizeof why);
        out("health: %s (%s)\n", health_text(h), why);
    }
    if (d->have_stlog) {
        if (d->have_errlog)
            out("ATA error log (01h): %d errors, version %d, index %d, checksum %d%s\n", d->elog.error_count, d->elog.version,
                d->elog.index, d->elog.checksum, d->elog.index ? "" : ", no entry");
        else if (d->errlog_rc) out("ATA error log (01h): rc 0x%08x\n", (unsigned)d->errlog_rc);
        if (d->have_errlog && d->elog.index)
            out("  newest error: command 0x%02x, error register 0x%02x, status 0x%02x\n", d->elog.last_command,
                d->elog.last_error, d->elog.last_status);
        if (d->ds.have_general) {
            out("device statistics (GPL 04h page 1): power-on hours %lld, resets %lld, sectors written %lld (%.2f TB), "
                "sectors read %lld, write commands %lld, read commands %lld\n", d->ds.power_on_hours, d->ds.resets,
                d->ds.sectors_written, d->ds.sectors_written >= 0 ? (double)d->ds.sectors_written * d->id.logical_size / 1e12 : -1.0,
                d->ds.sectors_read, d->ds.write_cmds, d->ds.read_cmds);
            if (d->ds.have_temp) out("  temperature (page 5): now %d, lifetime max %d, min %d (-999 = not reported)\n",
                                      d->ds.temp_now, d->ds.temp_max, d->ds.temp_min);
            if (d->ds.have_ssd) out("  SSD (page 7): percentage used endurance indicator %d (some firmware reports the remaining percent)\n",
                                    d->ds.endurance_used);
        } else if (d->gpl_rc) out("device statistics (GPL 04h): rc 0x%08x\n", (unsigned)d->gpl_rc);
        if (d->have_phy)
            out("SATA Phy counters (GPL 11h, %d counters): ICRC %lld, CRC in H2D FIS %lld, R_ERR data %lld, "
                "R_ERR non-data %lld, PhyRdy drops %lld, resets %lld (-1 = not reported)\n", d->phy.count, d->phy.icrc,
                d->phy.crc_h2d, d->phy.rerr_data, d->phy.nonfis_errors, d->phy.phy_nrdy, d->phy.comreset);
        else if (d->phy_rc) out("SATA Phy counters (GPL 11h): rc 0x%08x\n", (unsigned)d->phy_rc);
        out("GPL supported %s, HPA feature %s%s, AMAC %s\n", yesno(d->id.gpl), yesno(d->id.hpa),
            d->id.hpa_enabled ? " (enabled)" : "", yesno(d->id.amac));
        out("\nself-test log (newest first), checksum %d:\n", d->log.checksum);
        for (int k = 0; k < d->log.count; k++) {
            const selftest_entry *e = &d->log.e[k];
            out("  %-20s %-26s at %u h, LBA 0x%08x\n", selftest_type_text(e->type), selftest_status_text(e->status),
                e->hours, e->lba);
        }
        if (!d->log.count) out("  empty\n");
    }
    if (d->selftest_rc) out("\nself-test start rc=0x%08x\n", (unsigned)d->selftest_rc);
    {
        char body[1024];
        compat_body(d, body, sizeof body);
        out("\n--- compatibility report (paste into a GitHub issue) ---\n%s", body);
    }
    write_file(path, rep, rlen, 0);
    if (d->demo) return rlen;                /* demo sectors never replace the real dumps */
    if (d->have_identify) write_file(APP_DIR "/identify.bin", d->identify, 512, 0);
    if (d->have_smart) {
        write_file(APP_DIR "/smart.bin", d->smart, 512, 0);
        char tag[96];
        snprintf(tag, sizeof tag, "%s | %s", when, d->id.model);
        write_file(APP_DIR "/smart.when", tag, strlen(tag), 0);
    }
    if (d->have_thresh) write_file(APP_DIR "/thresh.bin", d->thresh, 512, 0);
    if (d->have_stlog) write_file(APP_DIR "/selftest.bin", d->stlog, 512, 0);
    if (d->have_errlog) write_file(APP_DIR "/errlog.bin", d->errlog, 512, 0);
    if (d->have_devstat) write_file(APP_DIR "/devstat.bin", d->devstat, 512, 0);
    if (d->have_phy) write_file(APP_DIR "/phy.bin", d->phylog, 512, 0);
    write_file(APP_DIR "/devinfo.bin", d->info_raw, sizeof d->info_raw, 0);
    return rlen;
}

/* Copies the last report to the first USB stick found, into hdd_ssd_health/. */
int report_copy_usb(const char *report_path, char *dst, int n)
{
    const char *base = strrchr(report_path, '/');
    base = base ? base + 1 : report_path;
    for (int u = 0; u < 8; u++) {
        char dir[48];
        sysFSStat st;
        snprintf(dir, sizeof dir, "/dev_usb%03d", u);
        if (sysLv2FsStat(dir, &st)) continue;
        snprintf(dir, sizeof dir, "/dev_usb%03d/hdd_ssd_health", u);
        sysLv2FsMkdir(dir, 0777);
        snprintf(dst, n, "%s/%s", dir, base);
        s32 fd;
        u64 w = 0;
        if (sysLv2FsOpen(dst, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0)) return -2;
        sysLv2FsWrite(fd, rep, rlen, &w);
        sysLv2FsFsync(fd);
        sysLv2FsClose(fd);
        /* the freeze journal too: it tells at which step a console froze */
        static char jtext[32768];
        int jn = read_file(JOURNAL, jtext, sizeof jtext);
        if (jn > 0) {
            char jdst[64];
            snprintf(jdst, sizeof jdst, "%s/journal.txt", dir);
            write_file(jdst, jtext, (u64)jn, 0);
        }
        return w == (u64)rlen ? 0 : -3;
    }
    dst[0] = 0;
    return -1;
}
