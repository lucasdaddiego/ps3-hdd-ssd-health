# Preview on the host

The app's own sources compile for the Mac (or Linux) against stub PSL1GHT
headers, and run in a small simulated world: a software RSX for tiny3d's draws,
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
python3 tools/preview/diff.py A B [D]  # pixel diff of two snapshot folders
```

`run.zsh` writes its snapshots to `build/out/*.png`; the scenario names and pad
scripts are in its `case`. Everything under `build/` is ignored by git.

## The raster

`raster.c` draws as the console does, so that the pictures of two renderers
compare pixel for pixel. Each primitive splits into triangles (QUADS into
(0,1,2) and (0,2,3) per quad, a FAN around vertex 0, a STRIP with every second
triangle turned), and each triangle is drawn once, in order. A triangle covers
a pixel whose centre is inside it; a centre on an edge belongs to it only on a
top or a left edge, so triangles that share an edge never cover a pixel twice.
Then the alpha test GEQUAL 16/255, and source-alpha blending in 8-bit math into
an 8-bit XRGB buffer. Model choices, not console facts: positions snap to 1/256
pixel, pixel centres are at +0.5, the interpolation and the texture sampling
are float, the alpha test compares before the 8-bit conversion, and the 8-bit
conversions round to nearest.

tiny3d_AllocTexture gets the RSX memory that tiny3D leaves free on the console:
249 MB less the colour buffers, the depth buffer, the fragment programs and the
vertex buffer. So the RSX memory test (scenario `memrsx`) counts 222 MB at
1080p, 230 at 720p and 235 at 480p, as the console.

## Comparing snapshots

`diff.py` prints one line per snapshot: the count of different pixels and the
largest difference of one channel. With a third folder it writes diff images
(a pixel 1 off in yellow, more in red). The job threads run in real time, so a
screen that shows a running job (`mem_job`) can change from run to run: run
twice and compare to see which snapshots repeat.

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
| `PV_RES=720`, `PV_RES=480` | 1280x720 or 720x480 output instead of 1920x1080 |
| `PV_ST=F3` | the self-test status byte (F3: running, 70 % done) |
| `PV_RISE=1` | temperatures that rise under load |
| `PV_TEMP_OFF=n` | add n degrees to every reading |
| `PV_383_RC=80010003` | syscall 383 refused with this rc |
| `PV_STICKS=lx,ly,rx,ry` | stick offsets from the centre |
| `PV_VSTAT=1` | print tiny3d's vertex memory per frame at exit (1 MB is the limit) |
| `PV_MAXFRAMES=n` | stop after n frames |
