# Preview on the host

The app's own sources compile for the Mac (or Linux) against stub PSL1GHT
headers, and run in a small simulated world: the app's renderer
(`source/gfx/gfx.c`) on a software back end, a scripted controller, 1/60 s per
frame, the `/dev_*` paths inside a sandbox folder, and an emulated drive that
answers syscall 616 from a console's sector dumps. It renders chosen frames to
PNG. It is a check of layouts and text, not of the console: the RSX timing, the
real file speeds and the network are not simulated.

Needs: zsh, a C compiler (`cc`), Python 3 with Pillow for the PNG step
(`PYTHON=...` picks the interpreter, as in `build.sh`).

```sh
zsh tools/preview/build.zsh            # build/pv; PV_CFLAGS adds cc flags (below)
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

## The renderer's back end

`gfx_soft.c` is the preview's back end of `source/gfx` (the console's is
`gfx_rsx.c`), so the preview runs the real `gfx.c`: the batching, the
viewport and the vertex area with its drains. The draws wait in a list until
a drain or the end of the frame, as the RSX reads the vertices only after a
flush; then `raster.c` draws them, and the vertices that they used become NaN.
A draw that reads a vertex from before the last drain draws nothing, and the
snapshot shows it. A build with a small vertex area drains many times in
every frame, and must give the same snapshots:

    PV_CFLAGS='-DGFX_VTX_BYTES=8192' zsh tools/preview/build.zsh

`gfx_vram` gets the RSX memory that gfx leaves free on the console: 249 MB (the
`gcmGetConfiguration` size) less what `gfx_rsx.c` takes first, in its order
and alignment: the two colour buffers and the two fragment programs. So the
RSX memory test (scenario `memrsx`) counts 231 MB at 1080p, 239 at 720p and
244 at 480p. 2.0.0 counted 222, 230 and 235 (its depth buffer and local
vertex buffer took 9 MB).

## gfx traces

`PV_GFXTRACE=file` writes every gfx call of the app, for the end-to-end check
of the trace harness (replay through `gfx.c` and `gfx_rsx.c`). `gfx_trace.c`
wraps the API: `build.zsh` compiles `gfx.c` with it renamed to `gfx_real_*`.
One line per call, integers in hex, floats as hex floats (`%a`, exact):

    video 0x780 0x438                      the output that gfx_init set
    gfx_init = 0
    gfx_vram 0x2aa80 0x80 = 0xfd2200       size, align = the RSX offset (or NULL)
    gfx_viewport 0x1.0cp+6 0x1.3p+5 0x1p+0 0x1p+0
    gfx_begin
    gfx_texture 0xfd2200 0x1a0 0xd2 0x340 0    offset w h pitch linear, or NULL
    gfx_prim 8 4                           type n, then n vertex lines:
    v 0x1.28p+5 0x1.7p+4 0xf2f5faff 0x0p+0 0x1.b6db7p-2
                                           x y rgba u v, as the app wrote them
    gfx_end
    repeat 57                              57 more frames equal to the one before
    end 7402                               the number of frames

A frame is the calls up to and with `gfx_end`. The vertex lines of a
`gfx_prim` go out at the app's next gfx call (then they are written).
`gfx_vram` of the RSX memory test comes from a job thread, so its line can
sit between two calls of a frame. A replay maps the offset of a `gfx_texture`
line to the offset that its own `gfx_vram` gave for the same call (a layout
other than the preview's, such as the diagnostic build, moves the blocks), and
passes one `gfx_tex` object per distinct texture: gfx compares textures by
address. `PV_GFXTRACE_FRAMES=n`
ends the trace after n frames (the app runs on). Run one scenario per trace
file: `run.zsh all` would write every scenario into the same file.

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
| `PV_STAT=1` | print the most vertices and draws in a frame and the drains at exit (the area holds 49,152 vertices) |
| `PV_GFXTRACE=file` | write a gfx trace (above); `PV_GFXTRACE_FRAMES=n` stops it after n frames |
| `PV_MAXFRAMES=n` | stop after n frames |
