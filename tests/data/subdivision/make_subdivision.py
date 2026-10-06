# How subdivision.txt was made: meshes subdivided by Blender's Subdivision
# Surface modifier -- OpenSubdiv's Catmull-Clark, its points as refined, not
# pushed to the limit -- the points tests/test_topology.cpp holds
# src/pg/nodes/Topology.cpp's Subdivide to.
#
#   pip install bpy==4.5.3 numpy
#   python tests/data/subdivision/make_subdivision.py > tests/data/subdivision/subdivision.txt
#
# Each case: its name, how many steps, the boundary ("edgeAndCorner": its
# corners stay; "edgeOnly": they round off), the points, the faces (the
# count of corners, then the points), the sharp edges (the two points and
# how sharp, as OpenSubdiv and USD have it: 10 infinitely), the sharp points
# (the point and how sharp), then the points Blender makes of it -- in
# Blender's order, which the test does not hold to.
import math

import bpy
import numpy as np


def subdivided(points, faces, levels, edges, corners, boundary):
    mesh = bpy.data.meshes.new("cage")
    mesh.from_pydata(points, [], faces)
    mesh.update()
    # Blender's crease c is OpenSubdiv's sharpness 10 c^2.
    if edges:
        crease = mesh.attributes.new("crease_edge", "FLOAT", "EDGE")
        sharp = {tuple(sorted(e[:2])): e[2] for e in edges}
        for e in mesh.edges:
            crease.data[e.index].value = math.sqrt(min(sharp.get(tuple(sorted(e.vertices)), 0.0), 10.0) / 10.0)
    if corners:
        crease = mesh.attributes.new("crease_vert", "FLOAT", "POINT")
        for i, s in corners:
            crease.data[i].value = math.sqrt(min(s, 10.0) / 10.0)
    obj = bpy.data.objects.new("cage", mesh)
    bpy.context.scene.collection.objects.link(obj)
    mod = obj.modifiers.new("subdivision", "SUBSURF")
    mod.levels = mod.render_levels = levels
    mod.use_limit_surface = False
    mod.use_creases = True
    mod.uv_smooth = "NONE"
    mod.boundary_smooth = "PRESERVE_CORNERS" if boundary == "edgeAndCorner" else "ALL"
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    out = evaluated.to_mesh()
    result = np.array([v.co[:] for v in out.vertices])
    evaluated.to_mesh_clear()
    bpy.data.objects.remove(obj)
    bpy.data.meshes.remove(mesh)
    return result


CUBE = [(-1, -1, -1), (1, -1, -1), (-1, 1, -1), (1, 1, -1), (-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1)]
CUBE_FACES = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]


def grid(n, bump):
    """An n x n grid of quads, its middle raised by `bump`."""
    points = []
    for j in range(n + 1):
        for i in range(n + 1):
            x, z = i / n * 2 - 1, j / n * 2 - 1
            points.append((x, bump * max(0.0, 1 - x * x - z * z) + 0.1 * x * z, z))
    faces = [(j * (n + 1) + i, (j + 1) * (n + 1) + i, (j + 1) * (n + 1) + i + 1, j * (n + 1) + i + 1)
             for j in range(n) for i in range(n)]
    return points, faces


def prism():
    """A pentagon's prism, a pyramid of four triangles on it: n-gons, quads, triangles."""
    points = [(math.cos(a), -1.0, math.sin(a)) for a in (2 * math.pi * k / 5 for k in range(5))]
    points += [(x, 0.5, z) for x, _, z in points]
    points.append((0.1, 1.6, -0.1))
    faces = [(0, 1, 2, 3, 4)]
    faces += [(k, 5 + k, 5 + (k + 1) % 5, (k + 1) % 5) for k in range(5)]
    faces += [(5 + (k + 1) % 5, 5 + k, 10) for k in range(5)]
    return points, faces


cases = []
cases.append(("cube", 2, "edgeAndCorner", CUBE, CUBE_FACES, [], []))
every = sorted({tuple(sorted((f[i], f[(i + 1) % 4]))) for f in CUBE_FACES for i in range(4)})
cases.append(("cube_sharp", 2, "edgeAndCorner", CUBE, CUBE_FACES, [(a, b, 10.0) for a, b in every], []))
# The top's edges as sharp as 2.5 (a blend at the third step), an edge of
# the bottom as 0.4, one at the side as 10 alone: its ends darts.
cases.append(("cube_creases", 3, "edgeAndCorner", CUBE, CUBE_FACES,
              [(4, 5, 2.5), (5, 7, 2.5), (7, 6, 2.5), (6, 4, 2.5), (0, 1, 0.4), (1, 3, 10.0)], []))
cases.append(("cube_corners", 2, "edgeAndCorner", CUBE, CUBE_FACES, [(0, 2, 1.5)], [(7, 1.5), (0, 10.0), (5, 0.3)]))
points, faces = grid(3, 0.8)
cases.append(("grid_edge_and_corner", 2, "edgeAndCorner", points, faces, [(5, 6, 3.0)], [(10, 0.7)]))
cases.append(("grid_edge_only", 2, "edgeOnly", points, faces, [(5, 6, 3.0)], []))
points, faces = prism()
cases.append(("prism", 2, "edgeAndCorner", points, faces, [(5, 10, 1.6), (0, 1, 10.0), (1, 2, 10.0)], [(3, 0.5)]))

for name, levels, boundary, points, faces, edges, corners in cases:
    result = subdivided(points, faces, levels, edges, corners, boundary)
    print(f"case {name} {levels} {boundary}")
    print(f"points {len(points)}")
    for p in points:
        print(" ".join(f"{c:.9g}" for c in p))
    print(f"faces {len(faces)}")
    for f in faces:
        print(len(f), " ".join(str(i) for i in f))
    print(f"edges {len(edges)}")
    for a, b, s in edges:
        print(a, b, f"{s:.9g}")
    print(f"corners {len(corners)}")
    for i, s in corners:
        print(i, f"{s:.9g}")
    print(f"result {len(result)}")
    for p in result:
        print(" ".join(f"{c:.9g}" for c in p))
