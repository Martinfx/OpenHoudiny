"""Writes the USD files tests/test_usd_read.cpp reads -- with the USD library
itself, so that the reader is tested against what USD writes:

    values.usda / values.usdc   one stage, text and crate: values of every
                                kind, samples in time, list edits, variants,
                                a relationship, dictionaries
    old.usdc                    a crate of version 0.4.0, the oldest read
    package.usdz                a package: its layer refers to a file in it

    pip install usd-core
    python3 tests/data/usd/make_fixtures.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def values(path):
    from pxr import Gf, Sdf, Usd, UsdGeom

    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    stage.SetMetadata("metersPerUnit", 0.01)
    stage.SetMetadata("upAxis", "Z")
    stage.SetStartTimeCode(1001)
    stage.SetEndTimeCode(1010)
    stage.SetTimeCodesPerSecond(24)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    world.AddTranslateOp().Set(Gf.Vec3d(1, 2, 3))
    world.AddRotateXYZOp().Set(Gf.Vec3f(10, 20, 30))
    p = world.GetPrim()
    T = Sdf.ValueTypeNames
    p.CreateAttribute("ints", T.IntArray).Set(list(range(0, 300, 3)))
    p.CreateAttribute("bigints", T.IntArray).Set([i * i * 1000 - 70000 for i in range(40)])
    p.CreateAttribute("small_ints", T.IntArray).Set([5, -4, 3])
    p.CreateAttribute("int64s", T.Int64Array).Set([1, -5, 1 << 40, 7] * 10)
    p.CreateAttribute("uints", T.UIntArray).Set([4000000000 - i * 7 for i in range(30)])
    p.CreateAttribute("big64", T.Int64).Set(1 << 40)
    p.CreateAttribute("small64", T.Int64).Set(-7)
    p.CreateAttribute("floats_whole", T.FloatArray).Set([float(i % 17) for i in range(100)])
    p.CreateAttribute("floats_table", T.FloatArray).Set([0.5, 0.25, 0.125] * 30)
    p.CreateAttribute("floats", T.FloatArray).Set([i * 0.37 for i in range(50)])
    p.CreateAttribute("doubles_table", T.DoubleArray).Set([0.1, 0.2, 1e10] * 20)
    p.CreateAttribute("halfs", T.HalfArray).Set([0.5, 1.5, 2.25] * 10)
    p.CreateAttribute("half", T.Half).Set(0.333)
    p.CreateAttribute("double", T.Double).Set(0.1)
    p.CreateAttribute("double_exact", T.Double).Set(0.5)
    p.CreateAttribute("m4", T.Matrix4d).Set(Gf.Matrix4d(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16))
    p.CreateAttribute("m4_diagonal", T.Matrix4d).Set(Gf.Matrix4d(2))
    p.CreateAttribute("m3", T.Matrix3d).Set(Gf.Matrix3d(1, 0, 0, 0, 3, 0, 0, 0, -4))
    p.CreateAttribute("q", T.Quatf).Set(Gf.Quatf(0.5, 0.1, 0.2, 0.3))
    p.CreateAttribute("qd", T.Quatd).Set(Gf.Quatd(1, 0, 0, 0))
    p.CreateAttribute("qh", T.Quath).Set(Gf.Quath(0.5, 0.5, 0.5, 0.5))
    p.CreateAttribute("qs", T.QuatfArray).Set([Gf.Quatf(0.5, 0.1, 0.2, 0.3), Gf.Quatf(1, 0, 0, 0)])
    p.CreateAttribute("v3_small", T.Float3).Set(Gf.Vec3f(1, -2, 3))
    p.CreateAttribute("v3", T.Double3).Set(Gf.Vec3d(1.5, -2, 3))
    p.CreateAttribute("i2", T.Int2).Set(Gf.Vec2i(1000, -3))
    p.CreateAttribute("tokens", T.TokenArray).Set(["a", "b", "c"])
    p.CreateAttribute("strings", T.StringArray).Set(["x y", "z"])
    p.CreateAttribute("string", T.String).Set('say "hi"\n\ttab \\ back')
    p.CreateAttribute("asset", T.Asset).Set("./tex.png")
    p.CreateAttribute("assets", T.AssetArray).Set(["./a.png", "./b.png"])
    p.CreateAttribute("empty", T.FloatArray).Set([])
    p.CreateAttribute("flag", T.Bool).Set(True)
    p.CreateAttribute("byte", T.UChar).Set(200)
    blocked = p.CreateAttribute("blocked", T.Float)
    blocked.Set(1.0)
    blocked.Block()
    anim = p.CreateAttribute("anim", T.Float)
    anim.Set(1.0, 1001)
    anim.Set(2.0, 1005)
    anim.Set(Sdf.ValueBlock(), 1008)
    anim.Set(4.0, 1009)
    pts = UsdGeom.Points.Define(stage, "/World/points")
    base = [(i * 0.1, i * 0.2, i * 0.3) for i in range(100)]
    for f in range(1001, 1004):
        pts.GetPointsAttr().Set([(x + f, y, z) for x, y, z in base], f)
    pts.CreateIdsAttr(list(range(1000, 1100)))
    mesh = UsdGeom.Mesh.Define(stage, "/World/mesh")
    mesh.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    uv = UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", T.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
    uv.Set([(0, 0), (1, 0), (1, 1), (0, 1)])
    cam = UsdGeom.Camera.Define(stage, "/World/cam")
    cam.CreateFocalLengthAttr(35)
    cam.CreateHorizontalApertureAttr(36)
    cam.CreateVerticalApertureAttr(20.25)
    cam.AddTransformOp().Set(Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 1, 10)), 1001)
    cam.GetPrim().SetMetadata("kind", "component")
    cam.GetPrim().SetCustomDataByKey("note", "matchmove v3")
    cam.GetPrim().SetCustomDataByKey("nested:deep", 5)
    ref = stage.DefinePrim("/World/ref")
    ref.GetReferences().AddReference("./other.usda", "/Thing", Sdf.LayerOffset(10, 2))
    ref.GetReferences().AddInternalReference("/World/mesh")
    ref.GetPayloads().AddPayload("./pay.usdc", "/P", Sdf.LayerOffset(5))
    ref.GetInherits().AddInherit("/_class_Thing")
    sets = ref.GetVariantSets().AddVariantSet("lod")
    for v, kind in [("low", "Cube"), ("high", "Sphere")]:
        sets.AddVariant(v)
        sets.SetVariantSelection(v)
        with sets.GetVariantEditContext():
            getattr(UsdGeom, kind).Define(stage, "/World/ref/shape")
    sets.SetVariantSelection("high")
    rel = p.CreateRelationship("targets")
    rel.AddTarget("/World/mesh")
    rel.AddTarget("/World/points")
    stage.OverridePrim("/Over")
    stage.CreateClassPrim("/_class_Thing").CreateAttribute("thing", T.Float).Set(3.0)
    stage.GetRootLayer().Save()


def old(path):
    from pxr import Gf, Usd, UsdGeom

    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    mesh = UsdGeom.Mesh.Define(stage, "/World/mesh")
    n = 64
    mesh.CreatePointsAttr([(i * 0.5, (i % 7) * 0.25, 0) for i in range(n)])
    mesh.CreateFaceVertexCountsAttr([3] * (n - 2))
    mesh.CreateFaceVertexIndicesAttr([k for i in range(1, n - 1) for k in (0, i, i + 1)])
    world.AddTranslateOp().Set(Gf.Vec3d(1, 2, 3))
    ref = stage.DefinePrim("/World/ref")
    ref.GetReferences().AddInternalReference("/World/mesh")
    stage.GetRootLayer().Save()


def package(path):
    from pxr import Gf, Sdf, Usd, UsdGeom, UsdUtils

    work = tempfile.mkdtemp()
    part = os.path.join(work, "part.usdc")
    s = Usd.Stage.CreateNew(part)
    UsdGeom.Cube.Define(s, "/Part").CreateSizeAttr(3)
    s.SetDefaultPrim(s.GetPrimAtPath("/Part"))
    s.GetRootLayer().Save()
    root = os.path.join(work, "root.usda")
    s = Usd.Stage.CreateNew(root)
    w = UsdGeom.Xform.Define(s, "/World")
    s.SetDefaultPrim(w.GetPrim())
    p = s.DefinePrim("/World/part")
    p.GetReferences().AddReference("./part.usdc")
    UsdGeom.Xformable(p).AddTranslateOp().Set(Gf.Vec3d(1, 2, 3))
    s.GetRootLayer().Save()
    if os.path.exists(path):
        os.remove(path)
    UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(root), path)
    shutil.rmtree(work)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--old":
        old(sys.argv[2])
        sys.exit(0)
    values(os.path.join(HERE, "values.usda"))
    values(os.path.join(HERE, "values.usdc"))
    package(os.path.join(HERE, "package.usdz"))
    # The oldest crate: in a process of its own, the version set before USD loads.
    env = dict(os.environ, USD_WRITE_NEW_USDC_FILES_AS_VERSION="0.4.0")
    subprocess.run([sys.executable, __file__, "--old", os.path.join(HERE, "old.usdc")], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote values.usda, values.usdc, old.usdc, package.usdz")
