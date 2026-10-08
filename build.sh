#!/bin/zsh
# Build ps3_health.gnpdrm.pkg. Toolchain: $PS3DEV or ~/ps3dev, made by sdk_setup.sh
# (the ps3dev bundle with PSL1GHT's 2020 runtime). Runs the Mac-side decoder
# test first, and checks the signed EBOOT against the ELF before it trusts the pkg.
set -e
cd "${0:A:h}"
export PS3DEV=${PS3DEV:-$HOME/ps3dev}
export PSL1GHT=$PS3DEV
export PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin
[[ -x $PS3DEV/ppu/bin/ppu-gcc ]] || { print -u2 "no toolchain in $PS3DEV (see README)"; exit 1 }
[[ -f $PS3DEV/.runtime-2020 ]] || { print -u2 "$PS3DEV has the 2021+ PSL1GHT runtime, whose apps die before main on this console: run ./sdk_setup.sh"; exit 1 }
t=$(mktemp -d)
cc -std=c99 -Wall -Wextra -o $t/test_smart test/test_smart.c source/smart.c source/vendor.c source/compat.c source/qrcodegen.c
$t/test_smart
rm -r $t
rm -f ps3_health.pkg ps3_health.gnpdrm.pkg
make pkg
${PYTHON:-python3} -I verify_self.py build/pkg/USRDIR/EBOOT.BIN build/ps3_health.elf
ls -la ps3_health.gnpdrm.pkg
