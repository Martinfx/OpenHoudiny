"""The Python module pg, as a script uses it: networks built and read back,
geometry as numpy arrays over the core's memory, geometry built in Python,
simulations stepped and read back from a cache, the shot to USD, the
network as code that builds it again, pictures through prototype.

    ctest -R python    # or: PYTHONPATH=build/python python3 -m unittest discover -s tests/python
"""

import math
import os
import shutil
import tempfile
import unittest

import pg

try:
    import numpy as np
except ImportError:
    np = None

needs_numpy = unittest.skipIf(np is None, "numpy is not installed")


class Networks(unittest.TestCase):
    def test_node_types_and_examples(self):
        names = {t["name"] for t in pg.node_types()}
        for name in ("box", "voronoi_fracture", "rbd_solver", "liquid_surface", "output"):
            self.assertIn(name, names)
        box = pg.node_type("box")
        self.assertEqual(box["category"], "Geometry")
        self.assertIn("size", [p["name"] for p in box["params"]])
        self.assertIn("demolition", pg.examples())
        with self.assertRaises(pg.Error):
            pg.node_type("no_such_node")

    def test_parameters_of_every_kind(self):
        net = pg.Network()
        box = net.add("box", size=(2, 3, 4), center=(0, 1.5, 0))
        self.assertEqual(box.name, "box1")
        self.assertEqual(box.type, "box")
        self.assertEqual(box["size"], (2.0, 3.0, 4.0))
        box["divisions"] = 3
        self.assertEqual(box["divisions"], 3)
        box["size"] = "1 1 1"  # as files write it
        self.assertEqual(box["size"], (1.0, 1.0, 1.0))
        clip = net.add("clip", "cutter", keep="below", cap=False)
        self.assertEqual(clip.name, "cutter")
        self.assertEqual(clip["keep"], "below")  # a choice by its name
        self.assertIs(clip["cap"], False)
        clip["capgroup"] = "cut_faces"  # text
        self.assertEqual(clip["capgroup"], "cut_faces")
        wrangle = net.add("point_wrangle", snippet="@P.y += 1;")
        self.assertEqual(wrangle["snippet"], "@P.y += 1;")
        with self.assertRaises(pg.Error):
            box["no_such_param"] = 1
        with self.assertRaises(pg.Error):
            net.add("no_such_type")
        with self.assertRaises(pg.Error):
            clip["keep"] = "sideways"
        info = box.param("size")
        self.assertEqual(info["kind"], "vector")
        self.assertEqual(info["unit"], "m")
        box.reset("size")
        self.assertEqual(box["size"], (1.0, 1.0, 1.0))

    def test_links_flags_and_problems(self):
        net = pg.Network()
        box = net.add("box")
        pieces = net.add("voronoi_fracture", count=5)
        self.assertIs(box.connect(pieces), pieces)  # the first pins that fit
        (a, o, b, i), = net.links()
        self.assertEqual((a, o, b, i), (box, "geometry", pieces, "geometry"))
        pieces.display()
        self.assertEqual(net.displayed, pieces)
        pieces.bypass = True
        self.assertTrue(pieces.bypass)
        pieces.bypass = False
        self.assertEqual(net.disconnect(box, pieces), 1)
        self.assertEqual(net.links(), [])
        with self.assertRaises(pg.Error):
            net.connect(pieces, net.add("output"))  # geometry does not go into an Output
        # Geometry shown and nothing simulated: a model, nothing wrong with it --
        # and nothing to simulate.
        levels = [level for (level, node, message) in net.problems()]
        self.assertNotIn("error", levels)
        with self.assertRaises(pg.Error):
            net.simulate()
        net.add("pyro_solver")  # a simulation, which lacks what it needs
        levels = [level for (level, node, message) in net.problems()]
        self.assertIn("error", levels)

    def test_expressions_and_keys(self):
        net = pg.Network()
        box = net.add("box")
        box.expression("size.y", "$F * 0.5")
        self.assertEqual(box.expression("size.y"), "$F * 0.5")
        self.assertAlmostEqual(box.value("size", 4)[1], 2.0)
        with self.assertRaises(pg.Error):
            box.expression("size.w", "1")
        box.key("center", 1, (0, 0, 0)).key("center", 11, (0, 5, 0), "linear")
        self.assertEqual(len(box.keys("center")), 2)
        self.assertAlmostEqual(box.value("center", 6)[1], 2.5, places=4)
        box.clear_keys("center")
        self.assertEqual(box.keys("center"), [])

    def test_saved_and_read_back_the_same(self):
        net = pg.Network.example("rain_pond")
        again = pg.Network.from_text(net.text())
        self.assertEqual(again.text(), net.text())
        folder = tempfile.mkdtemp()
        try:
            path = net.save(os.path.join(folder, "pond.pgsim"))
            loaded = pg.Network.load(path)
            self.assertEqual(loaded.text(), net.text())
            self.assertEqual(loaded.folder, folder)
            self.assertEqual([n.name for n in loaded], [n.name for n in net])
        finally:
            shutil.rmtree(folder)

    def test_as_code_builds_the_network_again(self):
        # Every example: the code as_code() writes makes a network whose file
        # is the example's, node for node, link for link.
        for name in pg.examples():
            net = pg.Network.example(name)
            code = net.as_code()
            scope = {"pg": pg}
            exec(compile(code, name, "exec"), scope)
            again = scope["build"]()
            self.assertEqual(normalized(again), normalized(net), name)


class Examples(unittest.TestCase):
    def test_the_demolition_built_from_python_is_the_example(self):
        # examples/python/demolition.py builds the scene of step 2 node by
        # node: the same network as examples/sim/demolition.pgsim.
        here = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(here, "..", "..", "examples", "python", "demolition.py")
        scope = {"__name__": "demolition"}
        with open(path, encoding="utf-8") as f:
            exec(compile(f.read(), path, "exec"), scope)
        built = scope["build"]()
        self.assertEqual(normalized(built), normalized(pg.Network.example("demolition")))
        self.assertEqual([p for p in built.problems() if p[0] == "error"], [])


def normalized(net):
    """A network as its nodes (by name) and links (by names) say it: what
    does not depend on the order ids were given in."""
    links = sorted((a.name, o, b.name, i) for (a, o, b, i) in net.links())
    params = sorted((n.name, n.type, p, repr(n[p]), tuple(n.keys(p)),
                     tuple(n.expression(c) for c in net._net.channels(n.id, p)))
                    for n in net for p in n.params())
    flags = sorted((n.name, n.bypass, n.position) for n in net)
    return links, params, flags, net.displayed.name if net.displayed else None


@needs_numpy
class Geometry(unittest.TestCase):
    def test_cooked_geometry_is_numpy_without_a_copy(self):
        net = pg.Network()
        box = net.add("box", size=(2, 4, 6), divisions=2)
        geo = box.geometry()
        self.assertEqual(geo.primitive_count, 24)
        P = geo.points["P"]
        self.assertEqual(P.shape, (geo.point_count, 3))
        self.assertEqual(P.dtype, np.float32)
        lo, hi = geo.bounds()
        self.assertEqual((lo, hi), ((-1.0, -1.5, -3.0), (1.0, 2.5, 3.0)))
        # The core's memory: read again, the same bytes; not to be written.
        again = geo.points["P"]
        self.assertEqual(P.__array_interface__["data"][0], again.__array_interface__["data"][0])
        self.assertFalse(P.flags.writeable)
        with self.assertRaises(ValueError):
            P[0, 0] = 5.0
        # Cooked again, from the cache: the same memory still.
        cooked = box.geometry()
        self.assertEqual(cooked.P.__array_interface__["data"][0], P.__array_interface__["data"][0])
        sizes, points = geo.topology()
        self.assertEqual(int(sizes.sum()), geo.vertex_count)
        self.assertEqual(len(points), geo.vertex_count)
        self.assertEqual(list(geo.primitive(0)), [int(x) for x in points[: sizes[0]]])
        self.assertTrue(np.all(geo.closed() == 1))

    def test_changing_a_copy_leaves_the_cook_alone(self):
        net = pg.Network()
        box = net.add("box")
        geo = box.geometry()
        before = geo.P.copy()
        geo.points["Cd"] = np.tile([1.0, 0.0, 0.0], (geo.point_count, 1))
        moved = geo.P + 1.0
        geo.points["P"] = moved
        self.assertTrue(np.allclose(geo.P, moved))
        self.assertEqual(geo.points.type("Cd"), "vector3")
        fresh = box.geometry()
        self.assertTrue(np.array_equal(fresh.P, before))
        self.assertNotIn("Cd", fresh.points)

    def test_geometry_built_in_python_goes_to_files_and_back(self):
        geo = pg.Geometry()
        corners = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [2, 0, 0]], dtype=np.float64)
        self.assertEqual(geo.add_points(corners), 0)
        geo.add_polygons([4], [0, 1, 2, 3])
        geo.add_polylines([2], [1, 4])
        geo.points["pscale"] = np.linspace(0.1, 0.5, 5, dtype=np.float32)
        geo.points["id"] = [10, 11, 12, 13, 14]
        geo.prims["name"] = ["face", "edge"]
        geo.detail["note"] = "made in Python"
        self.assertEqual(geo.points.type("id"), "int")
        self.assertEqual(geo.points.type("pscale"), "float")
        self.assertEqual(geo.prims["name"], ["face", "edge"])
        self.assertEqual(geo.detail["note"], ["made in Python"])
        self.assertEqual(list(geo.closed()), [1, 0])
        with self.assertRaises(pg.Error):
            geo.points["pscale"] = [1.0, 2.0]  # five points, not two
        with self.assertRaises(pg.Error):
            geo.add_polygons([3], [0, 1, 9])  # no point 9
        geo.set_group("left", "point", [1, 0, 0, 1, 0])
        cls, members = geo.group("left")
        self.assertEqual((cls, list(members)), ("point", [1, 0, 0, 1, 0]))
        folder = tempfile.mkdtemp()
        try:
            geo.save(os.path.join(folder, "made.ply"))
            back = pg.Geometry.load(os.path.join(folder, "made.ply"))
            self.assertTrue(np.allclose(back.P, corners))
            self.assertTrue(np.allclose(back.points["pscale"], geo.points["pscale"]))
            geo.save(os.path.join(folder, "made.obj"))
            self.assertEqual(pg.Geometry.load(os.path.join(folder, "made.obj")).primitive_count, 2)
        finally:
            shutil.rmtree(folder)

    def test_volumes_are_indexed_as_their_voxels(self):
        geo = pg.Geometry()
        values = np.zeros((4, 3, 2), dtype=np.float32)
        values[3, 1, 0] = 7.0
        geo.add_volume("density", values, origin=(1, 0, 0), voxel=0.5)
        v = geo.volume("density")
        self.assertEqual(v.resolution, (4, 3, 2))
        self.assertEqual(v.values.shape, (4, 3, 2))
        self.assertEqual(v.values[3, 1, 0], 7.0)
        self.assertEqual(float(v.values.sum()), 7.0)


@needs_numpy
class Simulations(unittest.TestCase):
    def pond(self):
        net = pg.Network.example("rain_pond")
        net["solver"]["resolution"] = 16
        return net

    def test_a_simulation_steps_frame_by_frame(self):
        net = self.pond()
        sim = net.simulate()
        self.assertEqual(sim.frame, 0)
        self.assertEqual(sim.frames, 90)
        self.assertAlmostEqual(sim.fps, 30.0, places=3)
        frames = list(sim.run(3))
        self.assertEqual([f.number for f in frames], [1, 2, 3])
        f = sim.current
        self.assertTrue(f.has_water and f.has_rain)
        w = f.water
        self.assertEqual(w.positions.shape, (w.particle_count, 3) if len(w.positions) else (0, 3))
        self.assertGreater(w.litres, 100.0)
        flow = w.flow
        self.assertEqual(flow.shape[3], 3)
        self.assertEqual(flow.dtype, np.float16)
        rain = f.rain
        self.assertEqual(rain.positions.shape[1], 3)
        self.assertEqual(len(rain.ids), len(rain.positions))
        self.assertTrue(np.all(rain.velocities[:, 1] < 0.5))  # falling, if slanting
        surface = w.surface()
        self.assertGreater(surface.primitive_count, 100)
        self.assertEqual(sorted(surface.points), ["N", "P", "foam", "v"])
        # The camera of the shot, at this frame.
        self.assertEqual(sim.camera()["focal"], 32.0)

    def test_a_checkpoint_goes_on_to_the_bit_and_a_preview_is_coarser(self):
        net = pg.Network.example("campfire")
        straight = net.simulate(preview=0.5)
        list(straight.run(8))
        state = straight.save_state()
        self.assertIsInstance(state, bytes)
        resumed = net.simulate(preview=0.5)
        resumed.load_state(state)
        self.assertEqual(resumed.frame, 8)
        for _ in range(4):
            a, b = straight.step(), resumed.step()
            self.assertEqual(a.number, b.number)
            self.assertTrue(np.array_equal(a.gas(), b.gas()))
        # Another grid does not take it.
        with self.assertRaises(pg.Error):
            net.simulate().load_state(state)
        self.assertLess(straight.current.gas().size, net.simulate().step().gas().size)

    def test_frames_to_a_cache_and_back(self):
        net = self.pond()
        folder = tempfile.mkdtemp()
        try:
            sim = net.simulate()
            sim.cache(folder, frames=2)
            self.assertEqual(pg._pg.read_cache_info(folder)["frames"], 2)
            back = net.simulate(cache=folder)
            self.assertEqual(back.frames, 2)
            frames = list(back.run())
            self.assertEqual(len(frames), 2)
            self.assertTrue(np.array_equal(frames[-1].rain.ids, sim.current.rain.ids))
            self.assertTrue(np.array_equal(frames[-1].water.foam, sim.current.water.foam))
            with self.assertRaises(pg.Error):
                back.step()  # the cache has no third frame
        finally:
            shutil.rmtree(folder)

    def test_rigid_bodies_and_the_geometry_of_a_frame(self):
        net = pg.Network()
        box = net.add("box", size=(1, 1, 1), center=(0, 2, 0))
        pieces = net.add("voronoi_fracture", count=6)
        rbd = net.add("rbd_solver", glue=0)
        out = net.add("output", frames=20)
        box.connect(pieces).connect(rbd)
        net.connect(rbd, out)  # its Look
        back = net.add("rbd_pieces")
        net.connect(rbd, back)
        back.display()
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        sim = net.simulate()
        for f in sim.run(15):
            pass
        rigid = sim.current.rigid
        self.assertEqual(rigid.body_count, 6)
        self.assertEqual(rigid.centres.shape, (6, 3))
        self.assertTrue(np.all(rigid.centres[:, 1] < 1.9))  # fallen
        self.assertTrue(np.all(rigid.centres[:, 1] > 0.0))  # onto the floor, not through it
        # A point at rest, turned and moved by its body's pose, is where the
        # pieces are now: p' = rotate(q, p) + translation.
        rest = pieces.geometry()
        posed = rigid.pieces()
        body = rest.prims["piece"]
        q, t = rigid.rotations, rigid.translations
        for prim in range(0, rest.primitive_count, 7):
            b = int(body[prim])
            for point in rest.primitive(prim):
                u, w = q[b, :3].astype(np.float64), float(q[b, 3])
                p = rest.P[point].astype(np.float64)
                turned = p + 2.0 * w * np.cross(u, p) + 2.0 * np.cross(u, np.cross(u, p))
                self.assertTrue(np.allclose(turned + t[b], posed.P[point], atol=1e-4))
        norms = np.linalg.norm(rigid.rotations, axis=1)
        self.assertTrue(np.allclose(norms, 1.0, atol=1e-4))
        shown = sim.geometry()  # RBD Pieces at this frame
        self.assertEqual(shown.point_count, rigid.pieces().point_count)
        self.assertIn("v", shown.points)

    @needs_numpy
    def test_the_cloth_of_a_frame(self):
        net = pg.Network()
        grid = net.add("grid", sizex=1, sizez=1, rows=11, cols=11)
        raised = net.add("transform", t=(0, 1, 0))
        cloth = net.add("cloth_solver")
        out = net.add("output", frames=20)
        grid.connect(raised).connect(cloth)
        net.connect(cloth, out)  # its Look
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        sim = net.simulate()
        for f in sim.run(10):
            pass
        sheet = sim.current.cloth()
        self.assertEqual(sheet.point_count, 121)
        self.assertEqual(sheet.primitive_count, 100)
        self.assertIn("v", sheet.points)
        self.assertIn("N", sheet.points)
        self.assertTrue(np.all(sheet.P[:, 1] < 1.0))  # it fell
        self.assertTrue(np.all(sheet.points["v"][:, 1] < 0.0))

    @needs_numpy
    def test_concrete_breaks_rough_over_a_plain_proxy(self):
        net = pg.Network()
        box = net.add("box", size=(2, 1, 0.3), center=(0, 0.5, 0))
        concrete = net.add("concrete_fracture", count=12, chips=0.3, rough=0.02)
        rbd = net.add("rbd_solver", spread=0.3, rings=1)
        out = net.add("output", frames=10)
        box.connect(concrete).connect(rbd)
        net.connect(rbd, out)  # its Look
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        self.assertEqual(rbd["rings"], 1)
        self.assertAlmostEqual(rbd["spread"], 0.3, places=6)
        # The cracks rough, no further than Rough along each axis; the plain
        # cut under them in proxy; the spalls pieces of their own.
        geo = concrete.geometry()
        proxy = geo.points["proxy"]
        self.assertEqual(proxy.shape, geo.P.shape)
        moved = np.linalg.norm(geo.P - proxy, axis=1)
        self.assertGreater(moved.max(), 0.005)
        self.assertLessEqual(moved.max(), 0.02 * math.sqrt(3) + 1e-5)
        chips = geo.prims["chip"] == 1
        self.assertTrue(chips.any())
        self.assertGreater(len(set(geo.prims["piece"][chips].tolist())), 0)
        # Simulated as the proxy has them: glued where the plain cuts meet,
        # nothing knocks it -- it stands.
        sim = net.simulate()
        for f in sim.run(10):
            pass
        rigid = sim.current.rigid
        self.assertGreater(rigid.joints, 0)
        self.assertEqual(rigid.broken, 0)
        self.assertLess(float(np.abs(rigid.translations).max()), 0.01)

    @needs_numpy
    def test_rbd_cluster_groups_pieces_into_chunks(self):
        net = pg.Network()
        box = net.add("box", size=(3, 0.4, 0.4))
        pieces = net.add("voronoi_fracture", count=20)
        chunks = net.add("rbd_cluster", count=3, strength=12)
        box.connect(pieces).connect(chunks)
        geo = chunks.geometry()
        cluster, piece = geo.prims["cluster"], geo.prims["piece"]
        self.assertEqual(sorted(set(cluster.tolist())), [1, 2, 3])
        self.assertTrue(np.all(geo.prims["clusterglue"] == 12))
        for p in set(piece.tolist()):
            self.assertEqual(len(set(cluster[piece == p].tolist())), 1)  # a piece is in one chunk

    @needs_numpy
    def test_rebar_holds_a_beam_together(self):
        # A beam of pieces without glue across two supports: without bars
        # its middle falls through; with a cage of them it holds.
        def run(bars):
            net = pg.Network()
            beam = net.add("box", size=(2, 0.3, 0.3), center=(0, 1, 0))
            pieces = net.add("voronoi_fracture", count=8)
            rbd = net.add("rbd_solver", glue=0, substeps=4)
            out = net.add("output", frames=30)
            beam.connect(pieces)
            net.connect(pieces, rbd, input="pieces")
            for x in (-0.85, 0.85):
                net.connect(net.add("object", shape="box", center=(x, 0.425, 0), size=(0.3, 0.85, 0.5)), rbd,
                            input="colliders")
            if bars:
                cage = net.add("rebar")
                beam.connect(cage)
                net.connect(cage, rbd, input="rebar")
                self.assertEqual(cage.geometry().primitive_count, 8 + 11)  # bars round it, stirrups along it
            net.connect(rbd, out)
            self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
            sim = net.simulate()
            for f in sim.run(30):
                pass
            return sim.current.rigid
        loose = run(False)
        self.assertLess(float(loose.centres[:, 1].min()), 0.5)
        held = run(True)
        self.assertGreater(float(held.centres[:, 1].min()), 0.8)
        steel = held.rebar()
        self.assertGreater(steel.primitive_count, 0)
        self.assertTrue(np.allclose(steel.points["width"][steel.points["width"] > 0.01], 0.012))
        self.assertEqual(len(held.rebar_state), len(held.rebar_stations))

    def test_glass_breaks_as_glass(self):
        # A pane dropped flat: whole as it falls -- no crack drawn -- broken
        # where it lands, throwing chips of glass.
        net = pg.Network()
        pane = net.add("box", size=(0.5, 0.01, 0.5), center=(0, 0.4, 0))
        glass = net.add("glass_fracture", impact=(0.05, 0.4, 0), radials=6, rings=3)
        rbd = net.add("rbd_solver", density=2500, glue=30, substeps=4)
        out = net.add("output", frames=30)
        pane.connect(glass)
        net.connect(glass, rbd, input="pieces")
        net.connect(rbd, out)
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        shards = glass.geometry()
        kinds = shards.prims["glass"]
        self.assertEqual(sorted(set(kinds.tolist())), [1, 2])
        cracks = int((kinds == 2).sum())
        sim = net.simulate()
        frames = [f.rigid for f in sim.run(30)]
        falling, landed = frames[3], frames[-1]
        self.assertEqual(falling.unglued, [])
        self.assertEqual(int((falling.pieces().prims["glass"] == 2).sum()), 0)
        self.assertGreater(len(landed.unglued), 0)
        self.assertEqual(int((landed.pieces().prims["glass"] == 2).sum()), cracks)
        self.assertGreater(len(landed.grit), 0)
        self.assertEqual(len(landed.grit_glass), len(landed.grit))
        self.assertTrue((landed.grit_glass == 1).all())

    def test_brick_wall_is_laid_in_its_bond_and_stands_on_its_mortar(self):
        # A wall of bricks in Flemish bond, plastered at the back, a brick in
        # two cut in two: each brick a piece with its mortar, the halves of
        # one a cluster held Strength times as hard.
        net = pg.Network()
        wall = net.add("box", size=(2.08, 1.2, 0.265), center=(0, 0.6, 0))
        bricks = net.add("brick_wall", bond="flemish", plaster=0.015, plastersides="back", broken=0.5, strength=10,
                         plastercolor=(0.9, 0.1, 0.2))
        rbd = net.add("rbd_solver", glue=150, substeps=4)
        out = net.add("output", frames=10)
        wall.connect(bricks)
        net.connect(bricks, rbd, input="pieces")
        net.connect(rbd, out)
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        self.assertEqual(bricks["bond"], "flemish")
        geo = bricks.geometry()
        piece = geo.prims["piece"]
        self.assertGreater(len(set(piece.tolist())), 200)
        # The back -- z = -0.1325 -- is all plaster; the front none.
        P = geo.P
        cd = geo.prims["Cd"]
        back = [i for i in range(geo.primitive_count) if all(abs(P[p][2] + 0.1325) < 1e-5 for p in geo.primitive(i))]
        front = [i for i in range(geo.primitive_count) if all(abs(P[p][2] - 0.1325) < 1e-5 for p in geo.primitive(i))]
        self.assertGreater(len(back), 100)
        self.assertTrue(np.allclose(cd[back], (0.9, 0.1, 0.2)))
        self.assertFalse(np.isclose(cd[front], (0.9, 0.1, 0.2)).all(axis=1).any())
        cluster, glue = geo.prims["cluster"], geo.prims["clusterglue"]
        halves = {}
        for c, p in zip(cluster.tolist(), piece.tolist()):
            if c > 0:
                halves.setdefault(c, set()).add(p)
        self.assertGreater(len(halves), 20)
        self.assertTrue(all(len(h) == 2 for h in halves.values()))
        self.assertTrue((glue[cluster > 0] == 10).all())
        # Glued on its mortar, nothing knocks it: it stands.
        sim = net.simulate()
        for f in sim.run(10):
            pass
        rigid = sim.current.rigid
        self.assertGreater(rigid.joints, 0)
        self.assertEqual(rigid.broken, 0)
        self.assertLess(float(np.abs(rigid.translations).max()), 0.01)

    def test_the_glue_as_a_network(self):
        # A beam of pieces lying across a table's edge, its glue as a network:
        # as it is, the beam stands; its lines across the edge weakened to
        # nothing, the part over the edge tips off. The frame gives the network
        # back, and what became of each joint.
        def run(weaken):
            net = pg.Network()
            beam = net.add("box", size=(2, 0.1, 0.2), center=(0, 1.05, 0))
            pieces = net.add("voronoi_fracture", count=10, seed=3)
            glue = net.add("rbd_constraints")
            edge = net.add("primitive_wrangle", snippet=(
                'vector p0 = point(0, "P", primpoint(0, @primnum, 0));\n'
                'vector p1 = point(0, "P", primpoint(0, @primnum, 1));\n'
                'if ((p0.x - 0.2) * (p1.x - 0.2) < 0) f@strength *= %g;' % weaken))
            rbd = net.add("rbd_solver", glue=1000, floor=False)
            table = net.add("object", shape="box", center=(-0.7, 0.5, 0), size=(1.8, 1, 1))
            out = net.add("output", frames=40)
            back = net.add("rbd_pieces", output="constraints")
            beam.connect(pieces)
            net.connect(pieces, glue)
            glue.connect(edge)
            net.connect(pieces, rbd, input="pieces")
            net.connect(edge, rbd, input="constraints")
            net.connect(table, rbd, input="colliders")
            net.connect(rbd, out)
            net.connect(rbd, back)
            self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
            sim = net.simulate()
            for f in sim.run(40):
                pass
            return edge.geometry(), sim.current.rigid, sim.geometry(back)

        network, stood, joints = run(1.0)
        self.assertEqual(network.point_count, 10)
        self.assertGreater(network.primitive_count, 8)
        self.assertTrue((network.prims["strength"] == 1).all())
        self.assertTrue((network.prims["area"] > 0).all())
        self.assertGreater(float(stood.centres[:, 1].min()), 0.9)
        self.assertEqual(joints.primitive_count, stood.joints)
        self.assertEqual(int(joints.prims["broken"].sum()), 0)
        self.assertTrue((joints.prims["time"] == -1).all())
        network, fell, joints = run(0.0)
        across = int((network.prims["strength"] == 0).sum())
        self.assertGreater(across, 0)
        self.assertLess(float(fell.centres[:, 1].min()), 0.5)
        self.assertEqual(int((fell.joint_state == 2).sum()), across)  # they never held
        self.assertEqual(joints.primitive_count, network.primitive_count - across)
        self.assertEqual(fell.network().primitive_count, joints.primitive_count)

    @needs_numpy
    def test_grit_is_particles_that_lie_where_they_land(self):
        # A block blown to dust over a slab that does not move: the grit
        # lies on the slab or, flown past its edge, on the floor -- none in
        # the slab. Each bit is turned as it came to rest: a unit quaternion
        # x, y, z, w, in the frame and on RBD Pieces' grit points as orient.
        net = pg.Network()
        block = net.add("box", size=(0.5, 0.5, 0.5), center=(0, 1.6, 0))
        blow = net.add("primitive_wrangle", snippet="i@piece = 0; i@active = 1; f@release = 0.1; i@vanish = 1;")
        slab = net.add("box", size=(3, 0.2, 3), center=(0, 1, 0))
        still = net.add("primitive_wrangle", snippet="i@piece = 1; i@active = 0;")
        both = net.add("merge")
        rbd = net.add("rbd_solver", debris=4)
        out = net.add("output", frames=90)
        back = net.add("rbd_pieces", grit=True)
        block.connect(blow)
        slab.connect(still)
        net.connect(blow, both)
        net.connect(still, both)
        net.connect(both, rbd, input="pieces")
        net.connect(rbd, out)
        net.connect(rbd, back)
        self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
        sim = net.simulate()
        for f in sim.run(90):
            pass
        rigid = sim.current.rigid
        grit, turn = rigid.grit, rigid.grit_orient
        self.assertGreater(len(grit), 30)
        self.assertEqual(turn.shape, (len(grit), 4))
        self.assertTrue(np.allclose(np.linalg.norm(turn, axis=1), 1.0, atol=1e-4))
        x, y, z = grit[:, 0], grit[:, 1], grit[:, 2]
        over = (np.abs(x) < 1.5) & (np.abs(z) < 1.5)
        self.assertFalse(np.any(over & (y > 0.901) & (y < 1.099)))  # none in the slab
        self.assertGreater(int(np.sum(over & (y > 1.099))), 10)       # on it
        self.assertGreater(int(np.sum(y < grit[:, 3])), 0)             # on the floor
        points = sim.geometry(back)
        self.assertIn("orient", points.points)
        self.assertTrue(np.array_equal(points.points["orient"][-len(grit):], turn))

    def test_the_guide_leads_the_pieces_where_it_has_them(self):
        # A box on the floor and a Transform of it keyed from where it stands
        # to two metres up, turned a quarter round, over twenty frames: the
        # RBD Solver's Guide. Ten frames on the box is there -- the middle
        # of its box two metres up, turned as the guide turns it -- and stays;
        # with the guide done after a third of a second it flies on up as it
        # was led, and falls back to the floor.
        def run(until, frames):
            net = pg.Network()
            box = net.add("box", size=(1, 1, 1), center=(0, 0.5, 0))
            lift = net.add("transform")
            lift.key("t", 1, (0, 0, 0), "linear").key("t", 20, (0, 2, 0), "linear")
            lift.key("r", 1, (0, 0, 0), "linear").key("r", 20, (0, 90, 0), "linear")
            rbd = net.add("rbd_solver", guide_until=until)
            out = net.add("output", frames=frames)
            net.connect(box, rbd, input="pieces")
            box.connect(lift)
            net.connect(lift, rbd, input="guide")
            net.connect(rbd, out)
            self.assertEqual([p for p in net.problems() if p[0] == "error"], [])
            sim = net.simulate()
            for f in sim.run(frames):
                pass
            return sim.current.rigid
        led = run(0, 30)
        self.assertTrue(np.allclose(led.centres[0], (0, 2.5, 0), atol=0.02))
        quarter = np.array([0, np.sqrt(0.5), 0, np.sqrt(0.5)])
        self.assertGreater(abs(float(np.dot(led.rotations[0], quarter))), 0.999)
        dropped = run(1 / 3, 60)
        self.assertLess(abs(float(dropped.centres[0][1]) - 0.5), 0.02)

    def test_the_shot_to_usd(self):
        net = self.pond()
        folder = tempfile.mkdtemp()
        try:
            sim = net.simulate()
            path = sim.export_usd(os.path.join(folder, "pond.usda"), frames=3)
            self.assertTrue(os.path.exists(path))
            layer = os.path.join(folder, "pond_frames", "pond.0003.usda")
            self.assertTrue(os.path.exists(layer))
            try:
                from pxr import Usd, UsdGeom
            except ImportError:
                return
            stage = Usd.Stage.Open(path)
            water = UsdGeom.Mesh(stage.GetPrimAtPath("/World/water"))
            self.assertEqual(len(water.GetPointsAttr().Get(3)), sim.current.water.surface().point_count)
            drops = UsdGeom.Points(stage.GetPrimAtPath("/World/rain/drops"))
            self.assertEqual(list(drops.GetIdsAttr().Get(3)), [int(i) for i in sim.current.rain.ids])
        finally:
            shutil.rmtree(folder)


class Pictures(unittest.TestCase):
    def test_a_picture_through_prototype(self):
        try:
            pg.run("help")
        except (pg.Error, OSError) as e:
            self.skipTest(f"prototype does not run here: {e}")
        net = pg.Network.example("campfire")
        net["solver"]["resolution"] = 24
        folder = tempfile.mkdtemp()
        try:
            picture = os.path.join(folder, "fire.png")
            try:
                said = net.render(picture, frames=3, size="160x90")
            except pg.Error as e:
                if "EGL" in str(e) or "context" in str(e):
                    self.skipTest(f"no OpenGL here: {e}")
                raise
            self.assertTrue(os.path.exists(picture))
            self.assertIn("3 frames", said)
        finally:
            shutil.rmtree(folder)


if __name__ == "__main__":
    unittest.main()
