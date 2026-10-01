#!/usr/bin/env python3
"""Builds the texture library in examples/textures from the pictures it is
made of: for each material a folder of

  color.jpg    the colour, sRGB, no more than 1024 x 1024
  height.jpg   how high the surface is, 0..1: integrated from the normal
               map where the source has one (which way its green channel
               points decided by which way it integrates), else the
               lightness of the colour -- darker lower
  texture.txt  how many metres the picture covers, how deep its height is,
               its average colour, whether the colour Cd tints it, whether
               it is laid along a sloping face (projection face), where it
               comes from and its licence

What renders it: src/pg/render/Textures.h. The sources, fetched with git
(only the files below, the rest of the repositories left where they are):

  git clone --depth 1 --filter=blob:none --no-checkout \\
      https://github.com/mmp/pbrt-v4-scenes.git
  git -C pbrt-v4-scenes checkout HEAD -- <its files below>
  git clone --depth 1 --filter=blob:none --no-checkout \\
      https://github.com/BabylonJS/Assets.git babylon-assets
  git -C babylon-assets checkout HEAD -- <its files below>

and run as

  tools/textures/prepare.py pbrt-v4-scenes babylon-assets examples/textures
"""

import os
import sys

import numpy as np
from PIL import Image, ImageFilter

BISTRO = ("Amazon Lumberyard Bistro (Amazon, CC-BY 4.0, "
          "https://developer.nvidia.com/orca/amazon-lumberyard-bistro), via pbrt-v4-scenes")
BABYLON = "BabylonJS/Assets (https://github.com/BabylonJS/Assets), CC-BY 4.0"
LICENCE = "CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/)"


class Set:
    """One folder of the library: where its pictures come from and how big
    the surface in them is."""

    def __init__(self, repo, color, normal, metres, source, tint=True, light_depth=None, brushed=False,
                 along_face=False):
        self.repo = repo              # "pbrt" or "babylon": which checkout
        self.color = color            # the colour picture, in that checkout
        self.normal = normal          # its normal map, or None
        self.metres = metres          # how many metres across the picture is
        self.source = source          # where it comes from, for texture.txt
        self.tint = tint              # whether the colour Cd tints it
        self.light_depth = light_depth  # metres deep, where the height is the lightness
        self.brushed = brushed        # colour from the relief: a dark picture of 10 greys
        self.along_face = along_face  # laid along a sloping face: rows that stay level


SETS = {
    "concrete": Set("pbrt", "bistro/textures/MASTER_Concrete_Plaster_BaseColor.png",
                    "bistro/textures/MASTER_Concrete_Plaster_Normal.png", 3.0, BISTRO),
    "plaster": Set("pbrt", "bistro/textures/Concrete2_BaseColor.png",
                   "bistro/textures/Concrete2_Normal.png", 3.0, BISTRO),
    # A brick wall's mortar is lighter than its bricks: tinted by a brick's
    # colour, it would be white.
    "brick_wall": Set("pbrt", "bistro/textures/MASTER_Brick_Small_Red_BaseColor.png",
                      "bistro/textures/MASTER_Brick_Small_Red_Normal.png", 2.4, BISTRO, tint=False),
    "wood": Set("pbrt", "bistro/textures/MASTER_Wood_Brown_BaseColor.png",
                "bistro/textures/MASTER_Wood_Brown_Normal.png", 1.2, BISTRO),
    "bark": Set("pbrt", "bistro/textures/Foliage_Linde_Tree_Large_Trunk_BaseColor.png",
                "bistro/textures/Foliage_Linde_Tree_Large_Trunk_Normal.png", 1.0, BISTRO),
    "soil": Set("pbrt", "bistro/textures/Pavement_Ground_Wet_BaseColor.png",
                "bistro/textures/Pavement_Ground_Wet_Normal.png", 2.5, BISTRO),
    # Setts of about 18 x 14 cm: seven across, nine down.
    "paving": Set("pbrt", "bistro/textures/Pavement_Cobblestone_Big_BLENDSHADER_BaseColor.png",
                  "bistro/textures/Pavement_Cobblestone_Big_BLENDSHADER_Normal.png", 1.3, BISTRO),
    # Slates of about 22 x 15 cm: eight across, twelve rows -- level on a
    # roof whichever way it faces.
    "roof_tiles": Set("pbrt", "bistro/textures/MASTER_Roofing_Shingle_Grey_BaseColor.png",
                      "bistro/textures/MASTER_Roofing_Shingle_Grey_Normal.png", 1.8, BISTRO, along_face=True),
    "metal": Set("pbrt", "bistro/textures/Banner_Metal_BaseColor.png",
                 "bistro/textures/Banner_Metal_Normal.png", 1.0, BISTRO, brushed=True),
    "asphalt": Set("babylon", "meshes/PowerPlant/gravel_a.png", None, 2.0, BABYLON, light_depth=0.003),
    "broken_concrete": Set("babylon", "textures/rockyGround_basecolor.png", "textures/rockyGround_normal.png",
                           0.8, BABYLON),
    "lawn": Set("babylon", "textures/grass.png", None, 1.5, BABYLON, light_depth=0.006),
    "sand": Set("babylon", "textures/sand.jpg", None, 1.2, BABYLON, light_depth=0.002),
}

# name: (the folder whose pictures it takes, metres across)
ALIASES = {
    "roof": ("concrete", 6.0),   # flat roofs: the concrete's, stains a roof wide
    "mortar": ("sand", 0.4),     # sand and lime: the sand's grains, finer
}

SIZE = 1024


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def height_from_normals(normal_path, size, metres):
    """The surface the normal map is the slopes of (Frankot and Chellappa:
    the pictures tile, so the Fourier transform integrates them), in metres,
    and which way its green channel points: of the two, the one whose slopes
    are those of a surface -- the other way round, little of them is."""
    n = np.asarray(Image.open(normal_path).convert("RGB").resize((size, size), Image.LANCZOS),
                   dtype=np.float64) / 255.0 * 2.0 - 1.0
    nx, ny, nz = n[..., 0], n[..., 1], np.maximum(n[..., 2], 0.2)
    step = metres / size  # a pixel, in metres
    u = np.fft.fftfreq(size)[None, :] * 2.0 * np.pi
    v = np.fft.fftfreq(size)[:, None] * 2.0 * np.pi
    denom = u * u + v * v
    denom[0, 0] = 1.0
    P = np.fft.fft2(-nx / nz)  # dz/dx
    P[0, 0] = 0.0
    found = []
    for down in (False, True):
        # Rows go down the picture: up the surface is -y, unless the green
        # channel points down (DirectX).
        Q = np.fft.fft2((-ny if down else ny) / nz)  # dz/drow
        Q[0, 0] = 0.0
        H = (-1j * u * P - 1j * v * Q) / denom
        H[0, 0] = 0.0
        # What of the slopes no surface has: the part that is not a gradient.
        rest = (np.abs(P - 1j * u * H) ** 2 + np.abs(Q - 1j * v * H) ** 2).sum() / \
            max((np.abs(P) ** 2 + np.abs(Q) ** 2).sum(), 1e-30)
        found.append((rest, down, np.real(np.fft.ifft2(H)) * step))
    (rest_up, _, up), (rest_down, _, down) = found
    # Brushed metal, wood grain: lines that are surfaces either way --
    # green down, as all the rest of these sources.
    if rest_up * 1.5 < rest_down:
        return up, "green up", rest_up, rest_down
    return down, "green down", rest_up, rest_down


def highpass(h, cycles=4.0):
    """`h` without what is wider than a `cycles`-th of the picture: the drift
    an integration gathers, the light of the photograph."""
    size = h.shape[0]
    f = np.sqrt(np.fft.fftfreq(size)[None, :] ** 2 + np.fft.fftfreq(size)[:, None] ** 2) * size
    keep = 1.0 - np.exp(-(f / cycles) ** 2)
    return np.real(np.fft.ifft2(np.fft.fft2(h) * keep))


def brushed_colour(color_path, h, size):
    """A grey colour of the relief's fine scratches and the photograph's
    blotches: a picture too dark to be more than ten greys."""
    fine = highpass(h, 48.0)
    fine = (fine - fine.mean()) / max(fine.std(), 1e-12)
    blotches = np.asarray(Image.open(color_path).convert("L").resize((size, size), Image.LANCZOS)
                          .filter(ImageFilter.GaussianBlur(6)), dtype=np.float64)
    blotches = (blotches - blotches.mean()) / max(blotches.std(), 1e-12)
    lin = 0.5 * (1.0 + 0.15 * np.clip(fine, -3.0, 3.0) + 0.12 * np.clip(blotches, -3.0, 3.0))
    srgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(np.clip(lin, 0.0, None), 1.0 / 2.4) - 0.055)
    grey = np.round(np.clip(srgb, 0.0, 1.0) * 255.0).astype(np.uint8)
    return Image.fromarray(grey, "L").convert("RGB")


def main(pbrt, babylon, out):
    roots = {"pbrt": pbrt, "babylon": babylon}
    os.makedirs(out, exist_ok=True)
    for name, spec in SETS.items():
        folder = os.path.join(out, name)
        os.makedirs(folder, exist_ok=True)
        root = roots[spec.repo]
        src = Image.open(os.path.join(root, spec.color)).convert("RGB")
        size = min(SIZE, src.size[0])
        c = src.resize((size, size), Image.LANCZOS)
        depth, how = None, "lightness"
        if spec.normal:
            h, how, rest_up, rest_down = height_from_normals(os.path.join(root, spec.normal), size, spec.metres)
            how = "%s (not a surface: %.2f up, %.2f down)" % (how, rest_up, rest_down)
            if spec.brushed:
                c = brushed_colour(os.path.join(root, spec.color), h, size)
            h = highpass(h)
            lo, hi = np.percentile(h, 0.5), np.percentile(h, 99.5)
            if hi - lo >= 0.0005:
                depth = float(hi - lo)
        c.save(os.path.join(folder, "color.jpg"), quality=90, optimize=True)
        lin = srgb_to_linear(np.asarray(c, dtype=np.float64) / 255.0)
        mean = lin.reshape(-1, 3).mean(axis=0)
        if depth is None:
            # No normal map, or one all but flat: the lightness of the
            # colour, darker lower.
            h = highpass(lin.mean(axis=2))
            lo, hi = np.percentile(h, 0.5), np.percentile(h, 99.5)
            depth = spec.light_depth if spec.light_depth else 0.0005
            how = "lightness"
        h = np.clip((h - lo) / max(hi - lo, 1e-12), 0.0, 1.0)
        Image.fromarray(np.round(h * 255.0).astype(np.uint8), "L").save(os.path.join(folder, "height.jpg"), quality=95)
        with open(os.path.join(folder, "texture.txt"), "w") as f:
            f.write("# The texture %s: what src/pg/render/Textures.cpp reads.\n" % name)
            f.write("size %.3f\n" % spec.metres)
            f.write("depth %.5f\n" % depth)
            f.write("mean %.5f %.5f %.5f\n" % tuple(mean))
            f.write("tint %d\n" % (1 if spec.tint else 0))
            if spec.along_face:
                f.write("projection face\n")
            f.write("source %s\n" % spec.source)
            f.write("license %s\n" % LICENCE)
        print("%-15s %.1f m, depth %4.1f mm, mean %s, height from %s" %
              (name, spec.metres, depth * 1000.0, np.round(mean, 3), how))
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
            if "projection" in theirs:
                f.write("projection %s\n" % theirs["projection"])
            f.write("source %s\n" % theirs["source"])
            f.write("license %s\n" % theirs["license"])
        print("%-15s the pictures of %s, %.1f m" % (name, pictures, metres))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
