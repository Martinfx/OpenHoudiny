#!/usr/bin/env python3
"""The OpenVDB files of the examples (examples/sim/vdb_*.pgsim), written by
OpenVDB itself -- pyopenvdb, OpenVDB 10: half floats, compressed with
Blosc, as Houdini writes them:

  fireball.vdb  the explosion example 0.6 s in, its fireball rising and
                rolling into soot -- density, temperature, flame --
                simulated by this program, exported with a Gas Volume and
                written again, smaller
  rock.vdb      a boulder as a level set: a lumpy ball's distances, inside
                below 0, within three voxels of its surface

    /usr/bin/python3.12 examples/vdb/make_vdb.py build/prototype
"""
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
import pyopenvdb as vdb

here = os.path.dirname(os.path.abspath(__file__))
prototype = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "build", "prototype")


def fireball():
    """The explosion at frame 18, its gas exported as this program writes it."""
    with open(os.path.join(here, "..", "sim", "explosion.pgsim")) as f:
        text = f.read()
    ids = [int(line.split()[1]) for line in text.splitlines() if line.startswith("node ")]
    solver = next(int(line.split()[1]) for line in text.splitlines() if line.startswith("node ") and " pyro_solver " in line)
    n = max(ids) + 1
    text += "node %d gas_volume 1 volumes 490 170\nlink %d.gas -> %d.gas\n" % (n, solver, n)
    with tempfile.TemporaryDirectory() as tmp:
        net = os.path.join(tmp, "explosion.pgsim")
        with open(net, "w") as f:
            f.write(text)
        out = os.path.join(tmp, "boom.$F4.vdb")
        subprocess.run([prototype, "sim", net, "-", "--frames", "18", "--export-node", "volumes", "--export", out], check=True)
        grids, _ = vdb.readAll(os.path.join(tmp, "boom.0018.vdb"))
    for g in grids:
        g.saveFloatAsHalf = True
    vdb.write(os.path.join(here, "fireball.vdb"), grids=grids, metadata={"creator": "make_vdb.py"})


def rock():
    """A boulder 1.6 m wide and 0.9 high, its foot in the floor."""
    # An icosahedron, its faces split four times: a ball of 5120 triangles.
    t = (1.0 + math.sqrt(5.0)) / 2.0
    points = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
              (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    points = [np.array(p, dtype=float) / np.linalg.norm(p) for p in points]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
             (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
             (8, 6, 7), (9, 8, 1)]
    for _ in range(4):
        middle = {}

        def half(a, b):
            key = (min(a, b), max(a, b))
            if key not in middle:
                m = points[a] + points[b]
                points.append(m / np.linalg.norm(m))
                middle[key] = len(points) - 1
            return middle[key]

        split = []
        for a, b, c in faces:
            ab, bc, ca = half(a, b), half(b, c), half(c, a)
            split += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = split
    p = np.array(points)
    # Lumps: smooth noise -- values on a lattice, blended -- in three octaves.
    rng = np.random.default_rng(11)
    lattice = rng.random((32, 32, 32))

    def noise(q):
        i = np.floor(q).astype(int)
        f = q - i
        f = f * f * (3.0 - 2.0 * f)
        out = 0.0
        for dx in (0, 1):
            for dy in (0, 1):
                for dz in (0, 1):
                    w = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                    out = out + w * lattice[(i[:, 0] + dx) % 32, (i[:, 1] + dy) % 32, (i[:, 2] + dz) % 32]
        return out

    lumps = sum(0.5 ** o * noise(p * 1.6 * 2 ** o + 7.3) for o in range(3)) / 1.75
    p = p * (1.0 + 0.45 * (lumps - 0.5))[:, None]
    p = p * np.array([0.8, 0.5, 0.65]) + np.array([0.0, 0.38, 0.0])
    grid = vdb.FloatGrid.createLevelSetFromPolygons(p.astype(np.float32), triangles=np.array(faces, dtype=np.uint32),
                                                     transform=vdb.createLinearTransform(voxelSize=0.035), halfWidth=3.0)
    grid.name = "rock"
    grid.saveFloatAsHalf = True
    vdb.write(os.path.join(here, "rock.vdb"), grids=[grid], metadata={"creator": "make_vdb.py"})


fireball()
rock()
