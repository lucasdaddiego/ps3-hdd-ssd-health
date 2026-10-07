/* Mac-side test of the pure C layer (smart.c, vendor.c, compat.c, qrcodegen.c)
 * with synthetic sectors laid out as ACS-3 says. build.sh and CI run it. */
#include <stdio.h>
#include <string.h>
#include "../source/smart.h"
#include "../source/vendor.h"
#include "../source/compat.h"
#include "../source/version.h"
#include "../source/qrcodegen.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put_word(uint8_t *b, int i, unsigned w) { b[2 * i] = w & 0xff; b[2 * i + 1] = w >> 8; }

static void put_string(uint8_t *b, int first, int nwords, const char *s)
{
    char pad[64];
    memset(pad, ' ', sizeof pad);
    memcpy(pad, s, strlen(s));
    for (int k = 0; k < nwords; k++) put_word(b, first + k, (uint8_t)pad[2 * k] << 8 | (uint8_t)pad[2 * k + 1]);
}

static void seal(uint8_t *b)            /* make the byte sum 0, as the drive does */
{
    unsigned sum = 0;
    for (int i = 0; i < 511; i++) sum += b[i];
    b[511] = (uint8_t)(0x100 - (sum & 0xff));
}

static void identify(uint8_t *b)
{
    memset(b, 0, 512);
    put_string(b, 10, 10, "        AB12345678");
    put_string(b, 23, 4, "V0.1");
    put_string(b, 27, 20, "DHI-SSD-V800S1TB");
    put_word(b, 69, 1 << 14 | 1 << 5);
    put_word(b, 76, 0x000E);             /* Gen1-3 */
    put_word(b, 77, 3 << 1);             /* running at Gen3 */
    put_word(b, 82, 1);
    put_word(b, 83, 0x4000 | 1 << 10);
    put_word(b, 84, 0x4000 | 2);
    put_word(b, 85, 1);
    put_word(b, 87, 0x4000 | 2);
    put_word(b, 100, 0x6DB0); put_word(b, 101, 0x7470); put_word(b, 102, 0); put_word(b, 103, 0);  /* 1953525168 */
    put_word(b, 105, 8);
    put_word(b, 106, 0x4000);
    put_word(b, 169, 1);
    put_word(b, 217, 1);
    b[510] = 0xA5;
    seal(b);
}

static void attr(uint8_t *b, int slot, int id, int value, int worst, unsigned long long raw)
{
    uint8_t *e = b + 2 + slot * 12;
    e[0] = id; e[1] = 0x32; e[3] = value; e[4] = worst;
    for (int k = 0; k < 6; k++) e[5 + k] = raw >> (8 * k);
}

int main(void)
{
    uint8_t id[512], sw[512], data[512], th[512], log[512];

    identify(id);
    ata_identity o;
    CHECK(ata_parse_identify(id, NULL, &o) == 0);
    CHECK(!strcmp(o.model, "DHI-SSD-V800S1TB"));
    CHECK(!strcmp(o.serial, "AB12345678"));
    CHECK(!strcmp(o.firmware, "V0.1"));
    CHECK(o.sectors == 1953525168ull);
    CHECK(o.lba48 && o.smart_supported && o.smart_enabled && o.selftest_supported);
    CHECK(o.trim && o.drat && o.rzat && o.dsm_max_blocks == 8);
    CHECK(o.sata_max_gen == 3 && o.sata_cur_gen == 3);
    CHECK(o.rotation == 1 && o.logical_size == 512 && o.checksum == 1 && o.swapped == 0);

    memcpy(sw, id, 512);                 /* the same sector with every word swapped */
    ata_unswap(sw, 512);
    CHECK(ata_parse_identify(sw, NULL, &o) == 0 && o.swapped == 1);
    CHECK(!strcmp(o.model, "DHI-SSD-V800S1TB") && o.sectors == 1953525168ull);
    sw[511] = sw[510] = 0;               /* no signature: the fcntl model decides */
    CHECK(ata_identify_order(sw, "DHI-SSD-V800S1TB    ") == 1);
    CHECK(ata_identify_order(id, "dhi ssd v800s1tb") == 0);

    memset(data, 0, 512); memset(th, 0, 512);
    data[0] = 0x10;
    attr(data, 0, 1, 100, 100, 0);
    attr(data, 1, 5, 100, 100, 0);
    attr(data, 2, 9, 100, 100, 1234);
    attr(data, 3, 194, 67, 50, 0x0032001E0021ull);   /* 33 C now, min/max packed above */
    attr(data, 4, 241, 100, 100, 0x123456789Aull);
    data[363] = 0x00; data[367] = 0x5B; data[372] = 2;
    seal(data);
    th[2] = 1; th[3] = 50; th[14] = 5; th[15] = 10; th[26] = 9; th[27] = 0;
    seal(th);
    smart_data s;
    CHECK(smart_parse(data, th, &s) == 0);
    CHECK(s.count == 5 && s.temperature == 33 && s.short_minutes == 2);
    CHECK(s.a[2].raw == 1234 && s.a[4].raw == 0x123456789Aull);
    CHECK(s.a[0].thresh == 50 && s.a[1].thresh == 10 && s.a[2].thresh == 0);
    CHECK(s.checksum == 1 && s.thresh_checksum == 1 && (s.offline_caps & 0x10));

    char why[256];
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_OK);
    data[2 + 12 + 5] = 3; seal(data);  /* 3 reallocated sectors */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_WARN && strstr(why, "Reallocated sectors = 3"));
    data[2 + 3] = 40; seal(data);      /* attribute 1 falls to 40, threshold 50 */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_FAIL && strstr(why, "1 Raw read error rate at threshold"));

    memset(log, 0, 512);
    log[0] = 1;
    uint8_t *d = log + 2;                /* descriptor 1: short, passed at 1200 h */
    d[0] = 0x01; d[1] = 0x00; d[2] = 1200 & 0xff; d[3] = 1200 >> 8;
    d = log + 2 + 24;                    /* descriptor 2 (newest): short, read failure at LBA 0x1000 */
    d[0] = 0x01; d[1] = 0x70; d[2] = 1234 & 0xff; d[3] = 1234 >> 8; d[5] = 0x00; d[6] = 0x10;
    log[508] = 2;
    seal(log);
    selftest_log l;
    CHECK(selftest_parse(log, &l) == 2);
    CHECK(l.e[0].hours == 1234 && l.e[0].lba == 0x1000 && (l.e[0].status >> 4) == 7);
    CHECK(l.e[1].hours == 1200 && l.checksum == 1);
    CHECK(!strcmp(selftest_status_text(l.e[0].status), "FAILED (read)"));
    data[2 + 3] = 100; data[2 + 12 + 5] = 0; seal(data);
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, &l, 0, why, sizeof why) == HEALTH_WARN && strstr(why, "last self-test failed"));

    char masked[21];
    serial_mask("AB12345678", masked, sizeof masked);
    CHECK(!strcmp(masked, "AB******78"));
    serial_mask("S1234", masked, sizeof masked);          /* too short to keep any character */
    CHECK(!strcmp(masked, "*****"));
    serial_mask("", masked, sizeof masked);
    CHECK(!strcmp(masked, ""));
    serial_mask("WD-WCC4N1234567", masked, 8);           /* fits the buffer */
    CHECK(strlen(masked) == 7);
    CHECK(smart_counter(10) && smart_counter(199) && !smart_counter(9));
    CHECK(!strcmp(smart_attr_name(193), "Load cycles"));

    /* temperature warning: 33 C is fine for an SSD, a 65 C limit trips at 65 */
    CHECK(smart_health(&s, NULL, TEMP_LIMIT_SSD, why, sizeof why) == HEALTH_OK);
    attr(data, 3, 194, 67, 50, 0x0032001E0041ull); seal(data);   /* 65 C now */
    smart_parse(data, th, &s);
    CHECK(s.temperature == 65);
    CHECK(smart_health(&s, NULL, TEMP_LIMIT_SSD, why, sizeof why) == HEALTH_WARN && strstr(why, "temperature 65 C"));
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_OK);

    /* vendors */
    CHECK(vendor_detect("Samsung SSD 870 EVO 1TB") == VENDOR_SAMSUNG);
    CHECK(vendor_detect("MZ7LN512HMJP-000L7") == VENDOR_SAMSUNG);
    CHECK(vendor_detect("CT1000MX500SSD1") == VENDOR_MICRON);
    CHECK(vendor_detect("KINGSTON SA400S37480G") == VENDOR_KINGSTON);
    CHECK(vendor_detect("WDC WD10JPVX-22JC3T0") == VENDOR_WD);
    CHECK(vendor_detect("WDS100T2B0A-00SM50") == VENDOR_SANDISK);
    CHECK(vendor_detect("ST1000LM035-1RK172") == VENDOR_SEAGATE);
    CHECK(vendor_detect("INTEL SSDSC2KW512G8") == VENDOR_INTEL);
    CHECK(vendor_detect("TOSHIBA MQ01ABD100") == VENDOR_TOSHIBA);
    CHECK(vendor_detect("Dahua V800 2.5 inch SATA 1TB SSD") == VENDOR_MAXIO);
    CHECK(vendor_detect("DHI-SSD-V800S1TB") == VENDOR_MAXIO);
    CHECK(vendor_detect("Some Drive") == VENDOR_UNKNOWN && vendor_detect("") == VENDOR_UNKNOWN);
    CHECK(!strcmp(vendor_attr_name(VENDOR_MAXIO, 161), "Valid spare blocks"));
    CHECK(!strcmp(vendor_attr_name(VENDOR_MAXIO, 5), "Reallocated sectors"));      /* generic fallback */
    CHECK(!strcmp(vendor_attr_name(VENDOR_UNKNOWN, 161), "Vendor attribute"));
    CHECK(!strcmp(vendor_attr_name(VENDOR_SEAGATE, 1), "Raw read error rate (packed)"));
    CHECK(vendor_get(VENDOR_SAMSUNG)->writes_unit == UNIT_LBA && vendor_get(VENDOR_INTEL)->writes_unit == UNIT_MIB32);
    CHECK(vendor_get(99)->vendor == VENDOR_UNKNOWN);

    /* summary: Samsung layout, 241 in LBAs, 177 = life */
    memset(data, 0, 512);
    attr(data, 0, 9, 100, 100, 0x0001000003E8ull);   /* 1000 h, junk above the low 32 bits */
    attr(data, 1, 12, 100, 100, 250);
    attr(data, 2, 177, 97, 97, 40);
    attr(data, 3, 241, 100, 100, 4000000000ull);    /* 4e9 LBAs = 2.048 TB */
    seal(data);
    smart_parse(data, NULL, &s);
    smart_summary m;
    smart_summarize(&s, 1, VENDOR_SAMSUNG, &m);
    CHECK(m.hours == 1000 && m.cycles == 250 && m.life_left == 97 && m.life_id == 177);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_LBA && m.tb_written > 2.047 && m.tb_written < 2.049);
    CHECK(m.temp_limit == TEMP_LIMIT_SSD);
    smart_summarize(&s, 0, VENDOR_UNKNOWN, &m);
    CHECK(m.life_left == -1 && m.writes_id == 0 && m.tb_written < 0 && m.temp_limit == TEMP_LIMIT_HDD);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_MIB32 && m.writes_raw == 4000000000ull);
    smart_summarize(&s, 1, VENDOR_KINGSTON, &m);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_NONE && m.tb_written < 0);
    /* the Dahua test drive: no 169, life in 202, 241 in 32 MiB units */
    memset(data, 0, 512);
    attr(data, 0, 9, 96, 100, 921); attr(data, 1, 202, 100, 100, 100); attr(data, 2, 241, 100, 100, 72132); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    CHECK(m.life_left == 100 && m.life_id == 202 && m.tb_written > 2.41 && m.tb_written < 2.43);
    memset(data, 0, 512); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_SAMSUNG, &m);
    CHECK(m.hours == -1 && m.cycles == -1 && m.life_left == -1);

    /* delta: raw now - raw before, never for temperatures */
    smart_data before, now;
    memset(data, 0, 512);
    attr(data, 0, 9, 100, 100, 1000); attr(data, 1, 5, 100, 100, 0); attr(data, 2, 194, 60, 50, 40); seal(data);
    smart_parse(data, NULL, &before);
    attr(data, 0, 9, 100, 100, 1012); attr(data, 1, 5, 100, 100, 2); attr(data, 2, 194, 60, 50, 45);
    attr(data, 3, 12, 100, 100, 7); seal(data);
    smart_parse(data, NULL, &now);
    long long dl = 0;
    CHECK(smart_delta(&now, &before, 9, &dl) && dl == 12);
    CHECK(smart_delta(&now, &before, 5, &dl) && dl == 2);
    CHECK(!smart_delta(&now, &before, 194, &dl));      /* temperature packs min/max */
    CHECK(!smart_delta(&now, &before, 12, &dl));       /* only in one side */
    CHECK(smart_delta(&before, &now, 9, &dl) && dl == -12);

    /* SMART error log 01h */
    uint8_t el[512];
    memset(el, 0, sizeof el);
    el[0] = 1; el[1] = 2; el[452] = 2;
    uint8_t *e2 = el + 2 + 90;
    e2[4 * 12 + 7] = 0x25; e2[60 + 1] = 0x40; e2[60 + 8] = 0x51;   /* READ DMA EXT, UNC, ERR */
    el[511] = (uint8_t)(256 - (sector_checksum(el) & 0xff));
    error_log eo;
    CHECK(errorlog_parse(el, &eo) == 0 && eo.error_count == 2 && eo.index == 2 && eo.checksum);
    CHECK(eo.last_command == 0x25 && eo.last_error == 0x40 && eo.last_status == 0x51);
    memset(el, 0, sizeof el);
    CHECK(errorlog_parse(el, &eo) == -1 && eo.error_count == 0);

    /* device statistics (GPL 04h) and Phy counters (GPL 11h) */
    uint8_t pg[512];
    memset(pg, 0, sizeof pg);
    pg[0] = 1; pg[2] = 0; pg[8] = 3; pg[9] = 1; pg[10] = 5; pg[11] = 7;
    uint8_t pages[16];
    CHECK(devstat_pages(pg, pages, 16) == 3 && pages[1] == 5 && pages[2] == 7);
    dev_stats ds;
    memset(&ds, 0, sizeof ds);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 1;
    /* qword at 24: sectors written = 4750000000, supported + valid */
    unsigned long long swq = 4750000000ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[24 + i] = (uint8_t)(swq >> (8 * i));
    unsigned long long hrs = 931ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[16 + i] = (uint8_t)(hrs >> (8 * i));
    devstat_general(pg, &ds);
    CHECK(ds.have_general && ds.sectors_written == 4750000000ll && ds.power_on_hours == 931 && ds.resets == -1);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 5;
    unsigned long long tmax = 48ull | (3ull << 62), tmin = (unsigned long long)(uint8_t)(int8_t)-3 | (3ull << 62);
    for (int i = 0; i < 8; i++) { pg[32 + i] = (uint8_t)(tmax >> (8 * i)); pg[40 + i] = (uint8_t)(tmin >> (8 * i)); }
    devstat_temperature(pg, &ds);
    CHECK(ds.have_temp && ds.temp_max == 48 && ds.temp_min == -3 && ds.temp_now == -999);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 7;
    unsigned long long eu = 1ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[8 + i] = (uint8_t)(eu >> (8 * i));
    devstat_ssd(pg, &ds);
    CHECK(ds.have_ssd && ds.endurance_used == 1);
    devstat_general(pg, &ds);                       /* wrong page: ignored */
    CHECK(ds.sectors_written == 4750000000ll);
    uint8_t ph[512];
    memset(ph, 0, sizeof ph);
    /* id 0x0001 (16-bit) = 7, id 0x200A (32-bit) = 3, id 0x100B = 2 */
    ph[4] = 0x01; ph[5] = 0x10; ph[6] = 7; ph[7] = 0;
    ph[8] = 0x0A; ph[9] = 0x20; ph[10] = 3;
    ph[14] = 0x0B; ph[15] = 0x10; ph[16] = 2;
    phy_counters pc;
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 3 && pc.icrc == 7 && pc.comreset == 3 && pc.crc_h2d == 2 && pc.phy_nrdy == -1);
    memset(ph, 0, sizeof ph);
    CHECK(phy_parse(ph, &pc) == -1);

    /* issue form URL */
    char enc[64], url[2400];
    CHECK(url_encode("a b/c=d~", enc, sizeof enc) == 14 && !strcmp(enc, "a%20b%2Fc%3Dd~"));
    CHECK(url_encode("abcdefgh", enc, 6) == 2 && !strcmp(enc, "ab"));   /* bounded: room for one %XX and the NUL */
    const form_field f[] = {{"firmware", "4.93 (Europe)"}, {"health", "OK\nhours 921"}};
    int n = issue_form_url("compat-report.yml", "Compat: X on 4.93", f, 2, url, sizeof url);
    CHECK(n > 0 && n == (int)strlen(url));
    CHECK(!strncmp(url, APP_REPO "/issues/new?template=compat-report.yml&title=Compat%3A%20X%20on%204.93", 
                   strlen(APP_REPO) + 58));
    CHECK(strstr(url, "&firmware=4.93%20%28Europe%29&health=OK%0Ahours%20921") != NULL);
    CHECK(issue_form_url("t.yml", "title", f, 2, url, 40) == -1);   /* does not fit */
    /* no template: a blank issue, title first, then body (what the mobile app fills) */
    const form_field b[] = {{"body", "console: x\nfirmware: 4.93"}};
    n = issue_form_url(NULL, "Compat: X on 4.93", b, 1, url, sizeof url);
    CHECK(n > 0 && !strcmp(url, APP_REPO "/issues/new?title=Compat%3A%20X%20on%204.93&body=console%3A%20x%0Afirmware%3A%204.93"));
    /* a real-size link fits a QR code at version 25 */
    char big[700];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    const form_field g[] = {{"health", big}};
    n = issue_form_url("compat-report.yml", "t", g, 1, url, sizeof url);
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(25)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(25)];
    CHECK(n > 700 && qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true));
    CHECK(qrcodegen_getSize(qr) >= 21 && qrcodegen_getSize(qr) <= 117);

    printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
