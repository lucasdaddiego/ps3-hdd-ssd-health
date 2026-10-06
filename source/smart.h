/* Decoders for ATA IDENTIFY DEVICE, SMART READ DATA / THRESHOLDS and the
 * SMART self-test log. Pure C, no PS3 headers: test/test_smart.c builds this
 * file on the Mac. Every buffer is the raw 512-byte sector from the drive. */
#ifndef SMART_H
#define SMART_H

#include <stdint.h>

typedef struct {
    char model[41], serial[21], firmware[9];
    int swapped;                 /* 1: the words came back big-endian */
    uint64_t sectors;
    uint32_t logical_size;
    int lba48, smart_supported, smart_enabled, selftest_supported;
    int trim, drat, rzat, dsm_max_blocks;
    int sata_max_gen, sata_cur_gen;  /* 1 = 1.5, 2 = 3.0, 3 = 6.0 Gb/s, 0 = unknown */
    int rotation;                /* word 217: 1 = SSD, 0 = not reported, else rpm */
    int checksum;                /* 1 ok, 0 bad, -1 not present */
} ata_identity;

typedef struct {
    uint8_t id, value, worst, thresh;
    uint16_t flags;
    uint64_t raw;
} smart_attr;

typedef struct {
    int count;
    smart_attr a[30];
    uint8_t selftest;            /* byte 363: status << 4 | remaining tenths */
    uint8_t offline_caps;        /* byte 367: bit 4 = self-tests implemented */
    uint8_t short_minutes;       /* byte 372 */
    int temperature;             /* attribute 194 or 190, -1 if none */
    int checksum, thresh_checksum;
} smart_data;

typedef struct {
    uint8_t type, status;        /* type: 0x01 short, 0x02 extended, 0x81/0x82 captive */
    uint16_t hours;
    uint32_t lba;
} selftest_entry;

typedef struct {
    int count;                   /* newest first */
    selftest_entry e[21];
    int checksum;
} selftest_log;

enum { HEALTH_OK, HEALTH_WARN, HEALTH_FAIL };

int ata_identify_order(const uint8_t *buf, const char *model_hint);
int ata_parse_identify(const uint8_t *buf, const char *model_hint, ata_identity *out);
void ata_unswap(uint8_t *buf, int n);
int smart_parse(const uint8_t *data, const uint8_t *thresh, smart_data *out);
int selftest_parse(const uint8_t *buf, selftest_log *out);
const char *smart_attr_name(uint8_t id);
int smart_counter(uint8_t id);
const char *selftest_status_text(uint8_t status_byte);
const char *selftest_type_text(uint8_t type);
int smart_health(const smart_data *s, const selftest_log *log, char *why, int whylen);
int sector_checksum(const uint8_t *buf);
int model_match(const char *a, const char *b);
void trim_copy(char *dst, const char *src, int n);
void serial_mask(const char *serial, char *out, int n);

#endif
