"""Writes the USD files of the matchmove example (examples/sim/matchmove.pgsim)
with the USD library itself, as a studio's other programs would: Maya's
units (centimetres, Z up), a set that references an asset with variants, a
camera from a matchmove -- a hand-held push in, 24 frames a second from
1001 -- and a shot that brings the set and the camera in as sublayers.

    pip install usd-core
    python3 examples/usd/make_shot.py

The files are in the repository already; this is how they were made.
"""

import math
import os

from pxr import Gf, Sdf, Usd, UsdGeom

HERE = os.path.dirname(os.path.abspath(__file__))
START, END, FPS = 1001, 1072, 24


def new_stage(name):
    path = os.path.join(HERE, name)
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, UsdGeom.LinearUnits.centimeters)
    return stage


def usd(x, y, z):
    """The program's metres, Y up, to Maya's centimetres, Z up."""
    return Gf.Vec3d(x * 100.0, -z * 100.0, y * 100.0)


def block(stage, path, center, size, color):
    """A box as a Cube of size 1, placed and scaled -- in the program's
    metres, Y up."""
    cube = UsdGeom.Cube.Define(stage, path)
    cube.CreateSizeAttr(1.0)
    cube.CreateDisplayColorAttr([Gf.Vec3f(*color)])
    cube.AddTranslateOp().Set(usd(*center))
    cube.AddScaleOp().Set(Gf.Vec3f(size[0] * 100.0, size[2] * 100.0, size[1] * 100.0))
    return cube


# --- the crate: an asset with a variant set of sizes ------------------------------------------
crate = new_stage("crate.usda")
root = UsdGeom.Xform.Define(crate, "/Crate")
crate.SetDefaultPrim(root.GetPrim())
Usd.ModelAPI(root).SetKind("component")
sizes = root.GetPrim().GetVariantSets().AddVariantSet("size")
for name, edge in [("small", 50.0), ("large", 80.0)]:
    sizes.AddVariant(name)
    sizes.SetVariantSelection(name)
    with sizes.GetVariantEditContext():
        box = UsdGeom.Cube.Define(crate, "/Crate/box")
        box.CreateSizeAttr(edge)
        box.CreateDisplayColorAttr([Gf.Vec3f(0.45, 0.33, 0.2)])
        box.AddTranslateOp().Set(Gf.Vec3d(0, 0, edge / 2))
sizes.SetVariantSelection("small")
crate.GetRootLayer().Save()

# --- the set: a courtyard -- ground, walls, a beam on a pillar, two crates --------------------
court = new_stage("courtyard.usda")
setp = UsdGeom.Xform.Define(court, "/Set")
court.SetDefaultPrim(setp.GetPrim())
Usd.ModelAPI(setp).SetKind("assembly")
ground = UsdGeom.Mesh.Define(court, "/Set/ground")
n, half = 6, 300.0
points, counts, indices, colors = [], [], [], []
for j in range(n + 1):
    for i in range(n + 1):
        points.append(Gf.Vec3f(-half + 2 * half * i / n, -half + 2 * half * j / n, 0.0))
for j in range(n):
    for i in range(n):
        a = j * (n + 1) + i
        counts.append(4)
        indices += [a, a + 1, a + n + 2, a + n + 1]
        colors.append(Gf.Vec3f(0.5, 0.48, 0.45) if (i + j) % 2 else Gf.Vec3f(0.42, 0.4, 0.37))
ground.CreatePointsAttr(points)
ground.CreateFaceVertexCountsAttr(counts)
ground.CreateFaceVertexIndicesAttr(indices)
ground.CreateSubdivisionSchemeAttr("none")
ground.CreateDisplayColorPrimvar(UsdGeom.Tokens.uniform).Set(colors)
block(court, "/Set/back_wall", (0.0, 1.25, -1.0), (3.0, 2.5, 0.2), (0.62, 0.58, 0.52))
block(court, "/Set/side_wall", (-1.4, 1.25, 0.0), (0.2, 2.5, 2.0), (0.6, 0.56, 0.5))
block(court, "/Set/beam", (0.0, 1.7, 0.0), (2.6, 0.2, 0.3), (0.35, 0.25, 0.17))
block(court, "/Set/pillar", (1.2, 0.85, 0.0), (0.25, 1.7, 0.25), (0.6, 0.56, 0.5))
for name, variant, at in [("crate_a", "small", (0.75, 0.0, 0.6)), ("crate_b", "large", (-0.8, 0.0, 0.65))]:
    ref = UsdGeom.Xform.Define(court, "/Set/" + name)
    ref.GetPrim().GetReferences().AddReference("./crate.usda")
    ref.GetPrim().GetVariantSets().GetVariantSet("size").SetVariantSelection(variant)
    ref.AddTranslateOp().Set(usd(*at))
    ref.AddRotateZOp().Set(17.0 if variant == "small" else -8.0)
court.GetRootLayer().Save()

# --- the camera: a hand-held push in, as a matchmove delivers it -------------------------------
cams = new_stage("camera.usda")
cams.SetStartTimeCode(START)
cams.SetEndTimeCode(END)
cams.SetTimeCodesPerSecond(FPS)
cams.SetFramesPerSecond(FPS)
cams.SetDefaultPrim(UsdGeom.Xform.Define(cams, "/Cameras").GetPrim())
cam = UsdGeom.Camera.Define(cams, "/Cameras/plate")
cam.CreateFocalLengthAttr(24.0)
cam.CreateHorizontalApertureAttr(24.89)  # Super 35, 16:9
cam.CreateVerticalApertureAttr(14.0)
cam.CreateClippingRangeAttr(Gf.Vec2f(1.0, 100000.0))
translate = cam.AddTranslateOp()
rotate = cam.AddRotateXYZOp()
target = usd(0.0, 1.05, -0.2)
for f in range(START, END + 1):
    u = (f - START) / (END - START)
    ease = u * u * (3 - 2 * u)
    t = f / FPS
    shake = lambda k: sum(a * math.sin(t * w + p) for a, w, p in k)
    pos = usd(0.6 - 0.35 * ease + shake([(0.012, 5.1, 0.3), (0.006, 11.3, 1.7)]),
              1.6 - 0.15 * ease + shake([(0.01, 6.7, 2.1), (0.004, 13.9, 0.2)]),
              5.6 - 1.5 * ease)
    d = target - pos
    pan = math.degrees(math.atan2(-d[0], d[1])) + shake([(0.35, 4.3, 0.9), (0.15, 9.7, 2.5)])
    tilt = math.degrees(math.atan2(d[2], math.hypot(d[0], d[1]))) + shake([(0.3, 5.9, 1.3), (0.12, 12.1, 0.4)])
    roll = shake([(0.4, 3.7, 0.6)])
    translate.Set(pos, f)
    rotate.Set(Gf.Vec3f(90.0 + tilt, roll, pan), f)
cams.GetRootLayer().Save()

# --- the shot: the set and the camera as sublayers ---------------------------------------------
shot = new_stage("shot.usda")
shot.SetStartTimeCode(START)
shot.SetEndTimeCode(END)
shot.SetTimeCodesPerSecond(FPS)
shot.SetFramesPerSecond(FPS)
shot.GetRootLayer().subLayerPaths.append("./camera.usda")
shot.GetRootLayer().subLayerPaths.append("./courtyard.usda")
shot.GetRootLayer().documentation = "The matchmove example: the courtyard set and the plate camera."
shot.GetRootLayer().defaultPrim = "Set"
shot.GetRootLayer().Save()
print("wrote", ", ".join(["crate.usda", "courtyard.usda", "camera.usda", "shot.usda"]))
