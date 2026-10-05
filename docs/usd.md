# Export do USD

USD (Universal Scene Description, od Pixaru) je formát, ve kterém si VFX
studia předávají celé scény: modely, animaci, kamery, světla i objemy.
Čtou ho Houdini (Solaris), Blender, Maya, Omniverse, usdview a renderery
Karma, Arnold, RenderMan a další. Prototype do něj zapíše **celý záběr jako
jednu scénu `.usda`**:
- zobrazenou geometrii,
- kusy z RBD Solveru jako tělesa, která se pohybují,
- drť a zrna jako kamínky (PointInstancer),
- povrch vody jako uzavřenou síť s rychlostí a pěnou,
- kapky deště a kapičky odstřiků,
- prach a páru jako VDB soubory vedle,
- kameru, slunce, oblohu a podlahu,
- materiály zobrazené geometrie jako MaterialX s fotkami vedle
  ([materialx.md](materialx.md)).

Co je velké a v každém snímku jiné (voda, déšť, drť, měnící se geometrie),
jde do **souboru pro každý snímek** vedle scény a scéna si hodnoty bere
odtud (USD *value clips*, §3). Záběr libovolné délky se tak nemusí vejít do
paměti a scéna sama zůstane malá.

Zapisovač vznikl podle veřejné specifikace, bez knihovny USD (clean room).

## 1. Rychlý start

```bash
./build/prototype sim demolition - --export out/demolition.usda        # nasimuluje a zapíše
./build/prototype sim rain_pond - --export out/pond.usda               # voda a déšť: out/pond_frames/
./build/prototype sim wall_collapse - --from-cache cache/wall --export out/wall.usda   # z cache
./build/prototype cook street out/street.usda                          # jen geometrie, bez simulace
```

- **Soubor bez `$F`:** `--export` s příponou `.usda` zapíše celý záběr jako jednu scénu,
  s `--start` a `--end` jen jeho díl (časy scény od `--start`).
- **Soubor s `$F4`:** `--export 'geo.$F4.usda'` zapíše každý snímek do vlastního
  souboru, jen geometrii, jako u `.ply` a `.obj`.
- **Editor:** položka **File › Export USD Scene…** zapíše všechny snímky, které
  jsou v cache. **File › Export Geometry…** s příponou `.usda` zapíše geometrii
  jednoho snímku s jejími materiály, s `.mtlx` jen materiály ([materialx.md](materialx.md)).

Kde scénu otevřít:

- **Blender:** File › Import › Universal Scene Description.
- **Houdini:** v Solaris uzel Sublayer nebo Reference, nebo File › Import.
- **Kontrola:** `usdview demolition.usda`, nebo `usdcat` a `usdchecker` z balíku `usd-core`.

## 2. Co je ve scéně

```
/World                     Xform, výchozí prim
  /Looks/surface           Material: UsdPreviewSurface, barva z displayColor
  /Looks/water, /rain      Material: voda (průhledná, hladká, ior 1,33), déšť
  /Looks/glass             Material: sklo (čiré, hladké, ior 1,5), je-li v kusech
  /Materials/bark …        Material: MaterialX + UsdPreviewSurface, materiály zobrazené geometrie
  /<uzel>                  zobrazená geometrie: mesh, curves, points
      /mesh/bark …           GeomSubset: plochy jednoho materiálu
      /instances             PointInstancer: tráva, stromy jako instance
          /Prototypes/proto_0 …  … prototypy, jednou
  /pieces/body_0000 …      tělesa RBD Solveru: Xform (translate, orient)
      /mesh                  … nad tvarem tělesa kolem jeho středu
          /inside            GeomSubset: plochy, které vyřízl lom
          /glass             GeomSubset: plochy skla, s materiálem skla
      /cracks                … trhliny skla: neviditelné do snímku, kdy tabule praskla
  /grit                    PointInstancer: drť                ┐
      /Prototypes/proto_0 …  … 12 úlomků kamene, 6 střepů skla, │
                               jednou ve scéně                │
  /grains                  PointInstancer: zrna, tytéž tvary  │
  /cloth/mesh, /curves     Mesh a BasisCurves: látka a lana   │ hodnoty ze
  /water                   Mesh: povrch vody                  │ souborů po
  /rain/drops              Points: kapky                      │ snímcích
  /rain/droplets           Points: kapičky odstřiků           ┘
  /gas                     Volume: pole density, temperature, flame (a steam, vel)
      /density …             OpenVDBAsset → <jméno>_gas/<jméno>_gas.0001.vdb …
  /camera                  Camera
  /sun                     DistantLight
  /sky                     DomeLight
  /ground                  Mesh: podlaha
```

Na disku:

```
pond.usda                      scéna
pond_frames/pond.0001.usda …   hodnoty jednoho snímku (vrstva, "value clip")
pond_frames/pond.manifest.usda které atributy vrstvy dávají
pond_gas/pond_gas.0001.vdb …   prach, je-li
pond_textures/bark_color.jpg … fotky materiálů, jsou-li
pond.mtlx                      materiály jako dokument MaterialX, jsou-li
```

| Prim | Co obsahuje |
|---|---|
| geometrie | Zobrazený uzel podle svého jména (`street`, `city`). Uzavřené polygony jsou Mesh bez subdivize, otevřené čáry lineární BasisCurves a volné body Points. Body mají šířku podle `pscale`, `v` jako `velocities` a `id` jako `ids`; `v` meshe jsou také `velocities`. Ostatní atributy bodů (čísla, celá čísla, vektory, třeba `foam`) jdou jako primvars (`primvars:foam`). Instance (body, které zastupují prototypy — tráva z Grass, stromy s Output Instances) jsou PointInstancer `instances`: prototypy ve scope `Prototypes` pod ním, `protoIndices`, `positions`, `orientations`, `scales`, `primvars:tint`, `ids` ([vegetation.md](vegetation.md)). Geometrie, která se nemění, je ve scéně jednou; když se změní, je v souborech snímků, ve kterých se změnila — prototypy instancí zůstávají ve scéně, ve snímcích je jen to, jak instance stojí. |
| barva | `Cd` rohu, bodu, primitiva nebo celé geometrie jako `displayColor`. Zapíše se jednou pro celý objekt, jednou na plochu, nebo jednou na roh, podle toho, jak se barva mění. |
| materiály | Každý materiál zobrazené geometrie a jejích prototypů (z čeho je, drsnost, fotky, kladení, výška) jako `Material` v `/World/Materials`: graf MaterialX `standard_surface` nad fotkami, výška jako jeho `displacement`, a `UsdPreviewSurface`. Plochy jsou přiřazené `GeomSubset`y, jeden na materiál, a u měnící se geometrie jsou indexy ve vrstvách snímků. UV jsou `primvars:st`, fotky se zkopírují do `<jméno>_textures/` ([materialx.md](materialx.md)). |
| tělesa | Tvar tělesa (barvy jako v náhledu) se zapíše **jednou**, posunutý do středu tělesa. Každý snímek pak jen `translate` a `orient`. Rozmetané těleso má od toho snímku `visibility = invisible`. Kus, který se za běhu rozlomil ([lámání za běhu](destruction.md#lámání-za-běhu)), zmizí stejně, a jeho úlomky jsou tělesa navíc: `body_NNNN` za těmi, co byla, s tvarem úlomku, do snímku zlomu neviditelné (`visibility` `invisible`, od zlomu `inherited`), dřív na místě kusu. |
| drť | PointInstancer s kamínky: prototypy jsou tytéž úlomky, jaké kreslí renderery — dvanáct tvarů kamene (krabička zploštělá a protažená, s ulomenými rohy a hranami) a šest střepů skla, ploché, od středu k nejvzdálenějšímu rohu jednotka. Jsou ve scéně jednou (`Prototypes/proto_0` až `proto_17`), ve vrstvách snímků je jen, kde jaký kousek je: `protoIndices` (tvar podle čísla kousku, střepy od 12), `positions`, `orientations` (`quath[]`, natočení ze simulace), `scales` (poloviční velikost kousku), `velocities`, `ids` a `primvars:displayColor` po kouscích (barva řezu o odstín tmavší a každý kousek svým odstínem, jako v náhledu; sklo barvou skla). Číslo dostane kousek při vyhození a drží ho, dokud je ve scéně, takže ho renderer sleduje ze snímku na snímek a rozmaže pohybem. Instancer má materiál `surface`, střepy materiál `glass`, je-li ve scéně sklo. Cache starší než formát 4 čísla ani rychlosti nemá (tvar pak podle pořadí), starší než formát 9 natočení (pak náhodné podle čísla). Než první kousek vyletí, je drť neviditelná. |
| látka | Látka Cloth Solveru tam, kde jsou její body, s normálami (`normals`) a rychlostmi (`velocities`) pro rozmazání pohybem. Plochy tvoří Mesh, lana BasisCurves. Barva je z `Cd` geometrie, jinak z parametru Color. Každý snímek má celou síť ve své vrstvě včetně topologie. Roztržená látka má od snímku, kdy se roztrhla, víc bodů (odtržené kopie) a plochy přepojené na ně ([cloth.md](cloth.md)). Atributy, které čte jen řešič (`pin`, `mass`, `tear`), se nezapisují. |
| zrna | Zrna Grain Solveru jako PointInstancer `/World/grains` se stejnými dvanácti kamínky jako drť: velikost (`scales`) je poloměr zrna, dále `positions`, `orientations`, `velocities`, `ids`, `protoIndices` a barva po zrnech (`primvars:displayColor`), v každé vrstvě snímku ([grains.md](grains.md)). Drť, která se stala zrny (vstup Grit), je jen tady. |
| voda | Povrch vody jako uzavřená síť čtyřúhelníků, stejný jako z uzlu Liquid Surface ([geometry.md](geometry.md#povrch-vody-liquid-surface-a-convert-volume)): hladké normály, `velocities` z rychlosti vody (cache od formátu 5), `primvars:foam` (0 až 1) pro bílou pěnu, vlnky od deště na hladině. Uzavřená i u dna a stěn, aby jí renderer lámal světlo. Materiál `water`: barva z Water Looku, průhlednost 0,35, drsnost 0,02, index lomu 1,33. Když Water Look povrch skrývá (Surface vypnuté), voda se nezapíše. |
| déšť | Kapky a kapičky odstřiků jako dvoje Points s číslem (`ids`) a rychlostí (`velocities`); kapka je široká 2 mm, kapička 1 mm. Renderer s motion blurem z nich podle rychlosti udělá čáry, jako je kreslí náhled. Materiál `rain`: barva a průhlednost z Looku deště. |
| prach | Každý snímek zapíše jeden VDB soubor do složky `<jméno>_gas/` vedle scény: mřížky `density`, `temperature`, `flame` a `steam`, je-li v plynu pára ([quench.md](quench.md)), a vektorovou `vel`, rychlost plynu na blocích 2 × 2 × 2 buněk. Pole `vel` má `fieldDataType` float3 a `vectorDataRoleHint` Vector, jak vektorové pole označuje schéma OpenVDBAsset. Cesty jsou relativní, takže složka jde přesunout spolu se scénou. |
| kamera | Poloha a otočení jako v uzlu Camera (`translate`, `rotateXYZ`: stupně kolem x, pak y, pak z), ohnisko, clona a `exposure` v EV. |
| světla | Slunce svítí ze směru, který má Look, a obloha má barvu a sílu z Looku. Intenzita je relativní jako v Looku, ne ve fyzikálních jednotkách. |
| podlaha | Čtverec kolem scény v barvě země z Outputu, je-li podlaha zapnutá. |

## 3. Soubory po snímcích (value clips)

Scéna `.usda` s tisíci snímků vody by měla gigabajty a celá by se musela
postavit v paměti. Proto jde všechno, co je velké a v každém snímku jiné,
do **vrstvy daného snímku** (`pond_frames/pond.0007.usda`) a scéna si z ní
hodnoty bere mechanismem USD, kterému se říká *value clips* — stejně jako
u exportu z Houdini s volbou „soubor na snímek“.

- **Vrstva snímku** obsahuje jen časové vzorky v tom snímku, pod prim,
  kterému patří (`over "World" { over "water" { point3f[] points.timeSamples = { 7: [...] } } }`).
  Zapíše se hned, jak snímek přijde: export drží v paměti jen jeden snímek.
- **Scéna** na primu `/World/water` (a `/World/rain`, `/World/grit`,
  měnící se geometrii) uvede, ze kterých vrstev a od kterého snímku bere:

  ```
  def Mesh "water" (
      clips = {
          dictionary default = {
              double2[] active = [(1, 0), (2, 1), …]
              asset[] assetPaths = [@./pond_frames/pond.0001.usda@, …]
              asset manifestAssetPath = @./pond_frames/pond.manifest.usda@
              string primPath = "/World/water"
              double2[] times = [(1, 1), (2, 2), …]
          }
      }
  )
  ```

  Atributy, které dávají vrstvy, scéna jen deklaruje (typ a u primvars
  `interpolation`), bez hodnoty. Co se nemění (šířka kapek, barva vody,
  `subdivisionScheme`, materiál), má scéna sama.
- **Manifest** (`pond.manifest.usda`) deklaruje každý atribut, který
  vrstvy dávají. Vrstva, která pro něj vzorek nemá, znamená „v tomto
  snímku bez hodnoty“ — nic se nepřenáší z jiného snímku.
- **Měnící se geometrie** má vrstvu jen ve snímcích, kde se změnila;
  mezi nimi platí poslední. Dokud se zobrazená geometrie nezměnila, export
  vrstvu prvního snímku podrží: když se nezmění nikdy, zapíše se jednou do
  scény, jak bylo. Topologie (plochy) je ve vrstvě každého snímku, ve
  kterém se geometrie změnila, i když zůstala stejná.
- **Snímky, kde něco chybí** (drť před výbuchem, voda skrytá v Looku), mají
  na primu `visibility = invisible`.
- **Mezi snímky** (rozmazání pohybem, snímky 7,25 a 7,75) USD hodnoty
  interpoluje jako u obyčejných časových vzorků. Když se počet bodů mění
  (voda, déšť), drží hodnotu snímku a renderer posune body podle
  `velocities`.

Soubory po snímcích jsou vedle scény a cesty jsou relativní: složku
`pond_frames/` stačí přesunout spolu s `pond.usda`.

## 4. Konvence

- **Jednotky a čas:**
  - osa Y míří nahoru, jednotka je metr (`metersPerUnit = 1`);
  - time code je číslo snímku (1 … N), `timeCodesPerSecond` přijde z Outputu:
    30, ne 29.999998, které dá `1 / (1 / 30.0f)`.
- **Kamera podle specifikace USD:** ohnisko a clona se udávají v desetinách jednotky scény.
  - Při metrech je 38 mm = `0.38`.
  - Film je 24 mm vysoký (jako `Camera::fovY`), šířka odpovídá poměru stran obrazu.
  - Blender tuto konvenci dodržuje. Program, který čte ohnisko rovnou v mm, uvidí stokrát kratší ohnisko.
- **Tělesa jako transformace, ne mesh v každém snímku:**
  - Soubor je řádově menší.
  - Renderer rozmaže pohyb správně: těleso se mezi vzorky otáčí kolem svého středu, ne kolem počátku scény.
  - Quaternion zapisuje USD reálnou složkou napřed: `(w, x, y, z)`.
- **Časové vzorky jen tam, kde se hodnota mění:**
  - Stojící město se zapíše jednou.
  - Těleso, které se nehne, má jednu polohu.
  - Kamera, která stojí, má jednu polohu.
- **Materiál:** zobrazená geometrie má materiály MaterialX ([materialx.md](materialx.md)).
  Ostatní (kusy, drť, látka, podlaha) mají jeden `UsdPreviewSurface`, který bere barvu z `displayColor`
  (`UsdPrimvarReader_float3`). Plochy řezu jsou `GeomSubset` s rodinou
  `materialBind`, takže jim jde v Houdini nebo Blenderu přiřadit vlastní materiál (beton, cihla).
  Plochy skla (primitiva s `glass`) mají vlastní podmnožinu `glass` s materiálem
  `/World/Looks/glass`; trhliny skla jsou síť `cracks` vedle `mesh`, neviditelná,
  dokud je tabule celá, a skleněná drť jsou prototypy střepů (`protoIndices`
  od 12) s materiálem skla.

## 5. Ověření

Scény jsme ověřili Pixarovou knihovnou `usd-core` 26.08. Ta je jen v prostředí,
kde se ověřovalo, prototype ji nepotřebuje.

- **Validátory USD:** pro `wall_collapse`, `demolition`, `rain_pond`, `liquid_points`, `glass_window` i `debris_stairs` hlásí
  všech 28 validátorů 0 nálezů (schémata, metadata stage, rodiny GeomSubset, vazba
  materiálu, stínovače).
- **Soubory po snímcích:** knihovna složí hodnoty z vrstev tak, jak jsou ve snímcích:
  povrch vody `rain_pond` má ve snímku 24 37 173 bodů s normálami, rychlostmi a pěnou,
  kapky svá čísla; mezi snímky se extent interpoluje, body se drží. Drť `wall_collapse`
  je neviditelná, dokud nevyletí první zrnko, a pak má v každém snímku svá zrnka.
  Drť `debris_stairs` má v každém snímku tolik `primvars:orient` (`quatf[]`, po vrcholech),
  kolik má bodů, a všechny jsou jednotkové kvaterniony.
- **Tělesa:** body těles přepočítané knihovnou do světa (`ComputeLocalToWorldTransform`)
  sedí na kusy, které vrací `posedPieces`.
  - Medián odchylky je 1e-6 m, tedy přesnost floatu.
  - Nejvíc je 0,13 mm, a to u těles, jejichž bod sdílí sousední kus: náhled
    ho posune s prvním z nich, USD dá každému tělesu vlastní kopii.
- **Kamera:** osy odpovídají rotaci Rz·Ry·Rx uzlu Camera, ohnisko je 22 mm a clona 42,67 × 24 mm.
- **Prach:** cesty k souborům VDB se vyřeší relativně ke scéně.
- **Materiály:** jak je knihovna složí a co hlásí validátory, popisuje
  [materialx.md](materialx.md#5-ověření).

| Záběr | Snímky | Tělesa | `.usda` | Vrstvy snímků | VDB | Export z cache | Otevření |
|---|---|---|---|---|---|---|---|
| `wall_collapse` | 120 | 274 | 4,8 MB | 105 souborů, 12 MB | 148 MB | 14 s | 0,1 s |
| `demolition` | 180 | 710 (421 rozmetaných) | 16 MB | 150 souborů, 9,4 MB | 215 MB | 27 s | 0,3 s |
| `rain_pond` | 24 | — | 8 kB | 24 souborů, 116 MB | — | 1,5 s | 0,02 s |

Testy jsou v `tests/test_usd.cpp` (12):
- hodnoty jako je píše USD (čísla, quaternion `(w, x, y, z)`, řetězce, jména primů);
- scéna zapsaná prim po primu s časovými vzorky, přesně daný text;
- geometrie do `.usda` podle přípony;
- tělesa posunutá a otočená přesně jako `posedPieces`;
- drť jako PointInstancer s prototypy kamínků: čísla, rychlosti, natočení (`quath[]`), velikosti a tvary ze snímku ve vrstvě snímku, zapsané hned, jak snímek přišel;
- prach jako soubory VDB vedle scény;
- ohnisko v desetinách jednotky a slunce tam, kde ho má Look;
- to, co se nemění, zapsané jednou, geometrie, která se hýbe, ve vrstvách snímků, kde se
  změnila (první snímek až ve chvíli, kdy je jasné, že se hýbe), a jméno geometrie, které
  se nesrazí s jiným primem;
- voda a déšť ve vrstvě každého snímku, s materiály ve scéně a manifestem;
- sklo s materiálem skla a trhliny, které se ukážou, až tabule praskne;
- látka ve vrstvě každého snímku, roztržená, jak je;
- zrna štěrku jako PointInstancer s prototypy kamínků ve vrstvě každého snímku.

Že USD Import prototypy PointInstanceru nebere jako geometrii scény (drť
a zrna se tak při načtení vlastního exportu neobjeví jako hromada kamínků
v počátku), ověřuje `tests/test_usd_read.cpp` ([usd-import.md](usd-import.md)).

## 6. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/io/Usda.h` | Zapisovač USDA: hodnoty jako text, prim (`def` i `over`), stage, časové vzorky, `clips`. Geometrie jako Mesh, BasisCurves a Points (`geometryPrim`, `geometryStage`), instance jako PointInstancer (`instancerPrim`, `addPrototypes`), její atributy snímku (`fields`) a primvars z atributů bodů (`pointPrimvars`). |
| `src/pg/io/Export.cpp` | `.usda` ve `writeGeometry`: geometrie jako samostatná scéna |
| `src/pg/sim/UsdExport.h` | Záběr simulace: snímky přicházejí po jednom (`add`) a vrstva každého se hned zapíše; scéna a manifest se zapíší na konci (`finish`). `exportGeometry`: geometrie jako scéna s materiály, nebo `.mtlx` |
| `src/pg/render/MaterialGraph.h` | Materiály jako grafy MaterialX a jejich přiřazení plochám ([materialx.md](materialx.md)) |
| `src/pg/sim/WaterMesh.h` | Povrch vody snímku jako síť (`waterMesh`), stejný jako z uzlu Liquid Surface |
| `src/pg/nodes/Volumes.cpp` | Objem na síť polygonů (`volumeToMesh`, surface nets) a uzel Convert Volume |
| `tools/prototype/Commands.cpp` | `prototype sim --export ….usda` |
| `tools/prototype/SimWorkspace.cpp` | File › Export USD Scene… |

## 7. Omezení

- **Jen textový `.usda`.** Binární `.usdc` se nezapisuje, takže velké scény
  jsou větší a načítají se pomaleji než z Houdini.
- **Text je velký.** Snímek `rain_pond` (35 tisíc bodů povrchu, 5 tisíc
  kapek) má ve vrstvě 4,8 MB; binární `.usdc` s týmž obsahem má 1,4 MB.
- **Částice vody jako takové** (pro vlastní meshování v Houdini) do scény
  nejdou samy: stačí zobrazit Liquid Points a jdou jako měnící se geometrie.
- **Díl z farmy** (`--start`, `--end`) zapíše vlastní scénu jen se svými
  snímky. Vrstvy snímků jsou samostatné soubory, ale scénu celého záběru
  z dílů zatím nic neposkládá.
- **Objekty z uzlu Object** (koule, kvádry) se neexportují. Jsou to překážky
  simulace, ne to, co se renderuje. Kdo je chce vidět, zobrazí je jako geometrii.
- **Čtení** (kamera z matchmove, kulisa a modely z jiných programů, zpětné
  načtení vlastního exportu) popisuje [usd-import.md](usd-import.md).
- **Světla nemají fyzikální jednotky.** Sílu slunce a oblohy je potřeba
  v rendereru doladit.
