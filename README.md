# Procedurální geometrické jádro

Prototyp jádra open-source procedurálního systému pro zpracování geometrie:
node-based, nedestruktivní, headless-first.

- **[ROADMAP.md](ROADMAP.md)** — fáze, milníky, rozhodovací brány, rizika
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — datový model, cook engine, invarianty
- **[docs/shader-graph.md](docs/shader-graph.md)** — node editor shaderů pro
  OpenGL, OpenGL ES, Vulkan a Direct3D: jak funguje a jak ho rozšiřovat
- **[docs/pyro.md](docs/pyro.md)** — simulace kouře a ohně z uzlů (jako Pyro
  v Houdini): editor se sítí uzlů, objekty a gizmo ve viewportu, proudění
  na 3D mřížce, multigrid, objemové vykreslování

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
./build/pgtests            # 113 testů: 50 jádro, 25 shader graf, 38 simulace kouře a ohně a objekty
./build/pgbench            # měření tvrzení výše
./build/pgdemo out.obj --frames 24
./build/pgshader                                  # editor: simulace z uzlů, táborák (výchozí)
./build/pgshader --example tornado                # jiný příklad simulace
./build/pgshader examples/shaders/fire.pgsg       # editor na síti shaderů, s grafem
./build/pgshader gen examples/shaders/marble.pgsg --target all -o out/
./build/pgshader sim campfire fire.png            # simulace bez okna, do PNG
./build/pgshader sim explosion out/boom.png --every 2 --set charge.fuel=80
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

Hotovo a otestováno: COW geometrie, cook engine, časová závislost, LRU cache,
deterministický paralelismus, per-element jazyk, 10 typů uzlů, 50 testů
(čisté pod ASan, UBSan i ThreadSanitizerem).

Vedle geometrie je síť druhého typu: **shader graf** s knihovnou uzlů
v textových souborech, generátorem pro čtyři jazyky (GLSL 330, GLSL ES 300,
Vulkan GLSL 450 → SPIR-V, HLSL) a editorem s živým náhledem, včetně
animovaných efektů (oheň, kouř). Každý vestavěný uzel se v CTestu překládá pro
všechny cíle přes glslangValidator a spirv-val.

**Simulace kouře a ohně** (`src/pg/sim`) se skládá z uzlů jako Pyro
v Houdini: objekty scény (koule, kvádr, válec, kužel, prstenec, každý
posunutý, pootočený a protažený), zdroje stejných tvarů (palivo, kouř,
teplo, blikotání, pohyb, časové okno), síly (turbulence, vítr, vír,
atraktor, odpor), řešič, vzhled a výstup. Řešič počítá proudění plynu na 3D mřížce: posunutá
mřížka MAC, advekce MacCormack, hoření s rozpínáním, vorticity confinement
a tlak přes multigrid, který zná podlahu i překážky. Je deterministický na
libovolném počtu vláken. Editor má pro simulaci i shadery stejné rozložení:
vlastní plátno uzlů se zoomem, panel parametrů, viewport (plyn na podlaze
se stíny, záře ohně, objekty, vodítka) a časovou osu nad cache snímků,
se simulací ve vlastním vlákně, undo/redo a osmi příklady. Ve viewportu se
pracuje jako ve 3D programu: klik vybírá, gizmo posouvá, otáčí a mění
velikost (W, E, R, přichytávání, lokální i světové osy) a Shift+A přidá
objekt, zdroj nebo sílu rovnou propojené do sítě.

Vědomě chybí: I/O (USD, Alembic, VDB), JIT, packed primitives, digital assets,
serializace scény, Python vazby, GUI pro geometrii, simulace kapalin, těles a
látek. Podrobně v [ARCHITECTURE.md §9](ARCHITECTURE.md#9-co-prototyp-skutečně-umí).

## Licence

Apache 2.0 (viz [ROADMAP.md §0.1](ROADMAP.md#01-právní-a-identita)).
Implementace je clean room — žádný HDK, žádný reverse engineering.
