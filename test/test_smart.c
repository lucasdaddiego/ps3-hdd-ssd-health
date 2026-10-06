/* Mac-side test of source/smart.c with synthetic sectors laid out as ACS-3 says.
 *   cc -std=c99 -Wall -o /tmp/test_smart test/test_smart.c source/smart.c && /tmp/test_smart */
#include <stdio.h>
#include <string.h>
#include "../source/smart.h"

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
    CHECK(smart_health(&s, NULL, why, sizeof why) == HEALTH_OK);
    data[2 + 12 + 5] = 3; seal(data);  /* 3 reallocated sectors */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, why, sizeof why) == HEALTH_WARN && strstr(why, "Reallocated sectors = 3"));
    data[2 + 3] = 40; seal(data);      /* attribute 1 falls to 40, threshold 50 */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, why, sizeof why) == HEALTH_FAIL && strstr(why, "1 Raw read error rate at threshold"));

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
    CHECK(smart_health(&s, &l, why, sizeof why) == HEALTH_WARN && strstr(why, "last self-test failed"));

    CHECK(smart_counter(10) && smart_counter(199) && !smart_counter(9));
    CHECK(!strcmp(smart_attr_name(193), "Load cycles"));

    printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
