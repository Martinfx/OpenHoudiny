"""USD read by the program's own reader (pg.UsdStage) -- and, where the USD
library is installed (pip install usd-core), checked against it: the same
stages written by it, composed by both, compared prim by prim.

    PYTHONPATH=build/python python3 -m unittest discover -s tests/python
"""

import math
import os
import random
import shutil
import tempfile
import unittest

import pg

try:
    import numpy as np
except ImportError:
    np = None

try:
    from pxr import Gf, Sdf, Usd, UsdGeom
except ImportError:
    Usd = None

EXAMPLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples")


class Shot(unittest.TestCase):
    """The matchmove example's files: centimetres, Z up, from 1001."""

    def setUp(self):
        self.stage = pg.UsdStage(os.path.join(EXAMPLES, "usd", "shot.usda"))

    def test_the_shot_composes_its_set_and_camera(self):
        s = self.stage
        self.assertEqual(s.up_axis, "Z")
        self.assertAlmostEqual(s.meters_per_unit, 0.01)
        self.assertEqual((s.start_time_code, s.end_time_code), (1001.0, 1072.0))
        self.assertEqual(len(s.files), 4)
        self.assertEqual(s.warnings, [])
        paths = [p.path for p in s.prims()]
        self.assertIn("/Set/crate_b/box", paths)
        # The crates reference one asset; each chooses its size.
        self.assertEqual(s.prim("/Set/crate_a/box").get("size"), 50.0)
        self.assertEqual(s.prim("/Set/crate_b/box").get("size"), 80.0)
        self.assertEqual([c.path for c in s.cameras()], ["/Cameras/plate"])

    @unittest.skipIf(np is None, "numpy is not installed")
    def test_the_camera_in_metres_y_up(self):
        cam = self.stage.camera()
        self.assertEqual(cam["path"], "/Cameras/plate")
        self.assertTrue(cam["varies"])
        self.assertEqual(cam["focal_length"], 24.0)
        world = np.asarray(cam["world"])
        # Frame 1: 5.6 m in front of the set, 1.6 m up, looking at it (-z).
        self.assertAlmostEqual(float(world[3][2]), 5.6, delta=0.05)
        self.assertAlmostEqual(float(world[3][1]), 1.6, delta=0.05)
        back = world[2][:3] / np.linalg.norm(world[2][:3])  # the matrix carries the centimetres' scale
        self.assertGreater(float(back[2]), 0.9)
        self.assertLess(self.stage.camera(time=1072)["world"][3][2], 4.2)

    def test_the_set_as_geometry(self):
        geo = self.stage.geometry(prims=["/Set"])
        self.assertEqual(self.stage.notes, [])
        paths = set(geo.prims["path"])
        self.assertIn("/Set/beam", paths)
        self.assertIn("/Set/crate_b/box", paths)
        lo, hi = geo.bounds()
        self.assertAlmostEqual(lo[1], 0.0, places=4)   # the ground, at 0
        self.assertAlmostEqual(hi[1], 2.5, places=4)   # the walls' tops
        self.assertFalse(self.stage.geometry_varies(prims=["/Set"]))

    def test_the_example_network_renders_through_the_plate(self):
        net = pg.Network.example("matchmove")
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        net["solver"]["resolution"] = 16
        net["output"]["frames"] = 3
        sim = net.simulate()
        first, last = sim.camera(1), sim.camera(3)
        self.assertEqual((first["width"], first["height"]), (1280, 720))
        self.assertNotEqual(first["position"], last["position"])
        walls = sim.geometry(net["set"])
        self.assertEqual(walls.primitive_count, 36)  # four blocks and two crates, six faces each


@unittest.skipIf(Usd is None or np is None, "the USD library (usd-core) and numpy are not installed")
class AgainstUsd(unittest.TestCase):
    """The same stages composed by USD and by the program's reader."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def path(self, name):
        return os.path.join(self.dir, name)

    def test_transforms_of_every_kind_of_op(self):
        rng = random.Random(5)
        ops = ["translate", "scale", "rotateX", "rotateY", "rotateZ", "rotateXYZ", "rotateXZY", "rotateYXZ",
               "rotateYZX", "rotateZXY", "rotateZYX", "orient", "transform"]
        for trial in range(4):
            path = self.path(f"x{trial}." + ("usda", "usdc")[trial % 2])
            stage = Usd.Stage.CreateNew(path)
            prims = []

            def build(parent, depth):
                for k in range(2):
                    p = f"{parent}/n{k}"
                    x = UsdGeom.Xform.Define(stage, p)
                    if depth and rng.random() < 0.15:
                        x.SetResetXformStack(True)
                    made = []
                    for j in range(rng.randint(1, 4)):
                        if made and rng.random() < 0.2:
                            t, prec, suffix = rng.choice(made)
                            try:  # the inverse of an op, once
                                x.AddXformOp(getattr(UsdGeom.XformOp, "Type" + t[0].upper() + t[1:]), prec, suffix, True)
                            except Exception:
                                pass
                            continue
                        t = rng.choice(ops)
                        double = rng.random() < 0.5 or t == "transform"
                        prec = UsdGeom.XformOp.PrecisionDouble if double else UsdGeom.XformOp.PrecisionFloat
                        op = x.AddXformOp(getattr(UsdGeom.XformOp, "Type" + t[0].upper() + t[1:]), prec, f"s{j}")
                        made.append((t, prec, f"s{j}"))
                        vec = Gf.Vec3d if double else Gf.Vec3f

                        def value():
                            if t == "translate":
                                return vec(*[rng.uniform(-5, 5) for _ in range(3)])
                            if t == "scale":
                                return vec(*[rng.uniform(0.2, 3) * rng.choice([1, -1]) for _ in range(3)])
                            if t in ("rotateX", "rotateY", "rotateZ"):
                                return rng.uniform(-180, 180)
                            if t.startswith("rotate"):
                                return vec(*[rng.uniform(-180, 180) for _ in range(3)])
                            if t == "orient":
                                q = [rng.gauss(0, 1) for _ in range(4)]
                                n = math.sqrt(sum(a * a for a in q))
                                return (Gf.Quatd if double else Gf.Quatf)(*[a / n for a in q])
                            m = Gf.Matrix4d(1).SetRotate(Gf.Rotation(Gf.Vec3d(rng.random(), 1, rng.random()), rng.uniform(0, 360)))
                            return m.SetTranslateOnly(Gf.Vec3d(*[rng.uniform(-3, 3) for _ in range(3)]))

                        if rng.random() < 0.5:
                            for f in sorted(rng.sample(range(1, 20), rng.randint(1, 3))):
                                op.Set(value(), f)
                        else:
                            op.Set(value())
                    prims.append(p)
                    if depth < 2:
                        build(p, depth + 1)

            build("", 0)
            stage.GetRootLayer().Save()
            ours = pg.UsdStage(path)
            for p in prims:
                for t in (0, 3.5, 7.25, 19, 25):
                    want = np.array(UsdGeom.Xformable(stage.GetPrimAtPath(p)).ComputeLocalToWorldTransform(t))
                    got = ours.prim(p).world(t)
                    self.assertLess(np.abs(want - got).max() / max(1.0, np.abs(want).max()), 1e-5, f"{path} {p} {t}")

    def test_a_shot_of_several_files(self):
        fmt = "usdc"
        asset = Usd.Stage.CreateNew(self.path("tree." + fmt))
        root = UsdGeom.Xform.Define(asset, "/Tree")
        asset.SetDefaultPrim(root.GetPrim())
        root.AddTranslateOp().Set(Gf.Vec3d(0, 0, 0))
        UsdGeom.Cylinder.Define(asset, "/Tree/trunk").CreateHeightAttr(4)
        sets = root.GetPrim().GetVariantSets().AddVariantSet("lod")
        for v in ("high", "low"):
            sets.AddVariant(v)
            sets.SetVariantSelection(v)
            with sets.GetVariantEditContext():
                crown = (UsdGeom.Sphere if v == "high" else UsdGeom.Cube).Define(asset, "/Tree/crown")
                UsdGeom.Xformable(crown).AddTranslateOp().Set(Gf.Vec3d(0, 0, 4 if v == "high" else 3.5))
        sets.SetVariantSelection("high")
        asset.CreateClassPrim("/_leaf").CreateAttribute("leafiness", Sdf.ValueTypeNames.Float).Set(0.5)
        asset.DefinePrim("/Tree/leaf", "Xform").GetInherits().AddInherit("/_leaf")
        asset.DefinePrim("/Tree/copy").GetReferences().AddInternalReference("/Tree/crown")
        spin = UsdGeom.Xform.Define(asset, "/Tree/spin").AddRotateZOp()
        for f in (0, 10):
            spin.Set(f * 9.0, f)
        asset.GetRootLayer().Save()
        anim = Usd.Stage.CreateNew(self.path("anim.usda"))
        anim.SetMetadata("timeCodesPerSecond", 48)
        wob = anim.OverridePrim("/World/tree1").CreateAttribute("wobble", Sdf.ValueTypeNames.Double)
        for f in (0, 20):
            wob.Set(float(f), f)
        anim.GetRootLayer().Save()
        shot = Usd.Stage.CreateNew(self.path("shot.usda"))
        shot.GetRootLayer().subLayerPaths.append("./anim.usda")
        shot.GetRootLayer().subLayerOffsets[0] = Sdf.LayerOffset(1000, 2)
        world = UsdGeom.Xform.Define(shot, "/World")
        for i, (lod, offset) in enumerate([("high", Sdf.LayerOffset()), ("low", Sdf.LayerOffset(1000, 1)),
                                           (None, Sdf.LayerOffset(1005, 0.5))]):
            t = shot.DefinePrim(f"/World/tree{i + 1}", "Xform")
            t.GetReferences().AddReference("./tree." + fmt, Sdf.Path(), offset)
            t.GetAttribute("xformOp:translate").Set(Gf.Vec3d(i * 5.0, 0, 0))
            if lod:
                t.GetVariantSets().GetVariantSet("lod").SetVariantSelection(lod)
            t.SetInstanceable(i == 2)
        shot.OverridePrim("/World/tree2/leaf").CreateAttribute("leafiness", Sdf.ValueTypeNames.Float).Set(0.9)
        shot.DefinePrim("/World/special").GetSpecializes().AddSpecialize("/World/tree1/leaf")
        shot.DefinePrim("/World/payload", "Xform").GetPayloads().AddPayload("./tree." + fmt, "/Tree/crown", Sdf.LayerOffset(3))
        shot.GetRootLayer().Save()

        theirs = Usd.Stage.Open(self.path("shot.usda"))
        ours = pg.UsdStage(self.path("shot.usda"))
        want = {str(p.GetPath()): p for p in theirs.Traverse(Usd.TraverseInstanceProxies(Usd.PrimAllPrimsPredicate))}
        got = {p.path: p for p in ours.prims(all=True)}
        self.assertEqual(set(want) - {"/_leaf"}, set(got) - {"/_leaf"})
        for path, p in want.items():
            self.assertEqual(p.GetTypeName(), got[path].type, path)
            for attr in p.GetAttributes():
                for t in (0, 5, 1003, 1004.5, 1010):
                    if attr.GetResolveInfo(t).GetSource() in (Usd.ResolveInfoSourceFallback, Usd.ResolveInfoSourceNone):
                        continue
                    a, b = attr.Get(t), got[path].get(attr.GetName(), t)
                    if a is None or isinstance(a, bool):
                        continue
                    try:
                        want_ = np.asarray(a, dtype=float).ravel()
                    except (TypeError, ValueError):  # tokens, strings
                        self.assertEqual([str(x) for x in (a if not isinstance(a, str) else [a])],
                                         [str(x) for x in (b if not isinstance(b, str) else [b])], f"{path}.{attr.GetName()}")
                        continue
                    self.assertTrue(np.allclose(want_, np.asarray(b, dtype=float).ravel()),
                                    f"{path}.{attr.GetName()} at {t}: {a} vs {b}")
            if p.IsA(UsdGeom.Xformable):
                for t in (0, 1003):
                    m = np.array(UsdGeom.Xformable(p).ComputeLocalToWorldTransform(t))
                    self.assertTrue(np.allclose(m, got[path].world(t), atol=1e-6), f"{path} at {t}")

    def test_geometry_as_usd_places_it(self):
        stage = Usd.Stage.CreateNew(self.path("geo.usdc"))
        UsdGeom.SetStageUpAxis(stage, "Z")
        UsdGeom.SetStageMetersPerUnit(stage, 0.01)
        w = UsdGeom.Xform.Define(stage, "/W")
        w.AddTranslateOp().Set(Gf.Vec3d(100, 200, 300))
        w.AddRotateXYZOp().Set(Gf.Vec3f(10, 20, 30))
        m = UsdGeom.Mesh.Define(stage, "/W/m")
        rng = np.random.default_rng(2)
        m.CreatePointsAttr(rng.uniform(-50, 50, (40, 3)).astype(np.float32))
        m.CreateFaceVertexCountsAttr([3] * 20)
        m.CreateFaceVertexIndicesAttr(rng.integers(0, 40, 60).astype(np.int32))
        stage.GetRootLayer().Save()
        ours = pg.UsdStage(self.path("geo.usdc"))
        geo = ours.geometry()
        world = np.array(UsdGeom.Xformable(stage.GetPrimAtPath("/W/m")).ComputeLocalToWorldTransform(0))
        P = np.array(m.GetPointsAttr().Get(), dtype=float)
        P = (np.c_[P, np.ones(len(P))] @ world)[:, :3] @ (np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]]) * 0.01)
        self.assertTrue(np.allclose(np.asarray(geo.P), P, atol=1e-5))

    def test_value_clips_as_usd_reads_them(self):
        clips = []
        for f in (1, 2, 3):
            s = Usd.Stage.CreateNew(self.path(f"c{f}.usda"))
            p = UsdGeom.Points.Define(s, "/p")
            p.GetPointsAttr().Set([(f * 10.0, 0, 0)], f)
            s.GetRootLayer().Save()
            clips.append(f"./c{f}.usda")
        man = Usd.Stage.CreateNew(self.path("man.usda"))
        UsdGeom.Points.Define(man, "/p").CreatePointsAttr()
        man.GetRootLayer().Save()
        st = Usd.Stage.CreateNew(self.path("stage.usda"))
        c = Usd.ClipsAPI(st.DefinePrim("/p", "Points"))
        c.SetClipAssetPaths([Sdf.AssetPath(p) for p in clips])
        c.SetClipPrimPath("/p")
        c.SetClipActive([(1, 0), (2, 1), (3, 2)])
        c.SetClipTimes([(1, 1), (2, 2), (3, 3)])
        c.SetClipManifestAssetPath("./man.usda")
        st.GetRootLayer().Save()
        opened = Usd.Stage.Open(self.path("stage.usda"))
        theirs = opened.GetPrimAtPath("/p").GetAttribute("points")
        ours = pg.UsdStage(self.path("stage.usda")).prim("/p")
        for t in (0.5, 1, 1.25, 1.5, 2, 2.75, 3, 3.5):
            self.assertAlmostEqual(theirs.Get(t)[0][0], ours.get("points", t)[0][0], places=4, msg=f"at {t}")


    def test_value_clips_wherever_a_shot_names_them(self):
        """Random shots whose clips are named in a sublayer between a
        stronger and a weaker one: several clips, time mappings with jumps,
        manifests with and without defaults, missing values interpolated,
        templates, layer offsets -- on the prim and, through it, on its
        child. Every value at every quarter frame, the time samples and
        whether it varies, as USD has them."""
        rng = random.Random(11)
        times = [x / 4 for x in range(-24, 60)]
        for k in range(80):
            folder = self.path(str(k))
            os.makedirs(folder)
            _clip_shot(rng, folder)
            shot = os.path.join(folder, "shot.usda")
            theirs, ours = Usd.Stage.Open(shot), pg.UsdStage(shot)
            for path, names in (("/M", "abc"), ("/M/kid", "k")):
                for n in names:
                    a, p = theirs.GetPrimAtPath(path).GetAttribute(n), ours.prim(path)
                    where = f"shot {k}: {path}.{n}"
                    for t in times:
                        u, v = a.Get(t), p.get(n, t)
                        if u is None or v is None:
                            self.assertEqual(u, v, f"{where} at {t}")
                        else:
                            self.assertAlmostEqual(u, v, delta=1e-9 * max(1.0, abs(u)), msg=f"{where} at {t}")
                    self.assertEqual(len(a.GetTimeSamples()), len(p.sample_times(n)), where)
                    for u, v in zip(a.GetTimeSamples(), p.sample_times(n)):
                        self.assertAlmostEqual(u, v, places=9, msg=where)
                    self.assertEqual(a.ValueMightBeTimeVarying(), p.varies(n), where)


def _clip_shot(rng, folder):
    """A shot for the clip test: fx.usda names clips of /M between
    strong.usda (above it) and weak.usda (below). Their mappings reach every
    time asked about and every sample in them, and turn back only by jumps
    back -- where USD itself would read an empty optional otherwise."""
    def write(name, lines):
        with open(os.path.join(folder, name), "w") as f:
            f.write("\n".join(lines) + "\n")

    def samples(stamps):
        return ", ".join(f"{t!r}: {'None' if rng.random() < 0.08 else repr(rng.uniform(-50, 50))}" for t in stamps)

    template = rng.random() < 0.25
    count = rng.randint(1, 4)
    names = []
    for i in range(count):
        if template:
            # A frame a file, as a template's clips are: samples from its frame.
            frame = i + 1
            stamps = sorted({float(frame)} | {frame + rng.choice([0.25, 0.5]) for _ in range(rng.randint(0, 1))})
            name = f"clip.{frame:03d}.usda"
        else:
            stamps = sorted({round(rng.uniform(-3, 12), 1) for _ in range(rng.randint(1, 4))})
            name = f"clip{i}.usda"
        lines = ["#usda 1.0", 'over "Root"', "{"]
        for a in "abc":
            if rng.random() < 0.75:
                lines.append(f"    double {a}.timeSamples = {{ {samples(stamps)} }}")
        lines += ['    def "kid"', "    {", f"        double k.timeSamples = {{ {samples(stamps)} }}", "    }", "}"]
        write(name, lines)
        names.append(name)
    entries = ['string primPath = "/Root"']
    if template:
        entries += ['string templateAssetPath = "./clip.###.usda"', "double templateStartTime = 1",
                    f"double templateEndTime = {count + rng.randint(0, 1)}", "double templateStride = 1"]
        if rng.random() < 0.4:
            entries.append(f"double templateActiveOffset = {rng.choice([0.5, -0.5, 1.0])!r}")
    else:
        entries.append("asset[] assetPaths = [" + ", ".join(f"@./{n}@" for n in names) + "]")
        starts = sorted({round(rng.uniform(-2, 10), 1) for _ in range(count + 2)})[:count]
        order = list(range(count))
        rng.shuffle(order)
        entries.append("double2[] active = [" + ", ".join(f"({s!r}, {i})" for s, i in zip(starts, order)) + "]")
        if rng.random() < 0.7:
            stage, clip, knots = -20.0, rng.uniform(-8, -4), []
            while stage < 30 or clip < 13:
                knots.append((round(stage, 2), round(clip, 2)))
                if rng.random() < 0.2:
                    clip -= rng.uniform(0.5, 3)  # a jump back
                    knots.append((round(stage, 2), round(clip, 2)))
                stage += rng.uniform(2, 9)
                clip += rng.choice([0.0, rng.uniform(0.5, 4)])
            knots.append((round(stage, 2), round(clip, 2)))
            entries.append("double2[] times = [" + ", ".join(f"({s!r}, {c!r})" for s, c in knots) + "]")
    if rng.random() < 0.6:
        lines = ["#usda 1.0", 'over "Root"', "{"]
        for a in "abc":
            r = rng.random()
            if r < 0.4:
                lines.append(f"    double {a} = {rng.uniform(-5, 5)!r}" if r < 0.2 else f"    double {a}")
            elif r < 0.5:
                lines.append(f"    uniform double {a}")
            elif r < 0.9:
                lines.append(f"    double {a}")
        lines += ['    def "kid"', "    {", "        double k", "    }", "}"]
        write("manifest.usda", lines)
        entries.append("asset manifestAssetPath = @./manifest.usda@")
    if rng.random() < 0.2:
        entries.append("bool interpolateMissingClipValues = 1")
    write("fx.usda", ["#usda 1.0", 'over "M" (', "    clips = {", "        dictionary default = {"] +
          ["            " + e for e in entries] + ["        }", "    }", ")", "{", "}"])
    strong = ["#usda 1.0", 'over "M"', "{"]
    if rng.random() < 0.3:
        strong.append(f"    double b = {rng.uniform(-9, 9)!r}")
    strong += ['    over "kid"', "    {"]
    if rng.random() < 0.3:
        strong.append(f"        double k = {rng.uniform(-9, 9)!r}")
    write("strong.usda", strong + ["    }", "}"])
    weak = ["#usda 1.0", 'def Xform "M"', "{"]
    weak += [f"    double {a} = {rng.uniform(-9, 9)!r}" for a in "abc" if rng.random() < 0.5]
    weak += ['    def "kid"', "    {", f"        double k = {rng.uniform(-9, 9)!r}", "    }", "}"]
    write("weak.usda", weak)
    offset = rng.choice(["", " (offset = 2)", " (offset = -1.5; scale = 2)"])
    write("shot.usda", ["#usda 1.0", "(", "    subLayers = [", "        @./strong.usda@,",
                        f"        @./fx.usda@{offset},", "        @./weak.usda@", "    ]", ")"])


if __name__ == "__main__":
    unittest.main()
