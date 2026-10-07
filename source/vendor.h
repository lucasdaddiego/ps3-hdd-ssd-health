/* Vendor-specific SMART attribute names and units. Pure C, host-tested.
 * The tables are hand-written from the vendors' public SMART attribute notes
 * and the ATA specification, not copied from any GPL database. A name is in a
 * table only when the vendor documents it. */
#ifndef VENDOR_H
#define VENDOR_H

#include <stdint.h>

enum { VENDOR_UNKNOWN, VENDOR_SAMSUNG, VENDOR_MICRON, VENDOR_KINGSTON, VENDOR_WD, VENDOR_SEAGATE,
       VENDOR_SANDISK, VENDOR_INTEL, VENDOR_TOSHIBA, VENDOR_MAXIO };

/* Unit of the raw value of the host-writes attribute. */
enum { UNIT_NONE, UNIT_LBA, UNIT_GIB, UNIT_MIB32 };

typedef struct {
    int vendor;
    const char *name;        /* shown on the summary page: "Samsung" */
    int life_id, life_id2;   /* attributes whose normalized value = % life left, first one present wins; 0 = none */
    int writes_id;           /* host-writes attribute, 0 = none known */
    int writes_unit;         /* UNIT_* of writes_id's raw value */
} vendor_info;

int vendor_detect(const char *model);
const vendor_info *vendor_get(int vendor);
const char *vendor_attr_name(int vendor, uint8_t id);   /* falls back to smart_attr_name */

#endif
