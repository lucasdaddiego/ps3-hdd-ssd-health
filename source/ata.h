/* Access to the internal drive from a HEN app: LV2 storage syscalls, a journal
 * that remembers a call that froze the console, and the report file. */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "smart.h"

#define APP_DIR "/dev_hdd0/tmp/hdd_ssd_health"
#define DEMO_DIR APP_DIR "/demo"

/* Journal state of one named call. SKIPPED = declined at the first-run prompt,
 * kept until SQUARE twice clears the journal. */
enum { J_NONE, J_PENDING, J_OK, J_BAD, J_FROZEN, J_SKIPPED };

typedef struct {
    int info_rc, open_rc, identify_rc;
    uint64_t info_sectors;
    uint32_t info_sector_size;
    uint8_t info_raw[64];
    uint32_t handle;
    int opened;
    int demo;                    /* sectors came from DEMO_DIR: the drive is never touched */
    int ata_ok;                  /* IDENTIFY came back valid: SMART uses the same path */
    int identify_frozen, smart_frozen, selftest_frozen, selftest_long_frozen, speed_frozen;
    char identify_note[64];
    uint8_t identify[512], smart[512], thresh[512], stlog[512], errlog[512];
    int have_identify, have_smart, have_thresh, have_stlog, have_errlog;
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
    /* the console: firmware from version.txt, temperatures from syscall 383 */
    char firmware[16];
    int cpu_temp, rsx_temp, temps_rc;
    /* speed test: a 64 MB file written and read back through the file system */
    int speed_done, speed_rc;
    double speed_mb, speed_wsec, speed_rsec;
} drive_state;

int fs_selftest(int *mkdir_rc, int *open_rc, int *write_rc);
void drive_load_prev(drive_state *d);
void console_info(drive_state *d);
void drive_probe(drive_state *d, void (*progress)(const char *msg));
int drive_demo_load(drive_state *d);
int drive_read_smart(drive_state *d, int full);
int drive_start_selftest(drive_state *d, int type);   /* 1 short, 2 extended */
int drive_can_selftest(const drive_state *d);
int drive_speed_test(drive_state *d);
int drive_read_error_log(drive_state *d);
int drive_read_gpl(drive_state *d);
void drive_close(drive_state *d);
void journal_clear(void);
int journal_state(const char *name);
void journal_skip(const char *name);
int report_write(drive_state *d, char *path, int len);
int report_copy_usb(const char *report_path, char *dst, int n);
int compat_body(const drive_state *d, char *out, int n);
int compat_title(const drive_state *d, char *out, int n);
const char *writes_unit_text(int unit);
void hours_text(int hours, char *out, int n);

#endif
