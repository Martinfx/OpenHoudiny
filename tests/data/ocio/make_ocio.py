# How the OpenColorIO test configs and ocio.txt were made: two configs --
# config.ocio (version 2, in the shape of the ACES configs and Blender's)
# and config_v1.ocio (version 1) -- their tables in luts/, and what
# OpenColorIO itself makes of light through their views and colour spaces,
# which tests/test_ocio.cpp holds src/pg/render/Ocio.cpp to.
#
#   pip install opencolorio==2.6.0
#   python tests/data/ocio/make_ocio.py > tests/data/ocio/ocio.txt
#
# Each line of ocio.txt: what (V a view, I a view back, C a colour space to
# another, T the same back through a 3D table -- as OpenColorIO's
# processors take it by default, the table's inverse tabled), the config,
# then tab-separated the names, then three numbers in and three out.
import math
import os

import PyOpenColorIO as ocio

HERE = os.path.dirname(os.path.abspath(__file__))
LUTS = os.path.join(HERE, "luts")


def number(v):
    return f"{v:.7g}"


def write(name, text):
    with open(os.path.join(LUTS, name), "w") as f:
        f.write(text)


# --- the tables -------------------------------------------------------------------

def shaper(x):
    """A gamma 2.2 encoding, mirrored below 0."""
    return math.copysign(abs(x) ** (1 / 2.2), x)


def contrast(x, k):
    """An S-curve about 1/2, steeper for each channel k."""
    s = 1.0 + 0.25 * k
    return 0.5 + 0.5 * math.tanh(s * (x - 0.5) * 2.0) / math.tanh(s)


def twist(r, g, b):
    """A smooth colour twist: a little of each channel into the next."""
    return (0.85 * r + 0.1 * g + 0.05 * b, 0.05 * r + 0.9 * g + 0.05 * b, 0.1 * r ** 1.2 + 0.05 * g + 0.85 * b)


def look3d(r, g, b):
    """A rising 3D look: each channel bent, a little crosstalk."""
    return (0.9 * r ** 1.1 + 0.06 * g + 0.02 * b, 0.04 * r + 0.88 * g ** 0.95 + 0.04 * b, 0.02 * r + 0.08 * g + 0.86 * b ** 1.05)


def sigmoid(t):
    return 1.0 / (1.0 + math.exp(-8.0 * (t - 0.55)))


S0, S1 = sigmoid(0.0), sigmoid(1.0)
LUMA = (0.2126, 0.7152, 0.0722)


def film(r, g, b):
    """A film-like view of log2 light: an S-curve, then a tenth less saturation."""
    s = [(sigmoid(t) - S0) / (S1 - S0) for t in (r, g, b)]
    y = sum(w * v for w, v in zip(LUMA, s))
    return tuple(0.9 * v + 0.1 * y for v in s)


def film_back(r, g, b):
    """film(), back: the saturation, then the S-curve, held to 0 to 1."""
    y = sum(w * v for w, v in zip(LUMA, (r, g, b)))  # the luma is kept
    s = [(v - 0.1 * y) / 0.9 for v in (r, g, b)]
    out = []
    for v in s:
        p = min(max(v * (S1 - S0) + S0, 1e-6), 1 - 1e-6)
        out.append(min(max(0.55 - math.log(1.0 / p - 1.0) / 8.0, 0.0), 1.0))
    return tuple(out)


def cube3d(n, f, header):
    lines = [header, f"LUT_3D_SIZE {n}"]
    for b in range(n):
        for g in range(n):
            for r in range(n):
                out = f(r / (n - 1), g / (n - 1), b / (n - 1))
                lines.append(" ".join(number(v) for v in out))
    return "\n".join(lines) + "\n"


def make_luts():
    os.makedirs(LUTS, exist_ok=True)
    n = 64
    lo, hi = -0.125, 1.125
    values = [shaper(lo + (hi - lo) * i / (n - 1)) for i in range(n)]
    write("shaper.spi1d", "Version 1\nFrom -0.125 1.125\nLength 64\nComponents 1\n{\n" +
          "".join(f"    {number(v)}\n" for v in values) + "}\n")
    n = 33
    rows = []
    for i in range(n):
        x = i / (n - 1)
        rows.append(" ".join(number(contrast(x, k)) for k in range(3)))
    write("contrast.spi1d", "Version 1\nFrom 0 1\nLength 33\nComponents 3\n{\n" +
          "".join(f"    {r}\n" for r in rows) + "}\n")
    write("matrix.spimtx", "0.9 0.05 0.05 655.35\n0.02 0.96 0.02 0\n0.05 0.1 0.85 -327.675\n")
    n = 5
    lines = ["SPILUT 1.0", "3 3", f"{n} {n} {n}"]
    for r in range(n):
        for g in range(n):
            for b in range(n):
                out = twist(r / (n - 1), g / (n - 1), b / (n - 1))
                lines.append(f"{r} {g} {b} " + " ".join(number(v) for v in out))
    write("twist.spi3d", "\n".join(lines) + "\n")
    n = 20
    lines = ["# Resolve's kind: an input range, no title", f"LUT_1D_SIZE {n}", "LUT_1D_INPUT_RANGE 0 2"]
    for i in range(n):
        x = 2.0 * i / (n - 1)
        lines.append(" ".join(number(x / (1.0 + x) * (1.0 + 0.1 * k)) for k in range(3)))
    write("curve1d.cube", "\n".join(lines) + "\n")
    write("look3d.cube", cube3d(9, look3d, '# a look\nTITLE "look"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1'))
    write("film.cube", cube3d(9, film, 'TITLE "film"'))
    write("film_back.cube", cube3d(9, film_back, 'TITLE "film back"'))


# --- the configs ----------------------------------------------------------------------

CONFIG = r"""ocio_profile_version: 2.4

# A config in the shape of the ACES configs and Blender's, its tables found
# through an environment variable it gives a value of its own.
environment:
  LUT_DIR: luts
search_path: ${LUT_DIR}
strictparsing: true
luma: [0.2126, 0.7152, 0.0722]

roles:
  aces_interchange: ACES2065-1
  cie_xyz_d65_interchange: CIE-XYZ-D65
  color_timing: ACEScct
  data: Raw
  default: ACES2065-1
  scene_linear: ACEScg

file_rules:
  - !<Rule> {name: Default, colorspace: default}

shared_views:
  - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video, display_colorspace: <USE_DISPLAY_NAME>}
  - !<View> {name: ACES 2.0 - SDR 100 nits (Rec.709), view_transform: ACES 2.0 - SDR 100 nits (Rec.709), display_colorspace: <USE_DISPLAY_NAME>}
  - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: <USE_DISPLAY_NAME>}

displays:
  sRGB - Display:
    - !<View> {name: Raw, colorspace: Raw}
    - !<Views> [ACES 1.0 - SDR Video, ACES 2.0 - SDR 100 nits (Rec.709), Un-tone-mapped]
    - !<View> {name: Film, colorspace: Film sRGB}
    - !<View> {name: Graded, colorspace: sRGB Texture, looks: Warm}
    - !<View> {name: Video, view_transform: Video, display_colorspace: sRGB - Display}
  Display P3 - Display:
    - !<Views> [ACES 1.0 - SDR Video, Un-tone-mapped]
  Rec.1886 Rec.709 - Display:
    - !<Views> [ACES 2.0 - SDR 100 nits (Rec.709)]
  Hidden - Display:
    - !<View> {name: Raw, colorspace: Raw}

active_displays: [sRGB - Display, Display P3 - Display, Rec.1886 Rec.709 - Display]
active_views: [ACES 1.0 - SDR Video, ACES 2.0 - SDR 100 nits (Rec.709), Un-tone-mapped, Film, Graded, Video, Raw]
inactive_colorspaces: [CIE-XYZ-D65]

default_view_transform: Un-tone-mapped

view_transforms:
  - !<ViewTransform>
    name: ACES 1.0 - SDR Video
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0}

  - !<ViewTransform>
    name: ACES 2.0 - SDR 100 nits (Rec.709)
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-100nit-REC709_2.0}

  - !<ViewTransform>
    name: Un-tone-mapped
    from_scene_reference: !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD}

  - !<ViewTransform>
    name: Video
    description: A display's light to another's -- the default view transform brings the scene's to it.
    from_display_reference: !<CDLTransform> {slope: [0.9, 1, 1.1], sat: 0.8, style: noClamp}

display_colorspaces:
  - !<ColorSpace>
    name: CIE-XYZ-D65
    aliases: [cie_xyz_d65]
    isdata: false

  - !<ColorSpace>
    name: sRGB - Display
    aliases: [srgb_display]
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_sRGB}

  - !<ColorSpace>
    name: Display P3 - Display
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_DisplayP3}

  - !<ColorSpace>
    name: Rec.1886 Rec.709 - Display
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709}

  - !<ColorSpace>
    name: Hidden - Display
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_G2.2-REC.709}

looks:
  - !<Look>
    name: Warm
    process_space: ACEScct
    transform: !<CDLTransform> {slope: [1.05, 1, 0.92], offset: [0.01, 0, -0.01], power: [1, 1.02, 1.05], sat: 1.1}

  - !<Look>
    name: Contrast
    process_space: Log2
    transform: !<FileTransform> {src: contrast.spi1d, interpolation: linear}

  - !<Look>
    name: Cool
    process_space: ACEScg
    transform: !<MatrixTransform> {matrix: [0.95, 0.03, 0.02, 0, 0.01, 0.98, 0.01, 0, 0.01, 0.04, 1.05, 0, 0, 0, 0, 1]}
    inverse_transform: !<MatrixTransform> {matrix: [1.0526, -0.0322, -0.0197, 0, -0.0106, 1.0207, -0.0095, 0, -0.0096, -0.0388, 0.9529, 0, 0, 0, 0, 1]}

colorspaces:
  - !<ColorSpace>
    name: ACES2065-1
    aliases: [aces2065_1, lin_ap0]
    description: The reference.
    isdata: false

  - !<ColorSpace>
    name: ACEScg
    aliases: [lin_ap1, ACES - ACEScg]
    to_scene_reference: !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: ACEScct
    to_scene_reference: !<BuiltinTransform> {style: ACEScct_to_ACES2065-1}

  - !<ColorSpace>
    name: ACEScc
    to_scene_reference: !<BuiltinTransform> {style: ACEScc_to_ACES2065-1}

  - !<ColorSpace>
    name: Linear Rec.709 (sRGB)
    aliases: [lin_rec709_srgb, lin_rec709]
    to_scene_reference: !<GroupTransform>
      children:
        - !<BuiltinTransform> {style: UTILITY - ACES-AP1_to_LINEAR-REC709_BFD, direction: inverse}
        - !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: Gamut Compressed
    to_scene_reference: !<BuiltinTransform> {style: ACES-LMT - ACES 1.3 Reference Gamut Compression, direction: inverse}

  - !<ColorSpace>
    name: Log2
    description: ACEScg, its log2 from 8 stops below 1 to 6 above.
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<AllocationTransform> {allocation: lg2, vars: [-8, 6, 0.0001]}

  - !<ColorSpace>
    name: Camera Log
    to_scene_reference: !<GroupTransform>
      children:
        - !<LogCameraTransform> {base: 10, log_side_slope: 0.247189638318671, log_side_offset: 0.385536998692443, lin_side_slope: 5.55555555555556, lin_side_offset: 0.0522722750251688, lin_side_break: 0.0105909904954696, direction: inverse}
        - !<MatrixTransform> {matrix: [0.680205505106279, 0.236136601606481, 0.0836578932872399, 0, 0.0854149797421404, 1.01747087860704, -0.102885858349182, 0, 0.00205652166929683, -0.0625625003847921, 1.0605059787155, 0, 0, 0, 0, 1]}

  - !<ColorSpace>
    name: Cineon Log
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<LogAffineTransform> {base: 10, log_side_slope: 0.293255, log_side_offset: 0.669599, lin_side_slope: 0.989192, lin_side_offset: 0.0108077}

  - !<ColorSpace>
    name: Gamma 2.2 AP1
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<ExponentTransform> {value: 2.2, style: mirror, direction: inverse}

  - !<ColorSpace>
    name: Gamma 1.8 AP1
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<ExponentTransform> {value: [1.8, 1.8, 1.8, 1], direction: inverse}

  - !<ColorSpace>
    name: sRGB Texture
    aliases: [srgb_tx]
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: Linear Rec.709 (sRGB)}
        - !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}

  - !<ColorSpace>
    name: Graded
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<CDLTransform> {slope: [1.1, 0.95, 1], offset: [0.02, -0.01, 0], power: [0.9, 1.1, 1], sat: 0.85}
        - !<RangeTransform> {min_in_value: 0, max_in_value: 1, min_out_value: 0.1, max_out_value: 0.9, style: noClamp}

  - !<ColorSpace>
    name: Clamped
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: ACEScg}
        - !<RangeTransform> {min_in_value: 0, max_in_value: 4, min_out_value: 0, max_out_value: 1}

  - !<ColorSpace>
    name: Shaped
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: Linear Rec.709 (sRGB)}
        - !<FileTransform> {src: shaper.spi1d, interpolation: linear}
        - !<FileTransform> {src: matrix.spimtx}
        - !<FileTransform> {src: twist.spi3d, interpolation: tetrahedral}

  - !<ColorSpace>
    name: Cubed
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: Linear Rec.709 (sRGB)}
        - !<FileTransform> {src: curve1d.cube, interpolation: linear}
        - !<FileTransform> {src: look3d.cube, interpolation: linear}

  - !<ColorSpace>
    name: Film sRGB
    description: Blender's way -- a table there, another back.
    from_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: ACES2065-1, dst: Log2}
        - !<FileTransform> {src: film.cube, interpolation: tetrahedral}
        - !<ExponentTransform> {value: 2.4}
        - !<ColorSpaceTransform> {src: Linear Rec.709 (sRGB), dst: sRGB Texture}
    to_scene_reference: !<GroupTransform>
      children:
        - !<ColorSpaceTransform> {src: sRGB Texture, dst: Linear Rec.709 (sRGB)}
        - !<ExponentTransform> {value: 2.4, direction: inverse}
        - !<FileTransform> {src: film_back.cube, interpolation: tetrahedral}
        - !<ColorSpaceTransform> {src: Log2, dst: ACES2065-1}

  - !<ColorSpace>
    name: Raw
    aliases: [Non-Color]
    isdata: true
"""

CONFIG_V1 = r"""ocio_profile_version: 1

# A config of OpenColorIO 1: colour spaces to and from the reference, views
# of a colour space each.
search_path: luts
strictparsing: false
luma: [0.2126, 0.7152, 0.0722]

roles:
  scene_linear: linear
  color_picking: srgb
  data: raw

displays:
  sRGB:
    - !<View> {name: Film, colorspace: srgb, looks: +grade}
    - !<View> {name: Log, colorspace: log}
    - !<View> {name: Raw, colorspace: raw}

active_displays: []
active_views: []

looks:
  - !<Look>
    name: grade
    process_space: log
    transform: !<CDLTransform> {slope: [1.02, 1, 0.97], offset: [0.01, 0, 0], power: [1, 1, 1], sat: 1.05}

colorspaces:
  - !<ColorSpace>
    name: linear
    family: ln
    bitdepth: 32f
    isdata: false
    allocation: lg2
    allocationvars: [-15, 6]

  - !<ColorSpace>
    name: log
    family: lg
    bitdepth: 32f
    isdata: false
    from_reference: !<AllocationTransform> {allocation: lg2, vars: [-10, 6]}

  - !<ColorSpace>
    name: srgb
    family: srgb
    bitdepth: 8ui
    isdata: false
    from_reference: !<GroupTransform>
      children:
        - !<FileTransform> {src: shaper.spi1d, interpolation: linear}
        - !<ExponentTransform> {value: [1.1, 1.1, 1.1, 1]}

  - !<ColorSpace>
    name: raw
    family: raw
    bitdepth: 32f
    isdata: true
"""

# --- what OpenColorIO makes of them -------------------------------------------------

LIGHT = [(0, 0, 0), (0.18, 0.18, 0.18), (1, 1, 1), (0.5, 0.2, 0.05), (2, 1, 0.3), (0.01, 0.02, 0.03), (10, 5, 1),
         (-0.01, 0.5, 0.2), (0.9, 0.05, 0.6), (100, 100, 100), (0.001, 0.0005, 0.002), (5, 0.1, 0.01),
         (0.3, 0.9, 0.02), (0.05, 0.4, 0.8)]
PICTURES = [(0.5, 0.5, 0.5), (0.2, 0.6, 0.9), (0.9, 0.1, 0.1), (0.02, 0.02, 0.02), (0.95, 0.94, 0.93),
            (0.05, 0.3, 0.1), (0.7, 0.55, 0.3)]
LOSSLESS = ocio.OPTIMIZATION_LOSSLESS


def apply(processor, colours, flags=LOSSLESS):
    cpu = processor.getOptimizedCPUProcessor(flags)
    return [cpu.applyRGB(list(c)) for c in colours]


def viewing(cfg, src, display, view, looks, back):
    """A view as OpenColorIO's viewing pipeline puts it: looks asked for in
    place of the view's own."""
    dvt = ocio.DisplayViewTransform(src=src, display=display, view=view)
    if back:
        dvt.setDirection(ocio.TRANSFORM_DIR_INVERSE)
    pipeline = ocio.LegacyViewingPipeline()
    pipeline.setDisplayViewTransform(dvt)
    if looks:
        pipeline.setLooksOverrideEnabled(True)
        pipeline.setLooksOverride(looks)
    return pipeline.getProcessor(cfg)


def emit(kind, config, names, colours, results):
    for c, r in zip(colours, results):
        print(kind, config, "\t".join(names), " ".join(number(float(v)) for v in c),
              " ".join(number(float(v)) for v in r), sep="\t")


def main():
    make_luts()
    configs = {"config.ocio": CONFIG, "config_v1.ocio": CONFIG_V1}
    for name, text in configs.items():
        with open(os.path.join(HERE, name), "w") as f:
            f.write(text)
    os.environ.pop("LUT_DIR", None)
    for name in configs:
        cfg = ocio.Config.CreateFromFile(os.path.join(HERE, name))
        src = cfg.getColorSpace("scene_linear").getName()
        for display in cfg.getDisplays():
            for view in cfg.getViews(display):
                for looks in ("", "Contrast") if name == "config.ocio" else ("",):
                    names = [src, display, view, looks]
                    emit("V", name, names, LIGHT, apply(viewing(cfg, src, display, view, looks, False), LIGHT))
                    emit("I", name, names, PICTURES, apply(viewing(cfg, src, display, view, looks, True), PICTURES))
        for space in cfg.getColorSpaces():
            to = space.getName()
            emit("C", name, [src, to], LIGHT, apply(cfg.getProcessor(src, to), LIGHT))
            if to in ("Shaped", "Cubed"):
                emit("T", name, [to, src], PICTURES, apply(cfg.getProcessor(to, src), PICTURES, ocio.OPTIMIZATION_DEFAULT))
            else:
                emit("C", name, [to, src], PICTURES, apply(cfg.getProcessor(to, src), PICTURES))


main()
