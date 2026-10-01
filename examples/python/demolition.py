"""The demolition of step 2, built from Python: a tower of blocks broken by a
Voronoi Fracture, glued back by the RBD Solver and brought down by charges
into its footprint, in a city block, the dust rolling out into the streets.

The network is the example's (examples/sim/demolition.pgsim), node by node
-- Network.as_code() wrote build() below. Run it:

    PYTHONPATH=build/python python3 examples/python/demolition.py --frames 60

It simulates the shot, writes each frame to a cache and the shot to USD
(out/demolition.usda, the frames beside it), then -- with --video -- draws it
from the cache through prototype.
"""

import argparse
import os
import sys
import time

import pg


def build():
    net = pg.Network()
    tower = net.add('detail_wrangle', 'tower',
                    snippet=r'''// A tower as it is built: a slab for each floor, the walls between them --
// piers, sills and lintels round the windows -- four columns inside, a
// parapet on the roof. Every block a box of its own, touching the next face
// to face: what the Voronoi Fracture cuts, and what the RBD Solver glues
// where they touch. Faces on the outside in plaster, the slabs' edges in
// concrete, the rest -- in the rooms, in the windows -- in the shade.
void block(vector lo; vector hi; vector outside; vector inside; int fl; int kind; float hw; float hd) {
    int p[];
    for (int i = 0; i < 8; i++) {
        append(p, addpoint(0, set(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z)));
    }
    int f[];
    append(f, addprim(0, "poly", p[0], p[4], p[6], p[2]));  // -x
    append(f, addprim(0, "poly", p[1], p[3], p[7], p[5]));  // +x
    append(f, addprim(0, "poly", p[0], p[1], p[5], p[4]));  // -y
    append(f, addprim(0, "poly", p[2], p[6], p[7], p[3]));  // +y
    append(f, addprim(0, "poly", p[0], p[2], p[3], p[1]));  // -z
    append(f, addprim(0, "poly", p[4], p[5], p[7], p[6]));  // +z
    int out[] = {0, 0, 0, 0, 0, 0};
    out[0] = lo.x <= -hw + 0.001;
    out[1] = hi.x >= hw - 0.001;
    out[4] = lo.z <= -hd + 0.001;
    out[5] = hi.z >= hd - 0.001;
    out[3] = kind == 3 || kind == 4;  // the parapet's top and the roof
    // What each is made of, for the renderers: the walls and the parapet
    // plastered, the roof's top a roof, the rest -- slabs, columns, the
    // edges of the roof -- bare concrete.
    string made = kind == 1 || kind == 3 ? "plaster" : "concrete";
    for (int k = 0; k < 6; k++) {
        setprimattrib(0, "Cd", f[k], out[k] ? outside : inside);
        setprimattrib(0, "floor", f[k], fl);
        setprimattrib(0, "kind", f[k], kind);
        setprimattrib(0, "material", f[k], kind == 4 && k == 3 ? "roof" : made);
    }
}

// A block of wall `side` (0 front +z, 1 back, 2 left -x, 3 right), from u0
// to u1 along it, y0 to y1 up it.
void wall(int side; float u0; float u1; float y0; float y1; float t; vector outside; vector inside; int fl; float hw; float hd) {
    if (side == 0) block(set(u0, y0, hd - t), set(u1, y1, hd), outside, inside, fl, 1, hw, hd);
    if (side == 1) block(set(u0, y0, -hd), set(u1, y1, -hd + t), outside, inside, fl, 1, hw, hd);
    if (side == 2) block(set(-hw, y0, u0), set(-hw + t, y1, u1), outside, inside, fl, 1, hw, hd);
    if (side == 3) block(set(hw - t, y0, u0), set(hw, y1, u1), outside, inside, fl, 1, hw, hd);
}

int floors = max(chi("floors"), 1);
float fh = max(ch("floor_height"), 2.0);
float hw = ch("width") / 2;
float hd = ch("depth") / 2;
float slab = ch("slab");
float t = ch("wall");
float bay = max(ch("bay"), 1.0);
float win = ch("window");
float sill = ch("sill");
float head = ch("head");
vector plaster = chv("plaster");
vector concrete = chv("concrete");
vector shade = chv("interior");
for (int fl = 0; fl < floors; fl++) {
    float y0 = fl * fh;
    block(set(-hw, y0, -hd), set(hw, y0 + slab, hd), concrete, shade, fl, 0, hw, hd);
    float a = y0 + slab;
    float b = y0 + fh;
    // The walls: the front and the back their full width, the sides between them.
    for (int side = 0; side < 4; side++) {
        float u0 = side < 2 ? -hw : -hd + t;
        float u1 = side < 2 ? hw : hd - t;
        int n = max(1, int(rint((u1 - u0) / bay)));
        float bw = (u1 - u0) / n;
        float pier = max(bw - win, 0.4) / 2;
        for (int k = 0; k < n; k++) {
            float s = u0 + k * bw;
            wall(side, s, s + pier, a, b, t, plaster, shade, fl, hw, hd);
            wall(side, s + pier, s + bw - pier, a, a + sill, t, plaster, shade, fl, hw, hd);
            wall(side, s + pier, s + bw - pier, a + head, b, t, plaster, shade, fl, hw, hd);
            wall(side, s + bw - pier, s + bw, a, b, t, plaster, shade, fl, hw, hd);
        }
    }
    // The columns inside.
    for (int c = 0; c < 4; c++) {
        float x = (c & 1 ? 1 : -1) * hw / 2;
        float z = (c & 2 ? 1 : -1) * hd / 2;
        block(set(x - 0.35, a, z - 0.35), set(x + 0.35, b, z + 0.35), shade, shade, fl, 2, hw, hd);
    }
}
// The roof, and the parapet round it.
float top = floors * fh;
block(set(-hw, top, -hd), set(hw, top + slab, hd), concrete, shade, floors, 4, hw, hd);
float py = top + slab;
block(set(-hw, py, hd - t), set(hw, py + 1.0, hd), plaster, shade, floors, 3, hw, hd);
block(set(-hw, py, -hd), set(hw, py + 1.0, -hd + t), plaster, shade, floors, 3, hw, hd);
block(set(-hw, py, -hd + t), set(-hw + t, py + 1.0, hd - t), plaster, shade, floors, 3, hw, hd);
block(set(hw - t, py, -hd + t), set(hw, py + 1.0, hd - t), plaster, shade, floors, 3, hw, hd);''',
                    floors=14,
                    floor_height=3.0,
                    width=14.0,
                    depth=12.0,
                    slab=0.3,
                    wall=0.3,
                    bay=2.0,
                    window=1.3,
                    sill=0.9,
                    head=2.25,
                    plaster=(0.78, 0.74, 0.68),
                    concrete=(0.6, 0.58, 0.55),
                    interior=(0.33, 0.31, 0.29))
    tower.position = (0.0, 0.0)
    seeds = net.add('detail_wrangle', 'seeds',
                    snippet=r'''// Where the tower breaks: points in each of its blocks, as many as the
// block holds cells of Size -- smaller low down, where the charges are, and
// a little random, so that no two floors break alike.
float size = ch("size");
float low = ch("low_size");
float lowTop = ch("low_top");
int blocks = nprimitives(1) / 6;
for (int k = 0; k < blocks; k++) {
    vector lo = set(1e9, 1e9, 1e9);
    vector hi = set(-1e9, -1e9, -1e9);
    for (int f = 0; f < 6; f++) {
        foreach (int pt; primpoints(1, k * 6 + f)) {
            vector p = point(1, "P", pt);
            lo = set(min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z));
            hi = set(max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z));
        }
    }
    vector s = hi - lo;
    float c = lo.y < lowTop ? low : size;
    float n = s.x * s.y * s.z / (c * c * c);
    int count = int(floor(n + rand(k * 7 + 3)));
    for (int i = 0; i < count; i++) {
        vector r = set(rand(k * 131 + i * 3), rand(k * 131 + i * 3 + 1), rand(k * 131 + i * 3 + 2));
        addpoint(0, lo + s * (0.1 + 0.8 * r));
    }
}''',
                    size=1.6,
                    low_size=0.9,
                    low_top=9.0)
    seeds.position = (0.0, 130.0)
    fracture = net.add('voronoi_fracture', 'fracture')
    fracture.position = (250.0, 60.0)
    charges = net.add('primitive_wrangle', 'charges',
                      snippet=r'''// The charges. The columns inside go first, then the walls of the lowest
// floors, a floor after the other: blown to dust (vanish) -- what holds the
// tower up is gone, and it drops into its own footprint. The slabs between
// them come loose (release) and the blast kicks them out. The slab on the
// ground stays where it is.
float fire = ch("fire");
float push = ch("push");
int charged = chi("floors_charged");
i@active = 1;
f@glue = 1;
f@release = 0;
i@vanish = 0;
v@kick = set(0, 0, 0);
// What a falling floor lands on turns to dust: the walls and the columns
// crush easily, the slabs only under a hard blow -- the rest of them piles up.
f@crush = i@kind == 0 || i@kind == 4 ? ch("crush_slabs") : ch("crush");
if (i@floor == 0 && i@kind == 0) {
    i@active = 0;
} else if (i@floor < charged) {
    vector away = set(@P.x, 0, @P.z);
    float delay = (i@kind == 2 ? 0 : 0.12) + 0.1 * i@floor + 0.04 * rand(i@piece);
    f@release = fire + delay;
    i@vanish = i@kind != 0;
    v@kick = normalize(away) * push * (0.3 + 0.7 * rand(i@piece + 17));
}''',
                      fire=1.0,
                      push=5.0,
                      floors_charged=2,
                      crush_slabs=6.0,
                      crush=1.5)
    charges.position = (480.0, 60.0)
    rbd = net.add('rbd_solver', 'rbd',
                  density=2400.0,
                  friction=0.8,
                  bounce=0.05,
                  glue=150.0,
                  substeps=3,
                  impact_dust=3.0,
                  dust_size=2.5,
                  debris=0.05,
                  air=3.0,
                  inside_color=(0.64, 0.61, 0.56))
    rbd.position = (710.0, 60.0)
    west = net.add('building', 'west',
                   floor_height=3.2,
                   width=20.0,
                   depth=16.0,
                   bay=3.2,
                   frame=0.35,
                   recess=-0.3,
                   balconies=0,
                   wall=(0.72, 0.62, 0.5))
    west.position = (0.0, 330.0)
    at_west = net.add('transform', 'at_west', t=(-32.0, 0.0, 2.0))
    at_west.position = (230.0, 330.0)
    east = net.add('building', 'east',
                   floors=8,
                   floor_height=3.2,
                   width=18.0,
                   depth=18.0,
                   bay=3.2,
                   frame=0.35,
                   recess=-0.3,
                   balconies=0,
                   wall=(0.62, 0.64, 0.68))
    east.position = (0.0, 440.0)
    at_east = net.add('transform', 'at_east', t=(33.0, 0.0, -1.0))
    at_east.position = (230.0, 440.0)
    north = net.add('building', 'north',
                    floors=5,
                    floor_height=3.2,
                    width=26.0,
                    depth=14.0,
                    bay=3.2,
                    frame=0.35,
                    recess=-0.3,
                    balconies=0,
                    wall=(0.55, 0.3, 0.24))
    north.position = (0.0, 550.0)
    at_north = net.add('transform', 'at_north', t=(2.0, 0.0, -31.0))
    at_north.position = (230.0, 550.0)
    northwest = net.add('building', 'northwest',
                        floors=7,
                        floor_height=3.2,
                        width=18.0,
                        depth=16.0,
                        bay=3.2,
                        frame=0.35,
                        recess=-0.3,
                        balconies=0,
                        wall=(0.8, 0.78, 0.74))
    northwest.position = (0.0, 660.0)
    at_northwest = net.add('transform', 'at_northwest', t=(-32.0, 0.0, -31.0))
    at_northwest.position = (230.0, 660.0)
    northeast = net.add('building', 'northeast',
                        floors=4,
                        floor_height=3.2,
                        width=18.0,
                        depth=14.0,
                        bay=3.2,
                        frame=0.35,
                        recess=-0.3,
                        balconies=0,
                        wall=(0.5, 0.28, 0.22))
    northeast.position = (0.0, 770.0)
    at_northeast = net.add('transform', 'at_northeast', t=(33.0, 0.0, -32.0))
    at_northeast.position = (230.0, 770.0)
    south = net.add('building', 'south',
                    floors=3,
                    floor_height=3.2,
                    width=24.0,
                    depth=14.0,
                    bay=3.2,
                    frame=0.35,
                    recess=-0.3,
                    balconies=0,
                    wall=(0.7, 0.66, 0.58))
    south.position = (0.0, 880.0)
    at_south = net.add('transform', 'at_south', t=(1.0, 0.0, 31.0))
    at_south.position = (230.0, 880.0)
    southeast = net.add('building', 'southeast',
                        floors=4,
                        floor_height=3.2,
                        width=18.0,
                        depth=16.0,
                        bay=3.2,
                        frame=0.35,
                        recess=-0.3,
                        balconies=0,
                        wall=(0.64, 0.6, 0.55))
    southeast.position = (0.0, 990.0)
    at_southeast = net.add('transform', 'at_southeast', t=(33.0, 0.0, 32.0))
    at_southeast.position = (230.0, 990.0)
    southwest = net.add('building', 'southwest',
                        floors=3,
                        floor_height=3.2,
                        width=20.0,
                        depth=14.0,
                        bay=3.2,
                        frame=0.35,
                        recess=-0.3,
                        balconies=0,
                        wall=(0.52, 0.34, 0.28))
    southwest.position = (0.0, 1100.0)
    at_southwest = net.add('transform', 'at_southwest', t=(-32.0, 0.0, 32.0))
    at_southwest.position = (230.0, 1100.0)
    far_north = net.add('building', 'far_north',
                        floors=9,
                        floor_height=3.2,
                        width=24.0,
                        depth=16.0,
                        bay=3.2,
                        frame=0.35,
                        recess=-0.3,
                        balconies=0,
                        wall=(0.74, 0.7, 0.62))
    far_north.position = (0.0, 1210.0)
    at_far_north = net.add('transform', 'at_far_north', t=(1.0, 0.0, -64.0))
    at_far_north.position = (230.0, 1210.0)
    far_northwest = net.add('building', 'far_northwest',
                            floor_height=3.2,
                            width=20.0,
                            depth=18.0,
                            bay=3.2,
                            frame=0.35,
                            recess=-0.3,
                            balconies=0,
                            wall=(0.56, 0.36, 0.28))
    far_northwest.position = (0.0, 1320.0)
    at_far_northwest = net.add('transform', 'at_far_northwest', t=(-32.0, 0.0, -64.0))
    at_far_northwest.position = (230.0, 1320.0)
    far_northeast = net.add('building', 'far_northeast',
                            floors=7,
                            floor_height=3.2,
                            width=20.0,
                            depth=16.0,
                            bay=3.2,
                            frame=0.35,
                            recess=-0.3,
                            balconies=0,
                            wall=(0.66, 0.68, 0.7))
    far_northeast.position = (0.0, 1430.0)
    at_far_northeast = net.add('transform', 'at_far_northeast', t=(34.0, 0.0, -64.0))
    at_far_northeast.position = (230.0, 1430.0)
    corner_northwest = net.add('building', 'corner_northwest',
                               floors=10,
                               floor_height=3.2,
                               width=20.0,
                               depth=18.0,
                               bay=3.2,
                               frame=0.35,
                               recess=-0.3,
                               balconies=0,
                               wall=(0.78, 0.76, 0.72))
    corner_northwest.position = (0.0, 1540.0)
    at_corner_northwest = net.add('transform', 'at_corner_northwest', t=(-65.0, 0.0, -64.0))
    at_corner_northwest.position = (230.0, 1540.0)
    corner_northeast = net.add('building', 'corner_northeast',
                               floors=5,
                               floor_height=3.2,
                               width=18.0,
                               depth=16.0,
                               bay=3.2,
                               frame=0.35,
                               recess=-0.3,
                               balconies=0,
                               wall=(0.6, 0.52, 0.44))
    corner_northeast.position = (0.0, 1650.0)
    at_corner_northeast = net.add('transform', 'at_corner_northeast', t=(66.0, 0.0, -63.0))
    at_corner_northeast.position = (230.0, 1650.0)
    far_west = net.add('building', 'far_west',
                       floors=8,
                       floor_height=3.2,
                       width=20.0,
                       depth=18.0,
                       bay=3.2,
                       frame=0.35,
                       recess=-0.3,
                       balconies=0,
                       wall=(0.68, 0.6, 0.5))
    far_west.position = (0.0, 1760.0)
    at_far_west = net.add('transform', 'at_far_west', t=(-65.0, 0.0, 2.0))
    at_far_west.position = (230.0, 1760.0)
    far_west_north = net.add('building', 'far_west_north',
                             floors=5,
                             floor_height=3.2,
                             width=18.0,
                             depth=16.0,
                             bay=3.2,
                             frame=0.35,
                             recess=-0.3,
                             balconies=0,
                             wall=(0.5, 0.32, 0.26))
    far_west_north.position = (0.0, 1870.0)
    at_far_west_north = net.add('transform', 'at_far_west_north', t=(-65.0, 0.0, -31.0))
    at_far_west_north.position = (230.0, 1870.0)
    far_west_south = net.add('building', 'far_west_south',
                             floors=4,
                             floor_height=3.2,
                             width=20.0,
                             depth=16.0,
                             bay=3.2,
                             frame=0.35,
                             recess=-0.3,
                             balconies=0,
                             wall=(0.72, 0.7, 0.66))
    far_west_south.position = (0.0, 1980.0)
    at_far_west_south = net.add('transform', 'at_far_west_south', t=(-65.0, 0.0, 33.0))
    at_far_west_south.position = (230.0, 1980.0)
    far_east_north = net.add('building', 'far_east_north',
                             floor_height=3.2,
                             width=18.0,
                             depth=18.0,
                             bay=3.2,
                             frame=0.35,
                             recess=-0.3,
                             balconies=0,
                             wall=(0.72, 0.66, 0.56))
    far_east_north.position = (0.0, 2090.0)
    at_far_east_north = net.add('transform', 'at_far_east_north', t=(66.0, 0.0, -31.0))
    at_far_east_north.position = (230.0, 2090.0)
    far_south_west = net.add('building', 'far_south_west',
                             floors=3,
                             floor_height=3.2,
                             width=20.0,
                             depth=16.0,
                             bay=3.2,
                             frame=0.35,
                             recess=-0.3,
                             balconies=0,
                             wall=(0.62, 0.58, 0.52))
    far_south_west.position = (0.0, 2200.0)
    at_far_south_west = net.add('transform', 'at_far_south_west', t=(-32.0, 0.0, 64.0))
    at_far_south_west.position = (230.0, 2200.0)
    corner_southwest = net.add('building', 'corner_southwest',
                               floors=5,
                               floor_height=3.2,
                               width=20.0,
                               depth=16.0,
                               bay=3.2,
                               frame=0.35,
                               recess=-0.3,
                               balconies=0,
                               wall=(0.54, 0.34, 0.27))
    corner_southwest.position = (0.0, 2310.0)
    at_corner_southwest = net.add('transform', 'at_corner_southwest', t=(-65.0, 0.0, 64.0))
    at_corner_southwest.position = (230.0, 2310.0)
    kerb_west = net.add('box', 'kerb_west', size=(26.0, 0.15, 22.0), center=(-32.0, 0.075, 2.0))
    kerb_west.position = (460.0, 330.0)
    kerb_east = net.add('box', 'kerb_east', size=(24.0, 0.15, 24.0), center=(33.0, 0.075, -1.0))
    kerb_east.position = (460.0, 440.0)
    kerb_north = net.add('box', 'kerb_north', size=(32.0, 0.15, 20.0), center=(2.0, 0.075, -31.0))
    kerb_north.position = (460.0, 550.0)
    kerb_northwest = net.add('box', 'kerb_northwest',
                             size=(24.0, 0.15, 22.0),
                             center=(-32.0, 0.075, -31.0))
    kerb_northwest.position = (460.0, 660.0)
    kerb_northeast = net.add('box', 'kerb_northeast',
                             size=(24.0, 0.15, 20.0),
                             center=(33.0, 0.075, -32.0))
    kerb_northeast.position = (460.0, 770.0)
    kerb_south = net.add('box', 'kerb_south', size=(30.0, 0.15, 20.0), center=(1.0, 0.075, 31.0))
    kerb_south.position = (460.0, 880.0)
    kerb_southeast = net.add('box', 'kerb_southeast',
                             size=(24.0, 0.15, 22.0),
                             center=(33.0, 0.075, 32.0))
    kerb_southeast.position = (460.0, 990.0)
    kerb_southwest = net.add('box', 'kerb_southwest',
                             size=(26.0, 0.15, 20.0),
                             center=(-32.0, 0.075, 32.0))
    kerb_southwest.position = (460.0, 1100.0)
    kerb_far_north = net.add('box', 'kerb_far_north',
                             size=(30.0, 0.15, 22.0),
                             center=(1.0, 0.075, -64.0))
    kerb_far_north.position = (460.0, 1210.0)
    kerb_far_northwest = net.add('box', 'kerb_far_northwest',
                                 size=(26.0, 0.15, 24.0),
                                 center=(-32.0, 0.075, -64.0))
    kerb_far_northwest.position = (460.0, 1320.0)
    kerb_far_northeast = net.add('box', 'kerb_far_northeast',
                                 size=(26.0, 0.15, 22.0),
                                 center=(34.0, 0.075, -64.0))
    kerb_far_northeast.position = (460.0, 1430.0)
    kerb_corner_northwest = net.add('box', 'kerb_corner_northwest',
                                    size=(26.0, 0.15, 24.0),
                                    center=(-65.0, 0.075, -64.0))
    kerb_corner_northwest.position = (460.0, 1540.0)
    kerb_corner_northeast = net.add('box', 'kerb_corner_northeast',
                                    size=(24.0, 0.15, 22.0),
                                    center=(66.0, 0.075, -63.0))
    kerb_corner_northeast.position = (460.0, 1650.0)
    kerb_far_west = net.add('box', 'kerb_far_west',
                            size=(26.0, 0.15, 24.0),
                            center=(-65.0, 0.075, 2.0))
    kerb_far_west.position = (460.0, 1760.0)
    kerb_far_west_north = net.add('box', 'kerb_far_west_north',
                                  size=(24.0, 0.15, 22.0),
                                  center=(-65.0, 0.075, -31.0))
    kerb_far_west_north.position = (460.0, 1870.0)
    kerb_far_west_south = net.add('box', 'kerb_far_west_south',
                                  size=(26.0, 0.15, 22.0),
                                  center=(-65.0, 0.075, 33.0))
    kerb_far_west_south.position = (460.0, 1980.0)
    kerb_far_east_north = net.add('box', 'kerb_far_east_north',
                                  size=(24.0, 0.15, 24.0),
                                  center=(66.0, 0.075, -31.0))
    kerb_far_east_north.position = (460.0, 2090.0)
    kerb_far_south_west = net.add('box', 'kerb_far_south_west',
                                  size=(26.0, 0.15, 22.0),
                                  center=(-32.0, 0.075, 64.0))
    kerb_far_south_west.position = (460.0, 2200.0)
    kerb_corner_southwest = net.add('box', 'kerb_corner_southwest',
                                    size=(26.0, 0.15, 22.0),
                                    center=(-65.0, 0.075, 64.0))
    kerb_corner_southwest.position = (460.0, 2310.0)
    kerb_tower = net.add('box', 'kerb_tower', size=(20.0, 0.15, 18.0), center=(0.0, -0.06, 0.0))
    kerb_tower.position = (460.0, 2420.0)
    pavement = net.add('color', 'pavement', color=(0.46, 0.45, 0.43), **{'class': 'primitive'})
    pavement.position = (690.0, 330.0)
    city = net.add('merge', 'city')
    city.position = (920.0, 330.0)
    billows = net.add('turbulence', 'billows', scale=2.5, speed=0.4)
    billows.position = (710.0, 240.0)
    dust = net.add('pyro_solver', 'dust',
                   size=(90.0, 48.0, 90.0),
                   resolution=176,
                   buoyancy=0.15,
                   weight=0.3,
                   vorticity=4.0,
                   cooling=1.5,
                   smoke_decay=0.01)
    dust.position = (940.0, 120.0)
    dust_look = net.add('volume_look', 'dust_look', smoke_color=(0.76, 0.66, 0.53), occlusion=2.0)
    dust_look.position = (1170.0, 120.0)
    in_west = net.add('object', 'in_west',
                      shape='box',
                      center=(-32.0, 9.4, 2.0),
                      size=(19.0, 18.8, 15.0),
                      color=(0.5, 0.5, 0.5))
    in_west.position = (940.0, 330.0)
    in_east = net.add('object', 'in_east',
                      shape='box',
                      center=(33.0, 12.6, -1.0),
                      size=(17.0, 25.2, 17.0),
                      color=(0.5, 0.5, 0.5))
    in_east.position = (940.0, 440.0)
    in_north = net.add('object', 'in_north',
                       shape='box',
                       center=(2.0, 7.8, -31.0),
                       size=(25.0, 15.6, 13.0),
                       color=(0.5, 0.5, 0.5))
    in_north.position = (940.0, 550.0)
    in_northwest = net.add('object', 'in_northwest',
                           shape='box',
                           center=(-32.0, 11.0, -31.0),
                           size=(17.0, 22.0, 15.0),
                           color=(0.5, 0.5, 0.5))
    in_northwest.position = (940.0, 660.0)
    in_northeast = net.add('object', 'in_northeast',
                           shape='box',
                           center=(33.0, 6.2, -32.0),
                           size=(17.0, 12.4, 13.0),
                           color=(0.5, 0.5, 0.5))
    in_northeast.position = (940.0, 770.0)
    in_south = net.add('object', 'in_south',
                       shape='box',
                       center=(1.0, 4.6, 31.0),
                       size=(23.0, 9.2, 13.0),
                       color=(0.5, 0.5, 0.5))
    in_south.position = (940.0, 880.0)
    in_southeast = net.add('object', 'in_southeast',
                           shape='box',
                           center=(33.0, 6.2, 32.0),
                           size=(17.0, 12.4, 15.0),
                           color=(0.5, 0.5, 0.5))
    in_southeast.position = (940.0, 990.0)
    in_southwest = net.add('object', 'in_southwest',
                           shape='box',
                           center=(-32.0, 4.6, 32.0),
                           size=(19.0, 9.2, 13.0),
                           color=(0.5, 0.5, 0.5))
    in_southwest.position = (940.0, 1100.0)
    output = net.add('output', 'output',
                     frames=180,
                     light_azimuth=212.0,
                     light_elevation=21.0,
                     light_color=(1.0, 0.78, 0.55),
                     light_intensity=4.2,
                     sky_color=(0.6, 0.66, 0.78),
                     sky_intensity=0.6,
                     ground_color=(0.2, 0.2, 0.2),
                     grid=False,
                     sky_behind=True)
    output.position = (1400.0, 60.0)
    camera = net.add('camera', 'camera', focal=32.0)
    camera.key('center', 1.0, (60.0, 75.0, 80.0), 'smooth')
    camera.key('center', 180.0, (50.0, 64.0, 67.0), 'smooth')
    camera.key('rotation', 1.0, (-35.2, 36.9, 0.0), 'smooth')
    camera.key('rotation', 180.0, (-34.9, 36.7, 0.0), 'smooth')
    camera.position = (1400.0, 200.0)
    net.connect(tower, seeds, output='geometry', input='input1')
    net.connect(tower, fracture, output='geometry', input='geometry')
    net.connect(seeds, fracture, output='geometry', input='points')
    net.connect(fracture, charges, output='geometry', input='geometry')
    net.connect(charges, rbd, output='geometry', input='pieces')
    net.connect(west, at_west, output='geometry', input='geometry')
    net.connect(at_west, city, output='geometry', input='geometry')
    net.connect(east, at_east, output='geometry', input='geometry')
    net.connect(at_east, city, output='geometry', input='geometry')
    net.connect(north, at_north, output='geometry', input='geometry')
    net.connect(at_north, city, output='geometry', input='geometry')
    net.connect(northwest, at_northwest, output='geometry', input='geometry')
    net.connect(at_northwest, city, output='geometry', input='geometry')
    net.connect(northeast, at_northeast, output='geometry', input='geometry')
    net.connect(at_northeast, city, output='geometry', input='geometry')
    net.connect(south, at_south, output='geometry', input='geometry')
    net.connect(at_south, city, output='geometry', input='geometry')
    net.connect(southeast, at_southeast, output='geometry', input='geometry')
    net.connect(at_southeast, city, output='geometry', input='geometry')
    net.connect(southwest, at_southwest, output='geometry', input='geometry')
    net.connect(at_southwest, city, output='geometry', input='geometry')
    net.connect(far_north, at_far_north, output='geometry', input='geometry')
    net.connect(at_far_north, city, output='geometry', input='geometry')
    net.connect(far_northwest, at_far_northwest, output='geometry', input='geometry')
    net.connect(at_far_northwest, city, output='geometry', input='geometry')
    net.connect(far_northeast, at_far_northeast, output='geometry', input='geometry')
    net.connect(at_far_northeast, city, output='geometry', input='geometry')
    net.connect(corner_northwest, at_corner_northwest, output='geometry', input='geometry')
    net.connect(at_corner_northwest, city, output='geometry', input='geometry')
    net.connect(corner_northeast, at_corner_northeast, output='geometry', input='geometry')
    net.connect(at_corner_northeast, city, output='geometry', input='geometry')
    net.connect(far_west, at_far_west, output='geometry', input='geometry')
    net.connect(at_far_west, city, output='geometry', input='geometry')
    net.connect(far_west_north, at_far_west_north, output='geometry', input='geometry')
    net.connect(at_far_west_north, city, output='geometry', input='geometry')
    net.connect(far_west_south, at_far_west_south, output='geometry', input='geometry')
    net.connect(at_far_west_south, city, output='geometry', input='geometry')
    net.connect(far_east_north, at_far_east_north, output='geometry', input='geometry')
    net.connect(at_far_east_north, city, output='geometry', input='geometry')
    net.connect(far_south_west, at_far_south_west, output='geometry', input='geometry')
    net.connect(at_far_south_west, city, output='geometry', input='geometry')
    net.connect(corner_southwest, at_corner_southwest, output='geometry', input='geometry')
    net.connect(at_corner_southwest, city, output='geometry', input='geometry')
    net.connect(kerb_tower, pavement, output='geometry', input='geometry')
    net.connect(pavement, city, output='geometry', input='geometry')
    net.connect(rbd, dust, output='dust', input='sources')
    net.connect(rbd, dust, output='collider', input='colliders')
    net.connect(billows, dust, output='force', input='forces')
    net.connect(in_west, dust, output='collider', input='colliders')
    net.connect(in_east, dust, output='collider', input='colliders')
    net.connect(in_north, dust, output='collider', input='colliders')
    net.connect(in_northwest, dust, output='collider', input='colliders')
    net.connect(in_northeast, dust, output='collider', input='colliders')
    net.connect(in_south, dust, output='collider', input='colliders')
    net.connect(in_southeast, dust, output='collider', input='colliders')
    net.connect(in_southwest, dust, output='collider', input='colliders')
    net.connect(dust, dust_look, output='gas', input='gas')
    net.connect(rbd, output, output='look', input='look')
    net.connect(dust_look, output, output='look', input='look')
    net.connect(camera, output, output='camera', input='camera')
    city.display()
    return net


def main():
    parser = argparse.ArgumentParser(description="The demolition of step 2, from Python.")
    parser.add_argument("--frames", type=int, help="how many frames (the Output's 180 by default)")
    parser.add_argument("--out", default="out", help="where the cache and the USD go")
    parser.add_argument("--video", help="then draw it: out/demolition.mp4, .avi, .png...")
    args = parser.parse_args()

    net = build()
    net.folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sim")
    for level, node, message in net.problems():
        print(f"{level}: {node.name + ': ' if node else ''}{message}", file=sys.stderr)
    sim = net.simulate()
    frames = args.frames or sim.frames
    cache = os.path.join(args.out, "demolition_cache")
    started = time.time()
    with pg.UsdExport(os.path.join(args.out, "demolition.usda"), sim) as usd:
        for frame in sim.run(frames):
            frame.save(cache)
            usd.add()
            if frame.number % 10 == 0 or frame.number == frames:
                rigid = frame.rigid
                print(f"frame {frame.number}: {rigid.broken} of {rigid.joints} joints broken, "
                      f"{len(rigid.vanished)} bodies to dust, {len(rigid.grit)} bits of grit "
                      f"({time.time() - started:.0f} s)")
    sim.write_cache_info(cache)
    print(f"wrote {usd.path}: {usd.bodies} bodies, {usd.frame_files} layers, {usd.gas_files} VDB files")
    if args.video:
        print(net.render(args.video, frames=frames, from_cache=cache).strip())


if __name__ == "__main__":
    main()
