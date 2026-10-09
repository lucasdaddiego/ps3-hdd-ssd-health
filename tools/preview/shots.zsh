#!/bin/zsh
# The README screenshots: renders at 1920x1080, saved as 1280x720 PNG in docs/.
#   zsh tools/preview/shots.zsh
# The drive data comes from $PV_DRIVE (default ~/.local/share/ps3-health/drive)
# through a masked copy in build/: identify.bin gets 15 x "X" as its serial, no
# WWN, zeros in the reserved words 236-254 (a vendor string on the test SSD)
# and a new checksum. The dumps themselves never go into the repo.
set -e
P=${0:A:h}
R=${P:h:h}
DRIVE=${PV_DRIVE:-$HOME/.local/share/ps3-health/drive}
[[ -x $P/build/pv ]] || { print -u2 "run build.zsh first"; exit 1 }
[[ -f $DRIVE/identify.bin ]] || { print -u2 "no dumps in $DRIVE (see README.md)"; exit 1 }
M=$P/build/drive_masked OUT=$P/build/shots
rm -rf $M $OUT
mkdir -p $M $OUT
cp $DRIVE/*.bin $M/
python3 -I - "$M/identify.bin" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
s = 'X' * 15 + ' ' * 5                       # words 10-19, left-justified, two chars per word, swapped in memory
for w in range(10):
    d[20 + 2 * w], d[21 + 2 * w] = ord(s[2 * w + 1]), ord(s[2 * w])
d[216:224] = bytes(8)                        # words 108-111: the world wide name
d[472:510] = bytes(38)                       # words 236-254: reserved, vendor data on some drives
d[511] = (-sum(d[:511])) % 256               # word 255: the 512 bytes sum to 0
open(p, 'wb').write(d)
PY
J=$'done error_log rc=0x00000000 ok=1\ndone gpl_devstat rc=0x00000000 ok=1\ndone gpl_phy rc=0x00000000 ok=1\ndone fan_policy rc=0x00000000 ok=1\n'
# the tiles of the home shot: results from the test console (2.0.0; the RSX memory
# test from 2.0.1), the hours as in the dumps
ST=$'drive|1|2026-10-08|Dahua V800 2.5 inch SATA 1TB SSD|Health OK, 934 h (38 d)\ncooling|1|2026-10-08|Cell 51>67 C, RSX 55>71 C|fan 33>49%, 2:00 load\npad|1|2026-10-08|Drift L 0.8%, R 0.8%|Circle L 100%, R 100%\ndisplay|1|2026-10-08|1920x1080p 16:9|Visible area 1920 x 1080\ntransfer|1|2026-10-08|USB 3/14 MB/s|net 33, LAN 79 Mbit/s\nmemory|1|2026-10-08|XDR 194 MB: 0 errors|RSX 231 MB: 0 errors\n'
shot() {   # name script [VAR=value ...]; AREA, JOURNAL, STATE from the caller
    local name=$1 script=$2
    shift 2
    local sbx=$P/build/sbx/shot_$name
    rm -rf $sbx
    mkdir -p $sbx/dev_hdd0/tmp/ps3_health $sbx/dev_flash/vsh/etc $sbx/dev_usb000
    print 'release:04.9300:' > $sbx/dev_flash/vsh/etc/version.txt
    [[ -n $AREA ]] && print -r -- "$AREA" > $sbx/dev_hdd0/tmp/ps3_health/safe_area.txt
    [[ -n $JOURNAL ]] && print -rn -- "$JOURNAL" > $sbx/dev_hdd0/tmp/ps3_health/journal.txt
    [[ -n $STATE ]] && print -rn -- "$STATE" > $sbx/dev_hdd0/tmp/ps3_health/state.txt
    env PV_ROOT=$sbx PV_DRIVE=$M PV_OUT=$OUT PV_SCRIPT="$script" "$@" $P/build/pv 2>&1 | grep -v '^snap' || true
}
FULL='0 0 1920 1080'
AREA=$FULL JOURNAL=$J STATE=$ST shot home "3:SNAP=home 4:EXIT"
AREA=$FULL JOURNAL=$J STATE= shot drive "4:CROSS 12:CROSS 120:SNAP=attributes 122:RIGHT 130:SNAP=summary 132:CIRCLE 140:TRIANGLE 150:SNAP=help 151:EXIT"
AREA=$FULL JOURNAL=$J STATE= shot cool "4:RIGHT 6:CROSS 60:CROSS 7400:SNAP=cooling 7401:EXIT" PV_RISE=1
AREA=$FULL JOURNAL=$J STATE= shot pad "4:RIGHT 6:RIGHT 8:CROSS 20-30:L2+UP+TRIANGLE+START 25:SNAP=controller 26:EXIT" PV_STICKS=3,-2,0,1
AREA= JOURNAL= STATE= shot calib "3:SNAP=calibration 4:EXIT"
AREA=$FULL JOURNAL=$J STATE= shot mem "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 20:CROSS 2000:SNAP=memory 2001:EXIT"
${PYTHON:-python3} -I - "$OUT" "$R/docs" <<'PY'
import glob, os, sys
from PIL import Image
out, docs = sys.argv[1], sys.argv[2]
for p in sorted(glob.glob(out + '/*.ppm')):
    name = os.path.basename(p)[:-4] + '.png'
    Image.open(p).convert('RGB').resize((1280, 720), Image.LANCZOS).save(os.path.join(docs, name), optimize=True)
    print('docs/' + name)
PY
