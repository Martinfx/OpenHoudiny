#!/usr/bin/env python3
"""Blosc frames as c-blosc 1.21 makes them (python-blosc 1.11), for
tests/test_vdb_read.cpp: n floats, value i ((37 i) % 101) / 4 - 3, each
frame another way -- compressor, shuffle, blocks, a block left over.

    pip install blosc==1.11.4
    python3 tests/data/blosc/make_blosc.py tests/data/blosc
"""
import os
import struct
import sys

import blosc

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def floats(n):
    return struct.pack("<%df" % n, *[((37 * i) % 101) / 4.0 - 3.0 for i in range(n)])


frames = [
    # name, floats, compressor, shuffle, typesize, level -- the lower, the
    # smaller the blocks c-blosc cuts the floats into
    ("lz4_shuffle", 4096, "lz4", blosc.SHUFFLE, 4, 9),
    ("lz4_blocks", 30000, "lz4", blosc.SHUFFLE, 4, 1),
    ("blosclz_split", 30001, "blosclz", blosc.SHUFFLE, 4, 1),
    ("blosclz_plain", 3000, "blosclz", blosc.NOSHUFFLE, 8, 9),
    ("zlib_wide", 30000, "zlib", blosc.NOSHUFFLE, 8, 5),
    ("lz4hc_leftover", 20001, "lz4hc", blosc.SHUFFLE, 4, 3),
]
for name, n, cname, shuffle, typesize, level in frames:
    frame = blosc.compress(floats(n), typesize=typesize, clevel=level, shuffle=shuffle, cname=cname)
    with open(os.path.join(out, name + ".blosc"), "wb") as f:
        f.write(frame)
