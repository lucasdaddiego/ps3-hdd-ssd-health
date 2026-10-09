# Preview on the host

The app's screens render on a Mac (or Linux) without a TV, through ps3gfx's
preview: `tools/preview/build.zsh` hands this app to the library's
`tools/preview/build.zsh` (the clone in `~/ps3dev/src/ps3gfx` that
`sdk_setup.sh` makes, or `$PS3GFX`), which compiles `source/*.c` and
`pv_app.c` with the library's own sources against stub PSL1GHT headers, on a
software back end with a scripted controller and a sandbox for the `/dev_*`
paths, into `build/pv`. The raster, the back end, the gfx traces
(`PV_GFXTRACE`), `diff.py` and the pad scripts are the library's: see its
`tools/preview/README.md`. A checkout at another commit than the one
`sdk_setup.sh` pins is announced ("preview on ps3gfx <sha>, the pkg links
<rev>"): the pkg links the pin.

Needs: zsh, a C compiler (`cc`), Python 3 with Pillow for the PNG step
(`PYTHON=...` picks the interpreter, as in `build.sh`).

```sh
zsh tools/preview/build.zsh            # build/pv; PV_CFLAGS adds cc flags
zsh tools/preview/run.zsh              # every scenario; or one: run.zsh drive
zsh tools/preview/shots.zsh            # the README screenshots, into docs/
python3 ~/ps3dev/src/ps3gfx/tools/preview/diff.py A B [D]   # pixel diff of two snapshot folders
```

`run.zsh` writes its snapshots to `build/out/*.png`; the scenario names and pad
scripts are in its `case`. Everything under `build/` is ignored by git. The
job threads run in real time, so a screen that shows a running job (`mem_job`)
can change from run to run: run twice and compare to see which snapshots
repeat.

## The app's part: pv_app.c

The library's world refuses every LV2 syscall and answers the network
offline. `pv_app.c` defines the strong `pv_syscall`: the drive (syscalls 600,
601, 609 and 616, answered from a console's sector dumps), the temperatures
(383) and the fan duty (409).

## Drive dumps

The emulated drive reads `identify.bin`, `smart.bin`, `thresh.bin`,
`selftest.bin`, `errlog.bin`, `devstat.bin`, `phy.bin` and `devinfo.bin` from
`$PV_DRIVE`, by default `~/.local/share/ps3-health/drive/`. Copy them from the
console's app folder (`/dev_hdd0/tmp/ps3_health/`, FTP). Without them the Drive
scenarios show the probe screen.

**Never commit the dumps.** `identify.bin` holds the drive's full serial
number. `shots.zsh` renders from a masked copy (serial, world wide name and
the reserved words 236-254 cleared, checksum fixed), so the screenshots show
`XXXXXXXXXXXXXXX`.

## Switches

`PV_SCRIPT`, `PV_ROOT`, `PV_OUT`, `PV_RES`, `PV_STICKS`, `PV_STAT`,
`PV_GFXTRACE` and `PV_MAXFRAMES` are the library's (its README). `pv_app.c`
adds:

| Variable | Effect |
|---|---|
| `PV_DRIVE` | the dumps (above) |
| `PV_ST=F3` | the self-test status byte (F3: running, 70 % done) |
| `PV_RISE=1` | temperatures that rise under load |
| `PV_TEMP_OFF=n` | add n degrees to every reading |
| `PV_383_RC=80010003` | syscall 383 refused with this rc |
