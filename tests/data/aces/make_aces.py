# How aces.txt was made: what OpenColorIO's own ACES configs show linear
# Rec. 709 light as on an sRGB screen, and back -- the values
# tests/test_aces.cpp holds src/pg/render/Aces.cpp to.
#
#   pip install opencolorio==2.6.0 numpy
#   python tests/data/aces/make_aces.py > tests/data/aces/aces.txt
#
# Each line: the view (1: "ACES 1.0 - SDR Video" of the ACES 1.3 CG config,
# 2: "ACES 2.0 - SDR 100 nits (Rec.709)" of the ACES 2.1 one), the way
# (f: light to the picture, i: the picture to light), three numbers in and
# three out. Then the colour spaces: "cg" (ACEScg) and "ap0" (ACES2065-1),
# f: linear Rec. 709 light in them.
import itertools
import random

import numpy as np
import PyOpenColorIO as ocio

VIEWS = {
    1: ("ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4", "ACES 1.0 - SDR Video"),
    2: ("ocio://cg-config-v5.0.0_aces-v2.1_ocio-v2.6", "ACES 2.0 - SDR 100 nits (Rec.709)"),
}

# Light: greys from deep shadow to far past white, the primaries and the
# colours between them, skin, sky, foliage, fire -- at many brightnesses --
# and colours at random.
colours = [(1, 1, 1), (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (1, 0, 1), (1, 0.5, 0), (0.5, 1, 0),
           (0, 1, 0.5), (0, 0.5, 1), (0.5, 0, 1), (1, 0, 0.5), (0.8, 0.55, 0.45), (0.35, 0.55, 0.9),
           (0.2, 0.45, 0.12), (1, 0.35, 0.05), (0.9, 0.9, 0.8), (0.05, 0.05, 0.06)]
levels = [0.0, 1e-4, 1e-3, 0.005, 0.02, 0.05, 0.1, 0.18, 0.3, 0.5, 0.8, 1.0, 1.5, 2.5, 4.0, 8.0, 16.0, 40.0, 100.0, 1000.0]
light = [tuple(level * c for c in colour) for colour, level in itertools.product(colours, levels)]
rng = random.Random(7)
for _ in range(400):
    level = 10 ** rng.uniform(-3, 2.5)
    light.append(tuple(level * rng.random() for _ in range(3)))
# Pictures: a grid of the screen's colours, and colours at random.
steps = [0.0, 0.02, 0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9, 0.98, 1.0]
pictures = list(itertools.product(steps, steps, steps))
for _ in range(200):
    pictures.append(tuple(rng.random() for _ in range(3)))

for view, (config, name) in VIEWS.items():
    cfg = ocio.Config.CreateFromFile(config)
    for way, values in (("f", light), ("i", pictures)):
        direction = ocio.TRANSFORM_DIR_FORWARD if way == "f" else ocio.TRANSFORM_DIR_INVERSE
        cpu = cfg.getProcessor("Linear Rec.709 (sRGB)", "sRGB - Display", name, direction).getDefaultCPUProcessor()
        data = np.array(values, dtype=np.float32).copy()
        cpu.applyRGB(data)
        for a, b in zip(values, data.reshape(-1, 3)):
            print(view, way, " ".join(f"{float(v):.9g}" for v in a), " ".join(f"{float(v):.9g}" for v in b))

cfg = ocio.Config.CreateFromFile(VIEWS[2][0])
for space, name in (("cg", "ACEScg"), ("ap0", "ACES2065-1")):
    cpu = cfg.getProcessor("Linear Rec.709 (sRGB)", name).getDefaultCPUProcessor()
    values = light[::7]
    data = np.array(values, dtype=np.float32).copy()
    cpu.applyRGB(data)
    for a, b in zip(values, data.reshape(-1, 3)):
        print(space, "f", " ".join(f"{float(v):.9g}" for v in a), " ".join(f"{float(v):.9g}" for v in b))
