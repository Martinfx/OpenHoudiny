"""A shot from USD, as the other departments deliver it: the matchmove's
camera and the layout's set (examples/usd -- centimetres, Z up, from frame
1001). What the file holds, the camera at its first and last frame, the set
as geometry -- and a network that burns a fire in the set, seen through the
shot's camera over its plate (examples/usd/make_plate.py films it), where
the set is what the plate shows: catchers of the smoke's shadow and the
fire's light.

    PYTHONPATH=build/python python3 examples/usd/make_plate.py
    PYTHONPATH=build/python python3 examples/python/matchmove.py --picture out/matchmove.png
"""

import argparse
import os

import numpy as np

import pg

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.normpath(os.path.join(HERE, "..", "usd", "shot.usda"))
PLATE = os.path.normpath(os.path.join(HERE, "..", "usd", "plate", "courtyard.####.jpg"))
WALLS = "/Set/back_wall /Set/side_wall /Set/beam /Set/pillar /Set/crate_a /Set/crate_b"


def main():
    parser = argparse.ArgumentParser(description="A fire in a set from USD, through the matchmove's camera.")
    parser.add_argument("--picture", help="the last frame, through the shot's camera, to this file (.png, .exr)")
    parser.add_argument("--frames", type=int, default=72, help="how many frames (72: the shot)")
    args = parser.parse_args()

    # What the shot is: its units, its layers, its prims.
    stage = pg.UsdStage(SHOT)
    print(f"{os.path.basename(SHOT)}: {stage.up_axis} up, {stage.meters_per_unit:g} m a unit, "
          f"time codes {stage.start_time_code:g} to {stage.end_time_code:g}")
    for f in stage.files:
        print("  layer", os.path.basename(f))
    for prim in stage.prims():
        print("  " + "  " * (prim.path.count("/") - 1) + prim.name, prim.type)

    # The camera, in metres with Y up, at the first and the last frame.
    for time in (stage.start_time_code, stage.end_time_code):
        cam = stage.camera(time=time)
        x, y, z = cam["world"][3][:3]
        print(f"camera at {time:g}: ({x:.2f}, {y:.2f}, {z:.2f}) m, a {cam['focal_length']:g} mm lens")

    # The set as geometry: how large it is, what it is made of.
    set_ = stage.geometry(prims=["/Set"])
    size = np.ptp(np.asarray(set_.P), axis=0)
    print(f"the set: {set_.point_count} points, {set_.primitive_count} faces, "
          f"{size[0]:.2f} x {size[1]:.2f} x {size[2]:.2f} m")
    for path in sorted(set(set_.prims["path"])):
        print("  ", path)

    # A fire in it: the walls, the beam and the crates are what the smoke
    # goes round; the shot's camera is what renders see through, and its
    # plate what they go over -- where the walls are the real ones.
    net = pg.Network()
    walls = net.add("usd_import", "set", file=SHOT, prims=WALLS)
    solid = net.add("object", "walls", color=(0.58, 0.55, 0.5), matte="catcher")
    walls.connect(solid, input="shape")
    fire = net.add("pyro_source", "fire", fuel=14, smoke=3, heat=1, velocity=(0, 0.4, 0), flicker=0.7)
    swirl = net.add("turbulence", "turbulence", strength=3.5)
    solver = net.add("pyro_solver", "solver", size=(2.6, 2.2, 2.0), buoyancy=0.9, vorticity=0.9, cooling=1.5,
                     smoke_decay=0.04)
    look = net.add("volume_look", "look", smoke_color=(0.2, 0.19, 0.18), smoke_density=24, fire_light=3)
    plate = net.add("usd_camera", "plate", file=SHOT, width=1280, plate=PLATE)
    out = net.add("output", "output", frames=args.frames, fps=24)
    for a, b in [(solid, solver), (fire, solver), (swirl, solver), (solver, look), (look, out)]:
        a.connect(b)
    plate.connect(out, input="camera")
    for level, node, message in net.problems():
        print(f"{level}: {node}: {message}")
    if args.picture:
        print(net.render(args.picture, frames=args.frames))
    else:
        print("--picture out/matchmove.png: the last frame through the shot's camera")


if __name__ == "__main__":
    main()
