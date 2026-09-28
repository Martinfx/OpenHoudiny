"""Pictures read by the program's own readers (pg.read_picture), the ones a
plate goes through, and written by its writers (pg.write_picture) -- and,
where Pillow and OpenEXR are installed, checked against them: random
pictures of every kind those libraries write, read back value for value.
Then a plate through the renderer (where the program `prototype` draws):
wherever nothing of the CG is in front of it, it comes out as it went in.

    PYTHONPATH=build/python python3 -m unittest discover -s tests/python
"""

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
    from PIL import Image
except ImportError:
    Image = None

try:
    import OpenEXR
except ImportError:
    OpenEXR = None

PICTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "pictures")


@unittest.skipIf(np is None, "numpy is not installed")
class Fixtures(unittest.TestCase):
    def test_a_jpeg_reads_as_libjpeg_read_it(self):
        pixels, linear = pg.read_picture(os.path.join(PICTURES, "420.jpg"))
        self.assertEqual(pixels.shape, (29, 37, 4))
        self.assertFalse(linear)
        with open(os.path.join(PICTURES, "420.rgb"), "rb") as f:
            ref = np.frombuffer(f.read(), np.uint8).reshape(29, 37, 3)
        self.assertTrue((np.round(pixels[..., :3] * 255) == ref).all())

    def test_an_exr_is_linear(self):
        pixels, linear = pg.read_picture(os.path.join(PICTURES, "piz.exr"))
        self.assertTrue(linear)
        self.assertGreater(pixels[..., 0].max(), 1.0)

    def test_what_is_not_a_picture_says_why(self):
        with self.assertRaises(pg.Error):
            pg.read_picture(os.path.join(PICTURES, "make_pictures.py"))


@unittest.skipIf(np is None, "numpy is not installed")
class Written(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_what_is_written_reads_back(self):
        gen = np.random.default_rng(3)
        # Floats as shown, rounded to 8 bits; no alpha where all of it is 1.
        rgb = gen.random((37, 53, 3)).astype(np.float32)
        path = os.path.join(self.dir, "rgb.png")
        pg.write_picture(path, rgb)
        back, linear = pg.read_picture(path)
        self.assertFalse(linear)
        self.assertTrue(np.array_equal(np.round(back[..., :3] * 255), np.round(rgb * 255)))
        self.assertTrue((back[..., 3] == 1).all())
        # Bytes as they are, alpha too -- more than one stored block of them.
        rgba = (gen.random((90, 300, 4)) * 255).astype(np.uint8)
        path = os.path.join(self.dir, "rgba.png")
        pg.write_picture(path, rgba)
        self.assertTrue(np.array_equal(np.round(pg.read_picture(path)[0] * 255), rgba))
        # Grey, sixteen bits of it, and a view that runs backwards.
        grey = (gen.random((10, 11)) * 65535).astype(np.uint16)
        path = os.path.join(self.dir, "grey.png")
        pg.write_picture(path, grey[:, ::-1])
        back = pg.read_picture(path)[0]
        self.assertTrue(np.array_equal(np.round(back[..., 1] * 255), np.round(grey[:, ::-1] / 65535 * 255)))
        # EXR: linear light, in half floats.
        light = (gen.standard_normal((16, 17, 3)) * 4).astype(np.float32)
        path = os.path.join(self.dir, "light.exr")
        pg.write_picture(path, light)
        back, linear = pg.read_picture(path)
        self.assertTrue(linear)
        self.assertTrue(np.array_equal(back[..., :3], light.astype(np.float16).astype(np.float32)))
        # JPEG: near what went in -- its noise and its colour at half the size.
        smooth = _smooth(random.Random(4), 64, 48, 3)
        path = os.path.join(self.dir, "smooth.jpg")
        pg.write_picture(path, smooth, quality=95)
        back = pg.read_picture(path)[0]
        self.assertLess(np.abs(back[..., :3] * 255 - smooth).mean(), 8.0)

    def test_what_cannot_be_written_says_why(self):
        picture = np.zeros((2, 3, 3), np.float32)
        for path, pixels in [("a.tif", picture), ("a.png", np.zeros((2, 3, 5), np.float32)),
                             ("a.png", picture.astype(np.float16)), ("a.png", np.zeros((2, 3, 3, 1), np.float32)),
                             (os.path.join("no", "such", "a.png"), picture)]:
            with self.assertRaises(pg.Error, msg=path):
                pg.write_picture(os.path.join(self.dir, path), pixels)


def _smooth(rng, w, h, channels):
    """A picture with gradients, edges and noise: what compressors meet."""
    y, x = np.mgrid[0:h, 0:w]
    out = []
    for c in range(channels):
        a, b = rng.uniform(2, 30), rng.uniform(2, 30)
        v = 128 + 90 * np.sin(x / a + c) * np.cos(y / b) + np.where((x // 17 + y // 13) % 2, 20, -20)
        v = v + np.asarray([rng.gauss(0, 6) for _ in range(w * h)]).reshape(h, w)
        out.append(v)
    return np.clip(np.dstack(out), 0, 255).astype(np.uint8)


@unittest.skipIf(np is None or Image is None, "numpy and Pillow are not installed")
class AgainstPillow(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_random_jpegs_read_as_libjpeg_reads_them(self):
        rng = random.Random(5)
        for k in range(60):
            w, h = rng.randint(1, 260), rng.randint(1, 180)
            grey = rng.random() < 0.2
            img = Image.fromarray(_smooth(rng, w, h, 1 if grey else 3)[..., 0] if grey else _smooth(rng, w, h, 3))
            options = dict(quality=rng.randint(5, 100))
            if not grey:
                options["subsampling"] = rng.choice([0, 1, 2])
            if rng.random() < 0.4:
                options["progressive"] = True
            if rng.random() < 0.3:
                options["optimize"] = True
            if rng.random() < 0.2:
                options["restart_marker_blocks"] = rng.randint(1, 9)
            path = os.path.join(self.dir, f"{k}.jpg")
            img.save(path, **options)
            ref = np.asarray(Image.open(path).convert("RGB"))
            ours, linear = pg.read_picture(path)
            self.assertFalse(linear)
            self.assertEqual(ours.shape, (h, w, 4), f"picture {k} {options}")
            diff = np.abs(np.round(ours[..., :3] * 255).astype(int) - ref.astype(int)).max()
            self.assertEqual(diff, 0, f"picture {k} ({w} x {h}, {options}) differs by {diff}")

    def test_random_pngs_read_to_the_value(self):
        rng = random.Random(9)
        for k in range(40):
            w, h = rng.randint(1, 200), rng.randint(1, 120)
            mode = rng.choice(["1", "L", "LA", "P", "RGB", "RGBA", "I;16"])
            base = _smooth(rng, w, h, 4)
            if mode == "I;16":
                img = Image.fromarray(base[..., 0].astype(np.uint16) * 257 + base[..., 1])
            elif mode == "P":
                img = Image.fromarray(base[..., :3]).quantize(colors=rng.randint(2, 256))
            else:
                img = Image.fromarray(base[..., :4]).convert(mode)
            path = os.path.join(self.dir, f"{k}.png")
            img.save(path, optimize=rng.random() < 0.5)
            ours, linear = pg.read_picture(path)
            self.assertFalse(linear)
            if mode == "I;16":
                ref = np.asarray(Image.open(path)).astype(np.float64) / 65535.0
                self.assertTrue(np.allclose(ours[..., 0], ref, atol=1e-7), f"picture {k} ({mode})")
                continue
            ref = np.asarray(Image.open(path).convert("RGBA")).astype(np.float64) / 255.0
            self.assertTrue(np.allclose(ours, ref, atol=1e-7), f"picture {k} ({mode}, {w} x {h})")

    def test_what_is_written_reads_in_pillow(self):
        rng = random.Random(8)
        for k in range(12):
            w, h = rng.randint(1, 300), rng.randint(1, 200)
            pixels = _smooth(rng, w, h, 4)
            if k % 3 == 0:
                pixels[..., 3] = 255  # opaque: written as RGB
            path = os.path.join(self.dir, f"{k}.png")
            pg.write_picture(path, pixels)
            self.assertTrue(np.array_equal(np.asarray(Image.open(path).convert("RGBA")), pixels), f"picture {k}")
            path = os.path.join(self.dir, f"{k}.jpg")
            pg.write_picture(path, pixels[..., :3], quality=rng.randint(10, 100))
            theirs = np.asarray(Image.open(path).convert("RGB")).astype(int)
            ours = np.round(pg.read_picture(path)[0][..., :3] * 255).astype(int)
            self.assertEqual(np.abs(theirs - ours).max(), 0, f"picture {k} ({w} x {h})")


@unittest.skipIf(np is None or OpenEXR is None, "numpy and OpenEXR are not installed")
class AgainstOpenExr(unittest.TestCase):
    COMPRESSIONS = ["NO_COMPRESSION", "RLE_COMPRESSION", "ZIPS_COMPRESSION", "ZIP_COMPRESSION", "PIZ_COMPRESSION",
                    "PXR24_COMPRESSION", "B44_COMPRESSION", "B44A_COMPRESSION"]

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_random_exrs_of_every_compression_read_as_openexr_reads_them(self):
        rng = random.Random(13)
        gen = np.random.default_rng(13)
        for k in range(48):
            compression = self.COMPRESSIONS[k % len(self.COMPRESSIONS)]
            w, h = rng.randint(1, 180), rng.randint(1, 90)
            wide = k % 16 == 4  # PIZ, wide and noisy: more than 2^14 values, its 16-bit wavelet
            if wide:
                w, h = 700, 40
            channels = {}
            for name in ("R", "G", "B", "A") if rng.random() < 0.6 else ("R", "G", "B"):
                kind = np.float32 if wide else rng.choice([np.float16, np.float16, np.float32, np.uint32])
                if wide:
                    v = (gen.standard_normal((h, w)) * 100).astype(kind)
                elif kind is np.uint32:
                    v = gen.integers(0, 1 << 20, (h, w)).astype(np.uint32)
                elif rng.random() < 0.5:
                    v = (gen.standard_normal((h, w)) * 4).astype(kind)
                else:
                    y, x = np.mgrid[0:h, 0:w]
                    v = (np.sin(x / 7.0) * np.cos(y / 5.0) * 3 + np.where((x // 4 + y // 4) % 3 == 0, 0.5, 0)).astype(kind)
                channels[name] = v
            path = os.path.join(self.dir, f"{k}.exr")
            header = {"compression": getattr(OpenEXR, compression), "type": OpenEXR.scanlineimage}
            OpenEXR.File(header, {n: OpenEXR.Channel(v) for n, v in channels.items()}).write(path)
            with OpenEXR.File(path, separate_channels=True) as f:
                theirs = {n: np.asarray(c.pixels).astype(np.float64) for n, c in f.channels().items()}
            ours, linear = pg.read_picture(path)
            self.assertTrue(linear)
            self.assertEqual(ours.shape, (h, w, 4))
            for i, name in enumerate(("R", "G", "B", "A")):
                if name not in theirs:
                    continue
                same = np.array_equal(ours[..., i].astype(np.float64), theirs[name].astype(np.float32).astype(np.float64),
                                      equal_nan=True)
                self.assertTrue(same, f"file {k} ({compression}, {w} x {h}): channel {name} differs")


@unittest.skipIf(np is None, "numpy is not installed")
class ThroughAPlate(unittest.TestCase):
    """A plate the size of the camera's picture, drawn behind the CG: pixel
    for pixel as it went in wherever the CG is not -- over holdouts and
    catchers too, and over the floor, the ground it was filmed on."""

    def setUp(self):
        try:
            pg.run("help")
        except (pg.Error, OSError) as e:
            self.skipTest(f"prototype does not run here: {e}")
        self.dir = tempfile.mkdtemp()
        pg.write_picture(os.path.join(self.dir, "plate.0001.jpg"), _smooth(random.Random(21), 160, 90, 3), quality=90)
        self.plate = np.round(pg.read_picture(os.path.join(self.dir, "plate.0001.jpg"))[0][..., :3] * 255)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def shot(self, matte=None, floor_matte="catcher"):
        """Nothing simulated: a box behind the camera, shown; an object in
        front of it, as `matte` says."""
        net = pg.Network()
        net.add("box", "behind", center=(5.0, 0.5, 5.0), size=(0.2, 0.2, 0.2)).display()
        camera = net.add("camera", "camera", width=160, height=90, plate=os.path.join(self.dir, "plate.####.jpg"))
        camera.connect(net.add("output", "output", frames=1, floor_matte=floor_matte), input="camera")
        if matte:
            net.add("object", "thing", shape="box", center=(0.0, 0.4, 0.0), size=(0.8, 0.8, 0.8), matte=matte)
        return net

    def render(self, net, name):
        path = os.path.join(self.dir, name)
        try:
            net.render(path)
        except pg.Error as e:
            if "EGL" in str(e) or "context" in str(e):
                self.skipTest(f"no OpenGL here: {e}")
            raise
        return pg.read_picture(path)[0]

    def test_the_plate_comes_out_as_it_went_in(self):
        for matte, floor in [(None, "catcher"), (None, "holdout"), ("holdout", "catcher"), ("catcher", "catcher")]:
            out = np.round(self.render(self.shot(matte, floor), f"{matte}_{floor}.png")[..., :3] * 255)
            differ = int((out != self.plate).any(axis=2).sum())
            self.assertEqual(differ, 0, f"{matte} in front, the floor a {floor}: {differ} pixels differ")

    def test_a_solid_is_drawn_over_it_and_shadows_the_ground(self):
        out = np.round(self.render(self.shot("solid"), "solid.png")[..., :3] * 255)
        differ = (out != self.plate).any(axis=2).mean()
        self.assertGreater(differ, 0.02)  # the box, and its shadow on the floor
        self.assertLess(differ, 0.6)
        # The floor drawn as itself hides the plate where it is.
        out = np.round(self.render(self.shot(None, "solid"), "floor.png")[..., :3] * 255)
        self.assertGreater((out != self.plate).any(axis=2).mean(), 0.2)

    def test_over_a_plate_the_exr_is_the_cg_alone(self):
        rgba = self.render(self.shot("solid"), "solid.exr")
        self.assertEqual(rgba.shape, (90, 160, 4))
        alpha = rgba[..., 3]
        # The box covers what it covers, and nothing else is there.
        self.assertGreater((alpha > 0.99).mean(), 0.01)
        self.assertTrue(((alpha > 0.99) | (alpha < 0.01) | _edge(alpha)).all())
        self.assertEqual(np.abs(rgba[..., :3][alpha < 0.01]).max(), 0.0)
        if OpenEXR is None:
            return
        with OpenEXR.File(os.path.join(self.dir, "solid.exr"), separate_channels=True) as f:
            channels = f.channels()
            catcher = np.dstack([np.asarray(channels[f"catcher.{c}"].pixels, np.float64) for c in "RGB"])
        # The plate as it is, but where the box takes the sun from the ground.
        self.assertLess(catcher.min(), 0.9)
        self.assertLessEqual(catcher.max(), 1.0)
        self.assertGreater((catcher == 1.0).all(axis=2).mean(), 0.5)


def _edge(mask):
    """Pixels next to one on the other side of a half: an edge, part covered."""
    solid = mask > 0.5
    near = np.zeros_like(solid)
    for dy, dx in [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]:
        near |= np.roll(np.roll(solid, dy, 0), dx, 1) != solid
    return near


if __name__ == "__main__":
    unittest.main()
