"""The pictures tests/test_picture.cpp reads, written by what other programs
use -- PIL (libjpeg), OpenEXR, ffmpeg -- and PNGs by a small writer here that
uses every filter. Where a file is lossy, the reference is what that
library itself decodes it to.

    python3 make_pictures.py      (needs numpy, Pillow, OpenEXR; ffmpeg on PATH)
"""
import os
import struct
import subprocess
import zlib

import numpy as np
import OpenEXR
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def path(name):
    return os.path.join(HERE, name)


# --- PNG: every colour type, bit depth and filter; Adam7 --------------------------------------

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filtered(rows, bpp):
    """Rows of bytes, each filtered by a filter of its own (0..4 in turn)."""
    out = bytearray()
    prior = bytes(len(rows[0])) if rows else b""
    for y, raw in enumerate(rows):
        f = y % 5
        out.append(f)
        for i, v in enumerate(raw):
            a = raw[i - bpp] if i >= bpp else 0
            b = prior[i]
            c = prior[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            out.append((v - pred) & 255)
        prior = raw
    return bytes(out)


def pack(samples, depth):
    """A row of samples as bytes: several to a byte below 8 bits, big-endian at 16."""
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def write_png(name, width, height, color, depth, pixel, interlace=False, plte=None, trns=None):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    bpp = max(1, channels * depth // 8)
    if not interlace:
        passes = [(0, 0, 1, 1)]
    else:
        passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    data = b""
    for x0, y0, dx, dy in passes:
        xs, ys = list(range(x0, width, dx)), list(range(y0, height, dy))
        if not xs or not ys:
            continue
        rows = [pack([s for x in xs for s in pixel(x, y)], depth) for y in ys]
        data += filtered(rows, bpp)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, int(interlace)))
    if plte:
        png += chunk(b"PLTE", plte)
    if trns:
        png += chunk(b"tRNS", trns)
    png += chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b"")
    with open(path(name), "wb") as f:
        f.write(png)


def pngs():
    # The same formulas are in tests/test_picture.cpp.
    w, h = 19, 13
    write_png("rgb8.png", w, h, 2, 8, lambda x, y: [(x * 37 + y * 11 + c * 71 + (x * y) % 13) % 256 for c in range(3)])
    write_png("rgb8_adam7.png", w, h, 2, 8, lambda x, y: [(x * 37 + y * 11 + c * 71 + (x * y) % 13) % 256 for c in range(3)],
              interlace=True)
    write_png("rgba16.png", w, h, 6, 16, lambda x, y: [(x * 9001 + y * 577 + c * 12345 + x * y * 31) % 65536 for c in range(4)])
    write_png("grey4.png", w, h, 0, 4, lambda x, y: [(x + 3 * y) % 16])
    write_png("grey1_adam7.png", w, h, 0, 1, lambda x, y: [(x + 3 * y) % 2], interlace=True)
    write_png("grey_alpha8.png", w, h, 4, 8, lambda x, y: [(x * 5 + y * 3) % 256, (x * y + 7) % 256])
    plte = b"".join(bytes([(i * 15) % 256, (255 - i * 13) % 256, (i * 47) % 256]) for i in range(17))
    trns = bytes((i * 29) % 256 for i in range(17))
    write_png("palette2.png", w, h, 3, 8, lambda x, y: [(x * 3 + y) % 17], plte=plte, trns=trns)
    write_png("palette_bits.png", w, h, 3, 4, lambda x, y: [(x * 3 + y) % 16], plte=plte[:48])


# --- JPEG: what libjpeg writes, and what it reads back ----------------------------------------

def picture(w, h):
    x, y = np.meshgrid(np.arange(w), np.arange(h))
    r = 128 + 100 * np.sin(x / 5.0 + y / 9.0)
    g = 128 + 100 * np.cos(y / 4.0) * np.cos(x / 11.0)
    b = (x * y * 3 + x * 7) % 256
    return np.clip(np.dstack([r, g, b]), 0, 255).astype(np.uint8)


def jpegs():
    img = Image.fromarray(picture(37, 29))
    variants = {
        "420.jpg": dict(quality=90, subsampling=2),
        "444.jpg": dict(quality=90, subsampling=0),
        "422.jpg": dict(quality=75, subsampling=1),
        "progressive.jpg": dict(quality=85, subsampling=2, progressive=True, optimize=True),
        "progressive444.jpg": dict(quality=95, subsampling=0, progressive=True),
        "restart.jpg": dict(quality=80, subsampling=2, restart_marker_blocks=3),
    }
    for name, options in variants.items():
        img.save(path(name), **options)
    img.convert("L").save(path("grey.jpg"), quality=90)
    # 4:4:0 (halved up and down only), as a camera or ffmpeg writes it.
    raw = picture(37, 29).tobytes()
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "37x29", "-i", "-",
                    "-pix_fmt", "yuvj440p", "-frames:v", "1", path("440.jpg")], input=raw, check=True)
    for name in list(variants) + ["grey.jpg", "440.jpg"]:
        decoded = np.asarray(Image.open(path(name)).convert("RGB"))
        with open(path(name[:-4] + ".rgb"), "wb") as f:
            f.write(decoded.tobytes())


# --- OpenEXR: each compression, and the library's own reading of the lossy ones ----------------

def exr_channels(w, h):
    rng = np.random.default_rng(7)
    x, y = np.meshgrid(np.arange(w), np.arange(h))
    r = (np.sin(x / 4.0) * 3 + y / 10.0).astype(np.float16)            # beyond 1 and below 0
    g = (np.where((x // 8 + y // 8) % 2 == 0, 0.25, 0.75)).astype(np.float16)  # flat blocks
    b = (np.round(rng.random((h, w)) * 8) / 256 + x / w).astype(np.float16)
    a = (np.sin(x / 3.0) * np.cos(y / 5.0) * 2 + np.round(rng.random((h, w)) * 4) / 1024).astype(np.float32)
    z = (x * 1000 + y).astype(np.uint32)
    return {"R": r, "G": g, "B": b, "A": a, "Z": z}


def write_exr(name, channels, compression, data_window=None, display_window=None):
    header = {"compression": compression, "type": OpenEXR.scanlineimage}
    if data_window:
        header["dataWindow"] = data_window
    if display_window:
        header["displayWindow"] = display_window
    OpenEXR.File(header, {k: OpenEXR.Channel(v) for k, v in channels.items()}).write(path(name))


def exrs():
    w, h = 29, 37
    full = exr_channels(w, h)
    for name, compression in [("none", OpenEXR.NO_COMPRESSION), ("rle", OpenEXR.RLE_COMPRESSION),
                              ("zips", OpenEXR.ZIPS_COMPRESSION), ("zip", OpenEXR.ZIP_COMPRESSION),
                              ("piz", OpenEXR.PIZ_COMPRESSION), ("pxr24", OpenEXR.PXR24_COMPRESSION),
                              ("b44", OpenEXR.B44_COMPRESSION), ("b44a", OpenEXR.B44A_COMPRESSION)]:
        write_exr(name + ".exr", full, compression)
    # The lossy ones as the library reads them, stored uncompressed.
    for name in ("pxr24", "b44", "b44a"):
        with OpenEXR.File(path(name + ".exr"), separate_channels=True) as f:
            channels = {k: np.array(v.pixels) for k, v in f.channels().items()}
        write_exr(name + "_ref.exr", channels, OpenEXR.NO_COMPRESSION)
    # A data window inside a larger display window.
    part = {k: np.ascontiguousarray(v[5:25, 3:27]) for k, v in exr_channels(w, h).items()}
    write_exr("window.exr", {k: part[k] for k in ("R", "G", "B")}, OpenEXR.ZIP_COMPRESSION,
              data_window=((3, 5), (26, 24)), display_window=((0, 0), (33, 29)))
    # Grey: a luminance channel only.
    write_exr("luminance.exr", {"Y": full["R"]}, OpenEXR.PIZ_COMPRESSION)
    # What is not read: DWA.
    write_exr("dwaa.exr", {k: full[k] for k in ("R", "G", "B")}, OpenEXR.DWAA_COMPRESSION)


if __name__ == "__main__":
    pngs()
    jpegs()
    exrs()
