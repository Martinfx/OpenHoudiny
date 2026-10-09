# Cycles: rendering with Blender's renderer

The **Render** tab in the editor and the command line render through
**Cycles**, Blender's renderer (Apache 2.0). Cycles is linked into the same
program as a library, not as Blender: the frame's scene is converted into a
Cycles scene, which Cycles computes on all CPU cores. Noise is removed by Intel
Open Image Denoise, as in Blender. Our own path tracer
([pathtracer.md](pathtracer.md)) remains as a second option and as a fallback
in a build without Cycles.

On top of that, Cycles lights the scene with a physical sky as in Blender,
converts light to an image through AgX, and adds detail to smooth surfaces ([§3](#3-sky-colors-and-surfaces)):

![Demolition: left as before, right with physical sky, AgX Punchy and surface detail](img/cycles-look.jpg)

![A street with the same settings as the path tracer: left our path tracer, right Cycles, 128 samples per pixel](img/cycles-street.jpg)

![Meadow close-up through Cycles, 1280 × 720, 128 samples per pixel](img/cycles-meadow.jpg)

## 1. Quick start

In the editor:

1. Above the viewport, click the **Render** tab. Cycles renders (on the left
   of the tab's toolbar is the **Cycles** / **Path tracer** choice).
2. The first image arrives immediately, from smaller and larger pixels, and
   sharpens with every sample. Denoising runs progressively.
3. During simulation playback the tab shows frame after frame, as fast as
   they can be computed. A newly opened scene is shown first, and only then
   does the next frame start.
4. The settings (samples, bounces, denoising, aperture, focus, clamp, motion
   blur, sun size, sky, color transform, surface detail) are in the
   **Output** node in the **Render** section, the same for both renderers.
5. A whole shot to video: the film icon in the tab's toolbar (**Video…** or
   **Frames (PNG)…**), or **File › Render Video with Cycles…**. Each frame is
   rendered to completion with the sample count from Output, at the tab's size
   (25 / 50 / 100 %) and with the shot camera. The progress window shows the
   last finished frame, the samples of the frame currently rendering, the time
   per frame and how much is left. See [render.md](render.md#rendering-video-through-cycles).

![The Render tab: meadow through Cycles, 54 of 128 samples per pixel](img/cycles-tab.jpg)

From the command line:

```bash
./build/prototype sim street ulice.png --renderer cycles                 # last frame, settings from Output
./build/prototype sim meadow louka.png --renderer cycles --samples 256 --size 1920x1080
./build/prototype sim campfire ohen.exr --renderer cycles                # EXR: R G B A, Z, albedo.*, N.*
./build/prototype sim campfire ohen.mp4 --renderer cycles --samples 32   # every frame into the video
```

From Python:

```python
net.render("ulice.png", renderer="cycles", samples=128)
```

The summary at the end says what was used to render: `rendering 8720 ms/image
through Cycles 5.3.0, 64 samples a pixel, denoised by Open Image Denoise`.

## 2. What is converted to Cycles

| in the scene | in Cycles |
|---|---|
| polygons of the displayed geometry | a triangle mesh with normals and `Cd` color (attribute `Col`) |
| instances (grass, trees from Copy to Points) | one mesh per variant and objects that place it: a meadow with 122,577 grass tufts, 84 trees and 65 shrubs is 22 meshes |
| spheres, boxes, cylinders, cones, tori | tessellated into triangles finely enough not to be visible; holdout and shadow catcher as in Blender |
| `roughness`, `metallic`, `Cd` | Principled BSDF with a specular as in Blender (Specular IOR Level 0.5) |
| `translucency` (grass blades, leaves) | Translucent BSDF mixed into the Principled BSDF |
| glass (`glass` 1) | Glass BSDF with an index of 1.5 and a tint of color; smooth according to the `N` normals of vertices or points where the mesh has them (bottle, lens, glass from USD), otherwise flat face by face |
| water surface | Glass BSDF with an index of 1.33, absorbing light inside according to Clarity and taking on the color of the Water Look |
| piece grit (free points with `pscale`) | angular stone fragments and glass shards as objects of one of 18 meshes, oriented by `orient`, each in a shade of its color ([pathtracer.md §4](pathtracer.md#grit-rain-and-wet-surfaces)) |
| raindrops | spindles as long as the distance a drop travels during Streak of a frame: Glass BSDF with an index of 1.33 mixed with Transparent BSDF according to Opacity, only transparent from behind; the object casts no shadow |
| wetness under rain | upward-facing surfaces darkened by half with a layer (Coat) of water: Coat Weight according to Wet Floor, roughness 0.03, index 1.33 |
| smoke, fire, dust | a volume: extinction and emission grids (NanoVDB) in a box around the gas, Principled Volume |
| floor | a square with the floor color, fading towards the edge as in the viewport; under the physical sky with Sky Behind, the ground extends to the horizon ([§3](#3-sky-colors-and-surfaces)) |
| sun | Sky `look`: a distant light (Sun) with the Sun Size angle and the same strength as ours; Sky `physical`: the sun of the single scattering (Nishita) sky |
| sky | Sky `physical`: the single scattering (Nishita) sky; Sky `look`: with **Sky Behind**, the Look's sky as an image all around; without Sky Behind, always the studio background for the camera |
| camera | the shot from the camera or the viewport view, lens, aperture and focus from Output |
| motion (point velocity `v`, gas velocity, animated objects, moving camera) | blur along the path while the shutter is open ([below](#motion-blur)) |
| shot camera plate | transparent film, also through glass; holdouts and catchers (objects and the floor) as Cycles holdouts and shadow catchers; sun and sky as real lights; the Shadow Catcher pass as a multiplier of the plate ([plate.md](plate.md#in-the-final-render-cycles-and-the-path-tracer)) |

The scene is rotated in Cycles, because Cycles has the Z axis up and we have Y.
Exposure stays the same as in the path tracer and in the viewport.

**Glass and water let sunlight through into shadows.** Cycles would find the
light behind glass and under water only along refracted paths (caustics), and
a room behind a window or the bottom of a pool would stay dark. Shadow rays
therefore pass through glass and water, only slightly attenuated, just as in
our path tracer.

### Motion blur

![Frame 72 of the house collapse in Cycles, left a sharp instant, right the shutter open for half a frame; below a crop magnified 3.2 times: the beam rotates as it falls and its free end, which moves fastest, is blurred the most, the grit is stretched into streaks and the fence, which stands still, stays sharp](img/cycles-motion-blur.jpg)

A camera does not capture an instant but the time during which the shutter is
open. Whatever moves in the meantime is blurred along its path. Cycles does
this just as in Blender: each ray gets its own instant between the opening and
closing of the shutter and sees the scene as it was at that moment.

How long the shutter is open is set by **Motion Blur** in the **Output** node
in the **Render** section: what fraction of a frame, centered on the frame. The
default is 0.5 of a frame, as with a film camera with a 180° shutter (Shutter
0.5 in Blender). At 24 frames per second that is 1/48 s. A value of 0 gives a
sharp instant.

| what moves | where Cycles knows it from | how it is blurred |
|---|---|---|
| RBD pieces, rebar bars, cloth, water surface, displayed geometry | point velocity `v` (m/s) | the mesh has three steps: triangle vertices at the start, middle and end of the shutter, offset by `v` × time |
| grit (free points with `pscale`) | point velocity `v` | a fragment is an object with three positions, it moves like the point |
| instances (Copy to Points) | velocity `v` of the instance point | like grit |
| scene objects (spheres, boxes, meshes, keyframe-animated) | velocity and rotation of the object, from its position and orientation one frame earlier ([animation.md](animation.md#obstacle-motion)) | the object has three positions: at the start, middle and end of the shutter, offset by velocity × time and rotated about its axis of rotation |
| smoke, fire, dust and steam (Pyro Solver, VDB Gas) | the gas velocity in the frame, m/s | Cycles reads the grids where the gas arrives from at that instant: it moves the point against the velocity by velocity × time, twice in succession, as in Blender |
| shot camera | the camera one frame earlier and one frame later | at the start of the shutter it is a quarter of the way to the previous frame's camera, at the end a quarter of the way to the next one's (at 0.5), it rotates along the shorter path; if the focal length changes, the field of view changes too |

Normals stay as in the middle in all steps. Cycles would compute them from the
faces, and smooth water would be faceted during the shutter. Pieces rotate only
negligibly in such a short time, so a straight-line offset is enough. A scene
object really rotates during the shutter, not just translates: the ends of the
paddle in the `fire_trail` example sweep a 3° arc in half a frame.

**Gas.** Every simulation frame carries the gas velocity: one per block of
2 × 2 × 2 cells, in half floats, only where there is gas
([cache.md](cache.md)). VDB Gas reads it from the vector grid `vel`
([vdb.md](vdb.md)). Cycles gets the velocity as a third grid next to
extinction and emission, and the gas box is larger by the path of the fastest
gas, so that the blurred edge is not cut off. In the gas, blur costs two extra
velocity reads wherever a renderer reads it: the campfire (400 × 600, 64
samples, frame 60) takes 26.9 s with a 0.5 shutter versus 22.3 s without it
in Cycles, in the path tracer 14.1 s versus 8.1 s. Gas that is at rest in all blocks has no velocity and
renders as before.

Blur is turned on only when something moves. A scene without motion renders as
fast as before, and the image is bit for bit identical to one with a zero
shutter. With the shutter, the render takes about 10 % longer (frame 66 of the
house collapse, 1280 × 720, 32 samples: 124 s versus 112 s).

From the command line:

```bash
./build/prototype sim house_collapse dum.png --renderer cycles --frames 66 --start 66 \
    --set output.render_motion_blur=0.5      # default: half a frame
./build/prototype sim house_collapse dum.png --renderer cycles --frames 66 --start 66 \
    --set output.render_motion_blur=0        # sharp instant
```

Only raindrops are not blurred. They do not need it, because they are already
drawn as streaks as long as the distance a drop travels during Streak of a
frame. The path tracer blurs the same things by the same amount
([pathtracer.md](pathtracer.md#motion-blur)).

## 3. Sky, colors and surfaces

Three options in the **Render** section of the Output node make Cycles more
than our path tracer:

| option | what it does |
|---|---|
| **Sky** `physical` (default) | sky and sun like Sky Texture in Blender (the Nishita model): blue sky, haze at the horizon, clouds according to **Clouds**. The sun is where the Look has it, with its color (Light Color) and strength. The sky adds the blue light to it that was missing in the shadows. With **Sky Behind** the camera sees the sky and the ground up to the horizon, distant ground fades into the haze. Without Sky Behind the dark studio background remains. |
| **Sky** `image` | sky from an image all around (HDRI, **Sky Image**): it lights the scene and with Sky Behind it is visible behind it |
| **Sky** `look` | sun and sky of the Look as in the viewport and in the path tracer |
| **Sky Image** | the sky image, equirectangular (2 : 1): `.hdr` or `.exr` with the light as it is (for example an HDRI from [Poly Haven](https://polyhaven.com/hdris), CC0), also `.png` and `.jpg`. A relative path is read from the network's folder. |
| **Sky Rotation**, **Sky Strength** | the image rotated about the vertical axis (the sun from the image where the shot wants it), its light times the strength |
| **Sky Sun** | adds the Look's sun to the image as well: sharp shadows under a sky without its own sun |
| **Clouds** 0–1 (0) | how much of the sky is covered by clouds: 0 clear, 0.3 a few clouds, 0.6 partly cloudy, 1 overcast (sun almost hidden, soft shadows) |
| **Cloud Size**, **Cloud Wind**, **Cloud Direction** | how big the clouds are (km, 1.5), how fast the wind carries them (m/s, 5) and where to (degrees from the +x axis): they move frame by frame |
| **View** `agx_punchy` (default), `agx`, `aces`, `aces1`, `aces2`, `standard` | how light is converted to an image: AgX as in Blender, bright colors go to white as on film. `agx_punchy` adds the Punchy look from Blender (more contrast and color, darker midtones), `aces` is the viewport curve, `aces1` and `aces2` are ACES 1.0 and 2.0 as in OpenColorIO, `standard` is sRGB without a curve ([color.md](color.md)). Applies to both Cycles and the path tracer. |
| **EXR Color Space** `rec709` (default), `acescg`, `aces2065_1` | which space the light in the EXR is in; the chromaticities attribute says so ([color.md §4](color.md#4-exr-in-aces-spaces)) |
| **Surface Detail** 0–1 (1) | surfaces that are smooth in the scene get color and roughness varying in patches one to two meters across and palm-sized, plus tiny bumps. The ground additionally gets patches several meters across. 0: smooth as in the viewport. A primitive with the `f@surface_detail` attribute gets only that many times as much (0 none): that is how USD Import leaves materials from other programs as their author made them ([usd-import.md](usd-import.md#materials)). |
| **Displacement** (off) | a surface whose material has a height image (bricks, bark and tiles from the library, the Material node's texture, displacement from USD) is really displaced by Cycles, not just shaded: the silhouette goes up and down, bricks stand out from the mortar and shadow it. The middle of the image stays on the surface, light goes out and dark goes in, by the depth of the set in total. Cycles dices such a surface into triangles the size of the **Dicing Rate**, so the render takes longer and uses more memory. Off: only bump, as in the path tracer and in the viewport |
| **Dicing Rate** 0.1–64 (1 px) | how small the triangles (in pixels, as the camera sees them) are that Cycles makes from a displaced surface: 1 as in Blender, 2 or 4 faster and with less memory. Anything finer stays as bump on the smooth surface. Outside the shot the dicing is four times coarser |
| **Textures**, **Texture Folder** | photographs of materials (concrete and its fracture, plaster, brick wall, mortar, metal, asphalt, wood, roofs and tiles, paving, bark, soil, lawn, sand) and textures from Material nodes; off: only patterns and colors. See [materials.md](materials.md) |

Primitives that say what they are made of (`s@material`) are drawn by Cycles as
that material: with a photograph from the program's library, or with a pattern
(concrete fracture with pebbles, windows with rooms, rusty steel), always
around their `Cd` color. Generators set the material themselves (Brick Wall,
Concrete Fracture, Wood Fracture, Tree, Grass…), and the **Material** node
gives it to other primitives. Details are in [materials.md](materials.md).

The sky strength is set so that the sun gives the same light as the Look's
sun. The test `render_cycles_lights_a_day_under_a_physical_sky` verifies this:
a floor under the sun at 45° elevation has the brightness of a matte floor
under the Look's sun, and the sky adds about 10 %. With a low sun the share of
blue light from the sky is larger.

![Demolition under clouds 0.4 and 0.85 and under an HDRI](img/cycles-skies.jpg)

The clouds are a layer two kilometers above the ground. Where they are and
where they are not is decided by Perlin noise the size of Cloud Size. The sun
lights them more at their thin edges and around itself; dense and overcast
clouds are greyer. Near the horizon they fade into the haze. The Look's sun is
a separate light, so a cloud passing over the sun does not turn the scene off.
An overcast sky dims the sun by itself: at Clouds 1, 15 % of its light
remains.

The options can be set from the command line with `--set`:

```bash
./build/prototype sim demolition odstrel.png --renderer cycles --set output.render_clouds=0.5
./build/prototype sim demolition odstrel.png --renderer cycles \
    --set output.render_sky=image --set output.render_sky_image=obloha.hdr --set output.render_sky_rotation=90
```

In Cycles the sky is also a light that is sampled according to brightness (as
in Blender): the bright parts of an image sky and the clouds are found by every
ray, not just the one that happens to hit them.

The path tracer always lights with the Look's sky and adds no surface detail.
It has the same color transform (View). The viewport draws the Look's sky;
clouds and the sky image exist only in the render.

### Displacement by height

![The displacement example in Cycles under a low sun: top bump only, bottom with Displacement. The bricks on the sphere stand out from the mortar and the sphere's silhouette is jagged, the trunk's silhouette goes up and down with the bark, and the paving stones shadow the joints](img/cycles-displacement.jpg)

With **Displacement**, Cycles really displaces surfaces whose set has a height
image by that height. Without it, the height becomes only bump, which changes
the shading but not the shape: the silhouette stays smooth and nothing casts a
shadow. The depth is the depth of the set (`depth` in `texture.txt`), 13 mm for
bricks and 22 mm for bark. For a material from USD it is the `scale` of its
`displacement` output ([usd-import.md](usd-import.md#materials)).

```bash
./build/prototype sim displacement posunuti.png --renderer cycles
./build/prototype sim displacement relief.png --renderer cycles --set output.render_displacement=0
```

The `displacement` example has a **Dicing Rate** of 2: triangles two pixels
across. A 960 × 540 render on four cores takes 2 min 15 s with displacement,
1 min 31 s without.

A surface does not split apart even at a sharp edge, because the vertices of
one point are a single vertex for Cycles. A side of a box is therefore
slightly bevelled at the edge towards the adjacent side.

## 4. Smoke, fire and dust

![Campfire and smoke: always path tracer on the left, Cycles on the right, 64 samples per pixel](img/cycles-gas.jpg)

Gas from the simulation is converted to Cycles as two grids (`Gas::sparse`). One
says how much light a cell stops per meter, the other how much the flame emits.
Both are computed the same way as in the path tracer: from the smoke,
temperature, flame and steam of each cell according to the Volume Look, with
the same falloff at open walls and at the top. With steam a third grid is
added, the scattering color of each cell (`pg_albedo`): the average of the
smoke and steam colors weighted by how much light each of them stops ([quench.md](quench.md#steam)). Cycles reads the grids linearly between
cell centers, just like the viewport and the path tracer.

In Cycles it is a box around the tiles that contain gas, with a
**Principled Volume** material:

- **Density** is the extinction from the grid.
- **Color** is the fraction of light the smoke keeps when scattering. It is
  computed from Smoke Color just as in the path tracer; with steam it is the
  `pg_albedo` grid.
- **Anisotropy** is 0.31, the average of our two lobes (0.7 × 0.55 forward
  and 0.3 × 0.25 backward).
- **Emission** is the flame glow from the grid: a black body from 1000 K to 3000 K.

The grids go to Cycles as NanoVDB grids, the only volumes Cycles 5 reads:
every cell of the gas, as fine as it is simulated, however many there are.
They are made straight from the gas's own grid, tile by tile, without a dense
grid in between (before, gas of more than 32 million cells in the box round
it went in blocks of 2 × 2 × 2: a plume at resolution 256 with a Pyro Upres
of 3, 30 to 40 million cells at frame 70, takes 905 MB rather than 1110 MB
now). The velocity for motion blur stays a dense grid of cells twice as
large. Cycles does not march through the box: it
divides it into an octree, takes the most each node holds (the majorant) and
lets its rays stop in the gas at random by that most, keeping only the stops
where there really is smoke (null scattering). Empty space costs it little.
Without NanoVDB (`-DPG_NANOVDB=OFF`) neither Cycles nor
the path tracer renders gas.

## 5. Differences from the path tracer

The differences below apply with the same settings (Sky `look`, Surface Detail 0)
that the tests use to compare Cycles with the path tracer.

- **Rough surfaces are about 15 % brighter in Cycles.** Principled BSDF also
  accounts for light that bounces between microfacets several times. Our GGX
  specular loses it. A matte floor under the sun is the same in both; the test
  `render_cycles_lights_a_floor_as_the_sun_and_the_sky_do` verifies it to
  within 2.5 %.
- **Smoke is about 20 % brighter in Cycles** and flame 15 %. Cycles has one
  lobe instead of our two.
- **EXR passes:** the normal in Cycles always faces the camera, ours stays on
  the side the triangle faces. Depth in Cycles is from the pixel's first
  sample, ours is the average.
- **Gas is slower in Cycles** ([§7](#7-performance)), though both skip empty
  space now: Cycles with the majorants of its octree, our path tracer with
  delta tracking and tile maxima.
- **Wetness:** Cycles gives a wet surface a layer of water (Coat), our path
  tracer only lowers its roughness. Both darken it by half.
- **Rain under the physical sky is fainter**, because the drops refract the
  real sky. With Sky `look` the streaks have the same contrast as in the path
  tracer.
- **Motion blur** is equally long in both ([§2](#motion-blur)), but the
  path tracer moves meshes between the start and end of the shutter along a
  straight line over two steps, Cycles over three.
- **Height** displaces the surface only in Cycles and only with **Displacement**
  ([§3](#displacement-by-height)). The path tracer always turns it into bump.

## 6. Build

Cycles is downloaded from GitHub (`blender/cycles`, tag **v5.2.0**, a shallow
clone of about 30 MB; its sources call themselves 5.3.0) and built once along
with the rest of the program. For that it needs **OpenImageIO** and **TBB**
(development files; OpenEXR comes with OpenImageIO):

```bash
sudo apt install libopenimageio-dev libpugixml-dev libtbb-dev   # Debian, Ubuntu
pkg install openimageio pugixml onetbb                          # FreeBSD
```

Cycles is built for the CPU only: without GPU (CUDA, OptiX, HIP, Metal,
oneAPI), without OSL, OpenVDB, OpenSubdiv, Alembic, USD and OpenColorIO, with
the NanoVDB headers the path tracer uses too. Rays in it are traced by the same
Embree as in the path tracer, if it is on the system. Denoising is done by the
same Open Image Denoise as in our path tracer. On four cores the first build
of Cycles takes about 4 minutes (the whole program from scratch, including
Open Image Denoise, about 11 minutes more); later builds only link it. Each
version has its own directories (`build/_deps/cycles-v5.2.0-src`,
`build/cycles-v5.2.0-build`), so a build of an older version is left as it
was; it can be deleted.

The build changes a few lines of the Cycles sources, each once, after the
download. When CMake cannot find the place for a change in another version of
Cycles, it prints a warning:

- `src/scene/object.cpp`, five lines: a mesh with a velocity grid (our gas)
  gets the flag `SD_OBJECT_HAS_VOLUME_MOTION`, which Cycles otherwise gives
  only to Volume objects from OpenVDB. Without it the gas would not be
  blurred.
- `src/kernel/bake/bake.h`, one line: Cycles bounds how much a volume holds
  in each node of its octree from 16 points at the middle of the shutter.
  Gas that moves is blurred to where it is before and after that, and Cycles
  would not look for it there: the blur came out half as wide. The line reads
  the 16 points across the whole shutter instead.
- Without OpenColorIO: Cycles 5 always builds with it, but we hand Cycles
  linear light and do the colors ourselves, so the build turns it off in
  `src/cmake/external_libs.cmake`, `src/cmake/dependency_targets.cmake` and
  `src/CMakeLists.txt`. No package more to install.
- NanoVDB without OpenVDB: Cycles' own code that makes NanoVDB grids makes
  them from OpenVDB ones. In `src/util/nanovdb.h`, `src/util/nanovdb.cpp` and
  `src/scene/image_vdb.cpp` it is compiled only with OpenVDB; our code makes
  the grids itself (`VoxelImage` in `Cycles.cpp`).

Without OpenColorIO Cycles does not convert pictures in its own sRGB. Color
pictures (photographs of materials, leaves) therefore go to it as
`scene_linear_srgb`: the sRGB curve over our linear Rec. 709, which Cycles
takes off as it reads them, as Blender does with 8-bit pictures.

Cycles finds OpenImageIO through its CMake package, whose files on Debian and
Ubuntu name programs of another package (`/usr/bin/iconvert`). The build
hands it a package file of its own instead, made of the libraries CMake
found (`build/cycles-openimageio`).

When Cycles is not built and the path tracer renders:

- without OpenImageIO or TBB (CMake says so);
- when the system's OpenImageIO uses a different C++ standard library than the
  build (clang with libc++ against Ubuntu's OpenImageIO with libstdc++);
- with `-DPG_SANITIZE_THREAD=ON` (its TBB is not compiled with the thread
  sanitizer) and in Visual Studio;
- with `-DPG_CYCLES=OFF`.

`prototype sim … --renderer cycles` exits with an error in such a build,
and the Render tab offers only the path tracer.

## 7. Performance

Four cores (Xeon with AVX-512), Release, the same settings for both
renderers; Cycles 4.5 as it was before the move to 5.2, on the same machine:

| scene | resolution | samples | path tracer | Cycles 4.5 | Cycles 5.2 |
|---|---|---|---|---|---|
| street | 720 × 540 | 128 | 9.0 s | 35.7 s | 39.5 s |
| campfire (frame 60) | 400 × 600 | 64 | 6.8 s | 57.1 s | 21.5 s |
| smoke (frame 60) | 400 × 600 | 64 | 4.3 s | 23.1 s | 18.5 s |
| smoke_plume (frame 50, upres) | 480 × 270 | 32 | 9.1 s | 96.7 s | 31.1 s |

The campfire and the smoke are without gas blur (Motion Blur 0). With a 0.5
shutter the campfire is about 20 % slower in Cycles and 70 % slower in the path
tracer ([§2](#motion-blur)).

Cycles is slower for the same number of samples. On surfaces about four times
as slow: it is more general and computes more, for example specular with
multiple scattering between microfacets. Gas in Cycles 5 is up to three times
faster than in 4.5, which stepped through it a cell at a time: now it skips
empty space as our path tracer does (null scattering over an octree), the
pictures the same (the smoke_plume frame differs by 0.6 of 255 on average).
Our path tracer is still faster on gas: it is simpler and reads one grid where
Cycles reads three. The path tracer is there for a quick preview; Cycles
gives the final image, just as in Blender.

In the Render tab the first image from larger pixels is ready in a fraction of
a second. During playback the tab shows frames at a lower resolution; the frame
it stops on gets full resolution.

## 8. How it works

- `src/pg/render/Cycles.h`, `Cycles.cpp`: `CyclesRender` holds a Cycles
  session. For the command line each frame has its own session until it
  finishes. For the Render tab one session keeps running. Meshes that the new
  scene also has are not rebuilt. The tab receives images during the render
  through the Cycles display driver (`DisplayDriver`, half-float RGBA). At the
  end it receives, through the output driver (`OutputDriver`), the image,
  albedo, normals and depth for EXR. The physical sky is a Sky Texture node
  (single scattering, Nishita's) with a background light (`BackgroundLight`); surface detail is
  Noise Texture and Bump nodes in the shader of each material.
- Displacement by height (`Cycles.cpp`, `dice`): the shader of a material whose
  set has a height image gets a Displacement node (Midlevel 0.5, Scale the
  depth of the set). A mesh with such a material is a subdivision surface for
  Cycles (`SUBDIVISION_LINEAR`): each triangle a face and the vertices of one
  point a single vertex, so it is displaced once and no face tears away from its
  neighbor, not even at a sharp edge. Smooth faces (vertex normals differ from
  the face normal) have a shader with `DISPLACE_BOTH`: they are shaded by the
  normals they had and by the bump from the height. Flat faces (a side of a box)
  have its twin with `DISPLACE_TRUE` and are shaded by the triangles where
  Cycles displaced them. A vertex is displaced along the normal of the smooth
  faces that have it, otherwise along the average of all of them. Normals are on
  the vertices, color, uv and `pg_rest` on the corners (`subd_attributes`);
  Cycles transfers them to the triangles it makes according to the dicing
  camera (`dicing_camera`, the same as the shot camera). Such a mesh is rebuilt
  for every scene, because the camera may have moved.
- Motion blur (`Cycles.cpp`): a mesh with velocities (`Mesh::velocity`) gets
  `set_motion_steps(3)`, and its position and normal attributes
  (`ATTR_STD_POSITION`, `ATTR_STD_VERTEX_NORMAL`) two steps more besides now
  (`add_motion`): the vertices at the start and end of the shutter. A grit fragment is an object with `set_motion`
  (three positions), the camera has `set_motion` with three matrices and
  `MOTION_POSITION_CENTER`. A scene object has `set_motion` with three
  matrices: an offset by velocity × time and the rotation `Collider::turnAt` about
  its center. Gas has a voxel attribute `velocity` with the standard
  `ATTR_STD_VOLUME_VELOCITY`: velocity × the time the shutter is open, so the
  object's `velocity_scale` is 1 (set by the patch from [§6](#6-build)).
  The kernel (`volume_shader_motion_blur`) then moves the point where it reads
  the grids by (time − half) × velocity, and once more with the velocity at the
  new location. The integrator gets `set_motion_blur` only when something moves.
  A mesh that moves is rebuilt after a change of Motion Blur; the others stay.
- Plate (`Cycles.cpp`, `Plate.h`): over a plate the film is transparent
  (`Background::transparent`, `transparent_glass`). Objects and the floor have
  `set_use_holdout` or `set_is_shadow_catcher`, and the sun and the sky have
  `set_is_shadow_catcher`, because they are real lights. The
  `PASS_SHADOW_CATCHER` pass is named "catcher". When reading "combined",
  Cycles gives the CG without catchers, with alpha, and from that `overPlate`
  composes the image.
- `src/pg/render/PathTracer.cpp`: `shown()` converts linear light to an image
  (AgX, AgX Punchy, ACES Fit, and through `Aces.h` ACES 1.0 and 2.0) for both
  renderers.
- `src/pg/render/Gas.h`: `Gas::sparse` provides the gas grids for a renderer
  that reads sparse ones (Cycles), `Gas::dense` for one that reads dense
  grids, `Gas::denseVelocity` the velocity.
- `tools/prototype/RenderView.cpp`: the Render tab thread with both renderers.
  A newer scene (the next frame during playback) is taken once the current one
  has shown an image, or after 3 seconds.
- `tools/prototype/FrameRender.cpp`: a shot frame rendered to completion for
  Render Video and Render Frames with Cycles or the path tracer, on a dedicated
  thread. Each frame has its own Cycles session, just like the command line.
  Stop cancels it (`Session::cancel`), so the render ends immediately, not
  after the frame.
- `CMakeLists.txt`: Cycles is configured as a separate project in
  `build/cycles-v5.2.0-build` and its libraries are built as the `cycles_build`
  target.
  Options, paths and libraries are read from its own build.

Tests (`tests/test_render.cpp`, `tests/test_gas.cpp`):

- `render_cycles_lights_a_floor_as_the_sun_and_the_sky_do`: a floor under the
  sun and under the sky has the brightness of a matte floor (within 2.5 %).
- `render_cycles_shows_what_the_path_tracer_does`: the same shapes in the same
  places as in the path tracer, brightness within 25 %.
- `render_cycles_is_the_same_twice_and_its_passes_are_ours`: two renders
  are identical and the passes match the path tracer.
- `render_cycles_renders_the_gas_as_the_path_tracer_does`: smoke shadow,
  flame light and brightness within 30 % as in the path tracer.
- `gas_dense_grids_are_the_gas_at_the_cells_middles`: the grids match the
  gas at cell centers, large gas goes in blocks.
- `gas_sparse_grids_are_every_cell_of_the_gas`: the NanoVDB grids Cycles
  reads hold what the gas stops and gives off at every cell's center.
- `render_cycles_lights_a_day_under_a_physical_sky`: the sun of the physical sky
  shines like the Look's sun and has its color, the sky is blue.
- `render_cycles_surface_detail_makes_a_flat_surface_uneven`: detail varies the
  surface brightness from place to place, leaving it the same on average.
- `render_cycles_lights_the_scene_with_a_sky_picture`: a sky from an image
  lights the floor like a uniform sky of that color, Sky Strength
  brightens it, Sky Rotation rotates it.
- `render_cycles_clouds_cover_the_sky_and_drift_on_the_wind`: clouds whiten
  the sky and the wind moves them.
- `render_agx_shows_middle_grey_as_blender_does_and_bright_colours_going_white`:
  middle gray is halfway in AgX, bright red goes to white, Punchy
  has more contrast and color.
- `render_cycles_draws_the_grit_and_the_wet` (`tests/test_particles.cpp`):
  a fragment is visible and the floor under it is in shadow, a wet floor is darker
  than a dry one.
- `render_scene_carries_how_fast_what_moves_goes`: every triangle vertex
  and every grit fragment carries the velocity of its point. The camera between two frames
  is halfway and rotates along the shorter path.
- `render_cycles_draws_the_cg_over_a_plate`: over a plate the plate shows where
  the CG changes nothing, pixel by pixel, a CG box covers it, a holdout reveals it,
  a shadow on a catcher darkens it and it shows through glass
  ([plate.md](plate.md#in-the-final-render-cycles-and-the-path-tracer)).
- `render_cycles_blurs_what_moves_while_the_shutter_is_open`: a square
  flying at 24 m/s, a grit fragment, a sphere (a scene object) flying at 24 m/s
  and a camera moving past a stationary square are blurred. The trail is
  more than 8 pixels wider, the peak brightness is more than a fifth lower
  and the total light is the same (within 15 %). With a zero shutter the image is bit for bit
  identical to one without motion. The same test for the path tracer
  (`render_path_tracer_blurs_what_moves_while_the_shutter_is_open`) gives
  equally wide trails.
- `render_blurs_the_gas_along_its_velocity_while_the_shutter_is_open`
  (`tests/test_gas.cpp`): a ball of fire flying at 12 m/s stretches by 25 cm
  in half a frame. The variance of light across image columns grows by as much
  as a uniform trail of that length gives (L²/12): in Cycles by 3.33 px²,
  in the path tracer by 3.41 px², by calculation 3.34 px². The total light is the same
  (within 5 %).
- `uv_the_renderers_lay_a_picture_on_by_uv_and_bend_the_light_by_its_normal_map`
  (`tests/test_uv.cpp`): a photo by UV and a normal map the same in both
  renderers, also on a sphere (within 5 %,
  [materials.md](materials.md#by-uv-and-normal-map)).
- `foliage_leaves_are_cut_out_by_their_pictures_alpha`
  (`tests/test_foliage.cpp`): alpha cutout of leaves. Through the cut-out half of a board
  the ground is visible, lit as if there were no board, in both Cycles and the path tracer (within 2 %).

## 9. What is still missing

- GPU (CUDA, OptiX, HIP, Metal): Cycles is built for the CPU only.
- OSL shaders. Normal maps only with UV; from three sides the height makes bump
  ([materials.md](materials.md#by-uv-and-normal-map)).
- Clouds as a volume (cloud shadows on the ground, clouds you can fly into)
  and the image sky in the viewport.
- Catmull-Clark subdivision: Cycles is built without OpenSubdiv,
  and so it subdivides displaced surfaces (**Displacement**) only linearly. A coarse
  mesh therefore stays faceted, it is just displaced. Subdivision surfaces from USD are
  already smoothed by USD Import ([usd-import.md](usd-import.md#subdivision-surfaces)), and a
  Subdivide node can also be placed before Cycles.
