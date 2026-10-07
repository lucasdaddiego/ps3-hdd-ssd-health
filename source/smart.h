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
    int hpa, hpa_enabled;        /* word 82/85 bit 10: Host Protected Area feature set */
    int amac;                    /* word 119 bit 8: Accessible Max Address Configuration */
    int gpl;                     /* word 84 bit 5: General Purpose Logging (READ LOG EXT) */
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
    int ext_minutes;             /* byte 373, or the word at 375 when 373 is 0xFF */
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

/* SMART summary error log (log 01h): the device error count and the newest error. */
typedef struct {
    int version, index;          /* byte 0, byte 1 (1..5 = newest entry, 0 = none) */
    int error_count;             /* bytes 452-453 */
    uint8_t last_command, last_error, last_status;   /* of the newest entry */
    int checksum;
} error_log;

enum { HEALTH_OK, HEALTH_WARN, HEALTH_FAIL };

/* Derived numbers for the summary page. -1 / negative = not available. */
typedef struct {
    int hours, cycles;           /* attributes 9 and 12, raw low 32 bits */
    int life_left, life_id;      /* % left = normalized value of the vendor's life attribute */
    uint64_t writes_raw;
    int writes_id, writes_unit;  /* UNIT_* from vendor.h */
    double tb_written;           /* only when the unit is known */
    int temp_limit;              /* 55 C for an HDD, 65 C for an SSD */
} smart_summary;

#define TEMP_LIMIT_HDD 55
#define TEMP_LIMIT_SSD 65

int ata_identify_order(const uint8_t *buf, const char *model_hint);
int ata_parse_identify(const uint8_t *buf, const char *model_hint, ata_identity *out);
void ata_unswap(uint8_t *buf, int n);
int smart_parse(const uint8_t *data, const uint8_t *thresh, smart_data *out);
int errorlog_parse(const uint8_t *buf, error_log *o);

/* Device statistics log (GPL 04h): page 1 general, page 5 temperature, page 7
 * SSD. Each qword: bit 63 supported, bit 62 valid, bits 47:0 the value.
 * A field is -1 when the drive does not report it. */
typedef struct {
    int have_general, have_temp, have_ssd;    /* pages read and recognised */
    long long power_on_hours, resets;
    long long sectors_written, sectors_read, write_cmds, read_cmds;
    int temp_now, temp_max, temp_min;         /* lifetime extremes */
    int endurance_used;                       /* percentage used endurance indicator, 0..255 */
} dev_stats;
int devstat_pages(const uint8_t *page0, uint8_t *pages, int max);   /* the page list, count */
void devstat_general(const uint8_t *buf, dev_stats *o);
void devstat_temperature(const uint8_t *buf, dev_stats *o);
void devstat_ssd(const uint8_t *buf, dev_stats *o);

/* SATA Phy event counters log (GPL 11h): the ones that explain attribute 199. */
typedef struct {
    long long icrc, crc_h2d, rerr_data, phy_nrdy, comreset, nonfis_errors;   /* -1 = not reported */
    int count;                                 /* counters present in the log */
} phy_counters;
int phy_parse(const uint8_t *buf, phy_counters *o);
int selftest_parse(const uint8_t *buf, selftest_log *out);
const char *smart_attr_name(uint8_t id);
int smart_counter(uint8_t id);
const char *selftest_status_text(uint8_t status_byte);
const char *selftest_type_text(uint8_t type);
int smart_health(const smart_data *s, const selftest_log *log, int temp_limit, char *why, int whylen);
const smart_attr *smart_find(const smart_data *s, uint8_t id);
void smart_summarize(const smart_data *s, int is_ssd, int vendor, smart_summary *o);
int smart_delta(const smart_data *now, const smart_data *prev, uint8_t id, long long *delta);
int sector_checksum(const uint8_t *buf);
int model_match(const char *a, const char *b);
void trim_copy(char *dst, const char *src, int n);
void serial_mask(const char *serial, char *out, int n);

#endif
