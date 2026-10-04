#!/usr/bin/env python3
"""Draws the leaf and grass pictures of the texture library -- no
photographs, so no licence but the program's own:

  leaf/color.png    1024 x 1024, RGBA: four leaves, one a quarter, each its
                    base at the middle of the bottom of its quarter and its
                    tip at the middle of the top -- a broad leaf top left and
                    another top right, a narrow one bottom left, a spray of
                    needles bottom right. Each lies inside the outline the
                    Tree node gives a leaf of its shape (leafOutline in
                    src/pg/core/Tree.cpp), its edge cut out by alpha.
  leaf/normal.png   its normal map: the midrib and veins raised, green up
                    the picture as OpenGL has it
  grass/color.png   128 x 1024, RGB: one blade, across and up it, its midrib
                    and the streaks along it
  */texture.txt     what render/Textures.cpp reads: their mean colour over
                    what is there (tint 1: Cd tints them), laid on by uv
                    alone (projection uv), alpha 1 for the leaves

Each laid on by uv (the Tree and Grass nodes give it). Run as

  tools/textures/foliage.py examples/textures
"""

import os
import sys

import numpy as np
from PIL import Image

LEAF = 1024          # the leaf picture, square
CELL = LEAF // 2     # a quarter of it
GRASS = (128, 1024)  # the grass picture, across and up


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def outline_width(points, a):
    """Half the width of a leaf outline (along, across pairs, the side of
    positive across, base to tip) at `a` along it: the polygon's own."""
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return np.interp(a, xs, ys, left=0.0, right=0.0)


# The Tree node's outlines (leafOutline), the side of positive across.
BROAD = [(0.0, 0.0), (0.18, 0.2), (0.5, 0.3), (0.82, 0.18), (1.0, 0.0)]
NARROW = [(0.0, 0.0), (0.2, 0.07), (0.6, 0.08), (1.0, 0.0)]
NEEDLES = [(0.0, 0.0), (0.15, 0.16), (0.3, 0.08), (0.62, 0.15), (0.75, 0.06), (1.0, 0.0)]


def noise(shape, seed, scales=(4, 16, 64)):
    """Smooth value noise, about 0 +- 1: a few octaves, each bilinear."""
    rng = np.random.default_rng(seed)
    h, w = shape
    out = np.zeros(shape)
    for i, s in enumerate(scales):
        grid = rng.standard_normal((s + 1, s + 1))
        y = np.linspace(0, s, h, endpoint=False)
        x = np.linspace(0, s, w, endpoint=False)
        y0 = np.floor(y).astype(int)
        x0 = np.floor(x).astype(int)
        ty = (y - y0)[:, None]
        tx = (x - x0)[None, :]
        ty = ty * ty * (3 - 2 * ty)
        tx = tx * tx * (3 - 2 * tx)
        g00 = grid[y0][:, x0]
        g01 = grid[y0][:, x0 + 1]
        g10 = grid[y0 + 1][:, x0]
        g11 = grid[y0 + 1][:, x0 + 1]
        out += ((g00 * (1 - tx) + g01 * tx) * (1 - ty) + (g10 * (1 - tx) + g11 * tx) * ty) / (2 ** i)
    return out / 1.75


def blade(a, c, width, outline, seed, veins, vein_angle, base_green, serration=0.0, teeth=0):
    """A leaf blade on the grid `a` (along, 0 base to 1 tip) by `c` (across):
    its colour (linear), how high it is (veins raised) and its coverage.
    `width` its half width at each a, cut to the outline."""
    px = 1.0 / CELL
    # The smooth shape as large as fits inside the outline, a pixel or two
    # in from it, between its base and tip; at them, the outline's point.
    room = outline_width(outline, a) - 2.0 * px
    middle = (a > 0.15) & (a < 0.85) & (width > 1e-3)
    fit = np.min(room[middle] / width[middle])
    w = np.minimum(width * fit, room)
    if teeth:
        # Teeth along the edge, pointing to the tip: in from it, sharp on
        # their tip side.
        phase = (a * teeth) % 1.0
        w = w - serration * phase
    w = np.maximum(w, 0.0)
    inside = np.clip((w - np.abs(c)) / px + 0.5, 0.0, 1.0)
    # The stalk, up to the blade.
    stalk = np.clip((0.006 - np.abs(c)) / px + 0.5, 0.0, 1.0) * (a < 0.12) * (a > 1.5 * px)
    stalk *= np.clip((outline_width(outline, a) - np.abs(c)) / px, 0.0, 1.0)
    cover = np.maximum(inside, stalk)

    # The midrib, narrowing to the tip; veins from it toward the tip and the
    # edge, every 1/veins along it.
    rel = np.abs(c) / np.maximum(w, 1e-4)
    midrib = np.exp(-(np.abs(c) / (0.0045 * (1.1 - a))) ** 2)
    v = a - np.abs(c) * vein_angle
    d = np.abs(((v * veins) + 0.5) % 1.0 - 0.5) / veins
    vein = np.exp(-(d / 0.0028) ** 2) * (rel < 0.92) * (a > 0.08)
    rough = noise(a.shape, seed)
    shade = 1.0 + 0.10 * rough - 0.18 * rel ** 3 + 0.25 * midrib + 0.14 * vein
    color = srgb_to_linear(base_green)[None, None, :] * shade[:, :, None]
    color[stalk > inside] = srgb_to_linear((0.36, 0.42, 0.16))
    height = 0.6 * midrib + 0.35 * vein - 0.2 * rel ** 2
    return color, height, cover


def needles(a, c, outline, seed, base_green):
    """A spray of needles: a twig up the middle, needles off it in pairs
    leaning toward the tip, each no further out than the outline."""
    px = 1.0 / CELL
    twig = np.clip((0.007 * (1.2 - a) - np.abs(c)) / px + 0.5, 0.0, 1.0) * (a < 0.96) * (a > 1.5 * px)
    cover = twig.copy()
    height = 0.5 * twig
    tone = np.zeros_like(a)
    rng = np.random.default_rng(seed)
    lean = np.radians(52.0)
    ca, sa = np.cos(lean), np.sin(lean)  # along, across of a needle's way
    for base in np.arange(0.03, 0.93, 0.017):
        for side in (1.0, -1.0):
            jitter = rng.uniform(-0.006, 0.006)
            b = base + jitter
            # How far out it may go: the outline at its tip, a little short.
            length = 0.0
            for t in np.linspace(0.0, 0.3, 61):
                if outline_width(outline, b + t * ca) - 2.5 * px >= t * sa:
                    length = t
                else:
                    break
            if length < 0.02:
                continue
            # Distance from the segment (b, 0) -> (b + L ca, side L sa).
            da = a - b
            dc = c * side
            t = np.clip(da * ca + dc * sa, 0.0, length)
            dist = np.hypot(da - t * ca, dc - t * sa)
            half = 0.0045 * (1.0 - 0.6 * t / length)  # thinner to its tip
            n = np.clip((half - dist) / px + 0.5, 0.0, 1.0)
            cover = np.maximum(cover, n)
            height = np.maximum(height, n * (1.0 - (dist / 0.0045) ** 2))
            tone = np.where(n > 0, rng.uniform(-0.08, 0.08), tone)
    rough = noise(a.shape, seed)
    shade = 1.0 + 0.08 * rough + tone + 0.18 * height
    color = srgb_to_linear(base_green)[None, None, :] * shade[:, :, None]
    color[twig > 0.5] = srgb_to_linear((0.30, 0.24, 0.13))
    return color, height, cover


def cell_grid():
    """The along and across of each pixel of a quarter: rows from the top."""
    y = (np.arange(CELL) + 0.5) / CELL
    x = (np.arange(CELL) + 0.5) / CELL
    a = (1.0 - y)[:, None] * np.ones((1, CELL))
    c = np.ones((CELL, 1)) * (x - 0.5)[None, :]
    return a, c


def normals_of(height, strength):
    """A normal map of `height` (rows from the top): green up the picture."""
    gy, gx = np.gradient(height)
    nx = -gx * strength
    ny = gy * strength  # rows go down; up the picture is -row
    nz = np.ones_like(height)
    n = np.stack([nx, ny, nz], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def bleed(color, cover):
    """Where there is nothing, the colour of what is nearest -- so that a
    picture read between pixels at the edge is not darkened by black."""
    out = color.copy()
    filled = cover > 0.5
    mean = (color * cover[:, :, None]).sum(axis=(0, 1)) / max(cover.sum(), 1.0)
    out[~filled] = mean
    # A few rings of the nearest: average the filled neighbours outward.
    have = filled.copy()
    for _ in range(8):
        acc = np.zeros_like(out)
        cnt = np.zeros(cover.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            m = np.roll(have, (dy, dx), axis=(0, 1))
            acc += np.roll(out, (dy, dx), axis=(0, 1)) * m[:, :, None]
            cnt += m
        grow = (~have) & (cnt > 0)
        out[grow] = acc[grow] / cnt[grow][:, None]
        have = have | grow
    return out


def write_texture_txt(folder, name, mean, alpha, what):
    with open(os.path.join(folder, "texture.txt"), "w") as f:
        f.write(f"# The texture {name}: what src/pg/render/Textures.cpp reads.\n")
        f.write("size 1.000\n")
        f.write("depth 0.00100\n")
        f.write("mean %.5f %.5f %.5f\n" % tuple(mean))
        f.write("tint 1\n")
        f.write("projection uv\n")
        if alpha:
            f.write("alpha 1\n")
        f.write(f"source drawn by tools/textures/foliage.py: {what}\n")
        f.write("license the program's own\n")


def leaves(out):
    a, c = cell_grid()
    cells = [
        # top left: a broad leaf, oval, finely toothed
        blade(a, c, 0.285 * np.sin(np.pi * np.clip(a, 0, 1) ** 0.85) ** 0.9, BROAD, 11, 9.0, 1.3,
              (0.30, 0.52, 0.17), serration=0.008, teeth=34),
        # top right: another, rounder at its base, coarser teeth
        blade(a, c, 0.3 * np.sin(np.pi * np.clip(a, 0, 1) ** 0.7) ** 0.75, BROAD, 12, 7.0, 1.0,
              (0.28, 0.50, 0.15), serration=0.014, teeth=20),
        # bottom left: a narrow one, a willow's
        blade(a, c, 0.078 * np.sin(np.pi * np.clip(a, 0, 1) ** 0.8) ** 0.7, NARROW, 13, 14.0, 4.0,
              (0.33, 0.50, 0.20)),
        # bottom right: needles
        needles(a, c, NEEDLES, 14, (0.18, 0.34, 0.14)),
    ]
    color = np.zeros((LEAF, LEAF, 3))
    height = np.zeros((LEAF, LEAF))
    cover = np.zeros((LEAF, LEAF))
    for i, (col, h, cov) in enumerate(cells):
        y0 = 0 if i < 2 else CELL
        x0 = 0 if i % 2 == 0 else CELL
        color[y0:y0 + CELL, x0:x0 + CELL] = col
        height[y0:y0 + CELL, x0:x0 + CELL] = h * (cov > 0)
        cover[y0:y0 + CELL, x0:x0 + CELL] = cov
    # Over the broad leaves (Cd tints the picture by its mean: the leaves'
    # own green there).
    mean = (color * cover[:, :, None]).sum(axis=(0, 1)) / cover.sum()
    color = bleed(color, cover)
    rgba = np.concatenate([linear_to_srgb(color), cover[:, :, None]], axis=-1)
    folder = os.path.join(out, "leaf")
    os.makedirs(folder, exist_ok=True)
    Image.fromarray((rgba * 255.0 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(folder, "color.png"),
                                                                       optimize=True)
    n = normals_of(height, 6.0)
    Image.fromarray((n * 255.0 + 0.5).astype(np.uint8), "RGB").save(os.path.join(folder, "normal.png"),
                                                                    optimize=True)
    write_texture_txt(folder, "leaf", mean, True, "two broad leaves, a narrow one, a spray of needles")
    print("leaf    mean", np.round(mean, 4), "covered", round(float(cover.mean()), 3))


def grass(out):
    w, h = GRASS
    x = (np.arange(w) + 0.5) / w - 0.5        # across, -0.5..0.5
    y = 1.0 - (np.arange(h) + 0.5) / h        # up, 0 root .. 1 tip
    c = np.ones((h, 1)) * x[None, :]
    a = y[:, None] * np.ones((1, w))
    rng = np.random.default_rng(21)
    # Streaks along it: fine ribs across the blade, each its own shade.
    ribs = np.interp(c, np.linspace(-0.5, 0.5, 15), rng.uniform(-1.0, 1.0, 15))
    rib_lines = np.cos(2 * np.pi * (c + 0.5) * 14) * 0.5 + 0.5
    midrib = np.exp(-(c / 0.04) ** 2)
    rough = noise((h, w), 22, scales=(2, 8, 32))
    edge = np.abs(c) * 2.0
    shade = 1.0 + 0.06 * ribs + 0.05 * rib_lines + 0.2 * midrib + 0.06 * rough - 0.15 * edge ** 4
    # A little lighter and yellower toward the tip, a dry tip now and then
    # is the node's own (Dry).
    base = srgb_to_linear((0.32, 0.50, 0.17))
    tip = srgb_to_linear((0.40, 0.55, 0.20))
    color = (base[None, None, :] * (1 - a[:, :, None]) + tip[None, None, :] * a[:, :, None]) * shade[:, :, None]
    mean = color.mean(axis=(0, 1))
    folder = os.path.join(out, "grass")
    os.makedirs(folder, exist_ok=True)
    Image.fromarray((linear_to_srgb(color) * 255.0 + 0.5).astype(np.uint8), "RGB").save(
        os.path.join(folder, "color.png"), optimize=True)
    write_texture_txt(folder, "grass", mean, False, "one blade, its midrib and streaks")
    print("grass   mean", np.round(mean, 4))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    leaves(sys.argv[1])
    grass(sys.argv[1])
