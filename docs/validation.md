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

| Drive | App version | Reference | Result | Date |
|---|---|---|---|---|
| Dahua V800 1 TB (test console) | 1.2.0 rc2 | the drive's own device statistics log (GPL 04h, read by the app) | host writes: attribute 241 = 72570 x 32 MiB = 2.435 TB, device statistics = 4757916160 sectors = 2.436 TB (0.04 % apart); power-on hours 931 in both | 2026-10-07 |
| Dahua V800 1 TB (test console) | 1.1.0 | smartctl | pending: the drive is in the console | |

The unit of attribute 241 on the Maxio layout is settled: the device statistics
log, which counts sectors exactly, agrees with 241 x 32 MiB within 0.04 %. The
erase counters pointed the same way (average erase count 6 on 1 TB, 233 =
146185 and 241 = 72132 gave 4.9 TB of NAND and 2.4 TB of host writes in 921 h;
GiB would have meant 72 TB, LBAs 37 MB).

Two things the same log showed on this drive:
- The percentage-used endurance indicator (page 7) reads 100 on a drive whose
  attribute 202 says 100 % life left. The firmware stores the remaining
  percent. The app therefore shows the indicator as a number, not as a verdict.
- The Phy counters log lists two counters (ICRC errors 0, resets 1) and ends
  with a zero identifier that carries size bits, which the decoder accepts.
