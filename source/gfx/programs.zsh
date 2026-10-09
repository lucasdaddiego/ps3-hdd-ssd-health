#!/bin/zsh
# Rebuilds gfx's shader programs from their assembly with the SDK's cgcomp -a,
# and checks that the words equal fp_words and vp_words in gfx_rsx.c, and the
# vertex program's input and output masks VP_INPUTS and VP_OUTPUTS.
#   zsh source/gfx/programs.zsh        (exit 0: equal)
# cgcomp -a drops an unknown opcode and still exits 0, so the script also
# checks the instruction count and that only the last one has the END bit.
emulate -L zsh
setopt err_return pipe_fail
G=${0:A:h}
cgcomp=${PS3DEV:-$HOME/ps3dev}/bin/cgcomp
[[ -x $cgcomp ]] || { print -u2 "no $cgcomp (see sdk_setup.sh)"; return 2 }
t=$(mktemp -d)
trap 'rm -rf $t' EXIT
cat > $t/fp_color.asm <<'EOF'
!!FP2.0
MOV o[COLH], f[COL0];
END
EOF
cat > $t/fp_texcolor.asm <<'EOF'
!!FP2.0
TEXX R0, f[TEX0], texture[0], 2D;
MULX o[COLH], R0, f[COL0];
END
EOF
cat > $t/vp_pass.asm <<'EOF'
!!VP2.0
MOV o[HPOS], v[0];
MOV o[COL0], v[3];
MOV o[TEX0], v[8];
END
EOF
$cgcomp -a -f $t/fp_color.asm $t/fp_color.fpo
$cgcomp -a -f $t/fp_texcolor.asm $t/fp_texcolor.fpo
$cgcomp -a -v $t/vp_pass.asm $t/vp_pass.vpo
python3 -I - $t $G/gfx_rsx.c <<'PY'
import re, struct, sys
t, src = sys.argv[1], open(sys.argv[2]).read()
def array(name):           # the hex words of a C array in gfx_rsx.c
    body = re.search(name + r'\[[^=]*=\s*\{(.*?)\};', src, re.S).group(1)
    return [int(w, 16) for w in re.findall(r'0x([0-9a-f]{8})', body)]
def define(name):         # a hex #define of gfx_rsx.c
    return int(re.search(r'#define ' + name + r'\s+0x([0-9a-fA-F]+)', src).group(1), 16)
fp, vp = array('fp_words'), array('vp_words')
want = {'fp_color.fpo': (1, fp[:4]), 'fp_texcolor.fpo': (2, fp[4:]), 'vp_pass.vpo': (3, vp)}
ok = True
for name, (n, words) in want.items():
    d = open('%s/%s' % (t, name), 'rb').read()
    got = struct.unpack('>H', d[10:12])[0]
    off = struct.unpack('>I', d[20:24])[0]
    w = list(struct.unpack('>%dI' % (4 * got), d[off:off + 16 * got]))
    if name.startswith('fp'):   # END: bit 0 of each first word, halfwords swapped back
        ends = [((x >> 16) | (x << 16)) & 1 for x in w[0::4]]
    else:                       # END: bit 0 of each fourth word
        ends = [x & 1 for x in w[3::4]]
    good = got == n and ends == [0] * (n - 1) + [1] and w == words
    ok &= good
    print('%s %s: %d instructions, END %s, %s' % ('ok ' if good else 'BAD', name, got, ends,
          'the words of gfx_rsx.c' if w == words else 'NOT the words of gfx_rsx.c: ' + ' '.join('%08x' % x for x in w)))
    if name.startswith('vp'):   # the input and output masks in the header
        masks = struct.unpack('>II', d[0x18:0x20])
        mgood = masks == (define('VP_INPUTS'), define('VP_OUTPUTS'))
        ok &= mgood
        print('%s %s: masks %04x %04x, %s' % ('ok ' if mgood else 'BAD', name, masks[0], masks[1],
              'VP_INPUTS and VP_OUTPUTS' if mgood else 'NOT VP_INPUTS and VP_OUTPUTS'))
sys.exit(0 if ok else 1)
PY
