# How subdivision.usda was made: subdivision surfaces as Maya, Houdini or
# Blender hand them over -- coarse meshes of subdivisionScheme catmullClark,
# their sharp edges and points creases and corners -- for USD Import to
# smooth (docs/usd-import.md).
#
#   pip install usd-core
#   python examples/usd/make_subdivision.py
import math
import os

from pxr import Gf, Sdf, Usd, UsdGeom, Vt

HERE = os.path.dirname(os.path.abspath(__file__))
stage = Usd.Stage.CreateNew(os.path.join(HERE, "subdivision.usda"))
UsdGeom.SetStageUpAxis(stage, "Y")
UsdGeom.SetStageMetersPerUnit(stage, 1.0)
world = UsdGeom.Xform.Define(stage, "/World")
stage.SetDefaultPrim(world.GetPrim())


def mesh(path, points, faces, colour, scheme="catmullClark"):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr([Gf.Vec3f(*(round(c, 5) for c in p)) for p in points])
    m.CreateFaceVertexCountsAttr([len(f) for f in faces])
    m.CreateFaceVertexIndicesAttr([i for f in faces for i in f])
    m.CreateSubdivisionSchemeAttr(scheme)
    m.CreateDisplayColorPrimvar(UsdGeom.Tokens.constant).Set(Vt.Vec3fArray([Gf.Vec3f(*colour)]))
    return m


def creases(m, chains, sharpness):
    """Each chain of points one crease, all as sharp."""
    m.CreateCreaseIndicesAttr([i for c in chains for i in c])
    m.CreateCreaseLengthsAttr([len(c) for c in chains])
    m.CreateCreaseSharpnessesAttr([sharpness] * len(chains))


def box(centre, size):
    h = [s / 2 for s in size]
    points = [(centre[0] + sx * h[0], centre[1] + sy * h[1], centre[2] + sz * h[2])
              for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    return points, faces


# A cube as the surface makes it: round, but for a corner sharp for good.
points, faces = box((-1.05, 0.33, 0.1), (0.6, 0.6, 0.6))
drop = mesh("/World/drop", points, faces, (0.75, 0.12, 0.08))
drop.CreateCornerIndicesAttr([7])
drop.CreateCornerSharpnessesAttr([UsdGeom.Mesh.SHARPNESS_INFINITE])

# A bar of soap: every edge as sharp as 1.5 -- sharp a step, half so the
# next, then round -- a box with soft edges.
points, faces = box((0.0, 0.17, 0.0), (0.8, 0.34, 0.5))
edges = sorted({tuple(sorted((f[i], f[(i + 1) % 4]))) for f in faces for i in range(4)})
soap = mesh("/World/soap", points, faces, (0.85, 0.82, 0.62))
creases(soap, [list(e) for e in edges], 1.5)

# A tin: twelve sides, a ring round its middle; its lid's rim sharp for
# good, its bottom's as sharp as 1 -- a little rounded.
n, r = 12, 0.3
rings = (0.0, 0.3, 0.6)
points = [(1.05 + r * math.cos(2 * math.pi * k / n), y, -0.05 + r * math.sin(2 * math.pi * k / n))
          for y in rings for k in range(n)]
top = (len(rings) - 1) * n
faces = [tuple(range(n)), tuple(range(top + n - 1, top - 1, -1))]
faces += [(j * n + (k + 1) % n, j * n + k, (j + 1) * n + k, (j + 1) * n + (k + 1) % n)
          for j in range(len(rings) - 1) for k in range(n)]
tin = mesh("/World/tin", points, faces, (0.25, 0.42, 0.62))
tin.CreateCreaseIndicesAttr(list(range(top, top + n)) + [top] + list(range(n)) + [0])
tin.CreateCreaseLengthsAttr([n + 1, n + 1])
tin.CreateCreaseSharpnessesAttr([UsdGeom.Mesh.SHARPNESS_INFINITE, 1.0])

stage.GetRootLayer().Save()
