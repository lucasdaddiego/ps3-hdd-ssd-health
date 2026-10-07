/* Decoders for IDENTIFY DEVICE, SMART data and the self-test log (ACS-3 layouts).
 * The drive sends IDENTIFY as little-endian 16-bit words, and ATA strings hold
 * two characters per word, high byte first. SMART sectors are byte arrays. */
#include <stdio.h>
#include <string.h>
#include "smart.h"
#include "vendor.h"

static unsigned word(const uint8_t *b, int i, int swapped)
{
    return swapped ? (unsigned)(b[2 * i] << 8 | b[2 * i + 1]) : (unsigned)(b[2 * i] | b[2 * i + 1] << 8);
}

int sector_checksum(const uint8_t *buf)
{
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += buf[i];
    return sum & 0xff;
}

void trim_copy(char *dst, const char *src, int n)
{
    int a = 0, z = n;
    while (a < z && src[a] == ' ') a++;
    while (z > a && (src[z - 1] == ' ' || src[z - 1] == 0)) z--;
    for (int i = a; i < z; i++) dst[i - a] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : '?';
    dst[z - a] = 0;
}

/* The report keeps the first two and the last two characters of the serial
 * number: enough to tell two drives apart, not enough to identify one. */
void serial_mask(const char *serial, char *out, int n)
{
    int len = strlen(serial), j = 0;
    for (int i = 0; i < len && j < n - 1; i++)
        out[j++] = (len > 6 && (i < 2 || i >= len - 2)) ? serial[i] : '*';
    out[j] = 0;
}

static void ata_string(const uint8_t *b, int first, int nwords, int swapped, char *out)
{
    char raw[64];
    for (int k = 0; k < nwords; k++) {
        unsigned w = word(b, first + k, swapped);
        raw[2 * k] = (char)(w >> 8);
        raw[2 * k + 1] = (char)(w & 0xff);
    }
    trim_copy(out, raw, 2 * nwords);
}

/* Lower-case alphanumerics only, so "DHI-SSD-V800S1TB  " == "dhissdv800s1tb". */
static void squash(const char *s, char *out, int n)
{
    int j = 0;
    for (; *s && j < n - 1; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
    }
    out[j] = 0;
}

int model_match(const char *a, const char *b)
{
    char x[48], y[48];
    squash(a, x, sizeof x);
    squash(b, y, sizeof y);
    return *x && !strcmp(x, y);
}

/* 0 = raw little-endian words (what the drive sends), 1 = already swapped.
 * The signature 0xA5 in the low byte of word 255 decides; without it, a known
 * model string (model_hint) decides; without both, assume raw. */
int ata_identify_order(const uint8_t *buf, const char *model_hint)
{
    if (buf[510] == 0xA5) return 0;
    if (buf[511] == 0xA5) return 1;
    if (model_hint && *model_hint) {
        char m[41];
        for (int sw = 0; sw < 2; sw++) {
            ata_string(buf, 27, 20, sw, m);
            if (model_match(model_hint, m)) return sw;
        }
    }
    return 0;
}

void ata_unswap(uint8_t *buf, int n)
{
    for (int i = 0; i + 1 < n; i += 2) { uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
}

/* Words 82-84 count only when word 83 bits 15:14 = 01, words 85-87 when word 87 does. */
#define VALID(w) (((w) & 0xC000) == 0x4000)

int ata_parse_identify(const uint8_t *buf, const char *model_hint, ata_identity *o)
{
    memset(o, 0, sizeof *o);
    int sw = o->swapped = ata_identify_order(buf, model_hint);
#define W(i) word(buf, i, sw)
    ata_string(buf, 10, 10, sw, o->serial);
    ata_string(buf, 23, 4, sw, o->firmware);
    ata_string(buf, 27, 20, sw, o->model);
    unsigned w83 = W(83), w87 = W(87);
    o->lba48 = VALID(w83) && (w83 & (1 << 10));
    if (o->lba48)
        o->sectors = (uint64_t)W(100) | (uint64_t)W(101) << 16 | (uint64_t)W(102) << 32 | (uint64_t)W(103) << 48;
    else
        o->sectors = (uint64_t)W(60) | (uint64_t)W(61) << 16;
    unsigned w106 = W(106);
    o->logical_size = 512;
    if (VALID(w106) && (w106 & (1 << 12)))
        o->logical_size = (W(117) | W(118) << 16) * 2;
    o->smart_supported = VALID(w83) && (W(82) & 1);
    o->smart_enabled = VALID(w87) && (W(85) & 1);
    o->selftest_supported = (VALID(w83) && (W(84) & 2)) || (VALID(w87) && (W(87) & 2));
    o->drat = (W(69) >> 14) & 1;
    o->rzat = (W(69) >> 5) & 1;
    o->trim = W(169) & 1;
    o->dsm_max_blocks = W(105);
    unsigned w76 = W(76), w77 = W(77);
    if (w76 && w76 != 0xFFFF) {
        o->sata_max_gen = (w76 & 8) ? 3 : (w76 & 4) ? 2 : (w76 & 2) ? 1 : 0;
        if (w77 != 0xFFFF) o->sata_cur_gen = (w77 >> 1) & 7;
    }
    o->rotation = W(217);
    o->hpa = VALID(w83) && (W(82) & (1 << 10));
    o->hpa_enabled = VALID(w87) && (W(85) & (1 << 10));
    unsigned w119 = W(119);
    o->amac = VALID(w119) && (w119 & (1 << 8));
    o->gpl = VALID(w83) && (W(84) & (1 << 5));
    o->checksum = (W(255) & 0xff) == 0xA5 ? sector_checksum(buf) == 0 : -1;
#undef W
    return o->model[0] ? 0 : -1;
}

int smart_parse(const uint8_t *data, const uint8_t *thresh, smart_data *o)
{
    memset(o, 0, sizeof *o);
    o->temperature = -1;
    for (int i = 0; i < 30; i++) {
        const uint8_t *e = data + 2 + i * 12;
        if (!e[0]) continue;
        smart_attr *a = &o->a[o->count++];
        a->id = e[0];
        a->flags = e[1] | e[2] << 8;
        a->value = e[3];
        a->worst = e[4];
        for (int k = 5; k >= 0; k--) a->raw = a->raw << 8 | e[5 + k];
        if (thresh)
            for (int j = 0; j < 30; j++)
                if (thresh[2 + j * 12] == a->id) { a->thresh = thresh[2 + j * 12 + 1]; break; }
    }
    for (int i = 0; i < o->count && o->temperature < 0; i++)
        if (o->a[i].id == 194) o->temperature = o->a[i].raw & 0xff;
    for (int i = 0; i < o->count && o->temperature < 0; i++)
        if (o->a[i].id == 190) o->temperature = o->a[i].raw & 0xff;
    o->selftest = data[363];
    o->offline_caps = data[367];
    o->short_minutes = data[372];
    o->checksum = sector_checksum(data) == 0;
    o->thresh_checksum = thresh ? sector_checksum(thresh) == 0 : -1;
    return o->count ? 0 : -1;
}

/* 21 descriptors of 24 bytes from byte 2; byte 508 = index of the newest (1-based, 0 = empty). */
int selftest_parse(const uint8_t *buf, selftest_log *o)
{
    memset(o, 0, sizeof *o);
    o->checksum = sector_checksum(buf) == 0;
    int idx = buf[508];
    if (idx < 1 || idx > 21) return 0;
    for (int k = 0; k < 21; k++) {
        const uint8_t *d = buf + 2 + ((idx - 1 - k + 21) % 21) * 24;
        if (!d[0] && !d[1] && !d[2] && !d[3]) continue;
        selftest_entry *e = &o->e[o->count++];
        e->type = d[0];
        e->status = d[1];
        e->hours = d[2] | d[3] << 8;
        e->lba = d[5] | d[6] << 8 | d[7] << 16 | (uint32_t)d[8] << 24;
    }
    return o->count;
}

/* Names only where the meaning is the same across vendors; the rest are vendor-defined. */
const char *smart_attr_name(uint8_t id)
{
    switch (id) {
    case 1: return "Raw read error rate";
    case 3: return "Spin-up time";
    case 4: return "Start/stop count";
    case 5: return "Reallocated sectors";
    case 7: return "Seek error rate";
    case 9: return "Power-on hours";
    case 10: return "Spin retry count";
    case 12: return "Power cycles";
    case 171: return "Program fails";
    case 172: return "Erase fails";
    case 173: return "Wear leveling count";
    case 174: return "Unexpected power loss";
    case 177: return "Wear leveling count";
    case 181: return "Program fails (total)";
    case 182: return "Erase fails (total)";
    case 183: return "SATA downshifts";
    case 184: return "End-to-end errors";
    case 187: return "Reported uncorrectable";
    case 188: return "Command timeouts";
    case 190: return "Airflow temperature";
    case 191: return "G-sense errors";
    case 192: return "Unsafe shutdowns";
    case 193: return "Load cycles";
    case 194: return "Temperature";
    case 195: return "ECC recovered";
    case 196: return "Reallocation events";
    case 197: return "Pending sectors";
    case 198: return "Offline uncorrectable";
    case 199: return "CRC errors (link)";
    case 200: return "Write error rate";
    case 231: return "Life left";
    case 232: return "Spare remaining";
    case 240: return "Head flying hours";
    case 241: return "Host writes";
    case 242: return "Host reads";
    default: return "Vendor attribute";
    }
}

const char *selftest_status_text(uint8_t status_byte)
{
    switch (status_byte >> 4) {
    case 0: return "completed, no error";
    case 1: return "aborted by host";
    case 2: return "interrupted by reset";
    case 3: return "fatal error";
    case 4: return "FAILED (unknown element)";
    case 5: return "FAILED (electrical)";
    case 6: return "FAILED (servo/seek)";
    case 7: return "FAILED (read)";
    case 8: return "FAILED (handling damage)";
    case 15: return "in progress";
    default: return "reserved status";
    }
}

const char *selftest_type_text(uint8_t type)
{
    switch (type) {
    case 0x00: return "offline scan";
    case 0x01: return "short";
    case 0x02: return "extended";
    case 0x03: return "conveyance";
    case 0x04: return "selective";
    case 0x81: return "short, captive";
    case 0x82: return "extended, captive";
    case 0x83: return "conveyance, captive";
    case 0x84: return "selective, captive";
    default: return "vendor test";
    }
}

/* Error counters whose raw value should stay 0 on a healthy drive, HDD or SSD. */
static const uint8_t counters[] = {5, 10, 184, 187, 196, 197, 198, 199};

int smart_counter(uint8_t id)
{
    for (unsigned c = 0; c < sizeof counters; c++)
        if (counters[c] == id) return 1;
    return 0;
}

static void add_why(char *why, int len, const char *msg)
{
    int n = strlen(why);
    if (n + 3 >= len) return;
    snprintf(why + n, len - n, "%s%s", n ? "; " : "", msg);
}

/* FAIL: a normalized value at or below its threshold now. WARN: below it in the
 * past, a non-zero error counter (smart_counter), or a failed last self-test. */
int smart_health(const smart_data *s, const selftest_log *log, int temp_limit, char *why, int whylen)
{
    int level = HEALTH_OK;
    char msg[96];
    why[0] = 0;
    for (int i = 0; i < s->count; i++) {
        const smart_attr *a = &s->a[i];
        if (a->thresh == 0 || a->thresh >= 0xFE) continue;
        if (a->value <= a->thresh) {
            snprintf(msg, sizeof msg, "%u %s at threshold", a->id, smart_attr_name(a->id));
            add_why(why, whylen, msg);
            level = HEALTH_FAIL;
        } else if (a->worst <= a->thresh) {
            snprintf(msg, sizeof msg, "%u was at threshold", a->id);
            add_why(why, whylen, msg);
            if (level < HEALTH_WARN) level = HEALTH_WARN;
        }
    }
    for (unsigned c = 0; c < sizeof counters; c++)
        for (int i = 0; i < s->count; i++)
            if (s->a[i].id == counters[c] && (s->a[i].raw & 0xFFFFFFFFull)) {
                snprintf(msg, sizeof msg, "%u %s = %llu", counters[c], smart_attr_name(counters[c]),
                         (unsigned long long)(s->a[i].raw & 0xFFFFFFFFull));
                add_why(why, whylen, msg);
                if (level < HEALTH_WARN) level = HEALTH_WARN;
            }
    if (log && log->count) {
        int st = log->e[0].status >> 4;
        if (st >= 3 && st <= 8) {
            add_why(why, whylen, "last self-test failed");
            if (level < HEALTH_WARN) level = HEALTH_WARN;
        }
    }
    if (temp_limit > 0 && s->temperature >= temp_limit) {
        snprintf(msg, sizeof msg, "temperature %d C (limit %d)", s->temperature, temp_limit);
        add_why(why, whylen, msg);
        if (level < HEALTH_WARN) level = HEALTH_WARN;
    }
    if (level == HEALTH_OK) add_why(why, whylen, "no attribute at threshold, no error counts");
    return level;
}

const smart_attr *smart_find(const smart_data *s, uint8_t id)
{
    for (int i = 0; i < s->count; i++)
        if (s->a[i].id == id) return &s->a[i];
    return NULL;
}

/* Hours and cycles come from the low 32 bits: some vendors pack extra data in
 * the upper bytes of attribute 9. TB written only when the vendor states the unit. */
void smart_summarize(const smart_data *s, int is_ssd, int vendor, smart_summary *o)
{
    const vendor_info *v = vendor_get(vendor);
    const smart_attr *a;
    memset(o, 0, sizeof *o);
    o->hours = o->cycles = o->life_left = -1;
    o->tb_written = -1;
    o->temp_limit = is_ssd ? TEMP_LIMIT_SSD : TEMP_LIMIT_HDD;
    if ((a = smart_find(s, 9))) o->hours = (int)(a->raw & 0xFFFFFFFFull);
    if ((a = smart_find(s, 12))) o->cycles = (int)(a->raw & 0xFFFFFFFFull);
    int life_ids[2] = {v->life_id, v->life_id2};
    for (int k = 0; k < 2 && o->life_left < 0; k++)
        if (life_ids[k] && (a = smart_find(s, life_ids[k]))) {
            o->life_id = life_ids[k];
            o->life_left = a->value <= 100 ? a->value : -1;
        }
    if (v->writes_id && (a = smart_find(s, v->writes_id))) {
        o->writes_id = v->writes_id;
        o->writes_unit = v->writes_unit;
        o->writes_raw = a->raw;
        double bytes = -1;
        if (v->writes_unit == UNIT_LBA) bytes = (double)a->raw * 512.0;
        else if (v->writes_unit == UNIT_GIB) bytes = (double)a->raw * 1073741824.0;
        else if (v->writes_unit == UNIT_MIB32) bytes = (double)a->raw * 33554432.0;
        if (bytes >= 0) o->tb_written = bytes / 1e12;
    }
}

/* Raw difference now - prev for one attribute; 1 when both sides have it.
 * Temperatures pack min/max above the current value, so they are never compared. */
int smart_delta(const smart_data *now, const smart_data *prev, uint8_t id, long long *delta)
{
    if (id == 194 || id == 190) return 0;
    const smart_attr *a = smart_find(now, id), *b = smart_find(prev, id);
    if (!a || !b) return 0;
    *delta = (long long)a->raw - (long long)b->raw;
    return 1;
}

/* Log 01h: byte 0 version, byte 1 index of the newest of 5 error data
 * structures (90 bytes each from byte 2: five 12-byte command blocks, then a
 * 30-byte error block whose byte 1 is the error register and byte 8 the status
 * register), bytes 452-453 the device error count, byte 511 the checksum. */
int errorlog_parse(const uint8_t *buf, error_log *o)
{
    memset(o, 0, sizeof *o);
    o->version = buf[0];
    o->index = buf[1];
    o->error_count = buf[452] | buf[453] << 8;
    o->checksum = sector_checksum(buf) == 0;
    if (o->index >= 1 && o->index <= 5) {
        const uint8_t *e = buf + 2 + (o->index - 1) * 90;
        o->last_command = e[4 * 12 + 7];     /* the fifth command block: the command that failed */
        o->last_error = e[60 + 1];
        o->last_status = e[60 + 8];
    }
    return o->version == 1 ? 0 : -1;
}

/* ---- GPL logs ------------------------------------------------------------- */

static long long qword(const uint8_t *b, int off, int *valid)
{
    uint64_t q = 0;
    for (int i = 7; i >= 0; i--) q = q << 8 | b[off + i];
    *valid = (q >> 63) && (q >> 62 & 1);
    return (long long)(q & 0xFFFFFFFFFFFFull);
}

static long long stat(const uint8_t *b, int off)
{
    int valid;
    long long v = qword(b, off, &valid);
    return valid ? v : -1;
}

static int stat_temp(const uint8_t *b, int off)
{
    int valid;
    long long v = qword(b, off, &valid);
    return valid ? (int)(int8_t)(v & 0xff) : -999;
}

/* Page 0: byte 8 = number of entries, bytes 9.. = the supported page numbers. */
int devstat_pages(const uint8_t *page0, uint8_t *pages, int max)
{
    int n = page0[8];
    if (n > max) n = max;
    for (int i = 0; i < n; i++) pages[i] = page0[9 + i];
    return n;
}

static int page_is(const uint8_t *buf, int page) { return buf[2] == page && buf[0] >= 1; }

void devstat_general(const uint8_t *buf, dev_stats *o)
{
    if (!page_is(buf, 1)) return;
    o->have_general = 1;
    o->resets = stat(buf, 8);
    o->power_on_hours = stat(buf, 16);
    o->sectors_written = stat(buf, 24);
    o->write_cmds = stat(buf, 32);
    o->sectors_read = stat(buf, 40);
    o->read_cmds = stat(buf, 48);
}

void devstat_temperature(const uint8_t *buf, dev_stats *o)
{
    if (!page_is(buf, 5)) return;
    o->have_temp = 1;
    o->temp_now = stat_temp(buf, 8);
    o->temp_max = stat_temp(buf, 32);
    o->temp_min = stat_temp(buf, 40);
}

void devstat_ssd(const uint8_t *buf, dev_stats *o)
{
    if (!page_is(buf, 7)) return;
    o->have_ssd = 1;
    long long v = stat(buf, 8);
    o->endurance_used = v < 0 ? -1 : (int)(v & 0xff);
}

/* Log 11h: from byte 4, counters of (16-bit id, value of 2 << ((id >> 12) & 7)
 * bytes... the size code in bits 14:12: 1 = 16, 2 = 32, 3 = 64 bits); id 0 ends. */
int phy_parse(const uint8_t *buf, phy_counters *o)
{
    memset(o, 0, sizeof *o);
    o->icrc = o->crc_h2d = o->rerr_data = o->phy_nrdy = o->comreset = o->nonfis_errors = -1;
    int off = 4;
    while (off + 2 <= 510) {
        unsigned id = buf[off] | buf[off + 1] << 8;
        if ((id & 0x0FFF) == 0) break;       /* identifier 0 ends the list (the Dahua writes it with size bits set) */
        int size = 2 << (((id >> 12) & 7) - 1);
        if (size < 2 || size > 8 || off + 2 + size > 510) break;
        long long v = 0;
        for (int i = size - 1; i >= 0; i--) v = v << 8 | buf[off + 2 + i];
        switch (id & 0x0FFF) {
        case 0x001: o->icrc = v; break;              /* command failed, ICRC error */
        case 0x002: o->rerr_data = v; break;         /* R_ERR response for data FIS */
        case 0x005: o->nonfis_errors = v; break;     /* R_ERR response for non-data FIS */
        case 0x009: o->phy_nrdy = v; break;          /* PhyRdy to PhyNRdy transitions */
        case 0x00A: o->comreset = v; break;          /* signature D2H FISes: resets */
        case 0x00B: o->crc_h2d = v; break;           /* CRC errors within H2D FIS */
        default: break;
        }
        o->count++;
        off += 2 + size;
    }
    return o->count ? 0 : -1;
}
