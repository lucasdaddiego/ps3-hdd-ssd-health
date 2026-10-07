#!/usr/bin/env python3
"""Draw the pkg art: ICON0.PNG (320x176, XMB icon) and PIC1.PNG (1920x1080,
XMB background) into pkgfiles/.

    python3 art/make_art.py <Inter variable TTF>

Font: Inter (SIL Open Font License), Inter[opsz,wght].ttf from
github.com/google/fonts/tree/main/ofl/inter; only the rendered text ends up in
the PNGs. Everything else is drawn here: a 2.5" drive whose left half is an
HDD platter and right half SSD flash, crossed by a heartbeat line.

The icon has a transparent background: on the XMB the drive, the words and
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
GREEN, CYAN = (62, 240, 138), (63, 208, 255)
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

def draw_drive(img, x0, y0, w):
    """2.5" drive seen from the top: HDD platter on the left, SSD flash on the right."""
    h = round(w * 0.70)
    x1, y1 = x0 + w, y0 + h
    r = round(w * 0.06)
    d = ImageDraw.Draw(img)
    # shadow
    sh = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x0 + w * .02, y0 + w * .04, x1 + w * .02, y1 + w * .04), r, fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(w * .03)))
    # metal body
    body = vgradient(w, h, (214, 221, 230), (128, 140, 154)).convert('RGBA')
    img.paste(body, (x0, y0), rounded_mask((w, h), (0, 0, w - 1, h - 1), r))
    d.rounded_rectangle((x0, y0, x1, y1), r, outline=(70, 80, 92, 255), width=max(2, w // 120))
    inset = w * .045
    d.rounded_rectangle((x0 + inset, y0 + inset, x1 - inset, y1 - inset), r * .6,
                        outline=(160, 170, 182, 255), width=max(1, w // 220))
    # screws
    for sx, sy in ((x0 + inset * .55, y0 + inset * .55), (x1 - inset * .55, y0 + inset * .55),
                   (x0 + inset * .55, y1 - inset * .55), (x1 - inset * .55, y1 - inset * .55)):
        sr = w * .016
        d.ellipse((sx - sr, sy - sr, sx + sr, sy + sr), fill=(96, 106, 118, 255))
        d.line((sx - sr * .7, sy, sx + sr * .7, sy), fill=(60, 66, 74, 255), width=max(1, w // 300))
    mid = x0 + w * .5
    # HDD half: platter, spindle, actuator arm
    pcx, pcy, pr = x0 + w * .265, y0 + h * .5, h * .36
    for i in range(24):
        t = i / 23
        rr = pr * (1 - t * .75)
        c = round(200 - 70 * math.sin(t * math.pi * 1.5) ** 2)
        d.ellipse((pcx - rr, pcy - rr, pcx + rr, pcy + rr), fill=(c, c + 6, c + 14, 255))
    for k in (.92, .78, .64, .50):
        rr = pr * k
        d.ellipse((pcx - rr, pcy - rr, pcx + rr, pcy + rr), outline=(150, 160, 172, 255), width=max(1, w // 400))
    hl = Image.new('RGBA', img.size, (0, 0, 0, 0))     # sheen
    ImageDraw.Draw(hl).pieslice((pcx - pr, pcy - pr, pcx + pr, pcy + pr), 200, 250, fill=(255, 255, 255, 70))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(w * .01)))
    sr = pr * .2
    d.ellipse((pcx - sr, pcy - sr, pcx + sr, pcy + sr), fill=(120, 130, 142, 255), outline=(80, 88, 98, 255),
              width=max(1, w // 300))
    px, py = x0 + w * .44, y0 + h * .80
    d.ellipse((px - w * .03, py - w * .03, px + w * .03, py + w * .03), fill=(70, 78, 88, 255))
    d.line((px, py, pcx + pr * .25, pcy - pr * .35), fill=(58, 64, 72, 255), width=max(2, round(w * .022)))
    d.line((px, py, pcx + pr * .25, pcy - pr * .35), fill=(110, 118, 128, 255), width=max(1, round(w * .007)))
    # SSD half: board with flash chips and a controller
    bx0, by0, bx1, by1 = mid + w * .03, y0 + h * .14, x1 - w * .07, y1 - h * .14
    d.rounded_rectangle((bx0, by0, bx1, by1), w * .015, fill=(24, 58, 52, 255), outline=(14, 36, 32, 255),
                        width=max(1, w // 300))
    cw, ch = (bx1 - bx0) * .40, (by1 - by0) * .36
    for cx, cy in ((bx0 + (bx1 - bx0) * .06, by0 + (by1 - by0) * .08), (bx0 + (bx1 - bx0) * .54, by0 + (by1 - by0) * .08),
                   (bx0 + (bx1 - bx0) * .06, by0 + (by1 - by0) * .56)):
        d.rounded_rectangle((cx, cy, cx + cw, cy + ch), w * .008, fill=(22, 24, 28, 255), outline=(60, 64, 70, 255),
                            width=max(1, w // 400))
        d.line((cx + cw * .12, cy + ch * .3, cx + cw * .55, cy + ch * .3), fill=(90, 96, 104, 255), width=max(1, w // 300))
    kx, ky = bx0 + (bx1 - bx0) * .58, by0 + (by1 - by0) * .60
    d.rounded_rectangle((kx, ky, kx + cw * .75, ky + ch * .80), w * .008, fill=(34, 38, 44, 255),
                        outline=CYAN + (255,), width=max(1, w // 300))
    # seam between the halves
    d.line((mid, y0 + inset * 1.4, mid, y1 - inset * 1.4), fill=(90, 100, 112, 255), width=max(1, w // 250))
    # SATA connector on the right edge
    for i in range(7):
        gy = y0 + h * (.30 + i * .06)
        d.rectangle((x1 - w * .028, gy, x1 - w * .008, gy + h * .03), fill=(214, 172, 74, 255))
    return h

def icon(font_path):
    W, H = 320 * SS, 176 * SS
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dw = round(W * .40)
    dh_nominal = round(dw * .70)
    y0 = round(H * ICON_TRACE - dh_nominal * .52)       # the drive sits so its trace is at ICON_TRACE
    dh = draw_drive(img, round(W * .04), y0, dw)
    cy = y0 + dh * .52
    pts = pulse_points(0, W * .47, cy, dh * .40) + [(W, cy)]   # the pulse over the platter, then flat to the edge
    glow_line(img, pts, GREEN, 4 * SS, 4 * SS)
    tx, room = W * .50, W * .48               # text column: fit "HDD/SSD" to its width
    size = 40 * SS
    while font(font_path, size, 800).getlength('HDD/SSD') > room:
        size -= SS
    f1, f2 = font(font_path, size, 800), font(font_path, round(size * .80), 600)
    shadow_text(img, (tx, H * .10), 'HDD/SSD', f1, (255, 255, 255, 255), 3 * SS)      # above the wallpaper's trace
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
    dw = round(W * .36)
    x0 = round(W * .56)
    y0 = round(H * TRACE_Y - round(dw * .70) * .52)     # the trace at TRACE_Y, like the icon's
    dh = draw_drive(img, x0, y0, dw)
    cy = y0 + dh * .52
    # the trace from TRACE_X0 to the right edge, its spike over the platter (x0 + .265 dw)
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
