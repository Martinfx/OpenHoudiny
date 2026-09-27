# Prototype

Prototyp profesionálního node-based softwaru pro procedurální geometrii
a vizuální efekty, po vzoru Houdini: geometrie z uzlů s atributy
a wranglem, simulace kouře, ohně, vody a deště, animace, render záběru do
obrázků a videa a export do dalších nástrojů. Program `prototype` bez
příkazu otevře editor, s příkazem (`sim`, `render`, `gen`, …) pracuje bez
okna.

- **[ROADMAP.md](ROADMAP.md)** — cíl, co je hotové, další kroky, rizika
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
- **[docs/wrangle.md](docs/wrangle.md)** — wrangle, jazyk pro výpočty nad
  geometrií jako VEX: proměnné, cykly, funkce, pole; běh nad body,
  primitivy i celou geometrií; sousedé, další vstupy, stavba a mazání
  geometrie; posuvníky z `ch()`
- **[docs/assets.md](docs/assets.md)** — digital assets: vybrané uzly
  jako jeden uzel s vlastními parametry a verzí, knihovna `.pgasset`,
  vstup dovnitř a zpět, síť nese své assety s sebou
- **[docs/animation.md](docs/animation.md)** — klíčové snímky na libovolném
  parametru, výrazy v parametrech (`$F`, `ch("../box1/sizex")`), pohyblivé
  překážky, jejichž pohyb převezme plyn i voda
- **[docs/cache.md](docs/cache.md)** — cache simulace na disku a export:
  body do PLY, objemy do OpenVDB, polygony do OBJ, snímek po snímku pro
  Houdini, Blender a renderery
- **[docs/render.md](docs/render.md)** — obrázky a video: PNG, sekvence,
  video `.avi` bez závislostí a `.mp4`/`.webm`/`.gif` přes ffmpeg, render na
  pozadí editoru s průběhem

> **Jméno.** Projekt se jmenuje **Prototype**; pracovní název byl příliš
> podobný ochranné známce SideFX. Jmenný prostor v kódu zůstává neutrální
> `pg` a formáty souborů (`.pgsim`, `.pgsg`, `.pgnodes`) se nemění.

## Co to je

Prototype **není produkt**. Ukazuje celou cestu, kterou jde profesionální
procedurální software — od geometrie přes simulace po obrázek a export —
a měří, kolik stojí. Jádro stojí na čtyřech tvrzeních, která prototyp
ověřuje měřením, dokud je ještě levné je vyvrátit:

1. **Copy-on-write na atributových polích** udrží paměť v mezích u dlouhých
   řetězců uzlů.
2. **Líná pull evaluace s verzováním** dělá interaktivní editaci možnou.
3. **Deterministické chunkování** dá bitově stejný výsledek nezávisle na
   počtu vláken.
4. **Per-element jazyk** vázaný na sloty je použitelný a měřitelný baseline
   pro budoucí JIT.

Jedno z kritérií původní roadmapy prototyp rovnou vyvrátil — viz
[ROADMAP.md §3](ROADMAP.md#3-co-je-hotové), poznámka u M5.

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
./build/pgtests            # 230 testů: 59 jádro, 27 jazyk wrangle a výrazy, 7 digital assets, 10 topologie, smyčky a vaření na pozadí, 25 shader graf, 86 simulace, voda, déšť, geometrie, animace, 11 cache a export, 5 video
./build/pgbench            # měření tvrzení výše
./build/pgdemo out.obj --frames 24
./build/prototype                                  # editor: prázdná scéna, Shift+A přidá oheň, vodu, déšť
./build/prototype --example campfire               # příklad simulace: táborák
./build/prototype sim campfire fire.mp4            # celý záběr do videa (.avi i bez ffmpeg)
./build/prototype examples/shaders/fire.pgsg       # editor na síti shaderů, s grafem
./build/prototype gen examples/shaders/marble.pgsg --target all -o out/
./build/prototype sim campfire fire.png            # simulace bez okna, do PNG
./build/prototype sim explosion out/boom.png --every 2 --set charge.fuel=80
./build/prototype sim lakeside shot.png            # záběr kamerou: oheň, voda, déšť, vítr
./build/prototype --example rock_garden            # geometrie v síti: kameny z kopií koule, déšť
./build/prototype --example spiral_stairs          # Detail Wrangle postaví schodiště, voda po něm stéká
./build/prototype sim liquid_points points.png     # částice vody jako body obarvené wranglem
./build/prototype --example wake                   # animace: koule projíždí bazénem, vlna a brázda
./build/prototype sim campfire_vdb - --cache cache/fire                          # simulace jednou, na disk
./build/prototype sim campfire_vdb fire.png --from-cache cache/fire --every 10   # render z cache
./build/prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
./build/prototype sim liquid_points - --export 'out/water.$F4.ply'                # částice do PLY
./build/prototype --example street                 # ulice ze tří digital assetů Building
./build/prototype cook street street.obj --set tower.floors=12   # geometrie bez okna, do OBJ
./build/prototype cook street - --hash --threads 1 # hash geometrie: stejný na 1 i 4 vláknech
./build/prototype help                             # příkazy: list, gen, check, render, sim, cook
```

`pgdemo` postaví graf `grid → pointwrangle → groupbox → blast → transform`,
cookne ho, vypíše tabulku atributů ve stylu geometry spreadsheetu a zapíše OBJ,
který jde otevřít v Blenderu nebo kdekoliv jinde.

## Příklad snippetu jazyka

```c
@P.y = noise(@P * 0.45 + vec3(@Time, 0.0, 0.0)) * 2.0 - 1.0;
@height = @P.y;
@Cd = vec3(fit(@P.y, -1.0, 1.0, 0.1, 1.0), 0.4, 0.8);
int near[] = nearpoints(0, @P, ch("radius"));
foreach (int pt; near) {
    if (pt > @ptnum) addprim(0, "polyline", @ptnum, pt);  // čáry k blízkým bodům
}
```

Typ vytvářeného atributu se odvodí z pravé strany: `@height` vznikne jako
`float`, `@Cd` jako `vector`. Uzel se sám označí za časově závislý, protože
snippet čte `@Time`, a `ch("radius")` mu přidá posuvník. Jazyk popisuje
[docs/wrangle.md](docs/wrangle.md).

## Stav

Hotovo a otestováno: COW geometrie s objemy, cook engine, časová závislost,
LRU cache, deterministický paralelismus, 26 typů uzlů (generátory,
primitiva, scatter, copy to points, OBJ, extrude, subdivide, clip…), 59 testů jádra (čisté pod ASan,
UBSan i ThreadSanitizerem). **Wrangle** je jazyk jako VEX: typy, proměnné,
cykly, funkce, pole, řetězce, matice; běží nad body, primitivy, rohy nebo
jednou nad celou geometrií, čte sousedy a další vstupy, staví a maže
geometrii a výsledek nezávisí na počtu vláken.

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
editor se jí dívá (s rámečkem obrazu) a render i `prototype sim` jdou
jejím pohledem v jejím rozlišení. Editor má pro simulaci i shadery stejné rozložení:
vlastní plátno uzlů se zoomem, panel parametrů, viewport (plyn na podlaze
se stíny, záře ohně, objekty, vodítka) a časovou osu nad cache snímků,
se simulací ve vlastním vlákně, undo/redo a dvaceti pěti příklady. Ve viewportu se
pracuje jako ve 3D programu: klik vybírá, gizmo posouvá, otáčí a mění
velikost (W, E, R, přichytávání, lokální i světové osy) a Shift+A přidá
objekt, zdroj kouře nebo vody, déšť, sílu či kameru rovnou propojené do
sítě.

**Geometrie** žije ve stejné síti jako simulace, jako SOP v Houdini: krychle,
koule, válec, mřížka, soubor OBJ, scatter, copy to points, transform, merge,
wrangle, PolyExtrude, Subdivide (Catmull-Clark), Clip s uzavřením řezu,
Fuse, Connectivity, Attribute Transfer a další. **Smyčky For-Each** pustí
část sítě pro každý kus, primitivum či bod, nebo opakovaně na vlastním
výsledku. Počítá ji geometrické jádro inkrementálně — tah posuvníkem
přepočítá jen uzly za ním. Uzel s *display flagem* je ve viewportu (polygony
v barvách `Cd`, body, čáry, objemy) a tabulka atributů ukáže body, rohy,
primitiva, detail i objemy. Geometrie může být tvarem překážky, zdroje kouře
nebo vody, a simulace se vracejí jako geometrie: částice vody, kapky deště
a mřížky plynu jako body a objemy pro další uzly.

**Digital assets**: vybrané geometrické uzly se stanou jedním uzlem
(Edit › Make Asset) s parametry, které si asset vybere (pravým na jméno
parametru › Promote). Dvojklik (I) vede dovnitř, U zpátky; každá změna
uvnitř je nová verze, kterou hned sledují všechny instance. Knihovna čte
soubory `.pgasset` z programu, z `$PROTOTYPE_ASSETS` a z uživatelské
složky; síť ukládá definice použitých assetů na svůj konec, takže se
otevře kdekoli. Asset **Building** postaví budovu z deseti posuvníků
(patra, rozměry, okna, balkony, barva) a příklad **street** z něj staví
ulici; `prototype cook` uvaří geometrii bez okna do OBJ, PLY nebo VDB
a vypíše její hash — stejný na 1 i 4 vláknech.

**Animace**: každý číselný parametr může mít klíčové snímky (Smooth, Linear,
Step) — kosočtverec u parametru, klíče na časové ose, K ve viewportu, gizmo
zapisuje klíče. Síť se překládá snímek po snímku a řešiče si každý krok
převezmou zdroje, síly a překážky toho snímku. Pohyblivé překážky předají
plynu i vodě svou rychlost i rotaci: koule v bazénu dělá vlnu a brázdu,
lopatka víří kouř, letící pochodeň nechává stopu.

**Cache a export**: snímky simulace jdou na disk a zpátky (editor:
Simulation › Save/Load Cache, `prototype sim --cache` a `--from-cache`) —
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

Vědomě chybí (zatím): USD, Alembic, čtení VDB, JIT, packed primitives,
Python vazby, simulace těles a látek — pořadí v
[ROADMAP.md §4](ROADMAP.md#4-další-kroky). Podrobně v
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-co-prototyp-skutečně-umí).

## Licence

Plánovaná licence je Apache 2.0 (viz [ROADMAP.md §7](ROADMAP.md#7-jméno-licence-clean-room)).
Implementace je clean room — žádný HDK, žádný reverse engineering.
