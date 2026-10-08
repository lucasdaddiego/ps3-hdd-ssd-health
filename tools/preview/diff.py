"""A pixel diff of two snapshot folders, for the preview's PNG (or PPM) snapshots.
    python3 tools/preview/diff.py A B [DIFF_DIR]
One line per snapshot name: the count of different pixels and the largest
difference of one channel ("0 pixels, max 0" when equal), or "only in A",
"only in B" or the two sizes. The last line counts the snapshots that
differ. With DIFF_DIR, each snapshot that differs gets DIFF_DIR/<name>.png:
A at a quarter of its brightness, a pixel that differs by 1 in yellow, by
more in red.
Exit status: 0 = all equal, 1 = a difference, 2 = a bad argument.
Needs Pillow."""
import os
import sys

from PIL import Image, ImageChops


def names(folder):
    return {os.path.splitext(f)[0]: os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(('.png', '.ppm'))}


def compare(pa, pb, out):
    a, b = Image.open(pa).convert('RGB'), Image.open(pb).convert('RGB')
    if a.size != b.size:
        return 'size %dx%d, %dx%d' % (a.size + b.size), True
    r, g, bl = ImageChops.difference(a, b).split()
    d = ImageChops.lighter(ImageChops.lighter(r, g), bl)        # the largest channel difference per pixel
    hist = d.histogram()
    n = a.size[0] * a.size[1] - hist[0]
    if not n:
        return '0 pixels, max 0', False
    if out:
        img = Image.eval(a, lambda v: v // 4)
        img.paste((255, 220, 0), mask=d.point(lambda v: 255 if v == 1 else 0))
        img.paste((255, 0, 0), mask=d.point(lambda v: 255 if v > 1 else 0))
        img.save(out)
    return '%d pixels, max %d' % (n, max(i for i, c in enumerate(hist) if c)), True


def main(argv):
    if len(argv) not in (3, 4) or not all(os.path.isdir(p) for p in argv[1:3]):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    a, b = names(argv[1]), names(argv[2])
    out = argv[3] if len(argv) == 4 else None
    if out:
        os.makedirs(out, exist_ok=True)
    differ = 0
    for name in sorted(set(a) | set(b)):
        if name not in b or name not in a:
            msg, bad = 'only in ' + ('A' if name in a else 'B'), True
        else:
            msg, bad = compare(a[name], b[name], out and os.path.join(out, name + '.png'))
        differ += bad
        print('%s: %s' % (name, msg))
    print('%d of %d differ' % (differ, len(set(a) | set(b))))
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
