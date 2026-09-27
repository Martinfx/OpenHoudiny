# Procedurální geometrické jádro

Prototyp jádra open-source procedurálního systému pro zpracování geometrie:
node-based, nedestruktivní, headless-first.

- **[ROADMAP.md](ROADMAP.md)** — fáze, milníky, rozhodovací brány, rizika
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — datový model, cook engine, invarianty
- **[docs/shader-graph.md](docs/shader-graph.md)** — node editor shaderů pro
  OpenGL, OpenGL ES, Vulkan a Direct3D: jak funguje a jak ho rozšiřovat
- **[docs/pyro.md](docs/pyro.md)** — simulace kouře, ohně, vody a deště
  z uzlů (jako Pyro a FLIP v Houdini): editor se sítí uzlů, objekty a gizmo
  ve viewportu, proudění na 3D mřížce, multigrid, voda z částic, déšť ve
  větru, objemové vykreslování, hladina s odrazy a lomem a kamera záběru
- **[docs/geometry.md](docs/geometry.md)** — geometrie v téže síti (uzly
  jako SOP v Houdini): display flag, viewport, tabulka atributů, geometrie
  jako tvar překážek a zdrojů, simulace zpátky jako body a objemy
- **[docs/animation.md](docs/animation.md)** — klíčové snímky na libovolném
  parametru, pohyblivé překážky, jejichž pohyb převezme plyn i voda
- **[docs/cache.md](docs/cache.md)** — cache simulace na disku a export:
  body do PLY, objemy do OpenVDB, polygony do OBJ, snímek po snímku pro
  Houdini, Blender a renderery
- **[docs/render.md](docs/render.md)** — obrázky a video: PNG, sekvence,
  video `.avi` bez závislostí a `.mp4`/`.webm`/`.gif` přes ffmpeg, render na
  pozadí editoru s průběhem

> **Jméno je zatím placeholder.** Název `OpenHoudiny` je zaměnitelně podobný
> registrované ochranné známce SideFX; jmenný prostor v kódu je proto neutrální
> `pg`. Volba jména je blokující úkol fáze 0 roadmapy.

## Co to je

Prototyp **není produkt**. Existuje proto, aby měřením ověřil čtyři tvrzení,
na kterých architektura stojí, dokud je ještě levné je vyvrátit:

1. **Copy-on-write na atributových polích** udrží paměť v mezích u dlouhých
   řetězců uzlů.
2. **Líná pull evaluace s verzováním** dělá interaktivní editaci možnou.
3. **Deterministické chunkování** dá bitově stejný výsledek nezávisle na
   počtu vláken.
4. **Per-element jazyk** vázaný na sloty je použitelný a měřitelný baseline
   pro budoucí JIT.

Jedno z kritérií roadmapy prototyp rovnou vyvrátil — viz poznámku u M5.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Výchozí build obsahuje editor: při konfiguraci stáhne Dear ImGui a GLFW,
pokud v systému není (`sudo apt install libglfw3-dev`). Bez editoru nemá
build žádné externí závislosti, stačí C++20 a standardní knihovna:

```bash
cmake -S . -B build -DPG_BUILD_GUI=OFF
```

Se sanitizery:

```bash
cmake -S . -B build-asan -DPG_SANITIZE=ON -DPG_BUILD_GUI=OFF        && cmake --build build-asan && ./build-asan/pgtests
cmake -S . -B build-tsan -DPG_SANITIZE_THREAD=ON -DPG_BUILD_GUI=OFF && cmake --build build-tsan && ./build-tsan/pgtests
```

## Spuštění

```bash
./build/pgtests            # 185 testů: 59 jádro, 25 shader graf, 85 simulace, voda, déšť, geometrie, animace, 11 cache a export, 5 video
./build/pgbench            # měření tvrzení výše
./build/pgdemo out.obj --frames 24
./build/pgshader                                  # editor: prázdná scéna, Shift+A přidá oheň, vodu, déšť
./build/pgshader --example campfire               # příklad simulace: táborák
./build/pgshader sim campfire fire.mp4            # celý záběr do videa (.avi i bez ffmpeg)
./build/pgshader examples/shaders/fire.pgsg       # editor na síti shaderů, s grafem
./build/pgshader gen examples/shaders/marble.pgsg --target all -o out/
./build/pgshader sim campfire fire.png            # simulace bez okna, do PNG
./build/pgshader sim explosion out/boom.png --every 2 --set charge.fuel=80
./build/pgshader sim lakeside shot.png                # záběr kamerou: oheň, voda, déšť, vítr
./build/pgshader --example rock_garden            # geometrie v síti: kameny z kopií koule, déšť
./build/pgshader sim liquid_points points.png     # částice vody jako body obarvené wranglem
./build/pgshader --example wake                   # animace: koule projíždí bazénem, vlna a brázda
./build/pgshader sim campfire_vdb - --cache cache/fire                          # simulace jednou, na disk
./build/pgshader sim campfire_vdb fire.png --from-cache cache/fire --every 10   # render z cache
./build/pgshader sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
./build/pgshader sim liquid_points - --export 'out/water.$F4.ply'                # částice do PLY
./build/pgshader help                             # příkazy: list, gen, check, render, sim
```

`pgdemo` postaví graf `grid → pointwrangle → groupbox → blast → transform`,
cookne ho, vypíše tabulku atributů ve stylu geometry spreadsheetu a zapíše OBJ,
který jde otevřít v Blenderu nebo kdekoliv jinde.

## Příklad snippetu jazyka

```c
@P.y = noise(@P * 0.45 + vec3(@Time, 0.0, 0.0)) * 2.0 - 1.0;
@height = @P.y;
@Cd = vec3(fit(@P.y, -1.0, 1.0, 0.1, 1.0), 0.4, 0.8);
```

Typ vytvářeného atributu se odvodí z pravé strany: `@height` vznikne jako
`float`, `@Cd` jako `vec3`. Uzel se sám označí za časově závislý, protože
snippet čte `@Time`.

## Stav

Hotovo a otestováno: COW geometrie s objemy, cook engine, časová závislost,
LRU cache, deterministický paralelismus, per-element jazyk, 19 typů uzlů
(generátory, primitiva, scatter, copy to points, OBJ…), 59 testů jádra
(čisté pod ASan, UBSan i ThreadSanitizerem).

Vedle geometrie je síť druhého typu: **shader graf** s knihovnou uzlů
v textových souborech, generátorem pro čtyři jazyky (GLSL 330, GLSL ES 300,
Vulkan GLSL 450 → SPIR-V, HLSL) a editorem s živým náhledem, včetně
animovaných efektů (oheň, kouř). Každý vestavěný uzel se v CTestu překládá pro
všechny cíle přes glslangValidator a spirv-val.

**Simulace kouře, ohně, vody a deště** (`src/pg/sim`) se skládá z uzlů jako Pyro
a FLIP v Houdini: objekty scény (koule, kvádr, válec, kužel, prstenec a modely
ze souborů OBJ, každý posunutý, pootočený a protažený), zdroje stejných
tvarů (palivo, kouř, teplo, blikotání, pohyb, časové okno), síly (turbulence, vítr, vír,
atraktor, odpor), řešič, vzhled a výstup. Řešič počítá proudění plynu na 3D mřížce: posunutá
mřížka MAC, advekce MacCormack, hoření s rozpínáním, vorticity confinement
a tlak přes multigrid, který zná podlahu i překážky. Vodu nesou částice
(FLIP) a mřížka jí drží objem: tlak s volnou hladinou (ghost fluid, stěny
částečně zakryté tělesy) řeší metoda sdružených gradientů s multigridem.
Voda padá, tříští se, obtéká tělesa a plní nádrže; v obraze odráží oblohu
a objekty, láme světlo a v hloubce bere svou barvu. Déšť padá z mraku,
vítr ho v nárazech (frontách, které putují s větrem) šikmí, od objektů
odstřikuje a na vodě dělá kroužky; podlaha je mokrá. Všechno je
deterministické na libovolném počtu vláken. Uzel kamery určuje záběr:
editor se jí dívá (s rámečkem obrazu) a render i `pgshader sim` jdou
jejím pohledem v jejím rozlišení. Editor má pro simulaci i shadery stejné rozložení:
vlastní plátno uzlů se zoomem, panel parametrů, viewport (plyn na podlaze
se stíny, záře ohně, objekty, vodítka) a časovou osu nad cache snímků,
se simulací ve vlastním vlákně, undo/redo a jednadvaceti příklady. Ve viewportu se
pracuje jako ve 3D programu: klik vybírá, gizmo posouvá, otáčí a mění
velikost (W, E, R, přichytávání, lokální i světové osy) a Shift+A přidá
objekt, zdroj kouře nebo vody, déšť, sílu či kameru rovnou propojené do
sítě.

**Geometrie** žije ve stejné síti jako simulace, jako SOP v Houdini: krychle,
koule, válec, mřížka, soubor OBJ, scatter, copy to points, transform, merge,
wrangle a další. Počítá ji geometrické jádro inkrementálně — tah posuvníkem
přepočítá jen uzly za ním. Uzel s *display flagem* je ve viewportu (polygony
v barvách `Cd`, body, čáry, objemy) a tabulka atributů ukáže body, rohy,
primitiva, detail i objemy. Geometrie může být tvarem překážky, zdroje kouře
nebo vody, a simulace se vracejí jako geometrie: částice vody, kapky deště
a mřížky plynu jako body a objemy pro další uzly.

**Animace**: každý číselný parametr může mít klíčové snímky (Smooth, Linear,
Step) — kosočtverec u parametru, klíče na časové ose, K ve viewportu, gizmo
zapisuje klíče. Síť se překládá snímek po snímku a řešiče si každý krok
převezmou zdroje, síly a překážky toho snímku. Pohyblivé překážky předají
plynu i vodě svou rychlost i rotaci: koule v bazénu dělá vlnu a brázdu,
lopatka víří kouř, letící pochodeň nechává stopu.

**Cache a export**: snímky simulace jdou na disk a zpátky (editor:
Simulation › Save/Load Cache, `pgshader sim --cache` a `--from-cache`) —
přehrají se, vykreslí a vyexportují bez nového počítání; nuly se
nezapisují, 150 snímků táboráku má 63 MB. Geometrie kteréhokoli uzlu jde
ven snímek po snímku: body s atributy do PLY, objemy do OpenVDB (vlastní
zapisovač bez knihovny, soubory ověřené čtením v OpenVDB 10), polygony do
OBJ.

**Obrázky a video**: záběr jde do PNG, do očíslované sekvence nebo do videa
— `.avi` (Motion JPEG, vlastní kodér JPEG i kontejner) bez jakékoli
závislosti, `.mp4`, `.mov`, `.mkv`, `.webm` a `.gif` přes ffmpeg. Editor
renderuje po snímcích na pozadí s oknem průběhu, počká na simulaci a nakonec
nabídne soubor otevřít; příkazy `sim` a `render` kreslí bez okna přes EGL,
a když EGL nejde, přes skryté okno.

Vědomě chybí: USD, Alembic, čtení VDB, JIT, packed primitives, digital assets,
Python vazby, simulace těles a látek. Podrobně v
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-co-prototyp-skutečně-umí).

## Licence

Apache 2.0 (viz [ROADMAP.md §0.1](ROADMAP.md#01-právní-a-identita)).
Implementace je clean room — žádný HDK, žádný reverse engineering.
