# Validation against smartctl

The app decodes the same sectors `smartctl` reads. This page compares the two on
the same drive, column by column, so a wrong decoder or a wrong vendor name
shows up.

## Method

1. Run the app, note the report file name.
2. Move the drive to a PC, or connect it over USB with an adapter that passes
   SMART through (`smartctl -d sat`). Run:

   ```
   smartctl -x /dev/sdX > smartctl.txt
   ```

3. Compare these lines. Every value must match, except the ones that change
   with time (power-on hours, temperature, power cycles) and the serial number
   (the report masks it).

| Report line | smartctl line | Must match |
|---|---|---|
| `model`, `firmware` | `Device Model`, `Firmware Version` | exact |
| `capacity ... sectors` | `User Capacity` | sector count |
| `type SSD` / `HDD, N rpm` | `Rotation Rate` | exact |
| `SATA gen max, now` | `SATA Version is: ... (current: ...)` | both speeds |
| `TRIM`, `DRAT`, `RZAT` | `TRIM Command` | yes/no |
| each attribute row: id, value, worst, thresh, raw | `SMART Attributes Data Structure` table | every column |
| `vendor table` names | smartctl's `ATTRIBUTE_NAME` column | same meaning; a different wording is fine |
| `self-test log` entries | `SMART Self-test log` | type, status, hours, LBA |
| `host writes ... TB` | smartctl `-x` "Device Statistics" `Logical Sectors Written`, or the vendor's tool | within 1 % |
| `life left N%` | the vendor's own tool (Magician, Storage Executive, ...) | same percent |

## Results

| Drive | App version | smartctl version | Result | Date |
|---|---|---|---|---|
| Dahua V800 1 TB (test console) | 1.1.0 | pending | pending: the drive is in the console | |

Open point for the Dahua: the unit of attribute 241 (host writes). The app
uses 32 MiB, the Maxio layout's unit, because it is the only unit that agrees
with the drive's own erase counters (2026-10-07 dump: average erase count 6 on
1 TB, 233 = 146185 and 241 = 72132 give 4.9 TB of NAND writes and 2.4 TB of host
writes in 921 h; GiB would mean 72 TB, LBAs 37 MB). `smartctl -x` or the vendor
tool should confirm it.
