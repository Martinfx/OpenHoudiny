"""Geometry and a simulation from Python, with numpy: a column broken into
pieces, how much of it each piece is, the pieces let fall, where they come to
rest -- and the shot to USD, the network to a file the editor opens.

    PYTHONPATH=build/python python3 examples/python/fracture_stats.py
"""

import os

import numpy as np

import pg

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out")

# A column, broken into pieces.
net = pg.Network()
column = net.add("box", "column", size=(0.6, 3.0, 0.6), center=(0.0, 1.5, 0.0))
pieces = net.add("voronoi_fracture", "pieces", count=25, seed=4)
column.connect(pieces)

# How much of the column each piece is: the volume under its faces, by the
# divergence theorem -- the arrays straight from the core, no copies.
geo = pieces.geometry()
P = geo.P
sizes, corners = geo.topology()
starts = geo.primitive_starts()
piece = geo.prims["piece"]
volume = np.zeros(int(piece.max()) + 1)
for face in range(geo.primitive_count):
    ring = corners[starts[face]:starts[face] + sizes[face]]
    a = P[ring[0]]
    for k in range(1, len(ring) - 1):
        volume[piece[face]] += np.dot(a, np.cross(P[ring[k]], P[ring[k + 1]])) / 6.0
print(f"{len(volume)} pieces, {volume.sum():.4f} m3 together -- the column is {0.6 * 3.0 * 0.6:.4f} m3")
print(f"the largest {volume.max() * 1000:.1f} litres, the smallest {volume.min() * 1000:.1f}")

# Let them fall: an RBD Solver without glue, and an Output that simulates it.
rbd = net.add("rbd_solver", "rbd", glue=0.0, bounce=0.2)
pieces.connect(rbd)
shot = net.add("output", "output", frames=60)
rbd.connect(shot)
sim = net.simulate()
with pg.UsdExport(os.path.join(out, "column.usda"), sim) as usd:
    for frame in sim.run():
        usd.add()

rigid = sim.current.rigid
print("where they came to rest, their middles' heights:", np.round(np.sort(rigid.centres[:, 1]), 2))
speed = np.linalg.norm(rigid.velocities, axis=1)
print(f"after {sim.frame} frames the fastest piece still goes {speed.max():.2f} m/s")
print(f"the shot: {usd.path} -- {usd.bodies} bodies moving")

# The network for the editor: prototype out/column.pgsim
net.layout().save(os.path.join(out, "column.pgsim"))
