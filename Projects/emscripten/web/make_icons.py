#!/usr/bin/env python3
"""Draw the web build's home-screen icons: a remembrance poppy on dark olive.

Run from this directory (needs Pillow); writes icon-180/192/512.png and icon-maskable-512.png.
"""

import math

from PIL import Image, ImageDraw, ImageFilter

S = 2048  # drawn large, then downscaled for smooth edges


def background(size):
    top, bottom = (0x26, 0x31, 0x1C), (0x0B, 0x0E, 0x09)
    img = Image.new("RGB", (size, size))
    draw = ImageDraw.Draw(img)
    for y in range(size):
        t = y / (size - 1)
        draw.line(
            [(0, y), (size, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
        )
    return img.convert("RGBA")


def petal(size, angle, length, width, offset):
    """One petal: an ellipse pushed out from the centre, rotated about the centre."""
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    c = size / 2
    box = [c - width / 2, c - offset - length, c + width / 2, c - offset + length * 0.35]
    # a darker rim shows where petals overlap; a soft highlight lifts the outer edge
    draw.ellipse(
        box, fill=(0xC8, 0x2A, 0x1E, 255), outline=(0x8C, 0x19, 0x10, 255), width=int(size / 150)
    )
    light = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(light).ellipse(
        [
            c - width * 0.30,
            c - offset - length * 0.92,
            c + width * 0.30,
            c - offset - length * 0.45,
        ],
        fill=(0xE8, 0x55, 0x3F, 110),
    )
    light = light.filter(ImageFilter.GaussianBlur(size / 45))
    layer = Image.alpha_composite(
        layer, Image.composite(light, Image.new("RGBA", (size, size)), layer)
    )
    draw = ImageDraw.Draw(layer)
    # darker base toward the centre, so the petals overlap with some depth
    shade = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shade).ellipse(
        [
            c - width * 0.34,
            c - offset - length * 0.15,
            c + width * 0.34,
            c - offset + length * 0.38,
        ],
        fill=(0x7A, 0x12, 0x0C, 150),
    )
    shade = shade.filter(ImageFilter.GaussianBlur(size / 60))
    layer = Image.alpha_composite(
        layer, Image.composite(shade, Image.new("RGBA", (size, size)), layer)
    )
    return layer.rotate(angle, resample=Image.BICUBIC, center=(c, c))


def poppy(size, scale):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    r = size * scale
    for angle, length, width in [
        (90, 0.50, 0.78),
        (270, 0.50, 0.78),
        (0, 0.46, 0.74),
        (180, 0.44, 0.72),
    ]:
        img = Image.alpha_composite(img, petal(size, angle, r * length, r * width, r * 0.02))
    draw = ImageDraw.Draw(img)
    c = size / 2
    core = r * 0.17
    draw.ellipse([c - core, c - core, c + core, c + core], fill=(0x14, 0x12, 0x10, 255))
    # the seed head: a ring of stamens around a small olive crown
    for i in range(18):
        a = i / 18 * 2 * math.pi
        x, y = c + math.cos(a) * core * 1.18, c + math.sin(a) * core * 1.18
        d = core * 0.13
        draw.ellipse([x - d, y - d, x + d, y + d], fill=(0x14, 0x12, 0x10, 255))
    crown = core * 0.45
    points = []
    for i in range(16):
        a = i / 16 * 2 * math.pi - math.pi / 2
        rad = crown if i % 2 == 0 else crown * 0.55
        points.append((c + math.cos(a) * rad, c + math.sin(a) * rad))
    draw.polygon(points, fill=(0x5E, 0x6B, 0x3A, 255))
    return img


def icon(scale):
    img = background(S)
    shadow = poppy(S, scale).split()[3].filter(ImageFilter.GaussianBlur(S / 40))
    img.paste((0, 0, 0, 110), (0, int(S * 0.012)), shadow)
    return Image.alpha_composite(img, poppy(S, scale)).convert("RGB")


full = icon(0.80)
for px in (180, 192, 512):
    full.resize((px, px), Image.LANCZOS).save(f"icon-{px}.png", optimize=True)
# maskable: the platform crops to a circle of 80% diameter, so keep the flower inside it
icon(0.62).resize((512, 512), Image.LANCZOS).save("icon-maskable-512.png", optimize=True)
