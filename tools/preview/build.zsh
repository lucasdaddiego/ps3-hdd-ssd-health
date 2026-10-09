#!/bin/zsh
# Build build/pv through ps3gfx's preview: the library's tools/preview/build.zsh
# compiles this app's source/ and tools/preview/pv_app.c (the drive, 383, 409)
# with the library's own sources, against its stub headers. The checkout is
# $PS3GFX, else $PS3DEV/src/ps3gfx, the clone sdk_setup.sh makes at the pinned
# commit; a checkout at another commit is announced, since the pkg links the pin.
#   PV_CFLAGS='-DGFX_VTX_BYTES=8192' zsh build.zsh    (extra flags, passed through)
L=${PS3GFX:-${PS3DEV:-$HOME/ps3dev}/src/ps3gfx}
[[ -f $L/tools/preview/build.zsh ]] || { print -u2 "no ps3gfx checkout in $L: run ./sdk_setup.sh, or set PS3GFX"; exit 1 }
rev=$(sed -n 's/^PS3GFX_REV=\([0-9a-f]*\).*/\1/p' ${0:A:h:h:h}/sdk_setup.sh)
head=$(git -C $L rev-parse HEAD 2>/dev/null); head=${head:-unknown}
[[ $head == $rev ]] || print "preview on ps3gfx ${head[1,7]}, the pkg links ${rev[1,7]}"
exec zsh $L/tools/preview/build.zsh APP=${0:A:h:h:h}
