"""Prototype from Python: networks, geometry, simulations, caches and USD.

    import pg

    net = pg.Network()
    box = net.add("box", size=(2, 6, 2), center=(0, 3, 0))
    pieces = net.add("voronoi_fracture", count=40)
    box.connect(pieces)
    pieces.display()
    geo = pieces.geometry()
    geo.points["P"]            # numpy array (N, 3), no copy
    geo.prims["piece"]         # which piece each face is of
    net.save("tower.pgsim")    # opens in the editor

A network is what the editor and the .pgsim files hold: nodes with
parameters, linked output to input. Geometry nodes cook to Geometry, whose
attributes come as numpy arrays over the core's own memory -- read only,
as the core shares it between geometries; set_attribute() and the tables'
item assignment copy new values in. A Simulation steps a network frame by
frame (or reads the frames back from a cache); UsdExport and AbcExport write
the shot to USD and to Alembic as it goes, and UsdStage reads USD -- a
matchmove's camera, a set -- with the program's own reader. Pictures and videos are drawn by the program `prototype`
(Network.render), so the module itself needs no OpenGL.

Without numpy the arrays are memoryviews of the same memory.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from collections.abc import Mapping

from . import _pg
from ._pg import Error

try:
    import numpy as _np
except ImportError:  # the arrays are memoryviews then
    _np = None

try:
    from . import _build
except ImportError:  # the package copied out of its build
    _build = None

__all__ = [
    "Error", "Network", "Node", "Geometry", "Simulation", "Frame", "UsdExport", "AbcExport", "UsdStage", "UsdPrim",
    "node_types", "node_type", "examples", "assets", "load_assets", "read_picture", "write_picture", "run",
]


def _array(a):
    """The core's memory as a numpy array without a copy, or a memoryview."""
    if a is None:
        return None
    return _np.asarray(a) if _np is not None else memoryview(a)


# --- node types --------------------------------------------------------------------------------

def node_types(category=None):
    """Every node type -- name, label, category, help, inputs, outputs,
    params -- or those of one category ("Geometry", "Simulation", ...)."""
    types = _pg.node_types()
    return [t for t in types if category is None or t["category"] == category]


def node_type(name):
    """One node type by name, as node_types() describes it."""
    for t in _pg.node_types():
        if t["name"] == name:
            return t
    raise Error(f"no node type '{name}'")


def examples(category=None):
    """The example networks the program carries: Network.example(name) opens one.
    With a category -- one of example_categories() -- only its examples."""
    return list(_pg.examples() if category is None else _pg.examples_in(category))


def example_categories():
    """What the examples are for, in the order the editor lists them: "Fire and smoke", "Wind", "Water"..."""
    return list(_pg.example_categories())


def example_category(name):
    """The category of an example; "Other" for one not given any."""
    return _pg.example_category(name)


def assets():
    """The digital assets known -- those the program carries, those of
    $PROTOTYPE_ASSETS and the user's folder, those loaded: (name, version,
    file) each. Their instances are nodes of their name."""
    return list(_pg.assets())


def load_assets(folder):
    """Every .pgasset of a folder; returns how many were read, and why the
    others were not."""
    return _pg.load_assets(os.fspath(folder))


def read_picture(path):
    """A picture file -- PNG, JPEG or OpenEXR -- read by the program's own
    readers, the ones a plate goes through: its pixels as rows x columns x
    RGBA floats (a numpy array where numpy is installed), and whether they
    are linear light (EXR) or as shown, sRGB (PNG, JPEG)."""
    pixels, linear = _pg.read_picture(os.fspath(path))
    return _array(pixels), linear


def write_picture(path, pixels, quality=92):
    """Pixels to a picture file, the kind its name says -- what read_picture
    reads: rows x columns (x 1 to 4 channels: grey, grey and alpha, RGB,
    RGBA) of floats, or of bytes 0 to 255. PNG and JPEG get them as shown,
    0 to 1 in 8 bits (a JPEG at `quality`, 1 to 100, and without alpha);
    OpenEXR in linear light, as half floats."""
    _pg.write_picture(os.fspath(path), pixels, int(quality))


# --- nodes -------------------------------------------------------------------------------------

class Node:
    """A node of a network: its parameters by item (node["size"]), its links,
    its flags. Cheap: the network holds the node; this names it."""

    __slots__ = ("network", "id")

    def __init__(self, network, id):
        self.network = network
        self.id = id

    @property
    def _n(self):
        return self.network._net

    @property
    def name(self):
        return self._n.name(self.id)

    @name.setter
    def name(self, value):
        self._n.rename(self.id, value)

    @property
    def type(self):
        return self._n.type(self.id)

    @property
    def help(self):
        return node_type(self.type)["help"]

    # parameters
    def __getitem__(self, param):
        return self._n.get(self.id, param)

    def __setitem__(self, param, value):
        self._n.set(self.id, param, value)

    def set(self, **params):
        """Several parameters at once; returns the node."""
        for name, value in params.items():
            self._n.set(self.id, name, value)
        return self

    def params(self):
        """The parameters' names, as the editor lists them."""
        return list(self._n.param_names(self.id))

    def param(self, name):
        """What a parameter is: kind, default, range, unit, help, choices."""
        return self._n.param_info(self.id, name)

    def value(self, param, frame):
        """A parameter at a frame: its keys' value there when it is animated."""
        return self._n.get_at(self.id, param, float(frame))

    def reset(self, param):
        self._n.reset(self.id, param)

    def expression(self, channel, text=None):
        """The expression on a channel ("size.y", "count"), or sets it:
        $F, $T, ch("../box1/sizex")... An empty text takes it off."""
        if text is None:
            return self._n.expression(self.id, channel)
        self._n.set_expression(self.id, channel, text)
        return self

    def key(self, param, frame, value, interp="smooth"):
        """A key on a parameter at a frame (smooth, linear or step)."""
        self._n.set_key(self.id, param, float(frame), value, interp)
        return self

    def keys(self, param):
        """The keys of a parameter: (frame, value, interp) each."""
        return list(self._n.keys(self.id, param))

    def clear_keys(self, param):
        self._n.clear_keys(self.id, param)

    # links
    @property
    def inputs(self):
        """The inputs: (name, type, takes many links) each."""
        return list(self._n.inputs(self.id))

    @property
    def outputs(self):
        return list(self._n.outputs(self.id))

    def connect(self, other, output=None, input=None):
        """Links an output of this node to an input of `other` -- the first
        that fit, unless named -- and returns `other`: a.connect(b).connect(c)."""
        self.network.connect(self, other, output=output, input=input)
        return other

    # flags
    def display(self):
        """Shows this node's geometry: in the viewport, the renders, the export."""
        self._n.set_display(self.id)
        return self

    @property
    def displayed(self):
        return self._n.displayed() == self.id

    @property
    def bypass(self):
        return self._n.bypass(self.id)

    @bypass.setter
    def bypass(self, on):
        self._n.set_bypass(self.id, bool(on))

    @property
    def position(self):
        """Where the editor draws it."""
        return self._n.position(self.id)

    @position.setter
    def position(self, xy):
        self._n.set_position(self.id, float(xy[0]), float(xy[1]))

    def geometry(self, frame=1):
        """The node's geometry cooked at a frame."""
        return self.network.cook(self, frame)

    def __eq__(self, other):
        return isinstance(other, Node) and other.network is self.network and other.id == self.id

    def __hash__(self):
        return hash((id(self.network), self.id))

    def __repr__(self):
        return f"<pg.Node {self.name} ({self.type})>"


# --- networks ----------------------------------------------------------------------------------

class Network:
    """The nodes of a network, as the editor and the .pgsim files hold them."""

    def __init__(self, _net=None):
        self._net = _net if _net is not None else _pg.Network()

    @classmethod
    def load(cls, path):
        """A .pgsim file; its relative paths (meshes) are read beside it."""
        with open(path, encoding="utf-8") as f:
            text = f.read()
        return cls(_pg.Network.from_text(text, os.path.dirname(os.path.abspath(path))))

    @classmethod
    def from_text(cls, text, folder=""):
        return cls(_pg.Network.from_text(text, folder))

    @classmethod
    def example(cls, name):
        """One of examples(): the demolition, rain_pond, campfire..."""
        return cls(_pg.Network.example(name))

    def text(self):
        """As a .pgsim file has it."""
        return self._net.text()

    def save(self, path):
        """To a .pgsim file the editor opens. Returns the path."""
        folder = os.path.dirname(os.path.abspath(path))
        os.makedirs(folder, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(self._net.text())
        if not self._net.folder:
            self._net.folder = folder
        return path

    @property
    def folder(self):
        """Where its relative paths (meshes, OBJ files) are read from."""
        return self._net.folder

    @folder.setter
    def folder(self, path):
        self._net.folder = os.fspath(path)

    @property
    def warnings(self):
        """What loading it said: parameters and links dropped."""
        return list(self._net.warnings)

    # nodes
    def add(self, type, name=None, **params):
        """A node of a type (node_types() has them), its parameters set."""
        node = Node(self, self._net.add(type, name or ""))
        node.set(**params)
        return node

    def remove(self, node):
        self._net.remove(self._id(node))

    def node(self, name):
        """The node of that name; KeyError if none."""
        id = self._net.find(name)
        if not id:
            raise KeyError(name)
        return Node(self, id)

    def nodes(self, type=None):
        """Every node -- or those of a type -- in the order they were added."""
        all = [Node(self, id) for id in self._net.ids()]
        return [n for n in all if type is None or n.type == type]

    def __getitem__(self, name):
        return self.node(name)

    def __contains__(self, name):
        return bool(self._net.find(name))

    def __iter__(self):
        return iter(self.nodes())

    def __len__(self):
        return len(self._net.ids())

    def _id(self, node):
        if isinstance(node, Node):
            if node.network is not self:
                raise Error(f"{node.name} is a node of another network")
            return node.id
        if isinstance(node, str):
            return self.node(node).id
        return int(node)

    # links
    def connect(self, a, b, output=None, input=None):
        """Links an output of `a` to an input of `b`: those named, or the
        first pair of the same type (an input without a link first)."""
        a, b = self._id(a), self._id(b)
        outs = self._net.outputs(a)
        ins = self._net.inputs(b)
        if output is not None:
            outs = [o for o in outs if o[0] == output]
            if not outs:
                raise Error(f"{self._net.name(a)} has no output '{output}'")
        if input is not None:
            ins = [i for i in ins if i[0] == input]
            if not ins:
                raise Error(f"{self._net.name(b)} has no input '{input}'")
        linked = {(l[2], l[3]) for l in self._net.links()}
        pairs = [(o, i) for o in outs for i in ins if o[1] == i[1]]
        pairs.sort(key=lambda p: (b, p[1][0]) in linked and not p[1][2])
        if not pairs:
            raise Error(f"nothing of {self._net.name(a)} fits into {self._net.name(b)}: "
                        f"outputs {[o[0] + ':' + o[1] for o in self._net.outputs(a)]}, "
                        f"inputs {[i[0] + ':' + i[1] for i in self._net.inputs(b)]}")
        (o, i) = pairs[0]
        self._net.connect(a, o[0], b, i[0])

    def disconnect(self, a, b, output=None, input=None):
        """Takes off the links from `a` into `b` (those of the pins named)."""
        a, b = self._id(a), self._id(b)
        gone = 0
        for (f, o, t, i) in self._net.links():
            if f == a and t == b and output in (None, o) and input in (None, i):
                gone += bool(self._net.disconnect(f, o, t, i))
        return gone

    def links(self):
        """(from node, output, to node, input) each."""
        return [(Node(self, f), o, Node(self, t), i) for (f, o, t, i) in self._net.links()]

    # what it makes
    @property
    def displayed(self):
        id = self._net.displayed()
        return Node(self, id) if id else None

    @displayed.setter
    def displayed(self, node):
        self._net.set_display(self._id(node) if node is not None else 0)

    def problems(self):
        """What the network says of itself as it compiles: (level, node or
        None, message) each -- errors keep it from simulating."""
        ok, found = self._net.problems()
        return [(level, Node(self, id) if id else None, message) for (level, id, message) in found]

    @property
    def fps(self):
        return 1.0 / self._net.time_step

    def cook(self, node, frame=1):
        """The geometry of a geometry node at a frame. Only what changed since
        the last cook cooks again."""
        return Geometry(self._net.cook(self._id(node), int(frame)))

    def simulate(self, cache=None, preview=1.0):
        """A Simulation of the network as it is now; `cache`: read its frames
        from a cache folder instead of simulating; `preview`: the gas and the
        water on grids that much as fine (0.5: half), to work on quickly."""
        return Simulation(self, cache=cache, preview=preview)

    def layout(self, spacing=(240.0, 120.0)):
        """Places the nodes for the editor: each a column further right than
        what feeds it, top to bottom in the order they were added."""
        depth = {}
        ids = self._net.ids()
        feeds = {}
        for (f, _, t, _) in self._net.links():
            feeds.setdefault(t, []).append(f)

        def level(id, seen=()):
            if id in depth:
                return depth[id]
            d = 0
            for f in feeds.get(id, []):
                if f not in seen:
                    d = max(d, level(f, seen + (id,)) + 1)
            depth[id] = d
            return d

        rows = {}
        for id in ids:
            column = level(id)
            row = rows.get(column, 0)
            rows[column] = row + 1
            self._net.set_position(id, column * spacing[0], row * spacing[1])
        return self

    def as_code(self, function="build"):
        """Python that builds this network again, node by node -- what
        Houdini's asCode() gives: a function `build()` that returns it."""
        import keyword
        names = {}
        for node in self.nodes():
            v = node.name if node.name.isidentifier() and not keyword.iskeyword(node.name) else f"n{node.id}"
            if v in ("net", "pg"):
                v += "_"
            names[node.id] = v
        lines = [f"def {function}():", "    net = pg.Network()"]
        after = []
        for node in self.nodes():
            v = names[node.id]
            params, extra, reserved = [], [], []
            for p in node.params():
                keys = self._net.keys(node.id, p)
                if keys:
                    for (frame, value, interp) in keys:
                        extra.append(f"    {v}.key({p!r}, {_code(frame)}, {_code(value)}, {interp!r})")
                elif not self._net.is_default(node.id, p):
                    if keyword.iskeyword(p):  # class=... is no keyword argument
                        reserved.append(f"{p!r}: {_code(node[p])}")
                    else:
                        params.append(f"{p}={_code(node[p])}")
                for channel in self._net.channels(node.id, p):
                    text = self._net.expression(node.id, channel)
                    if text:
                        extra.append(f"    {v}.expression({channel!r}, {_code(text)})")
            if reserved:
                params.append("**{" + ", ".join(reserved) + "}")
            head = f"    {v} = net.add({node.type!r}, {node.name!r}"
            one = head + "".join(", " + a for a in params) + ")"
            if len(one) <= 100 or not params:
                lines.append(one)
            else:
                # A parameter a line, under the first.
                indent = " " * len(f"    {v} = net.add(")
                lines.append(head + ",\n" + ",\n".join(indent + a for a in params) + ")")
            lines += extra
            if node.bypass:
                lines.append(f"    {v}.bypass = True")
            x, y = node.position
            lines.append(f"    {v}.position = ({_code(x)}, {_code(y)})")
        for (a, o, b, i) in self.links():
            after.append(f"    net.connect({names[a.id]}, {names[b.id]}, output={o!r}, input={i!r})")
        shown = self.displayed
        if shown is not None:
            after.append(f"    {names[shown.id]}.display()")
        return "\n".join(lines + after + ["    return net", ""])

    def render(self, path, **options):
        """Simulates the network and draws it with the program prototype:
        path .png (the last frame, or every=K), .exr (with passes), .mp4,
        .avi... Options as `prototype sim` has them: frames=, start=, every=,
        size="1920x1080", yaw=, pitch=, distance=, resolution=, guides=True,
        cache=, export=... renderer="path" (with samples=) renders through the
        path tracer, on the processor; renderer="cycles", through Cycles,
        Blender's renderer, where the build has it. Returns what it printed."""
        with tempfile.TemporaryDirectory() as tmp:
            network = os.path.join(tmp, "network.pgsim")
            with open(network, "w", encoding="utf-8") as f:
                f.write(self._net.text())
            args = ["sim", network, os.fspath(path)]
            if self._net.folder:
                args += ["--folder", self._net.folder]
            args += _options(options)
            return run(*args).stdout

    def __repr__(self):
        return f"<pg.Network of {len(self)} nodes>"


def _code(value):
    """A value as Python writes it: floats as short as their single precision
    reads back the same, code as a raw triple-quoted string."""
    import struct
    if isinstance(value, bool) or isinstance(value, int):
        return repr(value)
    if isinstance(value, float):
        exact = struct.pack("f", value)
        for digits in range(1, 10):
            text = f"{value:.{digits}g}"
            if struct.pack("f", float(text)) == exact:
                return repr(float(text))  # 130.0, not 1.3e+02
        return repr(value)
    if isinstance(value, tuple):
        return "(" + ", ".join(_code(x) for x in value) + ")"
    if isinstance(value, str):
        if "\n" in value and "\'\'\'" not in value and not value.endswith("\\") and not value.endswith("'"):
            return "r\'\'\'" + value + "\'\'\'"
        return repr(value)
    return repr(value)


def _options(options):
    """Keyword options as the command line writes them: every=5 -> --every 5."""
    out = []
    for key, value in options.items():
        flag = "--" + key.replace("_", "-")
        if value is True:
            out.append(flag)
        elif value is False or value is None:
            continue
        elif isinstance(value, (list, tuple)) and key == "set":
            for v in value:
                out += ["--set", str(v)]
        else:
            out += [flag, str(value)]
    return out


def run(*args, check=True):
    """Runs the program prototype with these arguments -- run("sim",
    "rain_pond", "pond.png") -- and returns its CompletedProcess; raises
    pg.Error with what it said when it fails (unless check=False)."""
    program = os.environ.get("PG_PROTOTYPE") or (_build.PROTOTYPE if _build else "prototype")
    done = subprocess.run([program, *map(os.fspath, args)], capture_output=True, text=True)
    if check and done.returncode != 0:
        raise Error(f"prototype {' '.join(map(str, args))}: {done.stderr.strip() or done.stdout.strip()}")
    return done


# --- geometry ----------------------------------------------------------------------------------

class Attributes(Mapping):
    """The attributes of one class -- points, vertices, primitives or the
    detail -- by name: numpy arrays without a copy (lists of str for
    strings). Assigning copies the values in: geo.points["Cd"] = colors."""

    def __init__(self, geometry, cls):
        self._geo = geometry
        self._cls = cls

    def __getitem__(self, name):
        value = self._geo._g.attribute(self._cls, name)
        if value is None:
            raise KeyError(name)
        return value if isinstance(value, list) else _array(value)

    def __setitem__(self, name, values):
        self._geo._g.set_attribute(self._cls, name, values)

    def set(self, name, values, type=""):
        """With the type said: int, float, vector2, vector3, vector4, string."""
        self._geo._g.set_attribute(self._cls, name, values, type)

    def __delitem__(self, name):
        if not self._geo._g.remove_attribute(self._cls, name):
            raise KeyError(name)

    def __iter__(self):
        return iter(self._geo._g.attribute_names(self._cls))

    def __len__(self):
        return len(self._geo._g.attribute_names(self._cls))

    def type(self, name):
        """int, float, vector2, vector3, vector4 or string."""
        return self._geo._g.attribute_type(self._cls, name)

    def __repr__(self):
        return f"<pg attributes of the {self._cls}s: {', '.join(self)}>"


class Volume:
    """A dense grid of values: values[i, j, k] is voxel (i, j, k), whose
    middle is at origin + (i + 0.5, j + 0.5, k + 0.5) * voxel."""

    def __init__(self, d):
        self.name = d["name"]
        self.origin = d["origin"]
        self.voxel = d["voxel"]
        self.resolution = d["resolution"]
        self.values = _array(d["values"])

    def __repr__(self):
        return f"<pg.Volume {self.name} {self.resolution[0]} x {self.resolution[1]} x {self.resolution[2]}>"


class Geometry:
    """Points, primitives, their attributes and groups, and volumes."""

    def __init__(self, _g=None):
        self._g = _g if _g is not None else _pg.Geometry()

    @classmethod
    def load(cls, path):
        """From .obj or .ply."""
        return cls(_pg.Geometry.load(os.fspath(path)))

    def save(self, path):
        """To .ply, .obj, .vdb (its volumes) or .usda, as the extension says."""
        self._g.save(os.fspath(path))
        return path

    def copy(self):
        return Geometry(self._g.copy())

    @property
    def point_count(self):
        return self._g.point_count

    @property
    def vertex_count(self):
        return self._g.vertex_count

    @property
    def primitive_count(self):
        return self._g.primitive_count

    @property
    def points(self):
        return Attributes(self, "point")

    @property
    def vertices(self):
        return Attributes(self, "vertex")

    @property
    def prims(self):
        return Attributes(self, "primitive")

    @property
    def detail(self):
        return Attributes(self, "detail")

    @property
    def P(self):
        """The points' positions, (N, 3)."""
        return self.points["P"]

    # topology
    def primitive(self, index):
        """The points of a primitive, corner by corner."""
        return self._g.primitive_points(index)

    def topology(self):
        """(sizes, points): each primitive's corner count, and each corner's
        point -- as USD's faceVertexCounts and faceVertexIndices. No copies."""
        return _array(self._g.primitive_sizes()), _array(self._g.vertex_points())

    def primitive_starts(self):
        """Each primitive's first corner."""
        return _array(self._g.primitive_starts())

    def closed(self):
        """1 for a closed primitive (a polygon), 0 for an open one (a line)."""
        return _array(self._g.primitive_closed())

    # building
    def add_points(self, positions):
        """Points at positions (N, 3) -- or a count of them at the origin.
        Returns the first one's number."""
        return self._g.add_points(positions)

    def add_polygons(self, sizes, points):
        """Closed primitives: `sizes` corners each, their points one after the other."""
        return self._g.add_primitives(sizes, points, True)

    def add_polylines(self, sizes, points):
        """Open primitives, as add_polygons."""
        return self._g.add_primitives(sizes, points, False)

    def append(self, other):
        self._g.append(other._g)
        return self

    # groups
    @property
    def groups(self):
        """The groups' names."""
        return list(self._g.group_names())

    def group(self, name):
        """(class, members): a flag each element of the class, 1 in the group."""
        found = self._g.group(name)
        if found is None:
            raise KeyError(name)
        cls, mask = found
        return cls, _array(mask)

    def set_group(self, name, cls, members):
        self._g.set_group(name, cls, members)

    # volumes
    @property
    def volumes(self):
        return [Volume(d) for d in self._g.volumes()]

    def volume(self, name):
        for v in self.volumes:
            if v.name == name:
                return v
        raise KeyError(name)

    def add_volume(self, name, values, origin=(0.0, 0.0, 0.0), voxel=0.1):
        """values[i, j, k] of a (nx, ny, nz) array."""
        self._g.add_volume(name, values, tuple(origin), float(voxel))

    # instances
    @property
    def prototypes(self):
        """What its instance points stand for: a Geometry each, held once
        however many points stand for it (Grass, Tree's Instances, Copy to
        Points' Instance)."""
        return [Geometry(g) for g in self._g.prototypes()]

    @property
    def instance_count(self):
        """How many points stand for a prototype: their int attribute
        instance says which, P where, orient how it is turned, pscale how big,
        tint what its colours are multiplied by."""
        return self._g.instance_count

    def add_prototype(self, prototype):
        """Another prototype; returns its number, for the points' instance."""
        return self._g.add_prototype(prototype._g)

    def clear_prototypes(self):
        self._g.clear_prototypes()

    def unpack(self):
        """A copy with each instance made the geometry it stands for, as the
        Unpack node makes it."""
        return Geometry(self._g.unpack())

    # the whole of it
    def bounds(self):
        """((x, y, z) least, (x, y, z) most) of its points; None without any."""
        return self._g.bounds()

    def hash(self):
        """The same for the same content, whatever made it."""
        return self._g.hash()

    def __repr__(self):
        text = (f"<pg.Geometry {self.point_count} points, {self.primitive_count} primitives, "
                f"{self._g.volume_count} volumes")
        if self._g.prototype_count:
            text += f", {self._g.instance_count} instances of {self._g.prototype_count} prototypes"
        return text + ">"


# --- simulations -------------------------------------------------------------------------------

class Frame:
    """What a frame of a simulation holds -- the gas, the water, the rain,
    the rigid bodies -- as arrays without a copy."""

    def __init__(self, _f):
        self._f = _f

    @property
    def number(self):
        return self._f.number

    @property
    def time(self):
        return self._f.time

    # gas
    @property
    def has_gas(self):
        return self._f.has_gas

    def gas(self, field="density"):
        """density, temperature, flame or steam: [i, j, k], half floats."""
        return _array(self._f.gas(field))

    def gas_domain(self):
        return self._f.gas_domain()

    # water
    @property
    def has_water(self):
        return self._f.has_water

    @property
    def water(self):
        return _Water(self._f)

    # rain
    @property
    def has_rain(self):
        return self._f.has_rain

    @property
    def rain(self):
        return _Rain(self._f)

    # rigid bodies
    @property
    def has_rigid(self):
        return self._f.has_rigid

    @property
    def rigid(self):
        return _Rigid(self._f)

    def cloth(self):
        """The Cloth Solver's cloth where its points are, with the velocity v
        and normal N of each -- as Cloth Geometry gives it: torn, the points
        split off with their own's attributes, the faces on them, the ropes
        parted. Empty without cloth."""
        return Geometry(self._f.cloth())

    def grains(self):
        """The Grain Solver's grains as points -- as Grain Points gives them:
        P, the velocity v, pscale (the radius), id (the same from frame to
        frame), Cd and orient. Empty without grains."""
        return Geometry(self._f.grains())

    def save(self, folder):
        """Into a cache folder, as frame.NNNN.pgframe. Returns the file."""
        os.makedirs(folder, exist_ok=True)
        return self._f.save(os.fspath(folder))

    @classmethod
    def read(cls, folder, number):
        return cls(_pg.Frame.read(os.fspath(folder), int(number)))

    def __repr__(self):
        parts = [name for name, on in (("gas", self.has_gas), ("water", self.has_water),
                                       ("rain", self.has_rain), ("rigid", self.has_rigid)) if on]
        return f"<pg.Frame {self.number}: {', '.join(parts) or 'empty'}>"


class _Water:
    def __init__(self, f):
        self._f = f

    positions = property(lambda self: _array(self._f.water_positions()), doc="(N, 3) of the particles")
    velocities = property(lambda self: _array(self._f.water_velocities()), doc="(N, 3), half floats")
    foam = property(lambda self: _array(self._f.water_foam()), doc="0 to 255 a particle")
    ids = property(lambda self: _array(self._f.water_ids()), doc="each particle's own number")
    flow = property(lambda self: _array(self._f.water_flow()),
                    doc="the velocity on the solver's grid, [i, j, k, axis], half floats")
    particle_count = property(lambda self: self._f.water_particle_count)
    litres = property(lambda self: self._f.water_litres)

    def domain(self):
        return self._f.water_domain()

    def surface(self, ripples=True):
        """The water's surface as a closed mesh with N, v and foam (Liquid Surface)."""
        return Geometry(self._f.water_surface(ripples))


class _Rain:
    def __init__(self, f):
        self._f = f

    def _drops(self):
        return _array(self._f.rain_drops())

    def _droplets(self):
        return _array(self._f.rain_droplets())

    positions = property(lambda self: self._drops()[:, 0:3], doc="(N, 3) of the drops")
    velocities = property(lambda self: self._drops()[:, 3:6], doc="(N, 3) of the drops")
    ids = property(lambda self: _array(self._f.rain_drop_ids()))
    droplet_positions = property(lambda self: self._droplets()[:, 0:3])
    droplet_velocities = property(lambda self: self._droplets()[:, 3:6])
    droplet_ids = property(lambda self: _array(self._f.rain_droplet_ids()))

    def ripples(self):
        """{origin, cell, heights[i, k]} of the ripples on the water, or None."""
        r = self._f.ripples()
        if r is not None:
            r["heights"] = _array(r["heights"])
        return r


class _Rigid:
    def __init__(self, f):
        self._f = f

    centres = property(lambda self: _array(self._f.body_centres()),
                       doc="where each body's middle is now (of the box round it at rest), (B, 3)")
    velocities = property(lambda self: _array(self._f.body_centre_velocities()),
                          doc="how fast each body's middle goes, (B, 3)")
    spins = property(lambda self: _array(self._f.body_spins()),
                     doc="the axis each body turns about, as long as radians a second, (B, 3)")
    rotations = property(lambda self: _array(self._f.body_rotations()),
                         doc="how each body is turned from rest: quaternions x, y, z, w, (B, 4)")
    translations = property(lambda self: _array(self._f.body_translations()),
                            doc="the pose's move: a point p at rest is now at rotate(p) + translation, (B, 3)")
    vanished = property(lambda self: list(self._f.vanished()),
                        doc="the bodies gone: blown to dust, or broken into fragments")
    shatters = property(lambda self: self._f.shatters(),
                        doc="the pieces broken as they were knocked, in order: {body, at (where, at rest), seed, "
                            "count (fragments asked for), time (seconds)} -- the fragments are the bodies after "
                            "those there were")
    unglued = property(lambda self: list(self._f.unglued()),
                       doc="the bodies a joint of which has broken: come loose from one they were glued to")
    grit = property(lambda self: _array(self._f.grit()), doc="x, y, z, size of each bit, (N, 4)")
    grit_velocities = property(lambda self: _array(self._f.grit_velocities()))
    grit_ids = property(lambda self: _array(self._f.grit_ids()))
    grit_orient = property(lambda self: _array(self._f.grit_orient()),
                           doc="how each bit is turned: unit quaternions x, y, z, w, (N, 4)")
    grit_glass = property(lambda self: _array(self._f.grit_glass()),
                          doc="1 for each bit that is a chip of glass; empty when none is")
    body_count = property(lambda self: self._f.body_count)
    joints = property(lambda self: self._f.joints)
    broken = property(lambda self: self._f.broken)

    def pieces(self):
        """The pieces where they are, with the velocity v of each point."""
        return Geometry(self._f.pieces())

    def network(self):
        """The glue as a network where the bodies are -- as RBD Pieces gives it
        with output Constraints: a point at each body's middle, with v; a line
        for each joint that held, broken 1 where it broke, at time (seconds;
        -1 where it holds), red where broken."""
        return Geometry(self._f.network())

    joint_state = property(lambda self: _array(self._f.joint_state()),
                           doc="for each joint of the glue: 0 it holds, 1 it broke, 2 it never held")
    joint_time = property(lambda self: _array(self._f.joint_time()),
                          doc="when each joint broke, seconds (0 for those that did not)")

    def rebar(self):
        """The steel bars where the pieces have taken them: a polyline for each
        stretch of a bar in one piece -- torn apart at a tear -- with the point
        attributes width and v. Empty without bars."""
        return Geometry(self._f.rebar())

    rebar_state = property(lambda self: _array(self._f.rebar_state()),
                           doc="for each stretch of a bar in a body: 1 the bar slid out of it, 2 it tore after it")
    rebar_stations = property(lambda self: self._f.rebar_stations(),
                              doc="(body, in, out, bar) of each stretch of a bar in a body, metres along the bar")


class Simulation:
    """A network simulated a frame at a time -- or read from a cache. It
    takes the network as it is when made; later changes do not reach it."""

    def __init__(self, network, cache=None, preview=1.0):
        self.network = network
        self._s = _pg.Simulation(network._net, os.fspath(cache) if cache else "", float(preview))

    @property
    def frame(self):
        """The frame last stepped to; 0 before the first."""
        return self._s.frame

    @property
    def frames(self):
        """As many as the Output says (or the cache has)."""
        return self._s.frames

    @property
    def fps(self):
        return self._s.fps

    @property
    def problems(self):
        """Warnings of the compile: (level, node or None, message)."""
        return [(level, Node(self.network, id) if id else None, message) for (level, id, message) in self._s.problems]

    @property
    def current(self):
        f = self._s.current
        return Frame(f) if f is not None else None

    def step(self):
        """On to the next frame; returns it."""
        return Frame(self._s.step())

    def save_state(self):
        """A checkpoint: all it takes to go on from this frame as if it had
        never stopped, as bytes -- to a file, to another machine."""
        return self._s.save_state()

    def load_state(self, state):
        """Goes on from a checkpoint save_state() gave, of the same network,
        in a simulation that has not stepped yet: the next step is the frame
        after it. The pieces of an RBD Solver are stepped there again."""
        self._s.load_state(bytes(state))

    def run(self, frames=None):
        """Steps up to frame `frames` (the Output's count), yielding each frame."""
        last = self.frames if frames is None else int(frames)
        while self.frame < last:
            yield self.step()

    def geometry(self, node=None):
        """A geometry node's geometry at the current frame -- the displayed
        one's by default: Liquid Surface, RBD Pieces... see this frame."""
        if node is None:
            id = self._s.displayed
            if not id:
                raise Error("no node is displayed: name one")
        else:
            id = self.network._id(node)
        return Geometry(self._s.geometry(id))

    def camera(self, frame=None):
        """The Output's camera at the current frame (or `frame`) -- position,
        rotation (degrees about x, y, z), focal (mm), width, height -- or None."""
        return self._s.camera(frame or 0)

    def cache(self, folder, frames=None):
        """Simulates on, writing each frame to a cache folder (and cache.txt)."""
        for frame in self.run(frames):
            frame.save(folder)
        self.write_cache_info(folder)
        return folder

    def write_cache_info(self, folder):
        """cache.txt of a cache folder: the frames so far, the frame rate and
        the network -- what `prototype sim --from-cache` and Simulation(...,
        cache=) read."""
        os.makedirs(folder, exist_ok=True)
        self._s.write_cache_info(os.fspath(folder))

    def export_usd(self, path, frames=None, node="displayed"):
        """Simulates on, writing the shot to USD (docs/usd.md). `node`: the
        geometry node shown in it -- the displayed one, or None."""
        with UsdExport(path, self, node=node) as usd:
            for _ in self.run(frames):
                usd.add()
        return path

    def export_alembic(self, path, frames=None, node="displayed"):
        """Simulates on, writing the shot to one Alembic archive
        (docs/alembic.md). `node`: the geometry node in it -- the displayed
        one, or None."""
        with AbcExport(path, self, node=node) as abc:
            for _ in self.run(frames):
                abc.add()
        return path

    def __repr__(self):
        return f"<pg.Simulation at frame {self.frame} of {self.frames}>"


class UsdExport:
    """The shot to USD a frame at a time: add() after each step, finish() at
    the end -- or as a context:

        with pg.UsdExport("shot.usda", sim) as usd:
            for frame in sim.run():
                usd.add()
    """

    def __init__(self, path, simulation, node="displayed"):
        if node == "displayed":
            node = simulation._s.displayed or None
        id = simulation.network._id(node) if node is not None else 0
        folder = os.path.dirname(os.path.abspath(path))
        os.makedirs(folder, exist_ok=True)
        self._u = _pg.UsdExport(os.fspath(path), simulation._s, id)
        self.path = path

    def add(self):
        """The simulation's current frame."""
        self._u.add()

    def finish(self):
        return self._u.finish()

    frames = property(lambda self: self._u.frames)
    bodies = property(lambda self: self._u.bodies)
    frame_files = property(lambda self: self._u.frame_files)
    gas_files = property(lambda self: self._u.gas_files)

    def __enter__(self):
        return self

    def __exit__(self, kind, value, trace):
        if kind is None:
            self.finish()
        return False


class AbcExport:
    """The shot to Alembic a frame at a time: add() after each step, finish()
    at the end -- or as a context:

        with pg.AbcExport("shot.abc", sim) as abc:
            for frame in sim.run():
                abc.add()
    """

    def __init__(self, path, simulation, node="displayed"):
        if node == "displayed":
            node = simulation._s.displayed or None
        id = simulation.network._id(node) if node is not None else 0
        folder = os.path.dirname(os.path.abspath(path))
        os.makedirs(folder, exist_ok=True)
        self._a = _pg.AbcExport(os.fspath(path), simulation._s, id)
        self.path = path

    def add(self):
        """The simulation's current frame."""
        self._a.add()

    def finish(self):
        return self._a.finish()

    frames = property(lambda self: self._a.frames)
    bodies = property(lambda self: self._a.bodies)
    gas_files = property(lambda self: self._a.gas_files)

    def __enter__(self):
        return self

    def __exit__(self, kind, value, trace):
        if kind is None:
            self.finish()
        return False


class UsdPrim:
    """A prim of a UsdStage: its path, type, and its values at a time."""

    def __init__(self, stage, path, type, defined, active, specifier):
        self.stage = stage
        self.path = path
        self.type = type
        self.defined = defined
        self.active = active
        self.specifier = specifier

    @property
    def name(self):
        return self.path.rsplit("/", 1)[-1]

    @property
    def children(self):
        return [self.stage.prim(p) for p in self.stage._s.children(self.path)]

    def properties(self):
        """The names of its attributes and relationships."""
        return self.stage._s.property_names(self.path)

    def get(self, name, time=None):
        """An attribute's value at `time` (a time code; frame 1's when None)
        -- numbers, tuples, lists of them, text -- or None where it has none."""
        return self.stage._s.value(self.path, name, self.stage._time(time))

    def varies(self, name):
        return self.stage._s.varies(self.path, name)

    def sample_times(self, name):
        """The time codes an attribute has samples at, in order -- value
        clips' too; empty for one that holds a single value."""
        return self.stage._s.sample_times(self.path, name)

    def targets(self, name):
        """A relationship's targets, as paths of the stage."""
        return self.stage._s.targets(self.path, name)

    def metadata(self, key):
        return self.stage._s.metadata(self.path, key)

    def world(self, time=None):
        """Its transform to the world at `time`, 4 x 4, rows (p x M), in the
        stage's own units and axes."""
        m = self.stage._s.world(self.path, self.stage._time(time))
        return _np.array(m) if _np is not None else m

    def local(self, time=None):
        m, resets = self.stage._s.local(self.path, self.stage._time(time))
        return _np.array(m) if _np is not None else m

    def __repr__(self):
        return f"<pg.UsdPrim {self.path} {self.type or '(no type)'}>"


class UsdStage:
    """A USD file (.usd, .usda, .usdc, .usdz) as the stage it composes to --
    sublayers, references, payloads, variants, value clips -- read by the
    program's own reader: no pxr needed.

        stage = pg.UsdStage("shot.usd")
        cam = stage.camera()                  # the first camera, frame 1
        set_ = stage.geometry(prims=["/World/set"])
    """

    def __init__(self, path):
        self.path = os.fspath(path)
        self._s = _pg.UsdStage.open(self.path)
        self.notes = []  # what the last geometry() did not read, and why

    meters_per_unit = property(lambda self: self._s.meters_per_unit)
    up_axis = property(lambda self: self._s.up_axis)
    start_time_code = property(lambda self: self._s.start_time_code)
    end_time_code = property(lambda self: self._s.end_time_code)
    has_time_range = property(lambda self: self._s.has_time_range)
    time_codes_per_second = property(lambda self: self._s.time_codes_per_second)
    default_prim = property(lambda self: self._s.default_prim)
    warnings = property(lambda self: self._s.warnings)
    files = property(lambda self: self._s.files)

    def _time(self, time):
        return self.time_code(1) if time is None else float(time)

    def time_code(self, frame, fps=None, offset=0.0):
        """The time code the program's frame `frame` reads (USD Import, USD
        Camera): frame 1 is the start time code, frames at `fps` (the
        stage's own time codes per second when None)."""
        start = self.start_time_code if self.has_time_range else 1.0
        rate = self.time_codes_per_second / fps if fps else 1.0
        return start + (frame - 1 + offset) * rate

    def prims(self, type=None, all=False):
        """Its prims in the order of the tree: the defined ones (all=True:
        overs, classes and inactive ones too), of `type` when given."""
        out = []
        for path, t, defined, active, spec in self._s.prims():
            if (all or defined) and (type is None or t == type):
                out.append(UsdPrim(self, path, t, defined, active, spec))
        return out

    def prim(self, path):
        for p, t, defined, active, spec in self._s.prims():
            if p == path:
                return UsdPrim(self, p, t, defined, active, spec)
        raise Error(f"no prim {path} on the stage")

    def cameras(self):
        return [self.prim(p) for p in self._s.cameras()]

    def camera(self, path=None, time=None, metres=True):
        """A camera's world matrix (rows; metres, Y up unless metres=False),
        focal length, apertures and their offsets, clipping range, focus
        distance, f-stop, whether it is orthographic and whether it moves."""
        if path is None:
            cams = self._s.cameras()
            if not cams:
                raise Error(f"no camera in {self.path}")
            path = cams[0]
        c = self._s.camera(path, self._time(time), metres)
        if _np is not None:
            c["world"] = _np.array(c["world"])
        c["path"] = path
        return c

    def geometry(self, time=None, prims=(), render=True, proxy=False, guide=False, metres=True,
                 subsets=True, path_attribute=True, materials=True, subdivision=2):
        """Its geometry at `time`, in the world -- what USD Import makes of it:
        meshes, curves, points, the implicit shapes as polygons, the
        instances of PointInstancers as instances (prototypes, and points
        with instance, orient, pscale), the fields of Volumes as volumes
        (density, vel.x...), under `prims` (all when empty),
        their materials as the program's (material, texture, roughness,
        metallic...); the subdivision surfaces smooth, their faces cut in
        four `subdivision` times (0: the coarse mesh), creaseweight and
        cornerweight as sharp as the file has them. What it could not read
        is in self.notes."""
        g, notes = self._s.geometry(self._time(time), list(prims), render, proxy, guide, metres, subsets,
                                    path_attribute, materials, subdivision)
        self.notes = notes
        return Geometry(g)

    def geometry_varies(self, prims=(), render=True, proxy=False, guide=False):
        return self._s.geometry_varies(list(prims), render, proxy, guide)

    def __repr__(self):
        return f"<pg.UsdStage {self.path}>"
