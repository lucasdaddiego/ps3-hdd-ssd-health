/* Vendor tables. Each table lists only the attributes the vendor documents
 * with a meaning that differs from, or is missing in, the generic list.
 * Sources: Samsung SSD SMART attribute note; Micron TN-FD-22/-23 (Crucial);
 * Kingston SA400/KC600 SMART attribute sheets; Intel SSD SMART attribute
 * specification; SanDisk/WD SSD Dashboard attribute list; the Seagate, WD and
 * Toshiba HDD product manuals; and, for the Maxio controller layout (Dahua
 * and other brands on MAS09xx/MAS11xx), the values seen on the test drive
 * (spare blocks, erase counts) that fit this layout and no other. */
#include <string.h>
#include "vendor.h"
#include "smart.h"

typedef struct { uint8_t id; const char *name; } attr_name;

static const attr_name samsung[] = {
    {177, "Wear leveling count"}, {179, "Used reserved blocks"}, {180, "Unused reserved blocks"},
    {181, "Program fails (total)"}, {182, "Erase fails (total)"}, {183, "Runtime bad blocks"},
    {187, "Uncorrectable errors"}, {190, "Airflow temperature"}, {195, "ECC error rate"},
    {199, "CRC errors (link)"}, {235, "POR recovery count"}, {241, "Total LBAs written"},
    {242, "Total LBAs read"}, {0, 0}};

static const attr_name micron[] = {
    {5, "Reallocated NAND blocks"}, {171, "Program fails"}, {172, "Erase fails"},
    {173, "Average block erase count"}, {174, "Unexpected power loss"}, {180, "Unused reserve blocks"},
    {183, "SATA downshifts"}, {184, "Error correction count"}, {187, "Reported uncorrectable"},
    {196, "Reallocation events"}, {197, "Pending ECC count"}, {198, "Offline scan uncorrectable"},
    {199, "CRC errors (link)"}, {202, "Lifetime remaining %"}, {206, "Write error rate"},
    {210, "RAIN recoveries"}, {246, "Host sector writes"}, {247, "Host program pages"},
    {248, "FTL program pages"}, {0, 0}};

static const attr_name kingston[] = {
    {170, "Bad blocks (early)"}, {172, "Erase fails"}, {181, "Program fails"}, {182, "Erase fails"},
    {187, "Uncorrectable errors"}, {192, "Unsafe shutdowns"}, {196, "Reallocation events"},
    {199, "CRC errors (link)"}, {233, "Flash writes (GB)"}, {241, "Host writes"}, {242, "Host reads"},
    {244, "Average erase count"}, {245, "Max erase count"}, {246, "Total erase count"}, {0, 0}};

static const attr_name wd_hdd[] = {
    {11, "Calibration retries"}, {192, "Power-off retracts"}, {193, "Load/unload cycles"},
    {200, "Multi-zone error rate"}, {0, 0}};

static const attr_name seagate[] = {
    {1, "Raw read error rate (packed)"}, {7, "Seek error rate (packed)"}, {183, "SATA downshifts"},
    {184, "End-to-end errors"}, {187, "Reported uncorrectable"}, {188, "Command timeouts"},
    {189, "High fly writes"}, {190, "Airflow temperature"}, {191, "G-sense errors"},
    {192, "Power-off retracts"}, {193, "Load cycles"}, {195, "ECC recovered (packed)"},
    {240, "Head flying hours (packed)"}, {241, "Total LBAs written"}, {242, "Total LBAs read"}, {0, 0}};

static const attr_name sandisk[] = {
    {165, "Total write/erase count"}, {166, "Min write/erase cycles"}, {167, "Min bad blocks per die"},
    {168, "Max erase count"}, {169, "Total bad blocks"}, {170, "Unused reserve blocks"},
    {171, "Program fails"}, {172, "Erase fails"}, {173, "Average write/erase count"},
    {174, "Unexpected power loss"}, {184, "End-to-end errors"}, {187, "Reported uncorrectable"},
    {188, "Command timeouts"}, {199, "CRC errors (link)"}, {230, "Media wearout indicator"},
    {232, "Available reserved space"}, {233, "NAND writes (GiB)"}, {241, "Host writes (GiB)"},
    {242, "Host reads (GiB)"}, {244, "Thermal throttle status"}, {0, 0}};

static const attr_name intel[] = {
    {170, "Available reserved space"}, {171, "Program fails"}, {172, "Erase fails"},
    {174, "Unexpected power loss"}, {175, "Power loss protection fail"}, {183, "SATA downshifts"},
    {184, "End-to-end errors"}, {187, "Uncorrectable errors"}, {190, "Airflow temperature"},
    {192, "Unsafe shutdowns"}, {199, "CRC errors (link)"}, {225, "Host writes (32 MiB)"},
    {226, "Timed workload media wear"}, {227, "Timed workload R/W ratio"}, {228, "Workload timer"},
    {232, "Available reserved space"}, {233, "Media wearout indicator"}, {241, "Host writes (32 MiB)"},
    {242, "Host reads (32 MiB)"}, {249, "NAND writes (GiB)"}, {0, 0}};

static const attr_name toshiba[] = {
    {220, "Disk shift"}, {222, "Loaded hours"}, {223, "Load retries"}, {224, "Load friction"},
    {226, "Load-in time"}, {240, "Head flying hours"}, {0, 0}};

static const attr_name maxio[] = {
    {160, "Uncorrectable errors"}, {161, "Valid spare blocks"}, {163, "Initial invalid blocks"},
    {164, "Total erase count"}, {165, "Max erase count"}, {166, "Min erase count"},
    {167, "Average erase count"}, {168, "Max erase count (spec)"}, {169, "Lifetime remaining %"},
    {202, "Lifetime remaining %"}, {233, "NAND writes (32 MiB)"},
    {177, "Wear leveling count"}, {178, "Runtime invalid blocks"}, {181, "Program fails"},
    {182, "Erase fails"}, {192, "Unsafe shutdowns"}, {195, "ECC recovered"}, {196, "Reallocation events"},
    {197, "Pending sectors"}, {198, "Offline uncorrectable"}, {199, "CRC errors (link)"},
    {232, "Available reserved space"}, {241, "Host writes (32 MiB)"}, {242, "Host reads (32 MiB)"},
    {245, "TLC writes (32 MiB)"}, {0, 0}};

/* The writes unit is set only where the vendor states it, or where the
 * drive's own counters confirm it. Kingston mixes controllers (GiB on some,
 * LBAs on others), so it stays UNIT_NONE and the summary shows the raw value.
 * Maxio: on the Dahua test drive, 32 MiB is the only unit that agrees with the
 * erase counters (average erase count 6 on 1 TB = about 6 TB of NAND writes;
 * 233 = 146185 x 32 MiB = 4.9 TB, 241 = 72132 x 32 MiB = 2.4 TB host writes in
 * 921 h; GiB would be 72 TB, LBAs 37 MB). That drive has no 169: 202 holds the
 * life percent (value 100, threshold 10). */
static const struct { vendor_info info; const attr_name *names; } tables[] = {
    {{VENDOR_UNKNOWN, "generic", 0, 0, 0, UNIT_NONE}, 0},
    {{VENDOR_SAMSUNG, "Samsung", 177, 0, 241, UNIT_LBA}, samsung},
    {{VENDOR_MICRON, "Micron/Crucial", 202, 0, 246, UNIT_LBA}, micron},
    {{VENDOR_KINGSTON, "Kingston", 0, 0, 241, UNIT_NONE}, kingston},
    {{VENDOR_WD, "WD/HGST", 0, 0, 241, UNIT_LBA}, wd_hdd},
    {{VENDOR_SEAGATE, "Seagate", 0, 0, 241, UNIT_LBA}, seagate},
    {{VENDOR_SANDISK, "SanDisk/WD SSD", 0, 0, 241, UNIT_GIB}, sandisk},
    {{VENDOR_INTEL, "Intel", 233, 0, 241, UNIT_MIB32}, intel},
    {{VENDOR_TOSHIBA, "Toshiba", 0, 0, 241, UNIT_LBA}, toshiba},
    {{VENDOR_MAXIO, "Maxio layout (Dahua)", 169, 202, 241, UNIT_MIB32}, maxio},
};

static int starts(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static int has(const char *s, const char *p)
{
    for (; *s; s++)
        if (starts(s, p)) return 1;
    return 0;
}

int vendor_detect(const char *model)
{
    if (!model || !*model) return VENDOR_UNKNOWN;
    if (has(model, "Samsung") || starts(model, "MZ")) return VENDOR_SAMSUNG;
    if (has(model, "Crucial") || has(model, "Micron") || starts(model, "MTFD") ||
        (starts(model, "CT") && model[2] >= '0' && model[2] <= '9')) return VENDOR_MICRON;
    if (has(model, "Kingston") || starts(model, "SA400") || starts(model, "SKC") || starts(model, "SUV") ||
        starts(model, "SV300") || starts(model, "SHFS")) return VENDOR_KINGSTON;
    if (has(model, "SanDisk") || starts(model, "SDSSD") || starts(model, "WDS") || starts(model, "WD Blue SA") ||
        starts(model, "WD Green SA")) return VENDOR_SANDISK;
    if (starts(model, "WDC") || starts(model, "WD ") || has(model, "HGST") || has(model, "Hitachi") ||
        starts(model, "HTS") || starts(model, "HUA") || starts(model, "HDS")) return VENDOR_WD;
    if (has(model, "Seagate") || has(model, "BarraCuda") || has(model, "FireCuda") ||
        (starts(model, "ST") && model[2] >= '0' && model[2] <= '9')) return VENDOR_SEAGATE;
    if (has(model, "Intel") || starts(model, "SSDSC") || starts(model, "SSDSA") || starts(model, "SSDPE")) return VENDOR_INTEL;
    if (has(model, "Toshiba") || has(model, "Kioxia") || starts(model, "THNS") || starts(model, "MQ0") ||
        starts(model, "MK")) return VENDOR_TOSHIBA;
    if (has(model, "Dahua") || starts(model, "DHI-")) return VENDOR_MAXIO;
    return VENDOR_UNKNOWN;
}

const vendor_info *vendor_get(int vendor)
{
    for (unsigned i = 0; i < sizeof tables / sizeof tables[0]; i++)
        if (tables[i].info.vendor == vendor) return &tables[i].info;
    return &tables[0].info;
}

const char *vendor_attr_name(int vendor, uint8_t id)
{
    for (unsigned i = 0; i < sizeof tables / sizeof tables[0]; i++)
        if (tables[i].info.vendor == vendor && tables[i].names)
            for (const attr_name *n = tables[i].names; n->id; n++)
                if (n->id == id) return n->name;
    return smart_attr_name(id);
}
