#!/bin/zsh
# Make $PS3DEV (default ~/ps3dev) an SDK whose apps start on this console:
# the ps3dev nightly-2026-07-26 bundle (host tools, ppu-gcc 7.2) with PSL1GHT's
# 2020 runtime (6e565a7), pinned to a full commit hash: a newer PSL1GHT runtime
# does not start on this console. The app uses nothing from the bundle's
# portlibs/: it draws through its own source/gfx/.
# Needs: zsh, curl, git, make, shasum (macOS arm64/x86_64 or Linux x86_64).
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
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  bundle=ps3dev-macos-ARM64.tar.gz; sum=cf69b6b520a871479dea2b734edcd5023e5ee472f7d28907fc6aaa6ecdf6ae77 ;;
    Darwin-x86_64) bundle=ps3dev-macos-X64.tar.gz;   sum=30c6189ed0ecfaa0af1941299215c3cc2063ec6babed3dcb83aaa41cc59b39c7 ;;
    Linux-x86_64)  bundle=ps3dev-linux-X64.tar.gz;   sum=dcbed747e094c6a382dae5b0aacc322a1d4390d9f2849a7d750a96ea398ee8ab ;;
    *) print -u2 "no ps3dev nightly-2026-07-26 bundle for $(uname -s) $(uname -m)"; exit 1 ;;
  esac
  d=$(mktemp -d)
  curl -fL --retry 3 -o $d/$bundle https://github.com/ps3dev/ps3dev/releases/download/nightly-2026-07-26/$bundle
  [[ $(shasum -a 256 $d/$bundle | cut -d' ' -f1) == $sum ]] \
    || { print -u2 "bundle checksum mismatch for $bundle"; exit 1 }
  mkdir -p ${PS3DEV:h}
  tar xzf $d/$bundle -C ${PS3DEV:h}
  rm -r $d
fi

PSL1GHT_REV=6e565a70e927f55813babe86dc6f64149535f908   # 2020-11-25
t=$(mktemp -d)
git clone -q https://github.com/ps3dev/PSL1GHT $t/psl1ght
git -C $t/psl1ght checkout -q $PSL1GHT_REV
make -C $t/psl1ght/ppu --no-print-directory > $t/psl1ght.log 2>&1 || { tail -20 $t/psl1ght.log; exit 1 }
make -C $t/psl1ght/ppu install --no-print-directory > /dev/null
rm -rf $t
print "PSL1GHT 6e565a7, $(date -u +%F)" > $PS3DEV/.runtime-2020
print "$PS3DEV: runtime PSL1GHT 6e565a7 (2020)"
