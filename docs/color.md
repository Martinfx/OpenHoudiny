# Color: AgX, ACES and OpenColorIO configs

Renderers compute light, not a picture: linear values in Rec. 709 (sRGB)
primaries, where 1 is white and the sun and flames go far above it. The
Output node's **view** (View) converts them for the screen. Besides AgX from Blender,
prototype supports both **ACES** output transforms as OpenColorIO shows them
in the ACES configs, and it writes and reads EXR in ACES color spaces.
The view can also come from a studio's **OpenColorIO config** (`config.ocio`),
an ACES one or Blender's (§3). All of this without the OpenColorIO library, verified against it
value by value (§5).

![Saturated colors and white, each row brighter to the right, from 5 stops below white to 7 above it, in four views. AgX goes to white smoothly, ACES 1.0 skews blue towards purple and orange towards yellow, ACES 2.0 holds the hue, Standard clips everything above white](img/color-ramps.jpg)

![The same campfire frame (Cycles, lighting from a single EXR) in the AgX Punchy, ACES 1.0, ACES 2.0 and Standard views](img/color-views.jpg)

## 1. Quick start

```bash
./build/prototype sim campfire out/fire.png --renderer cycles --set output.render_view=aces2
./build/prototype sim campfire out/fire.exr --renderer cycles --set output.render_exr_space=acescg
```

In the editor: the **Output** node, **Render** section, **View** and **EXR Color Space**.

## 2. Views

| View | What it does |
|---|---|
| `agx_punchy` (default) | AgX as in Blender with the Punchy look: more contrast and color |
| `agx` | AgX as in Blender: bright colors desaturate to white as on film |
| `aces` (ACES Fit) | the viewport curve (Narkowicz's ACES approximation), each channel separately |
| `aces1` (ACES 1.0) | "ACES 1.0 - SDR Video" from the OpenColorIO configs for ACES 1.3 |
| `aces2` (ACES 2.0) | "ACES 2.0 - SDR 100 nits (Rec.709)" from the configs for ACES 2.0 and 2.1 |
| `standard` | light as it is, in sRGB; anything above white is clipped |
| `ocio` (OpenColorIO) | a view from an OpenColorIO config according to the OCIO parameters (§3) |

All of them go to an sRGB screen and apply to both renderers: the path tracer
and Cycles, the Render tab, images, sequences and videos. Exposure (Output ›
Image › Exposure) is applied before the view.

- **ACES 1.0** is the Reference Rendering Transform and the video output as the
  Academy's CTL defines them: in the RRT, the glow in the shadows of saturated colors, the red
  modifier (bright red does not turn pink), a filmic curve in AP1. Then the output:
  the cinema curve (48 nits) stretched to 100 nits, a gamma for a dim
  living room and slightly less saturation. Bright saturated colors skew
  (orange flame towards yellow); that is a known property of ACES 1.
- **ACES 2.0** tone-maps the lightness of a color appearance model (Hellwig 2022, as
  ACES tunes it), compresses saturation along with it, and brings colors outside Rec. 709 inside along
  lines towards a focal point. It holds the hue: the flame stays orange, only brighter
  and whiter.

A **plate** (camera footage under the CG) is returned to light by the same view
in reverse ([plate.md](plate.md)). For ACES, the inverse transforms are those of
OpenColorIO. ACES 1.0 additionally refines the light with Newton's method: the inverse
red modifier is only approximate, and saturated red would otherwise come back
two steps out of 255 off (in OpenColorIO too).

## 3. OpenColorIO configs

The **OpenColorIO** view shows light through a view from any
`config.ocio`: a studio one, one of the ACES configs
([OpenColorIO-Config-ACES](https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES))
or Blender's (`datafiles/colormanagement/config.ocio`, the AgX,
Filmic, Khronos PBR Neutral views). It does not need the OpenColorIO library:
prototype reads the config itself (it is YAML), finds the display, view,
looks and color spaces in it, and computes the transforms the way OpenColorIO computes them
on the CPU.

```bash
./build/prototype sim campfire out/fire.png --renderer cycles \
    --set output.render_view=ocio \
    --set output.render_ocio_config=/path/to/config.ocio \
    --set "output.render_ocio_view=ACES 2.0 - SDR 100 nits (Rec.709)"
```

In the editor: the **Output** node, **Render** section, View **OpenColorIO**
and the OCIO parameters below it:

| Parameter | What it says |
|---|---|
| `render_ocio_config` (OCIO Config) | the config file; a relative path is resolved from the network's folder |
| `render_ocio_display` (OCIO Display) | the display, e.g. `sRGB - Display`; empty = the first one in the config (default) |
| `render_ocio_view` (OCIO View) | the display's view, e.g. `AgX`; empty = the display's first view |
| `render_ocio_looks` (OCIO Looks) | looks used instead of the view's own looks, as Blender and the OpenColorIO viewing pipeline provide them: `A, B`, `-B` in reverse |
| `render_ocio_space` (OCIO Light Space) | which of the config's color spaces the render's light is in; empty = the config's linear Rec. 709 (`Linear Rec.709 (sRGB)`, `lin_rec709`…), otherwise via ACES2065-1 (the `aces_interchange` role), otherwise the `scene_linear` role |

Display, view and space names are case-insensitive; a space
can also be given by an alias or a role. Anything missing from the config or that cannot be
read is reported by the Output as a warning (e.g. `OpenColorIO: no view Nope of sRGB -
Display (…)`) and the render shows the AgX Punchy view. The config is read once
per file, as it currently is: a change to the file takes effect at the next
cook. LUTs are looked up along `search_path` from the config's folder;
variables in paths (`${LUT_DIR}`) are taken from its `environment` section.

**What it reads.** Version 1 and 2 configs. Scene and display
color spaces, roles, aliases, `isdata`. Displays and views, including shared ones
(`shared_views`, `<USE_DISPLAY_NAME>`), `active_displays`, `active_views`
and `inactive_colorspaces`. View transforms from scene to display and from display
to display, with the default view transform as the bridge between them. Looks with a process
space. Transforms:

- matrices, exponents (all styles), exponent with a linear segment (sRGB),
  logarithms including `LogAffineTransform` and `LogCameraTransform` (ARRI,
  Sony, RED, Panasonic, DJI, Blackmagic…), CDL (ASC and without clamping), Range,
  Allocation (`uniform`, `lg2`);
- LUTs in `.spi1d`, `.spi3d`, `.spimtx` and `.cube` files (1D and 3D,
  Iridas and Resolve) with linear or tetrahedral interpolation;
- `ColorSpaceTransform`, `LookTransform`, `DisplayViewTransform`,
  `GroupTransform`;
- the built-in transforms of the ACES configs: outputs for SDR displays
  (ACES 1.0 and 1.1 for video and cinema, with gamut limiting and with simulated
  D60 and D65 white; ACES 2.0 at 100 nits including the D60 variants), the
  sRGB, Rec. 1886, Gamma 2.2 and 2.6, Display P3, P3-DCI, P3-D60, P3-D65
  and Rec. 2020 displays (including "MIRROR NEGS"), ACEScc, ACEScct, ACEScg, the AP0 and AP1
  to XYZ matrices and the ACES 1.3 reference gamut compression.

**Inverse** (plate, [plate.md](plate.md)): each step goes through whatever the config has for that
direction: the space's `to_scene_reference` (this is how Blender has AgX:
one LUT forward and a different one back), the look's `inverse_transform`, otherwise
the inverse of the step. 1D LUTs are inverted as in OpenColorIO (reversals are
flattened, flat ends skipped). 3D LUTs as in OpenColorIO's default processors:
an exact inverse (the cell tetrahedron that contains the color)
computed at 48 × 48 × 48 points and linear in between. Finally the plate is
refined with Newton's method against the view, so it displays as itself again:
for ACES and for Blender's AgX and Filmic within 0.1 step out of 255.

## 4. EXR in ACES spaces

**EXR Color Space** (Output › Render) says which space the light
in the EXR is in, whether it comes from Cycles, the path tracer or the viewport:

| `render_exr_space` | Space | When |
|---|---|---|
| `rec709` (default) | linear Rec. 709 (sRGB), D65 | how the renderers compute, Nuke and Blender by default |
| `acescg` | ACEScg: AP1 primaries, ACES white (about D60) | compositing in an ACES pipeline |
| `aces2065_1` | ACES2065-1: AP0 primaries, encompass every color | delivering material in ACES |

The conversion uses Bradford white adaptation, with the same matrices as
OpenColorIO. Light (`R`, `G`, `B`) and surface color
(`albedo.*`) go to the other space. Depth, normals, motion vectors and masks stay
as they are, and `catcher.*` remains a per-channel plate multiplier. The file
always has the **chromaticities** attribute (primaries and white), so Nuke, Resolve
or OpenImageIO recognize it.

**Reading.** prototype converts an EXR with chromaticities of a space other than Rec. 709 (e.g.
ACEScg from another program) to Rec. 709: the plate in the viewport
and in both renderers, a picture loaded from Python (`pg.read_picture`)
and a sky from an image in Cycles (Cycles loads that itself; the conversion is in its
shader). A file without the attribute is Rec. 709, as OpenEXR defines it.

## 5. Verification

The reference came from **OpenColorIO 2.6** (`pip install opencolorio`) with its own
built-in configs `cg-config-v2.2.0_aces-v1.3_ocio-v2.4` (ACES 1.0)
and `cg-config-v5.0.0_aces-v2.1_ocio-v2.6` (ACES 2.0), converting from "Linear
Rec.709 (sRGB)" to "sRGB - Display". The script `tests/data/aces/make_aces.py`
wrote `aces.txt`: grays from deep shadow up to a thousand times white, primaries
and colors between them, skin, sky, foliage, fire, each at 20 brightness levels, and 400
random colors. In addition, 1531 picture colors back, and conversions to ACEScg
and ACES2065-1.

| | deviation from OpenColorIO |
|---|---|
| ACES 1.0, light → picture (780 values) | at most 2.1 · 10⁻⁵ on a 0–1 scale (a hundredth of a step out of 255) |
| ACES 2.0, light → picture (780 values) | at most 8.8 · 10⁻⁶ |
| ACES 2.0, picture → light | at most 5 · 10⁻⁴ relative |
| ACEScg, ACES2065-1 | at most 2 · 10⁻⁶ relative |
| picture → light → picture, both ACES | at most 1.8 · 10⁻⁴ (OpenColorIO for ACES 1.0: 8.6 · 10⁻³) |

One pixel costs about 0.4 µs (ACES 1.0) and 0.5 µs (ACES 2.0). A
1280 × 720 frame is done in 0.13 s on four cores. The ACES 2.0 tables
(gamut boundaries per degree of hue) are computed on first use
in 7 ms.

Tests — `tests/test_aces.cpp` (5):
- both views and the space conversions against OpenColorIO values;
- a picture back to the light that displays it, within a quarter step out of 255;
- the Output's views are the same transforms, exposure is applied beforehand;
- an EXR in ACEScg has AP1 chromaticities and reads back in Rec. 709.

A sky from an image in ACEScg lights the floor in Cycles the same as the same sky
in Rec. 709, within 2 % in each channel
(`render_cycles_lights_the_scene_with_a_sky_picture`).

The test `render_unshown_gives_back_the_light_a_picture_shows` returns all
256 grays and 4000 colors of a photograph to their own step in each of the six views.

**OpenColorIO configs.** The script `tests/data/ocio/make_ocio.py` writes
two test configs (a version 2 `config.ocio` shaped like the ACES and
Blender configs, and a version 1 `config_v1.ocio`) with LUTs in all four
formats, and the values OpenColorIO 2.6 gives from them: views forward
and back with looks through its viewing pipeline, and conversions between all spaces.

| | values | deviation from OpenColorIO |
|---|---|---|
| views, light → picture | 322 | at most 8.8 · 10⁻⁶ |
| views, picture → light | 161 | at most 5.9 · 10⁻⁶ |
| color spaces forward and back | 532 | at most 4.6 · 10⁻⁷ |
| back through a 3D LUT (against the default processor) | 14 | at most 4.8 · 10⁻⁷ |

Against the five built-in ACES configs of OpenColorIO 2.6 (CG and Studio,
ACES 1.3 and 2.1) and the Blender 4.5 config (outside the repository): 5300 values
of views, inverse transforms and spaces. All views for SDR displays and
all spaces it supports match within 10⁻⁴. The only difference is the inverse of the Khronos PBR Neutral 3D
LUT versus OpenColorIO's exact inverse (up to 2.7 %). There, however,
OpenColorIO's exact inverse also differs from its own default
processor. HDR displays and outputs and the built-in Canon, Apple
and ADX camera transforms are reported by name.

Blender's AgX view loads in 0.11 s (reading the LUTs), Khronos PBR
Neutral in 0.53 s (inverting the 3D LUT). A 1920 × 1080 frame goes through AgX on
four cores in 0.07 s; a plate pixel back costs 2 to 3.5 µs.

Tests — `tests/test_ocio.cpp` (10): YAML as configs write it; what
a config names; views, inverse transforms and spaces against OpenColorIO;
the view for rendering (default display and view, plate forward and back within half
a step out of 255); light via ACES2065-1 when the config has no linear Rec.
709; what it does not support, it reports by name; the renderers and the Output node with a config.

## 6. In the code

| File | What it does |
|---|---|
| `src/pg/render/Aces.h` | ACES 1.0 and 2.0 forward and back, built-in ACES outputs from OpenColorIO, sRGB encoding |
| `src/pg/render/Ocio.h` | OpenColorIO configs: spaces, displays, views, looks, transforms, LUTs |
| `src/pg/io/Yaml.h` | YAML as OpenColorIO configs write it |
| `src/pg/core/ColorSpace.h` | ACEScg, ACES2065-1, chromaticities, Bradford |
| `src/pg/render/PathTracer.cpp` | `shown`, `unshown`: the view for both renderers |
| `src/pg/render/Save.cpp`, `src/pg/gl/Volume.cpp` | EXR in the space from the Output |
| `src/pg/io/Exr.cpp`, `ExrRead.cpp`, `Picture.cpp` | the chromaticities attribute, conversion on read |

## 7. Limitations

- The **OpenColorIO config** is set on the Output; the `OCIO` environment variable
  is not read, nor are other environment variables (paths take only
  values from the config's `environment` section). There are no Grading
  transforms (GradingPrimary, GradingTone, GradingRGBCurve), ExposureContrast
  or NamedTransform. There are no `.clf`, `.ctf`, `.3dl`, `.csp` files
  and no built-in Canon, Apple and ADX camera transforms. File and view
  rules (`file_rules`, `viewing_rules`) are not used. Blender looks
  with grading transforms (AgX - Punchy…) therefore report an error; the views
  themselves work.
- The **screen** is SDR. The ACES 1.0 and 2.0 views go to sRGB. Through
  an OpenColorIO config also to Rec. 1886, Display P3 and other SDR displays.
  No HDR (PQ, HLG).
- The **working space** is linear Rec. 709. The renderers do not compute in ACEScg;
  colors and textures are not converted to it.
- The **viewport** shows the ACES Fit curve, not the view chosen on the Output.
  The Output's view is visible in the Render tab and in images.
- **Looks** (LMTs, e.g. Reference Gamut Compression from ACES 1.3) and LUTs
  (`.cube`) work only through an OpenColorIO config.
