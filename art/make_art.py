#!/usr/bin/env python3
"""Draw the pkg art: ICON0.PNG (320x176, XMB icon) and PIC1.PNG (1920x1080,
XMB background) into pkgfiles/.

    python3 art/make_art.py <Inter variable TTF>

Font: Inter (SIL Open Font License), Inter[opsz,wght].ttf from
github.com/google/fonts/tree/main/ofl/inter; only the rendered text ends up in
the PNGs. Everything else is drawn here: a PS3 Super Slim seen from the front
(draw_console), crossed by a heartbeat line.

The icon has a transparent background: on the XMB the console, the words and
the icon's own trace sit directly on the wallpaper. The wallpaper's trace
starts right of the XMB title text (TRACE_X0) at the icon trace's screen
height (TRACE_Y, both measured from a TV photo of the selected icon), so the
line reads as one that passes behind the title. docs/icon.png is the same
icon on a dark card, for the README.
"""
import math, os, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'pkgfiles')
SS = 4                                     # supersampling factor

NAVY_TOP, NAVY_BOTTOM = (6, 14, 28), (10, 38, 72)
GREEN = (62, 240, 138)
TRACE_Y = 0.467          # the trace's screen height: between the two words of the selected XMB icon
TRACE_X0 = 0.545         # where the wallpaper's trace starts: right of the XMB title and date text
ICON_TRACE = 0.51        # the trace's height inside the icon (0..1), the same point on screen

def font(path, size, weight):
    f = ImageFont.truetype(path, size)
    f.set_variation_by_axes([min(32, max(14, size)), weight])
    return f

def vgradient(w, h, top, bottom):
    g = Image.new('RGB', (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return g.resize((w, h))

def radial_glow(w, h, cx, cy, radius, color, strength):
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    for i in range(40, 0, -1):
        r = radius * i / 40
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=round(strength * (1 - i / 40) ** 1.6))
    m = m.filter(ImageFilter.GaussianBlur(radius / 12))
    return m, Image.new('RGB', (w, h), color)

def pulse_points(x0, x1, y, amp):
    """A heartbeat trace from x0 to x1 around baseline y."""
    w = x1 - x0
    shape = [(0, 0), (.30, 0), (.34, -.12), (.38, 0), (.44, 0), (.47, .28), (.51, -1),
             (.555, .55), (.59, 0), (.66, 0), (.71, -.22), (.77, 0), (1, 0)]
    return [(x0 + fx * w, y + fy * amp) for fx, fy in shape]

def glow_line(base, pts, color, width, blur):
    layer = Image.new('RGBA', base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.line(pts, fill=color + (255,), width=width * 3, joint='curve')
    layer = layer.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(layer)
    d = ImageDraw.Draw(base)
    d.line(pts, fill=color + (255,), width=width, joint='curve')
    d.line(pts, fill=(235, 255, 242, 255), width=max(1, width // 3), joint='curve')

def shadow_text(img, xy, s, f, fill, blur):
    """Text with a soft dark shadow, legible on any XMB wallpaper."""
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((xy[0] + blur * .4, xy[1] + blur * .6), s, font=f, fill=(0, 0, 0, 230))
    img.alpha_composite(layer.filter(ImageFilter.GaussianBlur(blur)))
    ImageDraw.Draw(img).text(xy, s, font=f, fill=fill)

def rounded_mask(size, box, r):
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, r, fill=255)
    return m

def draw_console(img, x0, y0, w):
    """A PS3 Super Slim seen from the front and a little above: the sloped
    top with the sliding disc cover, the lower body, the power light."""
    h = round(w * 0.42)
    d = ImageDraw.Draw(img)
    r = round(w * 0.04)
    # shadow
    sh = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x0 + w * .02, y0 + h * .10, x0 + w * 1.02, y0 + h * 1.06), r, fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(w * .03)))
    # lower body: a dark slab
    body = vgradient(w, round(h * .55), (58, 62, 70), (22, 24, 28)).convert('RGBA')
    by = y0 + round(h * .45)
    img.paste(body, (x0, by), rounded_mask((w, round(h * .55)), (0, 0, w - 1, round(h * .55) - 1), r))
    d.rounded_rectangle((x0, by, x0 + w, y0 + h), r, outline=(12, 14, 18, 255), width=max(2, w // 160))
    # sloped top: a lighter parallelogram, glossy
    top = [(x0 + w * .06, y0), (x0 + w * .94, y0), (x0 + w, by + h * .02), (x0, by + h * .02)]
    d.polygon(top, fill=(96, 102, 112, 255), outline=(40, 44, 50, 255))
    hl = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(hl).polygon([(x0 + w * .08, y0 + h * .02), (x0 + w * .60, y0 + h * .02), (x0 + w * .50, by - h * .02),
                                (x0 + w * .02, by - h * .02)], fill=(255, 255, 255, 46))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(w * .012)))
    # the sliding disc cover: a seam and the cover's lip
    d.line((x0 + w * .36, y0 + h * .03, x0 + w * .30, by), fill=(30, 33, 38, 255), width=max(2, w // 200))
    d.line((x0 + w * .36, y0 + h * .03, x0 + w * .94, y0 + h * .03), fill=(30, 33, 38, 255), width=max(1, w // 300))
    # front: the vents and the two USB ports, the power light
    for i in range(14):
        vx = x0 + w * (.08 + i * .028)
        d.line((vx, by + h * .18, vx, by + h * .38), fill=(14, 16, 20, 255), width=max(1, w // 260))
    for ux in (.62, .68):
        d.rectangle((x0 + w * ux, by + h * .20, x0 + w * (ux + .045), by + h * .30), fill=(10, 12, 16, 255),
                    outline=(70, 76, 86, 255), width=max(1, w // 400))
    lx, ly = x0 + w * .86, by + h * .26
    glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((lx - w * .05, ly - w * .05, lx + w * .05, ly + w * .05), fill=GREEN + (120,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(w * .02)))
    d.ellipse((lx - w * .012, ly - w * .012, lx + w * .012, ly + w * .012), fill=GREEN + (255,))
    # the PS logo spot and the base feet
    d.ellipse((x0 + w * .49, by + h * .40, x0 + w * .51, by + h * .40 + w * .02), outline=(120, 126, 136, 255), width=max(1, w // 300))
    return h

def icon(font_path):
    W, H = 320 * SS, 176 * SS
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dw = round(W * .44)
    dh_nominal = round(dw * .42)
    y0 = round(H * ICON_TRACE - dh_nominal * .62)       # the console sits so its trace is at ICON_TRACE
    dh = draw_console(img, round(W * .03), y0, dw)
    cy = y0 + dh * .62
    pts = pulse_points(0, W * .50, cy, dh * .55) + [(W, cy)]   # the pulse over the console, then flat to the edge
    glow_line(img, pts, GREEN, 4 * SS, 4 * SS)
    tx, room = W * .52, W * .46               # text column: fit "Health" to its width
    size = 44 * SS
    while font(font_path, size, 800).getlength('Health') > room:
        size -= SS
    f1, f2 = font(font_path, size, 800), font(font_path, round(size * .80), 600)
    shadow_text(img, (tx, H * .08), 'PS3', f1, (255, 255, 255, 255), 3 * SS)          # above the wallpaper's trace
    shadow_text(img, (tx + SS, H * .58), 'Health', f2, GREEN + (255,), 3 * SS)       # below it
    return img.resize((320, 176), Image.LANCZOS)

def icon_card(ic):
    """The icon on a dark card, for the README (GitHub's page is white)."""
    W, H = ic.size
    card = vgradient(W, H, NAVY_TOP, NAVY_BOTTOM).convert('RGBA')
    card.alpha_composite(ic)
    return card.convert('RGB')

def background():
    W, H = 1920 * 2, 1080 * 2
    img = vgradient(W, H, (3, 8, 18), (8, 26, 52)).convert('RGB')
    m, c = radial_glow(W, H, W * .70, H * .48, W * .42, (20, 90, 150), 150)
    img = Image.composite(c, img, m).convert('RGBA')
    # soft XMB-style wave bands
    for k, (amp, phase, alpha) in enumerate(((90, 0.0, 34), (130, 1.7, 26), (70, 3.1, 30))):
        band = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        bd = ImageDraw.Draw(band)
        pts = [(x, H * (.60 + k * .06) + amp * 2 * math.sin(x / W * math.pi * 2.2 + phase)) for x in range(0, W + 40, 40)]
        bd.line(pts, fill=(120, 200, 255, alpha), width=36)
        img.alpha_composite(band.filter(ImageFilter.GaussianBlur(28)))
    dw = round(W * .38)
    x0 = round(W * .56)
    y0 = round(H * TRACE_Y - round(dw * .42) * .62)     # the trace at TRACE_Y, like the icon's
    dh = draw_console(img, x0, y0, dw)
    cy = y0 + dh * .62
    # the trace from TRACE_X0 to the right edge, its spike over the console
    shape = [(TRACE_X0, 0), (.575, 0), (.595, -.12), (.615, 0), (.635, 0), (.650, .28), (.668, -1),
             (.690, .55), (.708, 0), (.75, 0), (.78, -.22), (.815, 0), (.98, 0)]
    glow_line(img, [(W * fx, cy + fy * dh * .55) for fx, fy in shape], GREEN, 10, 18)
    # fade the left side, where the XMB draws its menu
    fade = Image.new('L', (W, 1))
    for x in range(W):
        fade.putpixel((x, 0), round(150 * max(0.0, 1 - x / (W * .45)) ** 1.5))
    dark = Image.new('RGBA', (W, H), (2, 6, 14, 255))
    dark.putalpha(fade.resize((W, H)))
    img.alpha_composite(dark)
    return img.resize((1920, 1080), Image.LANCZOS).convert('RGB')

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    ic = icon(sys.argv[1])
    ic.save(os.path.join(OUT, 'ICON0.PNG'), optimize=True)
    icon_card(ic).save(os.path.join(HERE, '..', 'docs', 'icon.png'), optimize=True)
    background().save(os.path.join(OUT, 'PIC1.PNG'), optimize=True)
    for p in (os.path.join(OUT, 'ICON0.PNG'), os.path.join(OUT, 'PIC1.PNG'), os.path.join(HERE, '..', 'docs', 'icon.png')):
        print(os.path.relpath(p, os.path.join(HERE, '..')), Image.open(p).size, Image.open(p).mode, os.path.getsize(p), 'bytes')
