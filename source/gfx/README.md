# gfx

The app's own 2D renderer: about 500 lines that write the RSX command words
themselves, with no librsx and no asm. The firmware (libgcm_sys) only sets up
the context, the display buffers and the flip.

- `gfx.h`: the API, the vertex and the texture.
- `gfx.c`: the portable front end: the vertex area, the batching and the
  viewport. The host preview (`tools/preview`) builds it too, on its own back
  end (`gfx_soft.c`).
- `gfx_rsx.c`: the console back end: the init, the RSX memory allocator, the
  state, the command ring, the label fence and the flip.
- `programs.zsh`: rebuilds the shader programs and checks their words.

## The frame

- `gfx_begin` sends the state of the frame: 119 words for the surface, the
  viewport, depth off, the alpha test (GEQUAL 0x10), the blend, the vertex
  program, the colour fragment program, the vertex format and the vertex
  buffers. It goes out after every flip, because the flip can reset the
  registers. Nothing is cleared: the app's background covers the whole output
  in every frame.
- `gfx_prim(type, n)` gives room for n vertices of 20 bytes (x, y, the colour
  0xRRGGBBAA, u, v), and the caller writes them with `gfx_put`, one 4-byte
  store per field: GCC 7.2 can merge plain stores into 8-byte ones, which RSX
  memory may not take. The position has two components, so the RSX adds
  z = 0 and w = 1, as for any short vertex attribute. Consecutive QUADS or
  TRIANGLES with the same texture join one draw; a strip or a fan is a draw of
  its own. A texture change binds the atlas and switches the fragment program.
- A draw is BEGIN_END, one VB_VERTEX_BATCH per 256 vertices, BEGIN_END 0. The
  first draw after each PUT move first sends the vertex cache invalidate
  (0x1710, then 0x1714 three times).
- `gfx_end` writes backend label 255 after the last draw, queues the flip
  (`gcmSetFlip`, `gcmSetWaitFlip`), and waits for the flip and for the label.
  When it returns, the frame is on the TV and the RSX has read every vertex,
  so the next frame writes its vertices from index 0 again. The frame stays
  serial: one frame of latency.
- A full vertex area drains: the label, the wait, index 0. Nothing is dropped.
  The largest frame in the preview's scenarios (Help, with the QR code of a
  drive report) takes 13,524 of the 49,152 vertices.
- Every wait (the flip, a label, GET at a ring wrap) ends after about 2 s,
  with 200 us polls: a hung RSX costs time, never the app.
- gfx registers no sysutil callback and no atexit handler. The app's
  `ui_flip` calls `sysUtilCheckCallback` once per frame, so Quit Game lets a
  running job end before the app exits.
- At exit, the app's own exit handler calls `gfx_exit` (through `ui_end`).
  The words after the last flush (the wait for the last flip) and a label go
  out, and gfx waits for the label, as `rsxFinish` does at exit in PSL1GHT's
  samples. Then the RSX is idle, and the ring holds no word that it has not
  read.
- Each check of the init has its own step number. When one fails, the app
  writes the number to `gfx_init.txt` in its folder, because it has no
  picture to show it. Note: `gcmGetConfiguration` returns nothing in the
  firmware (PSL1GHT declares a result), so gfx tests the values it writes.

## Memory

The IO area: 1 MB of main memory (`memalign`), mapped by `gcmInitBody` with a
64 KB command size.

| Offset | Size | Use |
|---|---|---|
| 0 | 4 KB | libgcm |
| 4 KB | 60 KB | the command ring |
| 64 KB | 960 KB | the vertex area: 49,152 vertices of 20 bytes |

`ctx->end` points at the last word of the ring, which is kept for the JUMP
back to the start (`0x20000000 | offset`). When a command does not fit,
`wrap()` moves PUT to the write pointer and waits until the RSX has read up to
there, writes the JUMP, moves PUT to the ring start and waits until the RSX is
there. `wrap()` is also the context callback (through `__get_opd32`) for the
firmware's `gcmSetFlip` and `gcmSetWaitFlip`; gfx keeps 64 words free before
them, so the firmware does not need it.

RSX local memory (`gcmGetConfiguration`) is a bump allocator that never gives
anything back (`gfx_vram`): the two colour buffers (pitch x height each,
aligned to 64), the two fragment programs (256 bytes each, aligned to 256),
then the app's five font atlases and the RSX memory test. There is no depth
buffer: only colour target 0 is on, and the depth test, the depth write and
the stencil test are off, so the zeta surface can point at the back buffer.
The RSX never reads or writes it. There is no vertex buffer in RSX memory
either. So the RSX memory test gets 231 MB at 1080p on the console (2.0.0,
with a depth buffer and a vertex buffer there, got 222).

## The words and their sources

The method names are PSL1GHT's (`rsx/nv40.h`). Where a word has the form that
a librsx function writes, the function is named (PSL1GHT `6e565a7`,
`ppu/librsx/commands_impl.h`); RPCS3 shows how libgcm and the RSX read them.

- Surface (`rsxSetSurface`, colour target 0 only): DMA_COLOR0 and DMA_ZETA in
  local memory, the offsets and the pitches, RT_FORMAT (A8R8G8B8, Z24S8,
  linear, log2 of the width and the height), RT_ENABLE 1, WINDOW_OFFSET 0,
  SHADER_WINDOW (origin at the bottom, pixel centre at the half, the height),
  RT_HORIZ and RT_VERT.
- The output size also sets VIEWPORT_HORIZ and VERT, the scissor and the 8
  clip rectangles (`rsxSetViewport`, `rsxSetScissor`, `rsxSetViewportClip`).
- Viewport: pixel = canvas x scale + translate. The clip z of 0 maps to 0.5,
  the middle of the default depth range.
- Depth: DEPTH_CONTROL 0x110 (`rsxSetZControl`: no near and far cull, z
  clamp, w ignored for the cull), the depth test and the depth write off.
- Alpha test GEQUAL 0x10 (a glyph pixel under 1/16 alpha is not drawn). Blend
  SRC_ALPHA and ONE_MINUS_SRC_ALPHA for colour, ZERO for alpha, ADD
  (`rsxSetAlphaFunc`, `rsxSetBlendFunc`, `rsxSetBlendEquation`).
- Vertex program: one 4-word VP_UPLOAD_INST method per instruction, then
  VP_ATTRIB_EN 0x109 (v0, v3, v8) and VP_RESULT_EN 0x4005 (COL0 front and
  back, TEX0): the masks that cgcomp writes in the header of the .vpo file.
- Fragment programs: in local memory, FP_ACTIVE_PROGRAM = offset | 1, and
  FP_CONTROL with 2 registers (`rsxLoadFragmentProgramLocation`).
- Vertex format: VTXFMT0 2 floats, VTXFMT3 4 unsigned bytes, VTXFMT8 2
  floats, stride 20. Bit 31 of the VTXBUF offsets selects main memory
  (`rsxBindVertexArrayAttrib`).
- Texture unit 0 (`rsxLoadTexture`, `rsxTextureControl`, `rsxTextureFilter`,
  `rsxTextureWrapMode`): A4R4G4B4 with linear rows, 2D, one mipmap, clamped to
  the edge, the ARGB remap, nearest or linear filter, border colour 0. A bind
  first invalidates the texture cache (`rsxInvalidateTextureCache`).
- Vertex cache: 0x1710, then 0x1714 three times (`rsxInvalidateVertexCache`).
- Fence: SEMAPHORE_OFFSET and SEMAPHORE_BACKENDWRITE_RELEASE
  (`rsxSetWriteBackendLabel`). PSL1GHT's rsxtest sample waits on label 255
  the same way. The label numbers have byte 0 equal to byte 2, so the swap of
  those two bytes that librsx makes for this method changes nothing.
- Ring: the JUMP of `rsxSetJumpCommand`.
- Init: CLEAR_COLOR_VALUE, CLEAR_BUFFERS 0xf0 (colour only) and a NOP for
  each colour buffer (`rsxSetClearColor`, `rsxClearSurface`). There is no flip
  at init.

## The shader programs

NV assembly, built with the SDK's `cgcomp` in its assembler mode
(`cgcomp -a -f in.asm out.fpo`, `cgcomp -a -v in.asm out.vpo`):

```
!!FP2.0                    !!FP2.0                             !!VP2.0
MOV o[COLH], f[COL0];      TEXX R0, f[TEX0], texture[0], 2D;   MOV o[HPOS], v[0];
END                        MULX o[COLH], R0, f[COL0];          MOV o[COL0], v[3];
                           END                                 MOV o[TEX0], v[8];
                                                               END
```

The words from offset 0x30 of each output file are `fp_words` and `vp_words`
in `gfx_rsx.c`, and the .vpo header gives the two masks (offsets 0x18 and
0x1c). cgcomp writes the fragment program's halfwords already swapped, as the
RSX reads them. `zsh source/gfx/programs.zsh` builds the three programs and
compares the words and the masks. cgcomp `-a` drops an unknown opcode and
still exits 0, so the script also checks the instruction counts and the END
bits.

The vertex program passes the position through: the vertex holds x and y in
canvas units, and the viewport maps the canvas to pixels.
