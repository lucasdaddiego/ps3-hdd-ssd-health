/* The report: a header, the console line, then the section of every module
 * that has a result. Written to APP_DIR/report-<date>.txt (demo-report-* when
 * the drive module runs on the demo sectors), copied to a USB stick on SELECT. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ppu-lv2.h>
#include <sys/file.h>
#include <sys/systime.h>
#include "app.h"
#include "report.h"
#include "ui.h"
#include "version.h"

static char paths[2][128];                   /* this session's report, then its demo report */
static const char *report_path = "";
static int report_rc;                        /* the last write: 0, or the LV2 rc */
static char rep[49152];
static int rlen;
static char last[sizeof rep];                /* the last text written, without its header line */
static int last_len = -1;
static const char *last_path;
static char when[32];

void rep_out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rep + rlen, sizeof rep - rlen, fmt, ap);
    va_end(ap);
    if (n > 0) rlen += n < (int)(sizeof rep - rlen) ? n : (int)(sizeof rep - rlen) - 1;
}

const char *rep_when(void) { return when; }

void report_footnote(void)
{
    if (!report_path[0]) return;
    if (report_rc) text_fit(MG, Y_NOTE, F_SMALL, YELLOW, SW - 2 * MG, "Report not written: %s (rc 0x%08x)", report_path, (unsigned)report_rc);
    else text_fit(MG, Y_NOTE, F_SMALL, DIM, SW - 2 * MG, "Report: %s", report_path);
}

void write_report(void)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    time_t t = (time_t)sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min);
    char *p = paths[D.demo != 0];            /* one file per session; demo mode gets its own */
    if (!p[0])
        snprintf(p, sizeof paths[0], APP_DIR "/%sreport-%04d%02d%02d-%02d%02d%02d.txt", D.demo ? "demo-" : "",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    report_path = p;
    rlen = 0;
    rep_out("PS3 Health %s report, %04d-%02d-%02d %02d:%02d:%02d UTC%s\n", APP_VERSION, tm.tm_year + 1900, tm.tm_mon + 1,
            tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
            D.demo ? " (DEMO MODE: drive sectors from files, not from the drive)" : "");
    int body = rlen;
    rep_out("console: firmware %s", console_fw[0] ? console_fw : "unknown");
    if (D.cpu_temp >= 0) rep_out(", Cell %d C, RSX %d C", D.cpu_temp, D.rsx_temp);
    rep_out("\n\n");
    for (int k = 0; k < MODULE_COUNT; k++) {
        int before = rlen;
        modules[k].report();
        if (rlen != before) rep_out("\n");
    }
    /* No new result since the last write: the file keeps its time, and no
     * fsync on the UI thread. A failed write is tried again. */
    if (report_path == last_path && !report_rc && rlen - body == last_len && !memcmp(rep + body, last, last_len)) return;
    last_len = rlen - body;
    memcpy(last, rep + body, last_len);
    last_path = report_path;
    report_rc = fs_write_file(report_path, rep, rlen, 0);
}

/* Copies the last report and the freeze journal to the first USB stick, into
 * ps3_health/: 0, USB_NONE when no stick, else the write rc (-1: short write). */
#define USB_NONE 1
static int report_copy_usb(char *dst, int n)
{
    char dir[48];
    const char *base = strrchr(report_path, '/');
    base = base ? base + 1 : report_path;
    dst[0] = 0;
    if (usb_find(dir, sizeof dir)) return USB_NONE;
    strcat(dir, "/ps3_health");
    sysLv2FsMkdir(dir, 0777);
    snprintf(dst, n, "%s/%s", dir, base);
    int rc = fs_write_file(dst, rep, rlen, 0);
    static char jtext[32768];                /* the freeze journal too: it tells at which step a console froze */
    int jlen = fs_read_file(APP_DIR "/journal.txt", jtext, sizeof jtext);
    if (rc == 0 && jlen > 0) {
        char jdst[64];
        snprintf(jdst, sizeof jdst, "%s/journal.txt", dir);
        fs_write_file(jdst, jtext, (u64)jlen, 0);
    }
    return rc;
}

static char usb_dst[128];
static int usb_rc;
static void job_copy(void) { usb_rc = report_copy_usb(usb_dst, sizeof usb_dst); }

/* The copy runs as a job: a slow stick shows the counter, not a still picture. */
u32 report_to_usb(char *msg, int n)
{
    write_report();                          /* the results of this moment, every module */
    run_job("Copying the report to the USB stick", job_copy);
    if (usb_rc == 0) snprintf(msg, n, "Report copied to %s", usb_dst);
    else if (usb_rc == USB_NONE) snprintf(msg, n, "No USB stick found (/dev_usb000 to 007).");
    else snprintf(msg, n, "Could not write %s (rc 0x%08x)", usb_dst, (unsigned)usb_rc);
    return usb_rc == 0 ? GREEN : YELLOW;
}
