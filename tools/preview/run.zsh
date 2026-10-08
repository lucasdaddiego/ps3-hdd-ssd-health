#!/bin/zsh
# Preview scenarios: each runs the app in a fresh sandbox with a pad script.
#   zsh tools/preview/run.zsh [scenario]     (default: all; names in the case below)
# Snapshots go to build/out/*.png. The drive answers from the dumps in
# $PV_DRIVE (default ~/.local/share/ps3-health/drive); without them the Drive
# scenarios show the probe screen.
P=${0:A:h}
OUT=$P/build/out
DRIVE=${PV_DRIVE:-$HOME/.local/share/ps3-health/drive}
[[ -x $P/build/pv ]] || { print -u2 "run build.zsh first"; exit 1 }
[[ -f $DRIVE/identify.bin ]] || print -u2 "no dumps in $DRIVE: the Drive scenarios show the probe screen"
mkdir -p $OUT
J_DONE=$'done error_log rc=0x00000000 ok=1\ndone gpl_devstat rc=0x00000000 ok=1\ndone gpl_phy rc=0x00000000 ok=1\ndone fan_policy rc=0x00000000 ok=1\n'
DEF_SAFE='67 38 1853 1042'
scn() {   # name script [VAR=value ...]; SAFE, STATE, JOURNAL, DEMO from the caller
    local name=$1 script=$2
    shift 2
    local sbx=$P/build/sbx/$name
    [[ -d $sbx ]] && chmod -R u+w $sbx
    rm -rf $sbx
    mkdir -p $sbx/dev_hdd0/tmp/ps3_health $sbx/dev_flash/vsh/etc $sbx/dev_usb000
    print 'release:04.9300:' > $sbx/dev_flash/vsh/etc/version.txt
    [[ -n $SAFE ]] && print -r -- "$SAFE" > $sbx/dev_hdd0/tmp/ps3_health/safe_area.txt
    [[ -n $STATE ]] && print -rn -- "$STATE" > $sbx/dev_hdd0/tmp/ps3_health/state.txt
    [[ -n $JOURNAL ]] && print -rn -- "$JOURNAL" > $sbx/dev_hdd0/tmp/ps3_health/journal.txt
    [[ -n $NOFS ]] && chmod 555 $sbx/dev_hdd0/tmp/ps3_health
    if [[ -n $DEMO ]]; then mkdir -p $sbx/dev_hdd0/tmp/ps3_health/demo; cp $DRIVE/*.bin $sbx/dev_hdd0/tmp/ps3_health/demo/ 2>/dev/null; fi
    env PV_ROOT=$sbx PV_DRIVE=$DRIVE PV_OUT=$OUT PV_SCRIPT="$script" "$@" $P/build/pv 2>&1 | grep -v '^snap' | head -5
    print -- "$name: exit ${pipestatus[1]}"
}
again() {   # name script: the next session in the sandbox of the last scn of name, as that run left it
    env PV_ROOT=$P/build/sbx/$1 PV_DRIVE=$DRIVE PV_OUT=$OUT PV_SCRIPT="$2" $P/build/pv 2>&1 | grep -v '^snap' | head -5
    print -- "$1 again: exit ${pipestatus[1]}"
}
case ${1:-all} in
first|all)
    SAFE= STATE= JOURNAL= scn first "3:SNAP=calib_first 4:SQUARE 8:SNAP=calib_single 9:CIRCLE 14:SNAP=home_empty 15:EXIT" ;|
drive|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn drive "4:CROSS 12:SNAP=drive_start 14:CROSS 80:SNAP=prompt_errlog 82:CROSS 160:SNAP=prompt_gpl 162:CROSS 260:SNAP=drive_attr 262:R1 270:SNAP=drive_attr2 272:RIGHT 280:SNAP=drive_summary 282:LEFT 296:TRIANGLE 306:SNAP=selftest_choice 308:CIRCLE 316:CIRCLE 326:SNAP=home_drive 328:TRIANGLE 340:SNAP=help_drive 342:CIRCLE 350:SNAP=home_after_help 351:EXIT" ;|
running|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn running "4:CROSS 12:CROSS 120:SNAP=drive_running 122:TRIANGLE 132:SNAP=selftest_screen 133:EXIT" PV_ST=F3 ;|
probe|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn probe "4:CROSS 12:CROSS 120:SNAP=probe 121:EXIT" PV_DRIVE=/nonexistent ;|
demo|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= DEMO=1 scn demo "4:CROSS 12:TRIANGLE 80:SNAP=demo_attr 81:EXIT" ;|
cool|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn cool "4:RIGHT 6:CROSS 14:SNAP=prompt_fan 16:CROSS 60:SNAP=cool_main 62:CROSS 3000:SNAP=cool_load 7400:SNAP=cool_result 7401:EXIT" PV_RISE=1 ;|
pad|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn pad "4:RIGHT 6:RIGHT 8:CROSS 20-30:L2+UP+TRIANGLE+START 25:SNAP=pad_live 40-42:SELECT 41:CROSS 100:SNAP=pad_rest 300-302:SELECT 301:TRIANGLE 310-900:SPIN 600:SNAP=pad_circle 1000:SNAP=pad_after 1001:EXIT" PV_STICKS=3,-2,0,1 ;|
display|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn display "4:DOWN 6:CROSS 14:SNAP=display_main 16:CROSS 24:SNAP=calib 26:CIRCLE 30:TRIANGLE 36:SNAP=pat_white 38:RIGHT 40:RIGHT 42:RIGHT 44:RIGHT 46:RIGHT 50:SNAP=pat_ramp 52:RIGHT 56:SNAP=pat_grid 58:RIGHT 62:SNAP=pat_stripes 64:RIGHT 68:SNAP=pat_lag 69:EXIT" ;|
mem|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn mem "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 18:SNAP=mem_main 20:CROSS 40:SNAP=mem_job 2000:SNAP=mem_done 2001:EXIT" ;|
memrsx|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn memrsx "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 20:TRIANGLE 1000:SNAP=mem_rsx 1002:CIRCLE 1010:SNAP=home_memrsx 1011:EXIT"
    grep -h "RSX memory" $P/build/sbx/memrsx/dev_hdd0/tmp/ps3_health/report-*.txt ;|
net|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn net "4:DOWN 5:RIGHT 7:CROSS 15:SNAP=net_main 17:CROSS 400:SNAP=net_usb 402:TRIANGLE 500:SNAP=net_after 501:EXIT" ;|
home|all)
    SAFE=$DEF_SAFE JOURNAL= STATE=$'drive|1|2026-10-08|Dahua V800 1TB|Health OK, 934 h (38 d)\ncooling|2|2026-10-08|Cell 52>74 C, RSX 55>77 C|fan 33>45%, 2:00 load\npad|1|2026-10-08|Drift L 0.8%, R 1.2%|Circle L 96%, R 94%\ndisplay|1|2026-10-08|1920x1080p 16:9|Visible area 1786 x 1004\ntransfer|1|2026-10-08|USB 18/22 MB/s|net 94, LAN 812 Mbit/s\nmemory|3|2026-10-08|XDR 200 MB: 2 errors|RSX 196 MB: 0 errors\n' scn home "3:SNAP=home_filled 4:RIGHT 6:SNAP=home_sel 7:EXIT" ;|
p720|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn p720 "3:SNAP=home_720 4:CROSS 12:CROSS 120:SNAP=drive_720 121:EXIT" PV_RES=720 ;|
memstop|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn memstop "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 20:CROSS 40:CIRCLE 80:SNAP=mem_stopped 82:CIRCLE 90:SNAP=home_memstop 91:EXIT" ;|
coolstop|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn coolstop "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 300:CIRCLE 320:SNAP=cool_stopped 322:CIRCLE 330:SNAP=home_coolstop 331:EXIT" PV_RISE=1 ;|
demohome|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= DEMO=1 scn demohome "4:CROSS 12:TRIANGLE 80:CIRCLE 90:CROSS 98:TRIANGLE 160:CIRCLE 170:SNAP=home_after_demo 171:EXIT"
    ls $P/build/sbx/demohome/dev_hdd0/tmp/ps3_health | grep report ;|
nofs|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= NOFS=1 scn nofs "3:SNAP=nofs_home 4:SQUARE 6:SQUARE 8:SNAP=nofs_square 10:SELECT 14:SNAP=nofs_usb 16:DOWN 17:RIGHT 19:CROSS 27:CROSS 60:SNAP=nofs_net 61:EXIT" ;|
cool383|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn cool383 "4:RIGHT 6:CROSS 16:CROSS 60:SNAP=cool_383 62:CIRCLE 70:EXIT" PV_383_RC=80010003
    grep -nE "cooling|383" $P/build/sbx/cool383/dev_hdd0/tmp/ps3_health/report-*.txt ;|
coolclamp|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn coolcold "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 1600:CIRCLE 1620:SNAP=cool_cold 1621:EXIT" PV_TEMP_OFF=-30
    SAFE=$DEF_SAFE STATE= JOURNAL= scn coolhot "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 1600:CIRCLE 1620:SNAP=cool_hot 1621:EXIT" PV_TEMP_OFF=45 ;|
selfexit|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn selfexit "4:CROSS 12:CROSS 120:TRIANGLE 130:SELECT 140:SNAP=selftest_left 141:EXIT" PV_ST=F3
    ls $P/build/sbx/selfexit/dev_usb000 ;|
speedexit|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn speedexit "4:CROSS 12:CROSS 120:R2 122:R2 200:TRIANGLE 210:SNAP=speed_left 211:EXIT" ;|
reprobe|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= DEMO=1 scn reprobe "4:CROSS 12:CROSS 120:CIRCLE 130:CROSS 140:SNAP=reprobe_start 142:TRIANGLE 200:SNAP=reprobe_demo 201:EXIT" PV_DRIVE=/nonexistent ;|
help|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn help "3:TRIANGLE 14:SNAP=help_empty 15:EXIT"
    SAFE='160 90 1760 990' STATE= JOURNAL= scn helpmin "3:SNAP=home_min 4:TRIANGLE 14:SNAP=help_min 15:EXIT" ;|
helpkept|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn helpkept "4:CROSS 12:CROSS 120:CIRCLE 130:EXIT"
    again helpkept "3:TRIANGLE 14:SNAP=help_kept 15:EXIT" ;|
esac
${PYTHON:-python3} -I - "$OUT" <<'PY'
import glob, os, sys
from PIL import Image
for p in glob.glob(sys.argv[1] + '/*.ppm'):
    Image.open(p).save(p[:-4] + '.png'); os.remove(p)
PY
print "$(ls $OUT | wc -l | tr -d ' ') snapshots in ${OUT#${P:h:h}/}"
