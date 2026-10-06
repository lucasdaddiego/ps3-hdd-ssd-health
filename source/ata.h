/* Access to the internal drive from a HEN app: LV2 storage syscalls, a journal
 * that remembers a call that froze the console, and the report file. */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "smart.h"

#define APP_DIR "/dev_hdd0/tmp/hdd_ssd_health"

typedef struct {
    int info_rc, open_rc, identify_rc;
    uint64_t info_sectors;
    uint32_t info_sector_size;
    uint8_t info_raw[64];
    uint32_t handle;
    int opened;
    int ata_ok;                  /* IDENTIFY came back valid: SMART uses the same path */
    int identify_frozen, smart_frozen, selftest_frozen;
    char identify_note[48];
    uint8_t identify[512], smart[512], thresh[512], stlog[512];
    int have_identify, have_smart, have_thresh, have_stlog;
    int smart_rc, thresh_rc, stlog_rc, selftest_rc;
    ata_identity id;
    smart_data s;
    selftest_log log;
} drive_state;

int fs_selftest(int *mkdir_rc, int *open_rc, int *write_rc);
void drive_probe(drive_state *d, void (*progress)(const char *msg));
int drive_read_smart(drive_state *d, int full);
int drive_start_short_selftest(drive_state *d);
int drive_can_selftest(const drive_state *d);
void drive_close(drive_state *d);
void journal_clear(void);
int report_write(const drive_state *d, char *path, int len);

#endif
