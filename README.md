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
- **[docs/editing.md](docs/editing.md)** — úpravy geometrie ve viewportu
  jako v Houdini: body, hrany a plochy vybrané myší (klik, obdélník, laso,
  štětec; jen viditelné, nebo i skryté), úchyt je posune, otočí
  a zvětší (uzel Edit; měkký výběr s náhledem, vzdálenost i po povrchu),
  skupina a mazání z vybraného (Group, Blast), štětec maluje atribut —
  piny a trhání látky (Attribute Paint) — a tvaruje geometrii jako hlínu:
  vytlačit, zatlačit, uhladit, chytit, zarovnat (Sculpt, přírůstkově
  i na milionu bodů)
- **[docs/trees.md](docs/trees.md)** — stromy, jak rostou rostliny (uzel
  Tree po vzoru Webera a Penna): kmen i s vidlicí do vůdčích větví, tři
  úrovně větví kolem rodiče o zlatý úhel, sedm tvarů koruny (smrk, dub,
  bříza, topol, akácie, vrba, lípa), listy i jehličí; les na bodech, každý
  strom jiný; vítr wranglem podle `flex`; kostra pro vlastní listy
- **[docs/vegetation.md](docs/vegetation.md)** — vegetace jako instance:
  tráva z trsů stébel (uzel Grass) po terénu podle namalované hustoty
  a sklonu, keře a stromy jako varianty, které zastupují body; viewport je
  kreslí přes GPU instancing, USD dostane PointInstancer, OBJ kopie; vítr
  otáčí `orient`; louka u lesa s 1,9 milionu stébel za 148 ms
- **[docs/cycles.md](docs/cycles.md)** — render přes Cycles z Blenderu:
  záložka Render vedle Viewportu (první obraz hned, při přehrávání snímek
  po snímku) i příkazová řádka `--renderer cycles`; scéna převedená do
  Cycles i s kouřem a ohněm, Principled BSDF, sklo a voda, fyzikální
  obloha a slunce jako v Blenderu s nastavitelnými mraky nebo obloha
  z obrázku (HDRI), převod barev AgX, detail povrchů, odšumění Open Image
  Denoise; drť jako hranaté úlomky kamene a skla, kapky deště jako čárky
  vody a mokrá zem, kam prší; rozmazání pohybem, dokud je otevřená
  závěrka (kusy, drť, látka, voda, kamera)
- **[docs/materials.md](docs/materials.md)** — materiály a textury: plochy
  říkají, z čeho jsou (`s@material`: beton, lom betonu, omítka, cihla,
  okno, ocel, dřevo, kůra, dlažba, tašky, trávník…), generátory si je
  nastaví samy, uzel Material komukoli; Cycles kreslí fotografie z knihovny
  (Bistro a Babylon.js, CC-BY 4.0) nebo procedurální vzory, path tracer
  fotografie bez reliéfu; tašky podél střechy, řady vodorovně; geometrie
  bez `Cd` v barvách svých materiálů; vlastní textury z Poly Haven
  a ambientCG; textura jde s kusem, který letí
- **[docs/pathtracer.md](docs/pathtracer.md)** — vlastní path tracer na
  procesoru, druhá volba záložky Render a `--renderer path`; odražené
  světlo, měkké slunce, prosvítající tráva a listí, sklo a voda, drť,
  déšť a mokrý povrch, hloubka ostrosti, odšumění neuronovou sítí (Intel
  Open Image Denoise); nastavení v uzlu Output, PNG i EXR
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
  Houdini, Blender a renderery; bake na pozadí s průběhem, zrušením
  a pokračováním z checkpointu, náhled v polovičním rozlišení, wedge
  (varianty parametru) a profil kroku
- **[docs/destruction.md](docs/destruction.md)** — destrukce: Voronoi
  Fracture, tuhá tělesa nad Jolt Physics, slepené kusy jako jedno těleso,
  které nárazy lámou, nálože, drcení na prach, drť jako částice, které
  narážejí do kusů a zůstávají na nich ležet, prach za letícími kusy,
  vzduch vytlačený zřícením, který žene prach do ulic, pád řízený
  animací (Guide), trosky ve vodě a v plynu (dřevo plave a proud ho
  unáší, drť nese tlaková vlna); odstřel věžáku ve městě a zřícení zdi
  z pohledu od země jako videa, povodeň na dvoře s plovoucími bednami
- **[docs/cloth.md](docs/cloth.md)** — látky, lana a měkká tělesa (XPBD,
  obdoba Vellum): ubrus přes stůl, vlajka ve větru, míč držící objem,
  přišpendlené body nesené animací, kolize s objekty, kusy RBD i sebou
  samou, vítr a proud plynu; trhání a obousměrná vazba s tuhými tělesy
  (plachta chytá bedny, betonový blok ji prorazí)
- **[docs/python.md](docs/python.md)** — Python API (`import pg`): sítě,
  parametry, geometrie jako pole numpy bez kopie, simulace po snímcích,
  cache, USD a render ze skriptu; síť jako Python kód (`as_code()`)
- **[docs/usd.md](docs/usd.md)** — celý záběr do USD pro Houdini, Blender
  a renderery: geometrie, kusy jako tělesa v pohybu, drť, povrch vody,
  déšť, prach jako VDB, kamera, slunce a obloha; co se mění každý snímek,
  v souboru pro každý snímek (value clips)
- **[docs/usd-import.md](docs/usd-import.md)** — čtení USD bez knihovny
  (`.usda`, `.usdc`, `.usdz`): scéna složená jako v USD (sublayers,
  reference, payloady, varianty, třídy, value clips), kamera z matchmove
  jako kamera záběru, kulisa a modely jako geometrie v síti; ověřené proti
  knihovně USD
- **[docs/render.md](docs/render.md)** — obrázky a video: PNG, sekvence,
  video `.avi` bez závislostí a `.mp4`/`.webm`/`.gif` přes ffmpeg, render na
  pozadí editoru s průběhem; EXR v lineárním světle s hloubkou, vektory
  pohybu a maskami pro compositing
- **[docs/plate.md](docs/plate.md)** — obraz záběru (plate) za CG kamerou
  záběru: sekvence PNG, JPEG a EXR čtené bez knihoven, holdout a shadow
  catcher, do EXR CG s alfou a průchodem `catcher`; ve viewportu, v Cycles
  i v path traceru

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

Vektory, matice a kvaterniony počítá celý program knihovnou **GLM**
(OpenGL Mathematics, MIT, jen hlavičky). Použije se systémová verze 1.0
nebo novější (`pkg install glm` na FreeBSD, `sudo apt install libglm-dev`
na Debianu 13 a Ubuntu 25.04), jinak ji CMake stáhne. Starší balíčky
(Ubuntu 24.04 má 0.9.9) se přeskočí.

Paprsky path traceru hledá knihovna **Intel Embree 4** (Apache 2.0):
`pkg install embree` na FreeBSD, `sudo apt install libembree-dev` na
Debianu 13 a Ubuntu 24.04. Když v systému není, CMake ji stáhne a jednou
postaví jen s tím, co path tracer potřebuje (na čtyřech jádrech asi
11 minut). Bez ní
(`-DPG_EMBREE=OFF`) path tracer použije vlastní BVH: stejný obraz, ale
pomaleji ([docs/pathtracer.md](docs/pathtracer.md)).

Kouř, oheň a prach renderuje path tracer přes **NanoVDB** (součást
OpenVDB, Apache 2.0, jen hlavičky, nic se nekompiluje). Použije se
systémová, pokud ji jde tímto překladačem přeložit. Jinak ji CMake
stáhne s OpenVDB 13.0 (35 MB za pár sekund). Ubuntu 24.04 má starou
verzi 10.0.1 s jiným rozhraním, ta se přeskočí. Přeskočí se i NanoVDB
z OpenVDB 13.1 s libc++ 18 (clang 18 na FreeBSD 14), protože potřebuje
`std::atomic_ref`, které tahle libc++ nemá. Bez NanoVDB
(`-DPG_NANOVDB=OFF`) path tracer plyn nevykreslí, viewport ano.

Šum, který render nechá, odstraní **Intel Open Image Denoise 2**
(Apache 2.0): `pkg install oidn` na FreeBSD. Když v systému není, CMake
ji stáhne a jednou postaví ze zdrojů: verzi 2.3.3, staticky, jen pro
procesor, asi za minutu. K tomu potřebuje ISPC 1.21 nebo novější a TBB
(`sudo apt install ispc libtbb-dev` na Debianu a Ubuntu). Bez ní
(`-DPG_OIDN=OFF`) odšumuje vlastní filtr.

Finální obraz renderuje **Cycles** z Blenderu (Apache 2.0). CMake ho
stáhne z GitHubu (značka v4.5.0) a jednou postaví jen pro procesor
(na čtyřech jádrech asi 2 minuty). Potřebuje **OpenImageIO** a TBB: `pkg
install openimageio pugixml onetbb` na FreeBSD, `sudo apt install
libopenimageio-dev libpugixml-dev libtbb-dev` na Debianu a Ubuntu. Bez
nich (nebo s `-DPG_CYCLES=OFF`) renderuje vlastní path tracer
([docs/cycles.md](docs/cycles.md)).

Výchozí build obsahuje editor: při konfiguraci stáhne Dear ImGui a GLFW,
pokud v systému není (`sudo apt install libglfw3-dev`). Když najde vývojové
soubory Pythonu (`python3-dev`), stáhne pybind11 a postaví i modul `pg` do
`build/python` ([docs/python.md](docs/python.md)); jiný Python vybere
`-DPython3_EXECUTABLE=…`. Bez editoru a bez Pythonu nemá build žádné
externí závislosti kromě Jolt, GLM, Embree, NanoVDB, Open Image Denoise a Cycles (s OpenImageIO), stačí C++20 a standardní
knihovna:

```bash
cmake -S . -B build -DPG_BUILD_GUI=OFF -DPG_BUILD_PYTHON=OFF
```

Se sanitizery:

```bash
cmake -S . -B build-asan -DPG_SANITIZE=ON -DPG_BUILD_GUI=OFF        && cmake --build build-asan && ./build-asan/pgtests
cmake -S . -B build-tsan -DPG_SANITIZE_THREAD=ON -DPG_BUILD_GUI=OFF && cmake --build build-tsan && ./build-tsan/pgtests
```

## Spuštění

```bash
./build/pgtests            # 384 testů: 65 jádro, 27 jazyk wrangle a výrazy, 7 digital assets, 13 topologie, fracture, smyčky a vaření na pozadí, 17 tuhá tělesa, 10 beton a kry, 8 výztuž, 8 sklo, 7 cihly, 7 síť vazeb, 8 drť, 6 usměrněná simulace, 7 trosky ve vodě a v plynu, 25 shader graf, 98 simulace (i řídká mřížka), voda, déšť, geometrie, animace, 19 cache a export, 11 checkpointy, bake, náhled a profil, 10 zápis USD, 16 čtení USD, 3 EXR, 5 video, 7 obrázky a plate
ctest --test-dir build -R python                   # 46 testů modulu pg (Python); proti knihovnám USD, Pillow a OpenEXR, jsou-li
./build/pgeditortests      # rozhraní editoru bez okna a bez OpenGL: písmo, Escape a menu, nabídka uzlů, řádky, záložky, jména uzlů v síti
PYTHONPATH=build/python python3 examples/python/fracture_stats.py
./build/pgbench            # měření tvrzení výše
./build/pgbench_rigid      # tuhá tělesa: věž odstřelu a desetkrát víc kusů, 1 a všechna vlákna
./build/pgbench_pyro 96 576 # prach odstřelu v rozlišeních: čas fází, paměť, kolik domény prach zabírá (--dense: hustě)
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
./build/prototype --example meadow                 # louka u lesa: tráva, keře a stromy jako instance
./build/prototype cook street street.obj --set tower.floors=12   # geometrie bez okna, do OBJ
./build/prototype cook street - --hash --threads 1 # hash geometrie: stejný na 1 i 4 vláknech
./build/prototype sim matchmove mm.png --every 24  # oheň v kulise z USD, přes kameru z matchmove (USD)
PYTHONPATH=build/python python3 examples/usd/make_plate.py   # plate záběru: pak hoří v natočeném dvoře
./build/prototype usd examples/usd/shot.usda       # co USD soubor obsahuje: vrstvy, strom, kamery, geometrie
./build/prototype help                             # příkazy: list, gen, check, render, sim, cook, usd
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
LRU cache, deterministický paralelismus, 28 typů uzlů (generátory,
primitiva, scatter, copy to points, OBJ, extrude, subdivide, clip, objem
na polygony…), 65 testů jádra (čisté pod ASan,
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
a tlak přes multigrid, který zná podlahu i překážky. Mřížka je řídká jako
v Sparse Pyro: počítají se jen dlaždice 8 × 8 × 8 buněk, kde je plyn, takže
prach odstřelu ve 103,5 milionu voxelů trvá 19 minut. Vodu nesou částice
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
se simulací ve vlastním vlákně, undo/redo a dvaceti šesti příklady. Ve viewportu se
pracuje jako ve 3D programu: klik vybírá, gizmo posouvá, otáčí a mění
velikost (W, E, R, přichytávání, lokální i světové osy) a Shift+A přidá
objekt, zdroj kouře nebo vody, déšť, sílu či kameru rovnou propojené do
sítě.

**Geometrie** žije ve stejné síti jako simulace, jako SOP v Houdini: krychle,
koule, válec, mřížka, soubor OBJ, scatter, copy to points, transform, merge,
wrangle, PolyExtrude, Subdivide (Catmull-Clark), Clip s uzavřením řezu,
Fuse, Connectivity, Attribute Transfer, Convert Volume (objem na
polygony) a další. **Smyčky For-Each** pustí
část sítě pro každý kus, primitivum či bod, nebo opakovaně na vlastním
výsledku. Počítá ji geometrické jádro inkrementálně — tah posuvníkem
přepočítá jen uzly za ním. Uzel s *display flagem* je ve viewportu (polygony
v barvách `Cd`, body, čáry, objemy) a tabulka atributů ukáže body, rohy,
primitiva, detail i objemy. Geometrie může být tvarem překážky, zdroje kouře
nebo vody, a simulace se vracejí jako geometrie: částice vody, kapky deště
a mřížky plynu jako body a objemy pro další uzly, a voda i jako uzavřený
povrch s rychlostí a pěnou (Liquid Surface), ze kterého ji renderer renderuje.

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

**Destrukce**: **Voronoi Fracture** rozřeže uzavřené těleso na kusy
(řezy rovinami s víčky, kusy dohromady jsou přesně původní těleso),
**Concrete Fracture** ho rozláme jako beton — nestejné kusy, nejmenší
kolem místa nárazu, odprýsklé rohy, hrubé lomy, které do sebe dál
přesně zapadají, a pod nimi rovný řez (`proxy`) pro simulaci —
**RBD Cluster** seskupí kusy do ker s pevnějším lepidlem uvnitř, které se
rozpadnou až při tvrdém dopadu (sekundární lámání), **Rebar** položí do
zdi síť a do trámu armokoš ocelových prutů, na kterých kusy visí i po
prasknutí lepidla — pruty se ohýbají, vytahují a trhají —, **Glass
Fracture** rozláme tabuli skla paprsky a kruhy kolem místa úderu (celá
zůstane, dokud nepraskne, a renderer ji kreslí průhlednou s odrazy),
**Brick Wall** vyzdí zeď z cihel ve vazbě s maltou, omítkou a otvory
(rozpadá se ve spárách, některé cihly se rozlomí vedví), **RBD
Constraints** udělá z lepidla geometrii — síť vazeb, bod na kus a čáru
na spoj — kterou jde zeslabit, smazat nebo dokreslit a zapojit zpátky do
solveru, a
**RBD Solver** nad [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
z nich udělá tuhá tělesa: konvexní obaly s hmotou; kusy slepené tam, kde
se dotýkají plochou, jsou jedno těleso, dokud je náraz silnější než
lepidlo (`glue` v kPa) nerozlomí. Nálože (`release`, `kick`, `vanish`)
lepidlo v daný čas přetrhnou, kusy s `crush` se pod padajícími patry
rozdrtí na prach, nárazy sypou drť a vytlačený vzduch žene prach do ulic.
Drť jsou částice: vylétá z okraje plochy, kde praskl spoj, vzduch ji
brzdí a točí se, naráží do kusů i překážek, zůstane ležet na schodu nebo
na kusu a jede s ním; za utrženými kusy se táhne prach (`trail`) a RBD
Pieces dá drť jako body s `orient` pro Copy to Points. Pád jde režírovat:
animace kusů (klíčovaný Transform kolem Pivotu) zapojená do **Guide**
RBD Solveru vede kusy, kam je chce záběr, a pustí je, když praskne
lepidlo, když uplyne čas nebo když je něco zastaví dál než `guide_reach`.
Klíčované objekty jsou kinematické překážky, kusy jdou jako pohyblivé
překážky do vody, plynu i deště, prach do Pyro Solveru, a uzel RBD Pieces
je vrací jako geometrii s rychlostí `v`. Jolt běží na všech vláknech
a deterministicky: stejné snímky při každém běhu a na libovolném počtu
vláken, snímky do cache. Věž odstřelu (593 kusů) se krokuje za 3 ms na
snímek, desetkrát víc kusů za 31 ms (4 vlákna); Voronoi Fracture řeže
každou buňku jen z blízkých částí tělesa, věž z 5 628 buněk za 1 s.
Příklad **demolition**: odstřel čtrnáctipatrového věžáku v bloku domů
za zlatého světla; příklad **wall_collapse**: průčelí cihlového domu
vyletí do ulice a kusy se kutálejí ke kameře těsně nad asfaltem, v prachu
proti slunci; příklad **concrete_wall**: demoliční koule prorazí
betonovou zeď na soklu (`rings` drží škodu kolem koule, zbytek zdi
stojí) a z díry se sypou kry s hrubými lomy, úlomky a prach; příklad
**concrete_drop**: betonový trám se zlomí přes kvádr a jeho poloviny se
rozpadnou na kry, až dopadnou; příklad **brick_wall**: demoliční koule
prorazí cihlovou zeď domu vedle okna, díra je stupňovitá po vrstvách a
okno zůstane celé; příklad **concrete_column**: nálož v půlce
železobetonového sloupu obnaží armokoš a kusy betonu na něm visí;
příklad **constraint_network**: síť vazeb zeslabená podél čáry — koule
vylomí roh zdi a zeď praskne přesně po ní; příklad **debris_stairs**:
podetnutý betonový sloup se skácí ze schodů a drť zůstane ležet na
stupních; příklad **guided_fall**: odstřelený komín padne podle Guide
přesně do ulice mezi dva domy a na silnici se rozlomí; příklad
**house_collapse**: rodinný dům postavený jako skutečný (zdi z tvárnic
v cyklu For-Each, stropy, střecha s taškami, okna se skly, okapy, plot) se
zřítí do zahrady a oblak prachu se plazí ulicí se stromy a sousedními
domy, fotorealisticky v Cycles ([destruction.md](docs/destruction.md#dvanáctý-příklad-zřícení-rodinného-domu)).

**Animace**: každý číselný parametr může mít klíčové snímky (Smooth, Linear,
Step) — kosočtverec u parametru, klíče na časové ose, K ve viewportu, gizmo
zapisuje klíče. Síť se překládá snímek po snímku a řešiče si každý krok
převezmou zdroje, síly a překážky toho snímku. Pohyblivé překážky předají
plynu i vodě svou rychlost i rotaci: koule v bazénu dělá vlnu a brázdu,
lopatka víří kouř, letící pochodeň nechává stopu.

**Cache a export**: snímky simulace jdou na disk a zpátky (editor:
Simulation › Save/Load Cache, `prototype sim --cache` a `--from-cache`) —
přehrají se, vykreslí a vyexportují bez nového počítání; nuly se
nezapisují, 150 snímků táboráku má 63 MB. **Bake to Disk** spočítá záběr
v plném rozlišení v samostatném procesu; editor zůstane volný, přehrává
snímky z disku, jak přibývají, a ukazuje průběh a odhad do konce. Každých
10 snímků se uloží celý stav simulace (checkpoint), takže přerušený bake
pokračuje, kde skončil, bitově stejně jako nepřerušený. **Preview
Resolution** mezitím počítá plyn a vodu v polovičním rozlišení na ladění.
**Wedge** spočítá parametr ve více hodnotách, každou do vlastní složky,
a **Profile** ukáže, kam jde čas kroku. Geometrie kteréhokoli uzlu jde
ven snímek po snímku: body s atributy do PLY, objemy do OpenVDB (vlastní
zapisovač bez knihovny, soubory ověřené čtením v OpenVDB 10), polygony do
OBJ. Celý záběr jde do **USD** jako jedna scéna `.usda` (`--export
shot.usda`, v editoru File › Export USD Scene…): geometrie, kusy jako
tělesa, která se pohybují (tvar jednou, pak jen poloha a otočení), drť,
povrch vody, déšť, prach jako VDB vedle, kamera, slunce a obloha. Co je
velké a v každém snímku jiné, jde do souboru pro každý snímek, zapsaného
hned, jak snímek přijde, a scéna ho skládá (USD value clips) — záběr
libovolné délky se nemusí vejít do paměti. Ověřeno Pixarovou knihovnou,
všechny validátory bez nálezu ([docs/usd.md](docs/usd.md)).

**Čtení USD**: záběr od ostatních oddělení jde do sítě bez knihovny USD,
z textu `.usda`, binárního `.usdc` (všechny verze od 0.4.0) i balíčku
`.usdz`, složený jako v USD: sublayers s posunem času, reference
a payloady, varianty (výběr záběru přebije výchozí volbu assetu), třídy,
value clips. Uzel **USD Camera** dá Outputu kameru z matchmove, snímek po
snímku, s objektivem napasovaným na film. **USD Import** přinese kulisu,
modely nebo cache jako geometrii (normály, uv, barvy, primvars, subsety
jako skupiny) v metrech s Y nahoru, i když soubor přišel z Mayi
v centimetrech se Z nahoru. Transformace, skládání, geometrie i value
clips sedí s knihovnou USD ([docs/usd-import.md](docs/usd-import.md)).

**Plate**: obraz záběru, sekvence PNG, JPEG nebo EXR, jde za CG, když se
díváte kamerou záběru, v editoru i v renderu. Čtečky jsou vlastní, bez
knihoven: JPEG bit po bitu jako libjpeg, PNG a EXR všech kompresí kromě DWA
hodnota po hodnotě jako Pillow a OpenEXR. Objekty i podlaha mohou být
skutečné věci ze záběru. **Holdout** schová CG za sebou. **Shadow
catcher** navíc na sebe vezme stíny kouře a světlo ohně. Kde CG nic
nemění, vyjde plate z renderu pixel po pixelu, jak do něj vešel. Do EXR jde
CG s alfou a průchod `catcher` pro compositing ([docs/plate.md](docs/plate.md)).

**Obrázky a video**: záběr jde do PNG, do očíslované sekvence nebo do videa
— `.avi` (Motion JPEG, vlastní kodér JPEG i kontejner) bez jakékoli
závislosti, `.mp4`, `.mov`, `.mkv`, `.webm` a `.gif` přes ffmpeg. Editor
renderuje po snímcích na pozadí s oknem průběhu, počká na simulaci a nakonec
nabídne soubor otevřít; příkazy `sim` a `render` kreslí bez okna přes EGL,
a když EGL nejde, přes skryté okno. Pro compositing jde snímek do **EXR**
(vlastní zapisovač, RLE): obraz v lineárním světle nad bílou, hloubka `Z`,
vektory pohybu `forward.u/v` (kusy podle své rychlosti, vše podle kamery)
a masky podlahy, geometrie, kusů, objektů, vody a kouře
([docs/render.md](docs/render.md#4-exr-pro-compositing)).

**Python**: modul `pg` postaví síť, nastaví parametry, uvaří geometrii
a simuluje ze skriptu; atributy, topologie, objemy i částice a tělesa
snímků jsou pole numpy nad pamětí jádra, bez kopie. Záběr jde ze skriptu
do cache, do USD i do obrazu, a `net.as_code()` vypíše síť jako Python,
který ji postaví znovu — tak vznikla scéna z kroku 2 postavená čistě
z Pythonu ([docs/python.md](docs/python.md)).

Vědomě chybí (zatím): materiály a instance z USD, Alembic, čtení VDB, JIT, packed primitives,
Python uvnitř sítě (Python SOP), simulace látek — pořadí v
[ROADMAP.md §4](ROADMAP.md#4-další-kroky). Podrobně v
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-co-prototyp-skutečně-umí).

## Licence

Plánovaná licence je Apache 2.0 (viz [ROADMAP.md §7](ROADMAP.md#7-jméno-licence-clean-room)).
Implementace je clean room — žádný HDK, žádný reverse engineering.
