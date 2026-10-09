#!/bin/zsh
# Build build/pv: the app's own sources for the host, against the stubs, with
# the real source/gfx/gfx.c on the preview's back end (gfx_soft.c).
#   PV_CFLAGS='-DGFX_VTX_BYTES=8192' zsh build.zsh    (extra flags, e.g. a small vertex area)
set -e
P=${0:A:h}
R=${P:h:h}
mkdir -p $P/build
python3 -I - "$R/data/fonts.bin" "$P/build/fonts_bin.c" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('#include "ppu-types.h"\nconst u8 fonts_bin[] __attribute__((aligned(64))) = {\n')
    for i in range(0, len(d), 24):
        f.write(','.join(str(b) for b in d[i:i + 24]) + ',\n')
    f.write('};\nconst u8 fonts_bin_end[1];\nconst u32 fonts_bin_size = %d;\n' % len(d))
PY
CC=(cc -O2 -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-parameter -Wno-incompatible-pointer-types-discards-qualifiers -Wno-macro-redefined
    -I"$P/stub" -I"$R/source" -I"$R/source/gfx" ${=PV_CFLAGS:-})
# gfx.c's API as gfx_real_*: gfx_trace.c wraps it (PV_GFXTRACE)
$CC -Dgfx_init=gfx_real_init -Dgfx_viewport=gfx_real_viewport -Dgfx_begin=gfx_real_begin -Dgfx_texture=gfx_real_texture \
    -Dgfx_prim=gfx_real_prim -Dgfx_end=gfx_real_end -c "$R/source/gfx/gfx.c" -o "$P/build/gfx.o"
$CC -o "$P/build/pv" "$R"/source/*.c "$P/stubs.c" "$P/gfx_soft.c" "$P/gfx_trace.c" "$P/raster.c" "$P/build/fonts_bin.c" \
    "$P/build/gfx.o" -lm -lpthread
print "built ${P#$R/}/build/pv"
