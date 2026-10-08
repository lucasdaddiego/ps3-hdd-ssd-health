#!/bin/zsh
# Build build/pv: the app's own sources for the host, against the stubs.
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
cc -O2 -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-parameter -Wno-incompatible-pointer-types-discards-qualifiers -Wno-macro-redefined \
   -I"$P/stub" -I"$R/source" -o "$P/build/pv" "$R"/source/*.c "$P/stubs.c" "$P/build/fonts_bin.c" -lm -lpthread
print "built ${P#$R/}/build/pv"
