# Preview on the host

The app's own sources compile for the Mac (or Linux) against stub PSL1GHT
headers, and run in a small simulated world: a software rasterizer for tiny3d,
a scripted controller, 1/60 s per frame, the `/dev_*` paths inside a sandbox
folder, and an emulated drive that answers syscall 616 from a console's sector
dumps. It renders chosen frames to PNG. It is a check of layouts and text,
not of the console: the RSX timing, the real file speeds and the network are
not simulated.

Needs: zsh, a C compiler (`cc`), Python 3 with Pillow for the PNG step
(`PYTHON=...` picks the interpreter, as in `build.sh`).

```sh
zsh tools/preview/build.zsh            # build/pv
zsh tools/preview/run.zsh              # every scenario; or one: run.zsh drive
zsh tools/preview/shots.zsh            # the README screenshots, into docs/
```

`run.zsh` writes its snapshots to `build/out/*.png`; the scenario names and pad
scripts are in its `case`. Everything under `build/` is ignored by git.

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

## Pad scripts and switches

`PV_SCRIPT` is a list of `frame:ACTION`: a button name (`CROSS`, `UP`,
`L2+UP+START`) pressed on that frame, `from-to:BUTTONS` held over a range,
`SPIN` for both sticks turning in circles, `SNAP=name` for a snapshot, `EXIT` to stop.

| Variable | Effect |
|---|---|
| `PV_ROOT`, `PV_OUT`, `PV_DRIVE` | the sandbox, the snapshot folder, the dumps |
| `PV_RES=720` | 720p output instead of 1080p |
| `PV_ST=F3` | the self-test status byte (F3: running, 70 % done) |
| `PV_RISE=1` | temperatures that rise under load |
| `PV_TEMP_OFF=n` | add n degrees to every reading |
| `PV_383_RC=80010003` | syscall 383 refused with this rc |
| `PV_STICKS=lx,ly,rx,ry` | stick offsets from the centre |
| `PV_VSTAT=1` | print tiny3d's vertex memory per frame at exit (1 MB is the limit) |
| `PV_MAXFRAMES=n` | stop after n frames |
