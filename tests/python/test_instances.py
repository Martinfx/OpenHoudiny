"""Instances from Python: a meadow's points and the clumps of grass they
stand for, made copies of as the Unpack node makes them, written as the
writers write them -- and, where the USD library is installed (pip install
usd-core), read back by it: its PointInstancer places each clump where the
program's own copy of it is.

    PYTHONPATH=build/python python3 -m unittest discover -s tests/python
"""

import os
import tempfile
import unittest

import pg

try:
    import numpy as np
except ImportError:
    np = None

try:
    from pxr import Usd, UsdGeom
except ImportError:
    Usd = None


def meadow():
    """Clumps of grass over a 2 m square: 3 kinds, 3 blades each."""
    net = pg.Network()
    ground = net.add("grid", sizex=2, sizez=2, rows=5, cols=5)
    grass = net.add("grass", density=4, variants=3, blades=3, seed=2)
    net.connect(ground, grass)
    return grass.geometry()


@unittest.skipIf(np is None, "numpy is not installed")
class Instances(unittest.TestCase):
    def test_grass_is_points_that_stand_for_clumps(self):
        geo = meadow()
        self.assertGreater(geo.point_count, 0)
        self.assertEqual(geo.instance_count, geo.point_count)
        self.assertEqual(len(geo.prototypes), 3)
        self.assertIn("instances of 3 prototypes", repr(geo))
        which = np.asarray(geo.points["instance"])
        self.assertTrue(np.all((which >= 0) & (which < 3)))
        # Unpacked: a copy of its clump on each point, nothing standing for anything.
        flat = geo.unpack()
        self.assertEqual(flat.instance_count, 0)
        self.assertEqual(flat.prototypes, [])
        prims = [c.primitive_count for c in geo.prototypes]
        self.assertEqual(flat.primitive_count, sum(prims[int(k)] for k in which))

    def test_instances_built_in_python(self):
        blade = pg.Geometry()
        blade.add_points(np.array([[0, 0, 0], [0.1, 0, 0], [0, 1, 0]], dtype=np.float32))
        blade.add_polygons([3], [0, 1, 2])
        field = pg.Geometry()
        field.add_points(np.array([[0, 0, 0], [5, 0, 0], [9, 9, 9]], dtype=np.float32))
        self.assertEqual(field.add_prototype(blade), 0)
        field.points["instance"] = [0, 0, -1]  # the last is a point, standing for nothing
        field.points["pscale"] = np.array([1.0, 2.0, 1.0], dtype=np.float32)
        self.assertEqual(field.instance_count, 2)
        flat = field.unpack()
        # The loose point first, as it is; then the copies, placed and sized.
        self.assertEqual(flat.point_count, 7)
        self.assertEqual(flat.primitive_count, 2)
        P = np.asarray(flat.P)
        self.assertTrue(np.allclose(P[0], [9, 9, 9]))
        self.assertTrue(np.allclose(P[-1], [5, 2, 0]))
        field.clear_prototypes()
        self.assertEqual(field.instance_count, 0)

    def test_writers_make_copies(self):
        geo = meadow()
        with tempfile.TemporaryDirectory() as tmp:
            obj = geo.save(os.path.join(tmp, "meadow.obj"))
            back = pg.Geometry.load(obj)
            flat = geo.unpack()
            self.assertEqual(back.point_count, flat.point_count)
            self.assertEqual(back.primitive_count, flat.primitive_count)
            self.assertTrue(np.allclose(np.asarray(back.P), np.asarray(flat.P), atol=1e-5))

    @unittest.skipIf(Usd is None, "the USD library is not installed")
    def test_usd_places_the_clumps_where_the_copies_are(self):
        geo = meadow()
        flat = geo.unpack()
        with tempfile.TemporaryDirectory() as tmp:
            stage = Usd.Stage.Open(geo.save(os.path.join(tmp, "meadow.usda")))
            instancer = UsdGeom.PointInstancer(stage.GetPrimAtPath("/meadow/instances"))
            self.assertTrue(instancer)
            prototypes = instancer.GetPrototypesRel().GetTargets()
            self.assertEqual(len(prototypes), 3)
            indices = list(instancer.GetProtoIndicesAttr().Get())
            self.assertEqual(indices, [int(k) for k in geo.points["instance"]])
            places = instancer.ComputeInstanceTransformsAtTime(Usd.TimeCode.Default(), Usd.TimeCode.Default())
            # The corners of each clump's faces as USD places them -- clump by
            # clump, in the points' order, as Unpack makes the copies. (A Mesh
            # numbers its points as its faces first use them.)
            placed = []
            for k, target in enumerate(prototypes):
                mesh = UsdGeom.Mesh(stage.GetPrimAtPath(str(target) + "/mesh"))
                points = mesh.GetPointsAttr().Get()
                corners = [points[c] for c in mesh.GetFaceVertexIndicesAttr().Get()]
                for i, which in enumerate(indices):
                    if which == k:
                        placed.extend(places[i].Transform(p) for p in corners)
            _, corners = flat.topology()
            # Its orientations are halves: less than a millimetre off on a 40 cm blade.
            self.assertTrue(np.allclose(np.asarray(placed), np.asarray(flat.P)[corners], atol=1e-3))


if __name__ == "__main__":
    unittest.main()
