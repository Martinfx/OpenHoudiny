# Path tracer: a proper render

The viewport draws the scene quickly through OpenGL. There, the shadow is a
single map, ambient light is an estimate and grass does not let light through.
The **path tracer** computes light the way it actually travels. From the
camera it follows rays that bounce off surfaces, refract in glass and water,
pass through blades and leaves, scatter in smoke and dust, and end at the sun,
the sky or a flame. It runs on the CPU, so it needs no graphics
card. The same render is produced by the **Render tab** in the editor next to
the Viewport and by the command line (`prototype sim … --renderer path`), for
example on a render farm.

The default renderer of the Render tab is now Blender's **Cycles**
([cycles.md](cycles.md)). The path tracer is selected in the tab's toolbar with
the **Path tracer** option and renders even in a build without Cycles. Only
Cycles has the physical sky and surface detail. Both have the color transform
(View: AgX Punchy, AgX, ACES Fit, ACES 1.0, ACES 2.0, Standard) ([color.md](color.md)).

![Meadow: viewport (OpenGL) on the left, path tracer on the right, 64 samples per pixel](img/pathtracer-meadow.jpg)

![Grass up close: viewport on the left, path tracer on the right. The blades let light through, and the shadow under the trees is lit by bounced light](img/pathtracer-grass.jpg)

## 1. Quick start

In the editor:

1. Above the viewport, click the **Render** tab.
2. The render starts immediately and becomes less grainy with every pass.
   Changing a parameter, the frame or the view restarts it.
3. The view is orbited with the mouse just as in the viewport: the left button
   rotates, the middle (or Shift + left) pans, the right button and the wheel
   zoom. Through the camera (the **0** key in the viewport) the camera's shot
   is rendered at its aspect ratio.
4. The settings are in the **Output** node, in the **Render** section. The
   Output icon in the tab's toolbar selects the node. A scene with geometry
   only (for example the `meadow` example) does not need an Output and is
   rendered with default settings. An Output added with Tab in the network
   then defines the sun, sky, camera and render.
5. The camera icon saves the render as **PNG**, or as **EXR**
   with linear light, depth, albedo and normals.

From the command line:

```bash
./build/prototype sim meadow louka.png --renderer path                 # last frame, settings from Output
./build/prototype sim meadow louka.png --renderer path --samples 256   # more samples, less noise
./build/prototype sim meadow louka.exr --renderer path                 # EXR: R G B A, Z, albedo.*, N.*
./build/prototype sim meadow louka.mp4 --renderer path --samples 32    # every frame into a video
./build/prototype sim meadow louka.png --renderer path --size 1920x1080 --yaw 40 --pitch 12
```

From Python:

```python
net.render("louka.png", renderer="path", samples=128)
```

The path tracer does not need OpenGL, so it also runs in a build without EGL
and without the editor.

## 2. The Render tab

![The Render tab: the meadow through the camera after 31 samples, with the settings in the Output › Render node on the right](img/pathtracer-tab.jpg)

| element | what it does |
|---|---|
| Cycles / Path tracer | what renders the image ([cycles.md](cycles.md)) |
| ▶ / ⏸ | pauses or starts the render (it resumes by itself after returning from the Viewport) |
| ↻ | starts again from scratch |
| 📷 | saves the render to PNG or EXR |
| 25 / 50 / 100 % | render size: from the panel, or through the camera from the camera resolution |
| Output icon | selects the Output node with the settings |
| `26 / 128 samples · 8.8 s · 0.46 M paths/s` | how many samples are done, how long it is taking and how fast it is going |

The render runs in its own thread on all cores and the window stays smooth.
When you switch to the Viewport tab it stops, so the viewport gets the
CPU. When the scene changes, the pass in progress is interrupted and a new one
starts. A new scene, however, first finishes its first pass, so while a
simulation is playing the tab shows frame after frame as fast as they can be
computed. The frame on which playback stops keeps rendering.
The image is denoised after the first pass, at the end, and in between
whenever the passes since the last denoise took at least as long as the
denoise itself. Denoising therefore never takes more than half the time. In
between, the last denoised image stays visible.

On a four-core machine without a graphics card, the first image of the meadow
(122,577 grass clumps, 84 trees) at 50 % of the panel is done in about a second.
It is usable after 16–32 samples and clean after 128.

## 3. Settings (Output › Render node)

| parameter | default | what it does |
|---|---|---|
| Samples | 128 | samples per pixel, after which the render stops; more is cleaner but slower |
| Bounces | 4 | the maximum number of times light bounces; 0 is only the direct sun and sky |
| Denoise | on | denoising: Intel Open Image Denoise, without it a custom filter guided by color, normal and depth |
| F-Stop | 0 | lens aperture; 0 means everything is sharp, 2.8 a shallow depth of field |
| Focus | 0 m | focus distance; 0 focuses on whatever is in the center of the image |
| Clamp | 20 | the most a single bounce can add to a pixel; removes bright dots (fireflies) |
| Sun Size | 0.53° | angular diameter of the sun: a larger sun gives softer shadows |
| Motion Blur | 0.5 frame | how long the shutter is open: whatever moves is smeared along its path, in the path tracer just as in Cycles ([below](#motion-blur)); 0 is a sharp instant |

Light, sky, floor, exposure and water color come from the same **Look**
as in the viewport, so the brightness of both matches. A test checks that the
floor in sunlight has the same value in the path tracer as in the viewport
(0.8035 versus 0.8005).

### Motion blur

The path tracer blurs the same things as Cycles
([cycles.md](cycles.md#motion-blur)): meshes by the point velocity `v`
(pieces, cloth, water, displayed geometry), grit and instances, scene objects
animated with keyframes (translation and rotation), gas and the shot camera.
Each pixel sample draws a random instant while the shutter is open, and its
whole path, including bounces and shadows, sees the scene at that instant.

- **Meshes**: Embree receives the corners at the start and at the end of the
  shutter and moves them along a straight line in between. The custom
  hierarchy has boxes enlarged by the corners' paths and moves the corners
  itself.
- **Grit and instances**: placement at the start and at the end of the shutter.
- **Scene objects**: the ray is transferred to where the object is at that
  instant: it is moved against the object's velocity and rotated back about
  the rotation axis. The object is then computed exactly as if it were standing
  still.
- **Gas**: smoke, temperature and flame are read from where the gas arrives
  here from at that instant. The point is moved against the velocity by
  velocity × time, and once more with the velocity at the new location, as in
  Cycles. The velocity also reaches into the tiles around the gas (one to four
  layers, depending on how far the gas travels), because blurred gas gets there
  too. Delta tracking steps in a tile are bounded by the most gas a point in it
  can read: from the tile and as many layers of cells around it as the gas in
  its neighborhood travels during the sample time. These are taken from the
  layer maxima of the neighboring tiles, above 6 cells from whole tiles.
  Tracking thus stays unbiased and slow smoke barely makes the steps denser.
  The campfire (400 × 600, 64 samples) takes 14.1 s with a 0.5 shutter versus
  8.1 s, the smoke (200 × 300) 1.6 s versus
  1.2 s.
- **Camera**: 33 cameras between the start and the end of the shutter; a
  sample takes the one between the two nearest.

A scene without motion renders as before: the sample does not draw a time and
the image is bit-for-bit identical. With a zero shutter, the image of a moving
scene is the same as without motion up to rounding: Embree computes the corners
at the frame instant from both steps.

## 4. Materials

The material is read from the geometry's attributes, as in Houdini: from the
primitive, otherwise from the first point, otherwise from the detail.

| attribute | default | what it does |
|---|---|---|
| `Cd` | gray | color |
| `roughness` | 0.5 | 0 mirror, 1 matte surface (GGX specular) |
| `metallic` | 0 | 1 metal: the color tints the reflection |
| `translucency` | 0 | how much diffuse light passes through to the other side: leaf, blade, paper |
| `glass` | – | 1 a pane of glass (refraction and Fresnel, index 1.5), 2 a crack (matte white); smooth according to its own normals `N`, flat without them. The order of the face's corners determines on which side the ray enters the glass |
| `material`, `texture…` | – | what the surface is made of and its photos: from three sides, or by the corners' `uv` with a normal map and alpha cutout ([materials.md](materials.md#by-uv-and-normal-map)) |

The water surface is water with a refractive index of 1.33. With depth it takes
on the color of the Water Look (`waterColor`, `waterClarity`).

**Vegetation** sets its translucency itself: blades from the Grass node 0.35,
leaves from the Tree node 0.4, bark 0. Against the sun, grass and foliage
therefore glow. `roughness` is not written. After a Merge with other geometry,
the missing value 0 would turn the terrain into a mirror.

### Grit, rain and wet surfaces

![Grit from a column on stairs (debris_stairs example, frame 45): Cycles on the left, path tracer on the right](img/render-grit.jpg)

Both renderers draw the **grit** of RBD Solver pieces (free points with
`pscale`) as angular fragments:

- **Shapes.** A dozen stone shapes and six glass shards. A stone is a box,
  flattened and stretched, whose corners and edges have been cut off by planes.
  A shard is a flat polygon with three to five sides, as thin as a pane.
- **Shape by `id`.** Each point gets a shape according to its `id`, so it keeps
  it in flight.
- **Size and orientation.** A fragment is as large as its `pscale`
  (the farthest corner is that far from the center) and oriented by `orient`.
- **Color.** Each fragment has its own shade of its color: some are
  lighter, darker or grayer.
- **Material.** Stone is broken concrete (`broken_concrete`, with a photo
  scaled as on a fragment of about 5 cm), glass is glass.
- **Instances.** The fragments are instances, so 40,000 pieces cost 18 meshes
  and their placements.

![Fragments up close (Cycles)](img/render-grit-close.jpg)

Both renderers draw **rain** the same way as the viewport:

- **Length.** Each drop is a streak that it travels during the fraction of a
  frame given by the Streak parameter of the Rain node. It runs from where the
  drop is, back along the direction of its velocity.
- **Shape.** A thin spindle, as thick as the drop at the head (2.5 mm, a
  splash droplet half that), tapering away to nothing towards the tail.
- **Opacity.** The spindle is made of water (refractive index 1.33) and is
  present only for the fraction of the time given by Opacity. A ray that hits
  the streak from the front meets it with this probability (it reflects off it
  or refracts into it), otherwise it passes through. That is what a
  motion-blurred drop looks like.
- **Shadow.** Rain casts no shadow.
- **Near the camera.** Drops closer than half a meter to the camera are
  skipped, because they would be out of focus.

**Wet surfaces.** Where it rains, upward-facing surfaces are as wet as the
Wet Floor parameter of the Rain node says (like the floor in the viewport).
They are half as bright and smoother, so the layer of water mirrors the
surroundings. The wetness reaches as far as the drops fall, and dries up 35 cm
beyond the edge.

![Storm (storm example, frame 60): path tracer on the left, Cycles with the physical sky on the right. The drops catch the firelight, and the wet ground mirrors the fire, the log and the stone](img/render-rain.jpg)

Under the physical sky the rain is weaker in Cycles, because the drops refract
the real sky, not the Look's sky. With Sky `look`, the streaks have the same
contrast in both renderers (in the storm example +29 levels above the background).

## 5. Smoke, fire and dust

![Campfire: viewport on the left, path tracer on the right, 128 samples per pixel. The smoke shades the floor and itself, and the flame lights the smoke](img/pathtracer-campfire.jpg)

The path tracer renders gas from the simulation (Pyro Solver) just like
surfaces, according to the same **Volume Look** as the viewport. It computes
the light, however, the way it actually travels:

- The sun shines into the smoke, the smoke shades itself and casts a shadow on
  the ground and on buildings.
- Light scatters in the smoke multiple times, so dense smoke lights itself up.
  The sky lights it from all sides.
- The flame lights the smoke around it and, faintly, the surroundings.

The viewport estimates this (Fire Light, Occlusion). The path tracer computes
it, which is why it does not use these two parameters.

| Volume Look parameter | in the path tracer |
|---|---|
| Smoke Color | the color that dense smoke appears to have (see below) |
| Smoke Density | how much light the smoke stops per meter; less in the flame, as in the viewport |
| Steam Color, Steam Density | steam ([quench.md](quench.md#steam)): how much light it stops per meter and with what color it scatters it; in a cell with both smoke and steam the extinction is the sum and the color is the average weighted by the extinction of each |
| Flame Intensity, Start, Range | how much light the flame emits and in what color by temperature: a black body from 1000 K to 3000 K, just like the viewport |
| Fire Light, Occlusion | not used: flame light and sky occlusion are computed |

**Smoke color.** A Smoke Color of 0.75 does not mean that the smoke absorbs a
quarter of the light at each scattering event. After dozens of scattering
events in dense smoke only a small part would remain and the smoke would come
out dark. The path tracer takes Smoke Color as the color that dense smoke
appears to have. It derives from it the fraction each scattering event keeps:
0.75 gives 0.984, 0.2 gives 0.61. This is the mapping of Chiang, Kutz and
Burley (2016) that Cycles uses for skin. The brightness therefore matches the
viewport: generic smoke (`smoke`) averages sRGB 129 versus 130 in the
viewport, the campfire is light gray in both, and the explosion comes out at
113 in the path tracer versus 95.

![Blast dust (frame 120): viewport on the left, path tracer on the right, 64 samples per pixel. The dust is lit through by multiple scattering and shades the street](img/pathtracer-dust.jpg)

Each scattering event in smoke counts as a bounce (Bounces in the Output node).
The path tracer renders gas through the **NanoVDB** library. Without it
(`-DPG_NANOVDB=OFF`) the gas is not rendered in the path tracer and the command
line reports it.

### Over the plate

Through a shot camera with a plate, the path tracer draws the CG over it.
Holdouts and shadow catchers are real things from the shot: the plate remains
there. On a catcher, the path tracer computes how much light the CG took from
it (shadows, smoke) and added (fire, bounced light), and multiplies the plate
by that. Through glass and water the plate is seen along the refracted ray.
Details are in
[plate.md](plate.md#in-the-final-render-cycles-and-the-path-tracer).

## 6. How it works

- **Scene** (`src/pg/render/Scene.h`): the triangles of the displayed geometry
  have the same normals and colors as in the viewport (`sim::shadedTriangles`).
  The scene also includes pieces and cloth from the solvers, the water surface
  and objects (spheres, boxes… computed exactly). **Instances**
  (`core/Instances.h`) have the prototype mesh once and are only placed, so a
  meadow with 122,000 clumps costs 8 clump meshes plus placements.
- **Grit and rain** (`src/pg/render/Particles.h`): `chipMesh` builds the
  fragment shapes once (a box clipped by planes, a shard prism), `placeChips`
  places them on free points with `pscale`, and `rainMesh` turns drops into
  spindles. The path tracer lets the rain material (`Material::Kind::Rain`)
  through with probability 1 − Opacity, as it does every streak hit from
  behind. Rain does not enter shadow rays at all (`Mesh::shadows`): Embree does
  not have it in the shadow scene, and the custom hierarchy skips it. Wetness
  (`Scene::wetAt`) darkens the surface and lowers its roughness towards 0.06.
- **Rays** (`src/pg/render/Embree.h`): what a ray hits is found by the
  **Intel Embree 4** library. It builds and traverses the bounding volume
  hierarchy (BVH) with the CPU's vector instructions (SSE, AVX2, AVX-512). Each
  mesh is one Embree scene. It is built once and kept with the mesh, so a grass
  clump that sways frame after frame is not rebuilt. Placements are instances
  of that scene. A mesh without a transform (terrain, pieces, water) is in the
  scene directly, and the ray is not rotated for it. Embree builds the
  hierarchy with SAH without spatial splits, so it is the same on any number of
  threads. In robust mode no ray slips through an edge between two
  triangles. A shadow ray first only checks whether anything
  opaque stands in its way. It then passes through glass and water face by
  face, picking up their color. Objects (spheres, boxes) are found by the
  custom hierarchy (`src/pg/render/Bvh.h`, SAH with 12 bins).
- **Without Embree** (`-DPG_EMBREE=OFF`, `PG_RAYS=own` in the environment, or
  in a build with the thread sanitizer, which cannot track Embree and TBB
  threads) rays are found by the custom hierarchy in meshes too. The result is
  the same up to rounding: the test `render_embree_meets_what_our_bvh_meets`
  compares 4,000 rays in both (meshes, instances, glass, objects, floor, shadows).
- **Gas** (`src/pg/render/Gas.h`): the smoke, temperature, flame and steam of
  a frame are in a **NanoVDB** grid (part of OpenVDB, Apache 2.0). The
  8 × 8 × 8-cell tiles that contain gas are the leaves of its tree, and values
  are read trilinearly between cell centers, like a texture in the viewport.
  Each tile stores the most smoke, flame and temperature that a point in it
  reads, i.e. including the layer of cells around it. A ray goes through gas
  by **delta tracking**: it takes steps as long as it would take in the densest
  smoke of the tile, and at each step the smoke decides whether light scatters
  there, with a probability proportional to how dense it is there. Empty tiles
  are skipped. Through a flame the ray steps at least one cell at a time, and
  each step adds the light the flame emits there. A shadow ray towards the sun
  takes the same steps and multiplies by the fraction of light each lets
  through (**ratio tracking**). When less than a tenth remains, Russian
  roulette decides. Both are unbiased: with more samples the noise decreases,
  not the accuracy (a test compares 20,000 rays with exactly computed
  transmittance, another does the same in moving smoke, including with a
  velocity over half a second). Scattering follows the same Henyey–Greenstein
  function as in the viewport: mostly forward, a little backward. The denoiser
  would have nothing to hold on to in smoke, because each sample either
  scatters in the smoke or passes through it. So, for each pixel, how much
  smoke the pixel sees and how far away is computed once, without noise.
  Albedo, normal and depth then blend smoothly from the surface behind the
  smoke into the smoke, according to how much of it the smoke covers.
- **Light** (`src/pg/render/PathTracer.h`): the sun is a disc, sampled directly
  at every bounce and weighted against bounces (multiple importance
  sampling). The sky is the Look formula, including Sky Behind. The surface is
  a combination of diffuse scattering, GGX specular and transmission to the
  other side (translucency). Glass and water reflect and refract according to
  Fresnel and Snell. From the third bounce on, Russian roulette decides.
- **Camera**: a thin lens (F-Stop, Focus). The pixel is sampled over its whole
  area, so edges are anti-aliased.
- **Determinism**: a sample's random numbers depend only on the pixel, the
  sample number and the seed, and the hierarchies are the same on any number
  of threads. The render is therefore the same on any number of threads (test
  `render_is_the_same_however_it_is_run`, with and without Embree). Embree,
  however, picks different instructions depending on the CPU, and those round
  differently. SSE2, SSE4.2 and AVX give the same image; AVX2 and AVX-512 each
  give a slightly different one. At 2 samples most pixels differ, but on
  average by 0.1 % of brightness. It is different noise, not a different image.
  The custom hierarchy gives the same image on all machines and with both GCC
  and clang.
- **Denoising** (`src/pg/render/Denoise.h`): **Intel Open Image Denoise**
  (Apache 2.0), a neural network trained on images from path tracers.
  It is used by Blender, Houdini (Karma), Arnold, V-Ray and Unreal. It receives
  the light, albedo and normals of the pixels. It first denoises the albedo and
  normals separately, because noise remains in them too: from depth of field,
  from gas, and from leaves and blades that cover a pixel only partially. Only
  then does it denoise the light. The filters are prepared once for a given
  image size. The same image is always denoised the same way. Without it
  (`-DPG_OIDN=OFF`, `PG_DENOISER=own` in the environment, or in a build with
  sanitizers) a custom à-trous wavelet filter (Dammertz et al.) denoises the
  light the surface received. The surface color is divided out and then
  restored, so the texture stays sharp. The filter stops at edges in color,
  normal and depth, and where pixels differ by more than the noise (variance of
  samples from a 5 × 5 neighborhood).
- **Output**: exposure and view from the Output: AgX, ACES 1.0 and 2.0 as in
  OpenColorIO, or the ACES tone curve (Narkowicz) and gamma 2.2 like the
  viewport ([color.md](color.md)). EXR stores linear light without the curve,
  in Rec. 709, ACEScg or ACES2065-1.

![Grass up close after 16 samples: no denoising, custom filter, Open Image Denoise; on the right 256 samples without denoising](img/pathtracer-denoise.jpg)

After 16 samples, the image from Open Image Denoise differs from the
256-sample image by 0.040, the one from the custom filter by 0.051 and the one
without denoising by 0.058 (root mean square error of the displayed values
0–1). It helps most in grass: 0.027 versus 0.045. The test
`render_open_image_denoise_comes_nearer_than_our_filter` measures the same in
linear light under an overcast sky after 4 samples against 512:
0.020, 0.035 and 0.074.

## 7. Performance

Four cores (Xeon 2.8 GHz with AVX-512), RelWithDebInfo, both columns measured
one after the other on the same machine:

| scene | resolution | samples | Embree | custom BVH |
|---|---|---|---|---|
| meadow from afar (grass, trees, shrubs) | 480 × 270 | 1 pass | 1.2 s | 2.0 s |
| meadow from afar | 640 × 360 | 64 | 60 s | 116 s |
| grass up close (rays go deep into the grass) | 640 × 360 | 64 | 85 s | 180 s |

In other scenes, a single pass with Embree is 1.6× faster for trees
(`tree_shapes`, 1.2 million triangles) and 1.5× for the street. Shadow rays
are 2–5× faster. Building the meadow scene takes 0.4 s with Embree and
0.8 s with the custom hierarchy.

Not even Embree makes the meadow faster. It has 122,000 small grass clumps
(112 triangles each) and their bounding boxes overlap.
A ray near the ground passes through 56 of them on average before it hits a
blade, and for each one it is rotated into the clump's space. Larger clumps
with more blades would be faster.

Denoising on four cores (Xeon 2.1 GHz with AVX-512): Open Image Denoise
needs 0.8 s for a 640 × 360 image, 3.4 s for 1280 × 720 and
7.9 s for 1920 × 1080, and up to 1.6 GB of memory. Two thirds of the time go
to the albedo and normals. The custom filter is about twice as fast (0.4 s,
1.7 s and 4.3 s). The Render tab therefore denoises only as often as keeps
denoising from taking more than half the time.

## 8. What is still missing

- Volumes of displayed geometry (for example from Convert Volume): only the
  viewport draws them; the path tracer draws simulation gas.
- Only the water mesh (`waterMesh`) has ripples from drops on water; running
  rivulets and wet vertical walls are missing.
- Flame light reaches the surroundings only through bounces that happen to hit
  the flame. Flames are not sampled directly like the sun, so the ground near
  a fire is noisier.
- Motion and mask passes in EXR.
- Bump from height: the path tracer bends light only with a normal map
  with UVs. Subsurface scattering.
- Lights other than the sun and sky (point, area), HDRI sky.
- Ray bundles (ray streams, wavefront): today one path is traced after
  another. Embree can handle 4, 8 or 16 concurrent rays at once.
- Adaptive sampling: more samples where there is noise.
