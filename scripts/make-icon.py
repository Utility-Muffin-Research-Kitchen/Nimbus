#!/usr/bin/env python3
"""Generate Nimbus' app icon (res/icon.png).

The original gradient cloud (lifted from the source logo by luminance-keying it
off its dark background) with the "Nimbus" wordmark above it in a delicate
Nunito Light. No subtitle. Run from anywhere; paths resolve relative to here.

    python3 scripts/make-icon.py

Source assets live in scripts/assets/ so this is fully reproducible:
  logo-src.png  — the original Nimbus logo (cloud + text)
  Nunito-var.ttf — Nunito variable font (OFL), used at the Light instance
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOGO = os.path.join(HERE, "assets", "logo-src.png")
NUNITO = os.path.join(HERE, "assets", "Nunito-var.ttf")

SIZE = 256
RADIUS = 46
SS = 4  # supersample for crisp edges

BG_TOP = (28, 31, 40)      # match the source logo's near-black slate
BG_BOT = (20, 23, 31)
WORD = (224, 234, 255)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def extract_cloud():
    """Crop the cloud out of the source logo and key it off the dark bg via
    luminance, returning a tightly-cropped RGBA image of just the cloud."""
    logo = Image.open(LOGO).convert("RGB")
    w, h = logo.size
    # The cloud sits in the middle band, between the title and the subtitle.
    band = logo.crop((0, int(h * 0.30), w, int(h * 0.80)))
    px = band.load()
    bw, bh = band.size

    LO, HI = 46, 100  # luma -> alpha ramp (dark bg transparent, cloud opaque)
    out = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
    op = out.load()
    minx, miny, maxx, maxy = bw, bh, 0, 0
    for y in range(bh):
        for x in range(bw):
            r, g, b = px[x, y]
            luma = (r * 299 + g * 587 + b * 114) // 1000
            a = 0 if luma <= LO else 255 if luma >= HI else int((luma - LO) * 255 / (HI - LO))
            if a:
                op[x, y] = (r, g, b, a)
                if x < minx: minx = x
                if y < miny: miny = y
                if x > maxx: maxx = x
                if y > maxy: maxy = y
    pad = 6
    box = (max(0, minx - pad), max(0, miny - pad), min(bw, maxx + pad), min(bh, maxy + pad))
    return out.crop(box)


def main():
    W = SIZE * SS
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    grad = Image.new("RGB", (1, W))
    for y in range(W):
        grad.putpixel((0, y), lerp(BG_TOP, BG_BOT, y / (W - 1)))
    grad = grad.resize((W, W))
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], RADIUS * SS, fill=255)
    img.paste(grad, (0, 0), mask)

    # "Nimbus" above, delicate Nunito Light.
    word_font = ImageFont.truetype(NUNITO, int(46 * SS))
    try:
        word_font.set_variation_by_name("Light")
    except Exception:
        pass
    d = ImageDraw.Draw(img)
    bb = d.textbbox((0, 0), "Nimbus", font=word_font)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    word_y = int(W * 0.16)
    d.text((W // 2 - tw / 2 - bb[0], word_y - bb[1]), "Nimbus", font=word_font, fill=WORD + (255,))

    # Original cloud below, scaled to a chunky width, centered in the lower area.
    cloud = extract_cloud()
    target_w = int(W * 0.64)
    scale = target_w / cloud.width
    cloud = cloud.resize((target_w, int(cloud.height * scale)), Image.LANCZOS)
    cx = (W - cloud.width) // 2
    cy = int(W * 0.40)
    img.alpha_composite(cloud, (cx, cy))

    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    out = os.path.join(ROOT, "res", "icon.png")
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
