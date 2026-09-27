# Cache na disk a export

Simulace se spočítá jednou a její snímky se uloží na disk. Pak se přehrají,
vykreslí jinou kamerou nebo jiným vzhledem a vyexportují bez nového
počítání — v editoru, z příkazové řádky, na jiném stroji. Geometrie
libovolného uzlu (částice vody, kapky deště, plyn jako objemy, polygony) jde
ven v souborech, které čtou ostatní programy: body do **PLY**, objemy do
**OpenVDB**, polygony do **OBJ**, snímek po snímku. Záběr tak jde do Houdini,
do Blenderu a do rendererů.

![Editor: snímky táboráku načtené z disku (přehled: „from fire_cache“, stavový řádek: „from disk“) a menu Simulation s Save Cache a Load Cache](img/editor-cache.png)

![Snímky 40, 90 a 150 táboráku vyexportované do VDB, načtené knihovnou OpenVDB 10 (pyopenvdb) a promítnuté podél osy z: kouř (density) a plamen (flame) — mimo prototype](img/vdb-export.png)

## 1. Rychlý start

```bash
# simulace jednou, snímky na disk ('-' místo OUT.png: žádný obrázek)
./build/prototype sim campfire_vdb - --cache cache/fire
# render z cache: nic se nepočítá
./build/prototype sim campfire_vdb out/fire.png --from-cache cache/fire --every 10
# plyn jako objemy OpenVDB, soubor na snímek -- z cache, bez simulace
./build/prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
# částice vody jako body PLY s rychlostí, pěnou a barvou
./build/prototype sim liquid_points - --export 'out/water.$F4.ply'
```

V editoru:

1. **Simulation › Save Cache to Disk…** — složka (vybraná, nebo napsaná
   nová); uloží všechny spočítané snímky.
2. **Simulation › Load Cache from Disk…** — snímky ze složky nahradí
   simulaci. Přehled sítě ukáže „from *složka*“, stavový řádek „from disk“.
3. **File › Export Geometry Frames…** — geometrie zobrazeného uzlu, soubor
   na snímek (`$F4` v názvu je číslo snímku). Totéž pro kterýkoli
   geometrický uzel: pravé tlačítko na uzlu › **Export Geometry Frames…**.
   Položka **Export Geometry…** zapíše jen snímek na obrazovce.

Příklad `campfire_vdb` je táborák s uzlem Gas Volume (`volumes`), který
vrací plyn jako tři objemy: `density` (kouř), `temperature` a `flame`.

## 2. Cache na disku

```
cache/fire/
  cache.txt             pgcache 1 / frames 150 / fps 30 / network dc17a5fbb4cdd2c0
  frame.0001.pgframe
  frame.0002.pgframe
  …
```

- Snímek je `sim::Frame` tak, jak ho drží editor: plyn v poloviční
  přesnosti (kouř, teplota, plamen v každé buňce), hladina vody po bajtech,
  částice vody (poloha, rychlost, bělost), kapky a vlnky deště, od verze 2
  polohy, otočení a rychlosti kusů tuhých těles, od verze 3 i drť a kusy,
  které se rozprášily ([destruction.md](destruction.md)), od verze 4
  číslo každé částice — vody, kapky, kapičky, zrnka drti —, stejné ze
  snímku na snímek, a rychlost drti, a od verze 5 rychlost vody na mřížce
  řešiče (ve vodě a asi dvě buňky kolem hladiny, dál nuly), ze které má
  povrch vody `v` pro rozmazání pohybem ([geometry.md](geometry.md#povrch-vody-liquid-surface-a-convert-volume)); klidová geometrie
  kusů je v síti a snímek načtený z disku ji dostane od ní. Binárně, little-endian,
  s hlavičkou `PGFRAME` a číslem verze; starší snímky se čtou dál.
- **Nuly se nezapisují**: běh nul je jedno číslo. Kouř táboráku zabírá jen
  část domény, takže 150 snímků mřížky 64 × 96 × 64 má na disku 63 MB,
  v paměti 338 MB.
- `network` je hash textu sítě **bez poloh uzlů na plátně**: posunutý uzel je
  pořád stejná síť, změněný parametr už ne. Cache jiné sítě (nebo jiné verze
  téže) se načte, ale editor i `prototype sim` upozorní.
- Načtené snímky platí, dokud se nezmění, co se simuluje. První taková
  změna (parametr zdroje, bypass uzlu…) je zahodí a simulace začne znovu od
  snímku 1. Změna vzhledu nebo kamery je nezahodí: cache z disku jde
  vykreslit jinak.
- Simulace je deterministická, a proto jsou snímky z cache bitově stejné
  jako spočítané. Render z cache je tentýž soubor PNG jako render po
  simulaci (ověřeno `cmp`) a VDB vyexportované z editoru i z příkazové řádky
  jsou bajt po bajtu stejné.
- Čtení nevěří souboru: snímek, jehož části nesedí s jeho mřížkami (počty
  buněk, částic, vlnek), neprojde. Poškozený soubor tak renderer nepošle
  mimo pole.

## 3. Export

| přípona | co zapíše | kdo to čte |
|---|---|---|
| `.ply` | body s atributy, uzavřené polygony jako `face`; binárně, little-endian | Houdini, Blender, MeshLab, CloudCompare, ParaView |
| `.vdb` | objemy jako float mřížky OpenVDB (soubor verze 224) | Houdini, Blender, renderery (Arnold, Redshift, V-Ray, Cycles, Karma) |
| `.obj` | body, polygony, čáry | skoro všechno |
| `.usda` | polygony jako Mesh, čáry, body; bez `$F` v `--export` celý záběr — tělesa, drť, prach, kamera, světla ([usd.md](usd.md)) | Houdini (Solaris), Blender, usdview, renderery |

Formát určuje přípona (na velikosti písmen nezáleží). `.vdb` z geometrie
bez objemů se nezapíše — chyba řekne, že objemy dělá uzel Gas Volume.

### PLY

| atribut bodu | vlastnosti PLY |
|---|---|
| `P` | `x y z` |
| `N` | `nx ny nz` |
| `Cd` | `red green blue` — bajty 0–255; u `vec4` i `alpha` |
| `v` | `vx vy vz` |
| číslo | jeho jméno: `float`, celé číslo jako `int` (přesně, ne přes float) |
| vektor | `jméno_x jméno_y jméno_z` (`_w` u `vec4`) |
| řetězec | vynechá se |

Stejný modul PLY i čte (`io::readPly`), ASCII i binární little-endian: stejná
jména dají zase stejné atributy, barvy v bajtech se dělí 255. Hlavičce, která
slibuje víc prvků, než soubor unese, nevěří a místo pro ně ani nealokuje.

### OpenVDB

Zapisovač je vlastní, bez knihovny OpenVDB, jádro tak zůstává bez
závislostí. Soubor má tvar, jaký píše OpenVDB 10:

- hlavička verze 224, UUID z obsahu (stejné objemy = stejné bajty), metadata
  `creator`;
- u každé mřížky jméno, typ `Tree_float_5_4_3`, metadata `class`, `name`,
  `file_bbox_min`, `file_bbox_max`, `file_voxel_count`, `file_mem_bytes`;
- transformace `UniformScaleTranslateMap`: voxel (i, j, k) má střed
  v `origin + (i + ½, j + ½, k + ½) · voxel`, přesně jako buňka simulace;
  objem sedí tam, kde byl kouř;
- strom: kořen, vnitřní uzly 32³ a 16³, listy 8³. Aktivní jsou nenulové
  voxely, zapíšou se jen listy, ve kterých nějaký je, pozadí je 0;
- `class` je `fog volume`, když jsou hodnoty kladné — tak renderery kreslí
  kouř.

Ověřeno čtením v OpenVDB 10.0 (pyopenvdb): počet aktivních voxelů, jejich
součet, bounding box i poloha voxelu 0 sedí s mřížkou simulace do posledního
bitu (táborák, snímek 12: `density` 6906 voxelů se součtem 1091.8103,
`temperature` 7140 / 8241.4198, `flame` 6489 / 549.1078 — v souboru VDB
i ve snímku cache).

Gas Volume dává mřížky `density`, `temperature` a `flame`: tak je
pojmenovávají pyro shadery Houdini a Blenderu.

## 4. Příkazová řádka

```
prototype sim NETWORK.pgsim|EXAMPLE OUT.png|- [--frames N] [--start N] [--every K] ...
             [--cache DIR] [--from-cache DIR] [--export PATH] [--export-node NODE]
```

| přepínač | co dělá |
|---|---|
| `--cache DIR` | každý snímek do složky `DIR` (vytvoří ji), nakonec `cache.txt` |
| `--from-cache DIR` | snímky čte ze složky místo simulace; `--frames N` jich vezme nejvýš N |
| `--start N`, `--end N` | obrázky a export jen od snímku N (do `--end`, což je totéž co `--frames`) — díl záběru pro jeden stroj farmy; cache se čte od N, simulace ale začíná snímkem 1 a do cache jde každý snímek |
| `--export PATH` | geometrii zobrazeného uzlu z každého snímku do souboru; `$F4` je číslo snímku na čtyři cifry, `$F` bez nul. Bez nich se číslo vloží před příponu (`fire.vdb` → `fire.0007.vdb`). Složky se vytvoří. Výjimka: `.usda` bez `$F` je celý záběr v jednom souboru ([usd.md](usd.md)). |
| `--export-node NODE` | geometrie uzlu `NODE` místo zobrazeného |
| `-` místo `OUT.png` | žádný obrázek, jen cache a export — funguje i v buildu bez EGL |

`--every K` se týká jen obrázků: cache i export dostanou každý snímek.

Farma: jeden stroj simuluje do cache, ostatní renderují nebo exportují
každý svůj díl z ní:

```
prototype sim demolition - --cache cache/demo                                   # stroj 1
prototype sim demolition shot.png --from-cache cache/demo --start 1 --end 60 --every 1
prototype sim demolition shot.png --from-cache cache/demo --start 61 --end 120 --every 1
```

```
$ prototype sim campfire_vdb - --cache cache/fire
campfire_vdb: simulated, gas 64 x 96 x 64 cells, 150 frames; simulation 73.7 ms/frame
cached 150 frames in cache/fire
$ prototype sim campfire_vdb fire.png --from-cache cache/fire --every 50
wrote fire_0150.png and 2 before it: campfire_vdb, read from cache/fire, 150 frames (5.0 s); reading 0.7 ms/frame, rendering 495 ms/image
$ prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
campfire_vdb: 150 frames read from cache/fire; reading 1.2 ms/frame
exported 150 frames of geometry, the last out/fire.0150.vdb
```

Cache má 63 MB, 150 souborů VDB 114 MB (bez komprese, viz omezení).

## 5. V kódu

| soubor | co dělá |
|---|---|
| `src/pg/io/Ply.h` | `formatPly`, `writePly`, `parsePly`, `readPly` |
| `src/pg/io/Vdb.h` | `formatVdb`, `writeVdb` |
| `src/pg/io/Export.h` | `writeGeometry` podle přípony, `framePath` (`$F4`, `$F`) |
| `src/pg/sim/Cache.h` | `formatFrame` / `parseFrame`, `writeFrame` / `readFrame`, `writeCacheInfo` / `readCacheInfo`, `networkHash` |
| `tools/prototype/SimRunner.h` | `adopt`: snímky z disku místo simulace, dokud nepřijde jiný svět |
| `tools/prototype/SimWorkspace.cpp` | menu, dialogy, uložení, načtení, export |
| `tools/prototype/Widgets.h` | `FileBrowser::openFolder`: výběr složky (i nové) |

## 6. Testy

`tests/test_export.cpp`, 13 testů:

- PLY tam a zpět se všemi druhy atributů — i celé číslo 2²⁴ + 1, které by
  float nezachoval —, barvami v bajtech a polygony; ASCII s CRLF, `alpha`,
  `short` a seznamy `uint`; odmítnutí big-endian, useknutého souboru,
  hlavičky, která slibuje víc, než soubor má, a polygonu s vrcholem mimo;
- VDB tak, jak ho čte OpenVDB: hlavička, metadata, pozice mřížek až po
  konec souboru; třída, počet voxelů, bounding box, determinismus, přípona
  u dvou mřížek téhož jména;
- číslování sekvence (`$F4`, `$F`, bez vzoru) a zápis podle přípony včetně
  chyb;
- snímky tam a zpět: vymyšlený snímek se všemi částmi a běhy nul všech
  délek, skutečné snímky táboráku, vody s částicemi a deště; odmítnutí
  useknutých, novějších a nesmyslných snímků, převrácený bajt nikdy nespadne;
  snímek verze 3 se čte dál (bez čísel částic) a verze 4 také (bez rychlosti
  vody); čísla, která nejsou jedno na částici, a rychlost vody, která není
  na mřížce řešiče, se odmítnou;
- složka cache s `cache.txt` (fps 30, ne 29.999998) a hash sítě bez poloh
  uzlů.

## 7. Omezení

- VDB se jen zapisuje, a jen husté float mřížky. Rychlost plynu (`vel`)
  snímek nedrží, takže ve VDB není a renderer z ní motion blur neudělá.
  Hladina vody jako level set také ne — voda jde ven jako částice nebo jako
  povrch (Liquid Surface, v USD `/World/water`).
- Uvnitř VDB není komprese (zip, blosc): soubory jsou větší, než by zapsal
  Houdini.
- Cache drží, co editor ukazuje (poloviční přesnost), ne stav řešiče. Ze
  snímku 80 v cache tedy nejde simulovat dál, a načtená cache se proto
  nedopočítává.
- PLY čte jen prvky `vertex` a `face`, ostatní přeskočí.
- Alembic a USD chybí.
