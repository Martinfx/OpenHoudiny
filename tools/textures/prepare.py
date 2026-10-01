#!/usr/bin/env python3
"""Builds the texture library in examples/textures from the pictures it is
made of: for each material a folder of

  color.jpg    the colour, sRGB, 1024 x 1024
  height.jpg   how high the surface is, 0..1: integrated from the normal
               map where the source has no height of its own
  texture.txt  how many metres the picture covers, how deep its height is,
               its average colour, whether the colour Cd tints it, where it
               comes from and its licence

What renders it: src/pg/render/Textures.h. The sources are the pictures of
pbrt-v4-scenes (github.com/mmp/pbrt-v4-scenes): Amazon Lumberyard's Bistro
and the Landscape scene, both CC-BY 4.0. Fetched with

  git clone --depth 1 --filter=blob:none --no-checkout \\
      https://github.com/mmp/pbrt-v4-scenes.git
  git -C pbrt-v4-scenes checkout HEAD -- <the files below>

and run as  tools/textures/prepare.py pbrt-v4-scenes examples/textures
"""

import os
import sys

import numpy as np
from PIL import Image

BISTRO = ("Amazon Lumberyard Bistro (Amazon, CC-BY 4.0, "
          "https://developer.nvidia.com/orca/amazon-lumberyard-bistro), "
          "via pbrt-v4-scenes")
LICENCE = "CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/)"

# name: (colour, normal or None, height or None, metres across, source)
SETS = {
    "concrete": ("bistro/textures/MASTER_Concrete_Plaster_BaseColor.png",
                 "bistro/textures/MASTER_Concrete_Plaster_Normal.png", None, 3.0, BISTRO),
    "plaster": ("bistro/textures/Concrete2_BaseColor.png",
                "bistro/textures/Concrete2_Normal.png", None, 3.0, BISTRO),
    "brick_wall": ("bistro/textures/MASTER_Brick_Small_Red_BaseColor.png",
                   "bistro/textures/MASTER_Brick_Small_Red_Normal.png", None, 2.4, BISTRO),
    "wood": ("bistro/textures/MASTER_Wood_Brown_BaseColor.png",
             "bistro/textures/MASTER_Wood_Brown_Normal.png", None, 1.2, BISTRO),
    "bark": ("bistro/textures/Foliage_Linde_Tree_Large_Trunk_BaseColor.png",
             "bistro/textures/Foliage_Linde_Tree_Large_Trunk_Normal.png", None, 1.0, BISTRO),
    "soil": ("bistro/textures/Pavement_Ground_Wet_BaseColor.png",
             "bistro/textures/Pavement_Ground_Wet_Normal.png", None, 2.5, BISTRO),
}

# name: (the folder whose pictures it takes, metres across)
ALIASES = {
    "roof": ("concrete", 6.0),
}

SIZE = 1024


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def height_from_normals(normal_path, size, metres, flip_green):
    """The surface the normal map is the slopes of (Frankot and Chellappa:
    the pictures tile, so the Fourier transform integrates them), in metres."""
    n = np.asarray(Image.open(normal_path).convert("RGB").resize((size, size), Image.LANCZOS),
                   dtype=np.float64) / 255.0 * 2.0 - 1.0
    nx, ny, nz = n[..., 0], n[..., 1], np.maximum(n[..., 2], 0.2)
    if flip_green:
        ny = -ny
    step = metres / size  # a pixel, in metres
    # Rows go down the picture: up the surface is -y.
    p = -nx / nz  # dz/dx
    q = ny / nz   # dz/drow
    u = np.fft.fftfreq(size)[None, :] * 2.0 * np.pi
    v = np.fft.fftfreq(size)[:, None] * 2.0 * np.pi
    denom = u * u + v * v
    denom[0, 0] = 1.0
    P, Q = np.fft.fft2(p), np.fft.fft2(q)
    H = (-1j * u * P - 1j * v * Q) / denom
    H[0, 0] = 0.0
    h = np.real(np.fft.ifft2(H)) * step
    return h


def highpass(h, cycles=4.0):
    """`h` without what is wider than a `cycles`-th of the picture: the drift
    an integration gathers, the light of the photograph."""
    size = h.shape[0]
    f = np.sqrt(np.fft.fftfreq(size)[None, :] ** 2 + np.fft.fftfreq(size)[:, None] ** 2) * size
    keep = 1.0 - np.exp(-(f / cycles) ** 2)
    return np.real(np.fft.ifft2(np.fft.fft2(h) * keep))


def main(src, out):
    os.makedirs(out, exist_ok=True)
    for name, (color, normal, height, metres, source) in SETS.items():
        folder = os.path.join(out, name)
        os.makedirs(folder, exist_ok=True)
        c = Image.open(os.path.join(src, color)).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
        c.save(os.path.join(folder, "color.jpg"), quality=90, optimize=True)
        lin = srgb_to_linear(np.asarray(c, dtype=np.float64) / 255.0)
        mean = lin.reshape(-1, 3).mean(axis=0)
        lum = lin.mean(axis=2)
        if height:
            h = np.asarray(Image.open(os.path.join(src, height)).convert("F").resize((SIZE, SIZE), Image.LANCZOS),
                           dtype=np.float64) / 255.0 * 0.01
        else:
            h = height_from_normals(os.path.join(src, normal), SIZE, metres, flip_green=False)
            # Mortar, cracks, pores down: what is darker sits lower -- the
            # sign of the green channel decided by which way agrees.
            if np.corrcoef(h.ravel(), lum.ravel())[0, 1] < 0:
                h = height_from_normals(os.path.join(src, normal), SIZE, metres, flip_green=True)
        h = highpass(h)
        lo, hi = np.percentile(h, 0.5), np.percentile(h, 99.5)
        if hi - lo < 0.0005:
            # A normal map all but flat (polished wood): the grain of the
            # colour, darker lower, half a millimetre deep.
            h = highpass(lum)
            lo, hi = np.percentile(h, 0.5), np.percentile(h, 99.5)
            depth = 0.0005
        else:
            depth = float(hi - lo)
        h = np.clip((h - lo) / max(hi - lo, 1e-12), 0.0, 1.0)
        Image.fromarray(np.round(h * 255.0).astype(np.uint8), "L").save(os.path.join(folder, "height.jpg"), quality=95)
        with open(os.path.join(folder, "texture.txt"), "w") as f:
            f.write("# The texture %s: what src/pg/render/Textures.cpp reads.\n" % name)
            f.write("size %.3f\n" % metres)
            f.write("depth %.5f\n" % depth)
            f.write("mean %.5f %.5f %.5f\n" % tuple(mean))
            # A brick wall's mortar is lighter than its bricks: tinted by a
            # brick's colour, it would be white. The rest round the colour Cd.
            f.write("tint %d\n" % (0 if name == "brick_wall" else 1))
            f.write("source %s\n" % source)
            f.write("license %s\n" % LICENCE)
        print("%-11s %.1f m, depth %.1f mm, mean %s" % (name, metres, depth * 1000.0, np.round(mean, 3)))
    # Flat roofs: the concrete's pictures, larger -- stains a roof wide.
    for name, (pictures, metres) in ALIASES.items():
        folder = os.path.join(out, name)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(out, pictures, "texture.txt")) as f:
            theirs = dict(line.split(" ", 1) for line in f.read().splitlines() if line and not line.startswith("#"))
        scale = metres / float(theirs["size"])
        with open(os.path.join(folder, "texture.txt"), "w") as f:
            f.write("# The texture %s: the pictures of %s, %.1f m across.\n" % (name, pictures, metres))
            f.write("pictures ../%s\n" % pictures)
            f.write("size %.3f\n" % metres)
            f.write("depth %.5f\n" % (float(theirs["depth"]) * scale))
            f.write("mean %s\n" % theirs["mean"])
            f.write("tint %s\n" % theirs["tint"])
            f.write("source %s\n" % theirs["source"])
            f.write("license %s\n" % theirs["license"])
        print("%-11s the pictures of %s, %.1f m" % (name, pictures, metres))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
