#!/bin/zsh
# Make $PS3DEV (default ~/ps3dev) an SDK whose apps start on this console:
# the ps3dev nightly-2026-07-26 bundle (host tools, ppu-gcc 7.2) with PSL1GHT's
# 2020 runtime (6e565a7) and tiny3D + libfont3d rebuilt against it.
#
# Apps linked with the bundle's own runtime (PSL1GHT 2021+) die before main on
# HFW 4.93 + PS3HEN 3.6.0: black screen, back to the XMB after ~10 s, even a
# main() that only sleeps 60 s (2026-10-06). The same one-line app with the
# 2020 runtime runs. The 2021 changes are the heap rewrite (d2ea732) and a
# pre-main strdup for chdir/getcwd (99dd0b9); which one is to blame is untested.
set -e
export PS3DEV=${PS3DEV:-$HOME/ps3dev}
export PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

if [[ ! -x $PS3DEV/ppu/bin/ppu-gcc ]]; then
  [[ ${PS3DEV:t} == ps3dev ]] || { print -u2 "PS3DEV must end in /ps3dev (the bundle extracts as ps3dev/)"; exit 1 }
  d=$(mktemp -d)
  gh release download nightly-2026-07-26 -R ps3dev/ps3dev -p 'ps3dev-macos-ARM64.tar.gz' -D $d
  [[ $(shasum -a 256 $d/ps3dev-macos-ARM64.tar.gz | cut -d' ' -f1) == cf69b6b520a871479dea2b734edcd5023e5ee472f7d28907fc6aaa6ecdf6ae77 ]] \
    || { print -u2 "bundle checksum mismatch"; exit 1 }
  mkdir -p ${PS3DEV:h}
  tar xzf $d/ps3dev-macos-ARM64.tar.gz -C ${PS3DEV:h}
  rm -r $d
fi

t=$(mktemp -d)
git clone -q https://github.com/ps3dev/PSL1GHT $t/psl1ght
git -C $t/psl1ght checkout -q 6e565a7
make -C $t/psl1ght/ppu --no-print-directory > $t/psl1ght.log 2>&1 || { tail -20 $t/psl1ght.log; exit 1 }
make -C $t/psl1ght/ppu install --no-print-directory > /dev/null
git clone -q https://github.com/wargio/tiny3D $t/tiny3d
git -C $t/tiny3d checkout -q 9b02ae6
make -C $t/tiny3d/lib install --no-print-directory > /dev/null 2>&1
make -C $t/tiny3d/libfont install --no-print-directory > /dev/null 2>&1
rm -rf $t
print "PSL1GHT 6e565a7 + tiny3D 9b02ae6, $(date -u +%F)" > $PS3DEV/.runtime-2020
print "$PS3DEV: runtime PSL1GHT 6e565a7 (2020), tiny3D 9b02ae6"
