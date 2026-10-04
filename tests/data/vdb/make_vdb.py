#!/usr/bin/env python3
"""The OpenVDB files tests/test_vdb_read.cpp reads -- written by the
library itself: pyopenvdb, OpenVDB 10.0.1, which writes file version 224
and compresses with Blosc (LZ4) and the active mask.

    /usr/bin/python3.12 tests/data/vdb/make_vdb.py tests/data/vdb

Each grid's values follow a rule the test follows again, voxel by voxel.
(make_vdb13.cpp writes the rest, with OpenVDB 13: zip, no compression,
double, int and half grids, a stream without offsets.)
"""
import math
import os
import sys

import pyopenvdb as vdb

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def inside(i, j, k):
    """The smoke: an egg round voxel (2, 10, 1), 24 x 20 x 16 voxels across."""
    return ((i - 2) / 12.0) ** 2 + ((j - 10) / 10.0) ** 2 + ((k - 1) / 8.0) ** 2 <= 1.0


def density(i, j, k):
    return 0.5 + 0.01 * ((i * 7 + j * 13 + k * 5) % 50)


def temperature(i, j, k):
    return 0.25 * (j - 2)


def smoke_grids(half):
    t = vdb.createLinearTransform(voxelSize=0.1)
    t.translate((0.25, 0.0, -0.5))
    d = vdb.FloatGrid()
    d.name = "density"
    d.gridClass = vdb.GridClass.FOG_VOLUME
    d.transform = t
    d.saveFloatAsHalf = half
    h = vdb.FloatGrid()
    h.name = "temperature"
    h.transform = t
    da, ha = d.getAccessor(), h.getAccessor()
    for i in range(-10, 15):
        for j in range(0, 21):
            for k in range(-7, 10):
                if inside(i, j, k):
                    da.setValueOn((i, j, k), density(i, j, k))
                    ha.setValueOn((i, j, k), temperature(i, j, k))
    return [d, h]


# Smoke and its heat, as full floats and as half floats.
vdb.write(os.path.join(out, "smoke.vdb"), grids=smoke_grids(False), metadata={"creator": "make_vdb.py"})
vdb.write(os.path.join(out, "smoke_half.vdb"), grids=smoke_grids(True)[:1])

# A level set: a ball 1 across at (0, 0.6, 0) -- a narrow band of
# distances, inside it tiles of minus the background.
ball = vdb.createLevelSetSphere(radius=0.5, center=(0.0, 0.6, 0.0), voxelSize=0.05, halfWidth=3.0)
ball.name = "surface"
vdb.write(os.path.join(out, "sphere.vdb"), grids=[ball])

# Tiles: a slab filled at once -- whole nodes of 8^3 and 128^3 voxels as
# tiles, leaves where it does not fill them -- a voxel apart, and a block
# made inactive again with another value.
fill = vdb.FloatGrid()
fill.name = "fill"
fill.transform = vdb.createLinearTransform(voxelSize=0.5)
fill.fill((-128, 0, 0), (127, 63, 15), 0.75, True)
fill.fill((-120, 40, 0), (-113, 47, 7), 2.0, False)
fill.getAccessor().setValueOn((-130, 5, 5), 3.0)
vdb.write(os.path.join(out, "tiles.vdb"), grids=[fill])

# A vector grid on voxels longer in y: resampled onto cubes.
vel = vdb.Vec3SGrid()
vel.name = "vel"
tv = vdb.createLinearTransform(voxelSize=1.0)
tv.scale((0.1, 0.2, 0.1))
vel.transform = tv
va = vel.getAccessor()
for i in range(0, 6):
    for j in range(0, 4):
        for k in range(0, 3):
            va.setValueOn((i, j, k), (0.1 * i, 0.2 * j, -0.3 * k))
vdb.write(os.path.join(out, "vel.vdb"), grids=[vel])

# A grid turned 30 degrees about y: an affine transform.
turned = vdb.FloatGrid()
turned.name = "turned"
tt = vdb.createLinearTransform(voxelSize=0.1)
tt.rotate(math.radians(30.0), vdb.Axis.Y)
tt.translate((1.0, 0.5, 0.0))
turned.transform = tt
ta = turned.getAccessor()
for i in range(0, 10):
    for j in range(0, 8):
        for k in range(0, 8):
            ta.setValueOn((i, j, k), 1.0 + 0.1 * i)
vdb.write(os.path.join(out, "turned.vdb"), grids=[turned])

# What is not read beside what is: a bool grid, a frustum; and two grids
# sharing one tree -- the second written as an instance of the first -- and
# two of one name.
mask = vdb.BoolGrid()
mask.name = "mask"
mask.getAccessor().setValueOn((1, 2, 3), True)
frustum = vdb.FloatGrid()
frustum.name = "frustum"
frustum.transform = vdb.createFrustumTransform(xyzMin=(0, 0, 0), xyzMax=(10, 10, 10), taper=0.5, depth=4.0)
frustum.getAccessor().setValueOn((1, 1, 1), 1.0)
a = vdb.FloatGrid()
a.name = "a"
a.getAccessor().setValueOn((4, 5, 6), 0.5)
b = a.copy()
b.name = "b"
b.transform = vdb.createLinearTransform(voxelSize=2.0)
twin = vdb.FloatGrid()
twin.name = "a"
twin.getAccessor().setValueOn((0, 0, 0), 0.25)
vdb.write(os.path.join(out, "mixed.vdb"), grids=[mask, frustum, a, b, twin])
