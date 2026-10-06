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

/* ---- journal ------------------------------------------------------------ */

enum { J_NONE, J_PENDING, J_OK, J_BAD, J_FROZEN };
static struct { char name[24]; int state; } jst[32];
static int jn;

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

static int jstart(const char *name)
{
    int *s = jstate(name);
    if (*s == J_FROZEN) return 0;
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
    memset(d, 0, sizeof *d);
    journal_load();
    d->identify_frozen = *jstate("identify") == J_FROZEN;
    d->smart_frozen = *jstate("smart_data") == J_FROZEN;
    d->selftest_frozen = *jstate("selftest_start") == J_FROZEN;

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
    progress("Reading SMART data");
    drive_read_smart(d, 1);
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

/* full = 0: SMART data only (self-test progress); 1: also thresholds and the self-test log. */
int drive_read_smart(drive_state *d, int full)
{
    if (!d->ata_ok) return -1;
    d->smart_frozen = *jstate("smart_data") == J_FROZEN;
    if (d->smart_frozen) return -1;
    d->smart_rc = smart_read(d, "smart_data", 0xD0, 0, d->smart, &d->have_smart);
    if (full) {
        d->thresh_rc = smart_read(d, "smart_thresh", 0xD1, 0, d->thresh, &d->have_thresh);
        d->stlog_rc = smart_read(d, "selftest_log", 0xD5, 0x06, d->stlog, &d->have_stlog);
    }
    if (d->have_smart) smart_parse(d->smart, d->have_thresh ? d->thresh : NULL, &d->s);
    if (d->have_stlog) selftest_parse(d->stlog, &d->log);
    return d->smart_rc;
}

int drive_can_selftest(const drive_state *d)
{
    return d->ata_ok && d->have_smart && !d->selftest_frozen &&
           ((d->s.offline_caps & 0x10) || d->id.selftest_supported);
}

/* SMART EXECUTE OFF-LINE IMMEDIATE, subcommand 1 = short self-test in off-line
 * mode: the command returns at once and the drive tests in the background.
 * Never subcommand 0x81 (captive), which holds the drive until the test ends. */
int drive_start_short_selftest(drive_state *d)
{
    if (!drive_can_selftest(d)) return -1;
    if (!jstart("selftest_start")) { d->selftest_frozen = 1; return -1; }
    d->selftest_rc = ata_command(d, 0xD4, 0, 0x01, 0xB0, PROTO_NON_DATA, 0);
    jdone("selftest_start", d->selftest_rc, d->selftest_rc == 0);
    return d->selftest_rc;
}

void drive_close(drive_state *d)
{
    if (d->opened) storage_close(d->handle);
    d->opened = 0;
}

/* ---- report --------------------------------------------------------------- */

static char rep[16384];
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

int report_write(const drive_state *d, char *path, int len)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    time_t t = (time_t)sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(path, len, APP_DIR "/report-%04d%02d%02d-%02d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    rlen = 0;
    out("HDD/SSD Health report, %04d-%02d-%02d %02d:%02d:%02d UTC\n\n", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
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
        out("\nSMART READ DATA rc=0x%08x, THRESHOLDS rc=0x%08x, SELF-TEST LOG rc=0x%08x%s\n", (unsigned)d->smart_rc,
            (unsigned)d->thresh_rc, (unsigned)d->stlog_rc, d->smart_frozen ? " (froze before: skipped)" : "");
    }
    if (d->have_smart) {
        const smart_data *s = &d->s;
        out("checksums: data %d, thresholds %d\n\n", s->checksum, s->thresh_checksum);
        out("  ID  attribute               value worst thresh  raw\n");
        for (int k = 0; k < s->count; k++) {
            const smart_attr *a = &s->a[k];
            out("  %3u %-22s  %3u   %3u   %3u    %llu (0x%012llx)\n", a->id, smart_attr_name(a->id), a->value,
                a->worst, a->thresh, (unsigned long long)a->raw, (unsigned long long)a->raw);
        }
        out("\ntemperature %d C\n", s->temperature);
        out("self-test now: %s, %d%% left (byte 363 = 0x%02x)\n", selftest_status_text(s->selftest),
            (s->selftest & 0xF) * 10, s->selftest);
        out("short self-test about %u min; off-line capabilities 0x%02x\n", s->short_minutes, s->offline_caps);
        char why[256];
        int h = smart_health(s, d->have_stlog ? &d->log : NULL, why, sizeof why);
        out("health: %s (%s)\n", h == HEALTH_OK ? "OK" : h == HEALTH_WARN ? "WARN" : "FAIL", why);
    }
    if (d->have_stlog) {
        out("\nself-test log (newest first), checksum %d:\n", d->log.checksum);
        for (int k = 0; k < d->log.count; k++) {
            const selftest_entry *e = &d->log.e[k];
            out("  %-20s %-26s at %u h, LBA 0x%08x\n", selftest_type_text(e->type), selftest_status_text(e->status),
                e->hours, e->lba);
        }
        if (!d->log.count) out("  empty\n");
    }
    if (d->selftest_rc) out("\nself-test start rc=0x%08x\n", (unsigned)d->selftest_rc);
    write_file(path, rep, rlen, 0);
    if (d->have_identify) write_file(APP_DIR "/identify.bin", d->identify, 512, 0);
    if (d->have_smart) write_file(APP_DIR "/smart.bin", d->smart, 512, 0);
    if (d->have_thresh) write_file(APP_DIR "/thresh.bin", d->thresh, 512, 0);
    if (d->have_stlog) write_file(APP_DIR "/selftest.bin", d->stlog, 512, 0);
    write_file(APP_DIR "/devinfo.bin", d->info_raw, sizeof d->info_raw, 0);
    return rlen;
}
