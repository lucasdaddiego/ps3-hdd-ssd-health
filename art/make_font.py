#!/usr/bin/env python3
"""Generate data/fonts.bin: the app's bitmap fonts, rendered from Inter.

    python3 art/make_font.py <Inter[opsz,wght].ttf> [preview.png]

Font: Inter (SIL Open Font License 1.1, no Reserved Font Name), the variable
Inter[opsz,wght].ttf from github.com/rsms/inter. The bitmaps are a modified
version of that font, so they stay under the OFL; the license text ships in
the pkg as USRDIR/OFL.txt (pkgfiles/USRDIR/OFL.txt).

The app draws in a 1920x1080 space, 1:1 at 1080p output, so each style is
rendered at the exact pixel size it is drawn at: no scaling, sharp text.
Characters 32..126 with proportional digits, then 127 (blank) and 128..137:
the digits 0..9 again, tabular (one advance, the glyph centred in it), for
tables and counters that must not move. source/ui.c maps them with TNUM.

fonts.bin, big-endian (the PPU's order), read by source/ui.c:
    'PHF1', u16 font count, u16 first char, u16 char count, u16 atlas columns
    per font: u16 cell w, cell h, baseline (from the cell top), left pad,
              ascent, line height, atlas w, atlas h;
              u8 advance[char count]; zero bytes to a multiple of 4;
              atlas, 4 bits per pixel (alpha 0..15), two pixels per byte,
              the left pixel in the high nibble.
A glyph is drawn as its whole cell: the cell's left edge is the pen position
minus the left pad, its top is the line top plus ascent minus baseline.
"""
import math, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

FIRST, LAST, COLS = 32, 137, 16
TNUM = 128                                    # the tabular digits start here
# name, pixel size, weight. The order is the F_* enum in source/ui.h.
STYLES = [('small', 22, 460), ('body', 27, 460), ('med', 32, 620), ('large', 52, 600), ('title', 44, 740)]
GAMMA = 0.85                                  # light text on a dark background: a little more weight at the edges

def load(ttf, px, weight):
    f = ImageFont.truetype(ttf, px)
    f.set_variation_by_axes([min(32, max(14, px)), weight])
    return f

def build(ttf, px, weight):
    f = load(ttf, px, weight)
    asc, desc = f.getmetrics()
    # slot i draws glyph[i]; slots 0..94 are 32..126, 95 is blank, 96..105 the tabular digits
    glyph = [chr(c) for c in range(FIRST, 127)] + [' '] + list('0123456789')
    adv = [round(f.getlength(g)) for g in glyph]
    off = [0] * len(glyph)
    dmax = max(round(f.getlength(d)) for d in '0123456789')
    for i in range(TNUM - FIRST, len(glyph)):
        off[i] = round((dmax - f.getlength(glyph[i])) / 2)
        adv[i] = dmax
    left, right, top, bottom = 0, 0, -asc, desc
    for i, g in enumerate(glyph):
        x0, y0, x1, y1 = f.getbbox(g, anchor='ls')
        left, right = min(left, x0 + off[i]), max(right, x1 + off[i], adv[i])
        top, bottom = min(top, y0), max(bottom, y1)
    pad = 1 - math.floor(left)                 # one clear pixel around every glyph
    cw = pad + math.ceil(right) + 1
    cw += cw & 1                               # even: the atlas pitch is a multiple of 64 bytes
    base = 1 - math.floor(top)
    ch = base + math.ceil(bottom) + 1
    rows = (len(glyph) + COLS - 1) // COLS
    atlas = Image.new('L', (COLS * cw, rows * ch), 0)
    d = ImageDraw.Draw(atlas)
    for i, g in enumerate(glyph):
        x, y = (i % COLS) * cw, (i // COLS) * ch
        d.text((x + pad + off[i], y + base), g, font=f, fill=255, anchor='ls')
    return dict(cw=cw, ch=ch, base=base, pad=pad, asc=asc, line=asc + desc, adv=adv, atlas=atlas)

def pack(fonts):
    out = bytearray(b'PHF1' + struct.pack('>HHHH', len(fonts), FIRST, LAST - FIRST + 1, COLS))
    for ft in fonts:
        a = ft['atlas']
        out += struct.pack('>8H', ft['cw'], ft['ch'], ft['base'], ft['pad'], ft['asc'], ft['line'], a.width, a.height)
        out += bytes(ft['adv'])
        out += bytes(-len(out) % 4)
        px = a.tobytes()
        q = [min(15, round(((v / 255) ** GAMMA) * 15)) for v in px]
        out += bytes((q[i] << 4) | q[i + 1] for i in range(0, len(q), 2))
        out += bytes(-len(out) % 4)
    return bytes(out)

def unpack(data):
    """Read fonts.bin back the way source/ui.c does."""
    assert data[:4] == b'PHF1'
    n, first, count, cols = struct.unpack_from('>HHHH', data, 4)
    pos, fonts = 12, []
    for _ in range(n):
        cw, ch, base, pad, asc, line, aw, ah = struct.unpack_from('>8H', data, pos)
        pos += 16
        adv = list(data[pos:pos + count])
        pos += count
        pos += -pos % 4
        raw = data[pos:pos + aw * ah // 2]
        pos += aw * ah // 2
        pos += -pos % 4
        px = bytearray()
        for b in raw:
            px += bytes(((b >> 4) * 17, (b & 15) * 17))
        fonts.append(dict(cw=cw, ch=ch, base=base, pad=pad, asc=asc, line=line, adv=adv, cols=cols, first=first,
                          atlas=Image.frombytes('L', (aw, ah), bytes(px))))
    return fonts

def preview(fonts, path):
    """Draw sample lines 1:1 on the app's background, glyph cells placed as the PS3 places them."""
    tab = lambda t: ''.join(chr(TNUM + ord(c) - 48) if c.isdigit() else c for c in t)
    samples = [(4, 'PS3 Health  v2.0.0'), (2, 'Drive   Cooling   Controller'), (1, 'Dahua V800 1TB   firmware DHTSA101'),
               (1, 'Health OK. Power-on 934 h (38 d), 174 power cycles, 2.44 TB written.'),
               (1, 'ID  Attribute                 Value  Worst  Thresh  Raw'), (1, tab('  9  Power-on hours  1111  96    100       0  934')),
               (0, 'CROSS read the drive   TRIANGLE demo mode   CIRCLE home   START exit'), (3, 'Write 25 MB/s  52 C  100%'),
               (0, 'abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 ()[]{}<>/\\|+-*=_.,:;!?\'"#$%&@^`~')]
    H = sum(fonts[s]['line'] + 14 for s, _ in samples) + 40
    img = Image.new('RGB', (1920, H), (12, 18, 34))
    y = 20
    for s, line in samples:
        ft = fonts[s]
        x = 40
        for c in line:
            k = ord(c) - ft['first']
            cx, cy = (k % ft['cols']) * ft['cw'], (k // ft['cols']) * ft['ch']
            cell = ft['atlas'].crop((cx, cy, cx + ft['cw'], cy + ft['ch']))
            img.paste((242, 245, 250), (x - ft['pad'], y + ft['asc'] - ft['base']), cell)
            x += ft['adv'][k]
        y += ft['line'] + 14
    img.save(path)

if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    data = pack([build(sys.argv[1], px, w) for _, px, w in STYLES])
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', 'data', 'fonts.bin')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'wb').write(data)
    fonts = unpack(data)
    for (name, px, w), ft in zip(STYLES, fonts):
        a = ft['atlas']
        print(f"{name:6} {px:3} px w{w}: cell {ft['cw']}x{ft['ch']}, line {ft['line']}, atlas {a.width}x{a.height} "
              f"({a.width * a.height * 2 // 1024} KB as A4R4G4B4)")
    print(os.path.relpath(out, os.path.join(here, '..')), len(data), 'bytes')
    if len(sys.argv) == 3:
        preview(fonts, sys.argv[2])
