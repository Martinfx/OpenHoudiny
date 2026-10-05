# How looks.usda was made: props bound to materials as other programs write
# them -- MaterialX, UsdPreviewSurface, OpenPBR -- on the pictures of
# examples/textures, for USD Import to read (docs/usd-import.md, Materials).
#
#   pip install usd-core
#   python examples/usd/make_looks.py
import math
import os

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

HERE = os.path.dirname(os.path.abspath(__file__))
stage = Usd.Stage.CreateNew(os.path.join(HERE, "looks.usda"))
UsdGeom.SetStageUpAxis(stage, "Y")
UsdGeom.SetStageMetersPerUnit(stage, 1.0)
world = UsdGeom.Xform.Define(stage, "/World")
stage.SetDefaultPrim(world.GetPrim())


def mesh(path, points, counts, indices, st=None, st_interpolation="faceVarying"):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr([Gf.Vec3f(*p) for p in points])
    m.CreateFaceVertexCountsAttr(counts)
    m.CreateFaceVertexIndicesAttr(indices)
    m.CreateSubdivisionSchemeAttr("none")
    if st is not None:
        pv = UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, st_interpolation)
        pv.Set([Gf.Vec2f(*t) for t in st])
    return m


def box(path, centre, size, turn=0.0):
    h = [s / 2 for s in size]
    c = (0.0, 0.0, 0.0) if turn else centre
    corners = [(c[0] + sx * h[0], c[1] + sy * h[1], c[2] + sz * h[2]) for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    st = [(0, 0), (1, 0), (1, 1), (0, 1)] * 6
    m = mesh(path, corners, [4] * 6, [i for f in faces for i in f], st)
    if turn:
        m.AddTranslateOp().Set(Gf.Vec3d(*centre))
        m.AddRotateYOp().Set(turn)
    return m


def sphere(path, centre, radius, rows=16, columns=32):
    points, normals, st = [], [], []
    for r in range(rows + 1):
        a = math.pi * r / rows - math.pi / 2
        for k in range(columns + 1):
            b = 2 * math.pi * k / columns
            n = (math.cos(a) * math.cos(b), math.sin(a), -math.cos(a) * math.sin(b))
            points.append(tuple(round(centre[k] + radius * n[k], 4) for k in range(3)))
            normals.append(tuple(round(x, 4) for x in n))
            st.append((round(k / columns, 4), round(r / rows, 4)))
    counts, indices = [], []
    for r in range(rows):
        for k in range(columns):
            a = r * (columns + 1) + k
            counts.append(4)
            indices += [a, a + 1, a + columns + 2, a + columns + 1]
    m = mesh(path, points, counts, indices, st, "vertex")
    m.CreateNormalsAttr([Gf.Vec3f(*n) for n in normals])
    m.SetNormalsInterpolation("vertex")
    return m


def material(name):
    return UsdShade.Material.Define(stage, f"/World/Looks/{name}")


def shader(m, name, id):
    s = UsdShade.Shader.Define(stage, m.GetPath().AppendChild(name))
    s.CreateIdAttr(id)
    return s


def bind(prim, m):
    UsdShade.MaterialBindingAPI.Apply(prim).Bind(m)


UsdGeom.Scope.Define(stage, "/World/Looks")

# A crate of wood: MaterialX's standard_surface, its colour and normal map
# from pictures.
wood = material("Wood")
surface = shader(wood, "Surface", "ND_standard_surface_surfaceshader")
picture = shader(wood, "Picture", "ND_image_color3")
picture.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("../textures/wood/color.jpg")
picture.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
bumps = shader(wood, "NormalPicture", "ND_image_vector3")
bumps.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("../textures/wood/normal.jpg")
bumps.CreateOutput("out", Sdf.ValueTypeNames.Float3)
normal = shader(wood, "Normal", "ND_normalmap")
normal.CreateInput("in", Sdf.ValueTypeNames.Float3).ConnectToSource(bumps.ConnectableAPI(), "out")
normal.CreateOutput("out", Sdf.ValueTypeNames.Float3)
surface.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).ConnectToSource(picture.ConnectableAPI(), "out")
surface.CreateInput("normal", Sdf.ValueTypeNames.Float3).ConnectToSource(normal.ConnectableAPI(), "out")
surface.CreateInput("specular_roughness", Sdf.ValueTypeNames.Float).Set(0.65)
wood.CreateSurfaceOutput("mtlx").ConnectToSource(surface.ConnectableAPI(), "out")
bind(box("/World/crate", (-1.3, 0.4, 0.15), (0.8, 0.8, 0.8)).GetPrim(), wood)

# A ball of bricks: UsdPreviewSurface with UsdUVTexture and its normal map.
bricks = material("Bricks")
preview = shader(bricks, "Surface", "UsdPreviewSurface")
reader = shader(bricks, "St", "UsdPrimvarReader_float2")
reader.CreateInput("varname", Sdf.ValueTypeNames.String).Set("st")
reader.CreateOutput("result", Sdf.ValueTypeNames.Float2)
colour = shader(bricks, "Colour", "UsdUVTexture")
colour.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("../textures/brick_wall/color.jpg")
colour.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), "result")
colour.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)
nmap = shader(bricks, "NormalMap", "UsdUVTexture")
nmap.CreateInput("file", Sdf.ValueTypeNames.Asset).Set("../textures/brick_wall/normal.jpg")
nmap.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(reader.ConnectableAPI(), "result")
nmap.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("raw")
nmap.CreateInput("scale", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(2, 2, 2, 1))
nmap.CreateInput("bias", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(-1, -1, -1, 0))
nmap.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)
preview.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(colour.ConnectableAPI(), "rgb")
preview.CreateInput("normal", Sdf.ValueTypeNames.Normal3f).ConnectToSource(nmap.ConnectableAPI(), "rgb")
preview.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.85)
bricks.CreateSurfaceOutput().ConnectToSource(preview.ConnectableAPI(), "surface")
bind(sphere("/World/ball", (-0.25, 0.45, -0.3), 0.45).GetPrim(), bricks)

# Red metal: values alone.
red = material("RedMetal")
s = shader(red, "Surface", "UsdPreviewSurface")
s.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.75, 0.06, 0.04))
s.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(1.0)
s.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.3)
red.CreateSurfaceOutput().ConnectToSource(s.ConnectableAPI(), "surface")
bind(sphere("/World/bead", (0.55, 0.28, 0.45), 0.28).GetPrim(), red)

# A block of glass.
glass = material("Glass")
s = shader(glass, "Surface", "UsdPreviewSurface")
s.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(0.05)
s.CreateInput("ior", Sdf.ValueTypeNames.Float).Set(1.5)
glass.CreateSurfaceOutput().ConnectToSource(s.ConnectableAPI(), "surface")
bind(box("/World/block", (1.35, 0.3, -0.2), (0.6, 0.6, 0.6), 35).GetPrim(), glass)

# The floor: two halves, two subsets -- blue OpenPBR plastic, and paving by
# the name of the program's preset. A centimetre up: the renderers' ground
# is at 0.
floor = mesh("/World/floor", [(-2.2, 0.01, -1.4), (0, 0.01, -1.4), (2.2, 0.01, -1.4), (-2.2, 0.01, 1.4), (0, 0.01, 1.4),
                              (2.2, 0.01, 1.4)], [4, 4],
             [0, 3, 4, 1, 1, 4, 5, 2])
plastic = material("Plastic")
s = shader(plastic, "Surface", "ND_open_pbr_surface_surfaceshader")
s.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.15, 0.3, 0.65))
s.CreateInput("specular_roughness", Sdf.ValueTypeNames.Float).Set(0.45)
plastic.CreateSurfaceOutput("mtlx").ConnectToSource(s.ConnectableAPI(), "out")
paving = material("paving")
s = shader(paving, "Surface", "UsdPreviewSurface")
s.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.16, 0.15, 0.14))
s.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.8)
paving.CreateSurfaceOutput().ConnectToSource(s.ConnectableAPI(), "surface")
api = UsdShade.MaterialBindingAPI.Apply(floor.GetPrim())
left = api.CreateMaterialBindSubset("left", [0], "face")
right = api.CreateMaterialBindSubset("right", [1], "face")
bind(left.GetPrim(), plastic)
bind(right.GetPrim(), paving)
stage.GetRootLayer().Save()
