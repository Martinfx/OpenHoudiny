# Vestavěné uzly shader grafu

Vygenerováno příkazem `./build/pgshader list --markdown` z
[src/pg/shader/builtin.pgnodes](../src/pg/shader/builtin.pgnodes); ručně needitovat.
Popisy pocházejí přímo z definic uzlů, proto jsou anglicky, stejně jako v editoru.
Jak přidat vlastní uzel, popisuje [shader-graph.md §7](shader-graph.md#7-rozšiřitelnost).

| Node | Category | Inputs | Outputs | What it does |
|---|---|---|---|---|
| **Color + Alpha** `rgba` | Color | rgb: vec3, alpha: float | rgba: vec4 | A colour and its opacity, for an output that blends. |
| **Color Ramp** `color_ramp` | Color | t: float, color0: vec3, color1: vec3, color2: vec3, color3: vec3, pos1: float, pos2: float | color: vec3 | Maps t from 0 to 1 onto four colours; the middle two sit at pos1 and pos2. |
| **Color** `color` | Input | - | color: vec3 | A colour baked into the shader. |
| **Color Parameter** `color_parameter` | Input | - | color: vec3 | A colour uniform u_&lt;name&gt; that the application can change without recompiling. |
| **Constant** `constant` | Input | - | result: float | A number baked into the shader. |
| **Float Parameter** `float_parameter` | Input | - | value: float | A uniform u_&lt;name&gt; that the application can change without recompiling. |
| **Light Direction** `light_direction` | Input | - | light: vec3 | Unit vector from the surface towards the light. |
| **Normal** `normal` | Input | - | normal: vec3 | World-space unit normal of the surface. |
| **Position** `position` | Input | - | position: vec3 | World-space position of the surface. |
| **Time** `time` | Input | - | time: float | Seconds since the start -- animates whatever it feeds. |
| **UV** `uv` | Input | - | uv: vec2 | Texture coordinates of the mesh. |
| **View Direction** `view_direction` | Input | - | view: vec3 | Unit vector from the surface towards the camera. |
| **Blinn-Phong Specular** `blinn_phong` | Lighting | normal: vec3 = $normal, light: vec3 = $light, view: vec3 = $view, shininess: float | specular: float | The highlight of a shiny surface; higher shininess, smaller spot. |
| **Fresnel** `fresnel` | Lighting | normal: vec3 = $normal, view: vec3 = $view, power: float | result: float | Rises towards 1 where the surface turns away from the camera. |
| **Lambert** `lambert` | Lighting | color: vec3, normal: vec3 = $normal, light: vec3 = $light, ambient: float | result: vec3 | Diffuse light from one directional light, plus ambient. |
| **Add** `add` | Math | a: any, b: any | result: any |  |
| **Divide** `divide` | Math | a: any, b: any | result: any |  |
| **Fraction** `fract` | Math | x: any | result: any | The part after the decimal point: repeats every 1. |
| **Mix** `mix` | Math | a: any, b: any, t: float | result: any | Blends a and b: t = 0 gives a, t = 1 gives b. |
| **Multiply** `multiply` | Math | a: any, b: any | result: any |  |
| **One Minus** `one_minus` | Math | x: any | result: any |  |
| **Power** `power` | Math | base: any, exponent: any | result: any |  |
| **Remap** `remap` | Math | x: any, from_min: float, from_max: float, to_min: float, to_max: float | result: any | Maps x from [from_min, from_max] onto [to_min, to_max]. |
| **Saturate** `saturate` | Math | x: any | result: any | Clamps to [0, 1]. |
| **Sine** `sine` | Math | x: any | result: any |  |
| **Smoothstep** `smoothstep` | Math | edge0: float, edge1: float, x: any | result: any | 0 below edge0, 1 above edge1, a smooth curve between. |
| **Subtract** `subtract` | Math | a: any, b: any | result: any |  |
| **Surface Output** `surface_output` | Output | color: vec4, offset: vec3 | - | The result: the colour of each pixel, and an optional world-space offset of each vertex. Alpha matters when blend is alpha or additive. |
| **Checker** `checker` | Pattern | uv: vec2 = $uv, scale: float, color1: vec3, color2: vec3 | color: vec3, mask: float |  |
| **Fractal Noise** `fractal_noise` | Pattern | position: vec3 = $position, scale: float, offset: vec3, octaves: float, roughness: float | value: float | Octaves of 3D value noise added up, between 0 and 1. Animate it through offset. |
| **Noise** `noise` | Pattern | uv: vec2 = $uv, scale: float | value: float | Smooth value noise between 0 and 1. |
| **Texture** `texture` | Texture | uv: vec2 = $uv | rgba: vec4, rgb: vec3, alpha: float | Samples the 2D texture the application binds as u_&lt;name&gt;. |
| **Combine XYZ** `combine` | Vector | x: float, y: float, z: float | vector: vec3 |  |
| **Dot Product** `dot` | Vector | a: vec3, b: vec3 | result: float |  |
| **Length** `length` | Vector | v: any | result: float |  |
| **Normalize** `normalize` | Vector | v: vec3 | result: vec3 |  |
| **Split XYZ** `split` | Vector | vector: vec3 | x: float, y: float, z: float |  |
