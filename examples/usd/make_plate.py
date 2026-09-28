"""Films the plate of the matchmove example (examples/sim/matchmove.pgsim):
the footage its fire goes over. There is no courtyard to film, so the
program films the set of shot.usda itself -- every prim of it, in the
colours the layout gave it -- through the matchmove's camera, frame by
frame; then gives each frame what a camera and a scan would: a lens that is
a little soft and darker towards the corners, highlights that bleed warm
into what is round them, a grade, and grain that is new every frame.

    PYTHONPATH=build/python python3 examples/usd/make_plate.py      (numpy)

JPEGs numbered as the shot's frames, courtyard.1001.jpg to .1072.jpg, the
size of the camera's picture, in examples/usd/plate/ -- which git leaves
out: they are made, not kept.
"""

import argparse
import os
import tempfile

import numpy as np

import pg

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.join(HERE, "shot.usda")
START = 1001  # the shot's first frame, as its time codes number it


def box(x, r, axis):
    """The mean of the 2r + 1 pixels round each along an axis; the edge
    pixels stand in for what is past the edge."""
    pad = [(0, 0)] * x.ndim
    pad[axis] = (r + 1, r)
    c = np.cumsum(np.pad(x, pad, mode="edge"), axis=axis, dtype=np.float64)
    n = x.shape[axis]
    return ((np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis) - np.take(c, np.arange(n), axis=axis))
            / (2 * r + 1)).astype(np.float32)


def soften(x, r, passes=3):
    """Blurred: box upon box -- three of them near enough a Gaussian of r + 1/2."""
    for _ in range(passes):
        x = box(box(x, r, 0), r, 1)
    return x


def film(shown, frame):
    """What a camera and a scan make of a picture, as shown (0 to 1)."""
    h, w, _ = shown.shape
    rng = np.random.default_rng(START + frame)  # grain of its own every frame, the same every time
    # The lens, in light: a little soft; the highlights bleed warm into what
    # is round them (halation); darker towards the corners.
    light = shown.astype(np.float32) ** 2.2
    light = 0.55 * light + 0.45 * soften(light, 1, 1)
    light += soften(np.maximum(light - 0.55, 0.0), 8) * np.array([0.45, 0.22, 0.1], np.float32)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    corner = ((x - w / 2) ** 2 + (y - h / 2) ** 2) / ((w / 2) ** 2 + (h / 2) ** 2)
    light *= ((1.0 + 0.2 * corner) ** -2)[..., None]
    shown = np.clip(light, 0.0, 1.0) ** (1 / 2.2)
    # The grade: the blacks lifted a little, and cool; the highlights warm.
    shown = shown * np.array([1.0, 0.975, 0.93], np.float32) + (1.0 - shown) * np.array([0.02, 0.024, 0.034], np.float32)
    # The grain: most in the midtones; clumps a pixel and a half across,
    # mostly alike in the three layers of the film.
    luma = shown @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    amount = 0.004 + 0.03 * np.sqrt(np.clip(luma * (1.0 - luma), 0.0, None))
    grain = 0.8 * rng.standard_normal((h, w, 1), dtype=np.float32) + 0.45 * rng.standard_normal((h, w, 3), dtype=np.float32)
    grain = soften(grain, 1, 1)
    grain /= grain.std()
    return np.clip(shown + grain * amount[..., None], 0.0, 1.0)


def main():
    parser = argparse.ArgumentParser(description="The matchmove example's plate: its set filmed through its camera.")
    parser.add_argument("--out", default=os.path.join(HERE, "plate"), help="the folder (examples/usd/plate)")
    parser.add_argument("--frames", type=int, default=72, help="how many frames from 1001 (72: the shot)")
    parser.add_argument("--quality", type=int, default=92, help="of the JPEGs, 1 to 100")
    args = parser.parse_args()

    # The set as it stands: all of it, its own ground a hair above the floor
    # that runs on past it to the horizon, under the sky.
    net = pg.Network()
    courtyard = net.add("usd_import", "set", file=SHOT, prims="/Set")
    standing = courtyard.connect(net.add("transform", "on_the_floor", t=(0.0, 0.002, 0.0)))
    standing.display()
    camera = net.add("usd_camera", "camera", file=SHOT, width=1280)
    output = net.add("output", "output", frames=args.frames, fps=24, grid=False, sky_behind=True,
                     ground_color=(0.3, 0.28, 0.25))
    camera.connect(output, input="camera")

    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        print(net.render(os.path.join(tmp, "set.png"), every=1).strip())
        for f in range(1, args.frames + 1):
            shown, _ = pg.read_picture(os.path.join(tmp, f"set_{f:04d}.png"))
            plate = film(np.asarray(shown)[..., :3], f)
            pg.write_picture(os.path.join(args.out, f"courtyard.{START + f - 1}.jpg"), plate, quality=args.quality)
    print(f"wrote {os.path.join(args.out, 'courtyard.####.jpg')}: frames {START} to {START + args.frames - 1}")


if __name__ == "__main__":
    main()
