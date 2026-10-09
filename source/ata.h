/* Access to the internal drive from a HEN app: LV2 storage syscalls, journaled
 * (journal.h) so that a call that froze the console is never run again, and
 * the report file. */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "smart.h"

#define APP_DIR "/dev_hdd0/tmp/ps3_health"
#define DEMO_DIR APP_DIR "/demo"

typedef struct {
    int info_rc, open_rc, identify_rc;
    uint64_t info_sectors;
    uint32_t info_sector_size;
    uint8_t info_raw[64];
    uint32_t handle;
    int opened;
    int probed;                  /* read in this session (or demo data loaded): the report has a drive section */
    int demo;                    /* sectors came from DEMO_DIR: the drive is never touched */
    int ata_ok;                  /* IDENTIFY came back valid: SMART uses the same path */
    int identify_frozen, smart_frozen, selftest_frozen, selftest_long_frozen, speed_frozen;
    char identify_note[64];
    uint8_t identify[512], smart[512], thresh[512], stlog[512], errlog[512];
    int have_identify, have_smart, have_thresh, have_stlog, have_errlog;
    int dumps_dirty;             /* a read happened since the .bin dumps and smart.when were written */
    int smart_rc, thresh_rc, stlog_rc, selftest_rc, errlog_rc;
    ata_identity id;
    smart_data s;
    selftest_log log;
    error_log elog;
    /* general purpose logs (READ LOG EXT): device statistics 04h and Phy counters 11h */
    uint8_t devstat[512], phylog[512];      /* page 1 of 04h, and 11h: the sectors saved to disk */
    int have_devstat, have_phy, gpl_rc, phy_rc;
    dev_stats ds;
    phy_counters phy;
    int vendor;                  /* VENDOR_* from the model string */
    smart_summary sum;
    /* the last read saved on disk before this start (the delta), and the
     * previous read of this session (rows that changed turn blue) */
    int have_prev, have_last;
    char prev_when[32], prev_model[41];
    smart_data prev, last;
    /* the console temperatures, syscall 383 */
    int cpu_temp, rsx_temp, temps_rc;
    /* speed test: a 64 MB file written and read back through the file system */
    int speed_done, speed_rc;
    double speed_mb, speed_wsec, speed_rsec;
} drive_state;

int usb_find(char *dir, int n);                              /* the first /dev_usb00N: 0, or -1 when none */
void drive_load_prev(drive_state *d);
void console_info(drive_state *d);
extern char console_fw[16];                                 /* "4.93" from version.txt, "" when unknown */
void console_firmware(void);                                /* fills console_fw, once at start */
int console_temps(int *cpu, int *rsx);                        /* syscall 383, journal "temps"; the rc */
int fan_policy_read(int *percent, int *mode);                 /* syscall 409, read-only; journal "fan_policy"; the rc */
/* A file of 64 x 1 MB written and read back in dir, then deleted; the journal name is jname. */
int file_speed_test(const char *dir, const char *jname, double *mb, double *wsec, double *rsec);
void drive_probe(drive_state *d, void (*progress)(const char *msg));
int drive_demo_load(drive_state *d);
int drive_read_smart(drive_state *d, int full);
int drive_start_selftest(drive_state *d, int type);   /* 1 short, 2 extended */
int drive_can_selftest(const drive_state *d);
int drive_speed_test(drive_state *d);
int drive_read_error_log(drive_state *d);
int drive_read_gpl(drive_state *d);
void drive_close(drive_state *d);
void drive_report(drive_state *d, const char *when);       /* the drive section of the report + the .bin dumps */
int compat_body(const drive_state *d, char *out, int n);
int compat_title(const drive_state *d, char *out, int n);
const char *writes_unit_text(int unit);
void hours_text(int hours, char *out, int n);

#endif
