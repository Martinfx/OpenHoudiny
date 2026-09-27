# Export do USD

USD (Universal Scene Description, od Pixaru) je formát, ve kterém si VFX
studia předávají celé scény: modely, animaci, kamery, světla i objemy.
Čtou ho Houdini (Solaris), Blender, Maya, Omniverse, usdview a renderery
Karma, Arnold, RenderMan a další. Prototype do něj zapíše **celý záběr do
jednoho textového souboru `.usda`**:
- zobrazenou geometrii,
- kusy z RBD Solveru jako tělesa, která se pohybují,
- drť,
- prach jako VDB soubory vedle,
- kameru, slunce, oblohu a podlahu.

Zapisovač vznikl podle veřejné specifikace, bez knihovny USD (clean room).

## 1. Rychlý start

```bash
./build/prototype sim demolition - --export out/demolition.usda        # nasimuluje a zapíše
./build/prototype sim wall_collapse - --from-cache cache/wall --export out/wall.usda   # z cache
./build/prototype cook street out/street.usda                          # jen geometrie, bez simulace
```

- **Soubor bez `$F`:** `--export` s příponou `.usda` zapíše celý záběr jako jednu scénu,
  s `--start` a `--end` jen jeho díl (časy scény od `--start`).
- **Soubor s `$F4`:** `--export 'geo.$F4.usda'` zapíše každý snímek do vlastního
  souboru, jen geometrii, jako u `.ply` a `.obj`.
- **Editor:** položka **File › Export USD Scene…** zapíše všechny snímky, které
  jsou v cache. **File › Export Geometry…** s příponou `.usda` zapíše geometrii
  jednoho snímku.

Kde scénu otevřít:

- **Blender:** File › Import › Universal Scene Description.
- **Houdini:** v Solaris uzel Sublayer nebo Reference, nebo File › Import.
- **Kontrola:** `usdview demolition.usda`, nebo `usdcat` a `usdchecker` z balíku `usd-core`.

## 2. Co je ve scéně

```
/World                     Xform, výchozí prim
  /Looks/surface           Material: UsdPreviewSurface, barva z displayColor
  /<uzel>                  zobrazená geometrie: mesh, curves, points
  /pieces/body_0000 …      tělesa RBD Solveru: Xform (translate, orient)
      /mesh                  … nad tvarem tělesa kolem jeho středu
          /inside            GeomSubset: plochy, které vyřízl lom
  /grit                    Points: drť
  /gas                     Volume: pole density, temperature, flame
      /density …             OpenVDBAsset → <jméno>_gas/<jméno>_gas.0001.vdb …
  /camera                  Camera
  /sun                     DistantLight
  /sky                     DomeLight
  /ground                  Mesh: podlaha
```

| Prim | Co obsahuje |
|---|---|
| geometrie | Zobrazený uzel podle svého jména (`street`, `city`). Uzavřené polygony jsou Mesh bez subdivize, otevřené čáry lineární BasisCurves a volné body Points. Body mají šířku podle `pscale`, `v` jako `velocities` a `id` jako `ids`. |
| barva | `Cd` rohu, bodu, primitiva nebo celé geometrie jako `displayColor`. Zapíše se jednou pro celý objekt, jednou na plochu, nebo jednou na roh, podle toho, jak se barva mění. |
| tělesa | Tvar tělesa (barvy jako v náhledu) se zapíše **jednou**, posunutý do středu tělesa. Každý snímek pak jen `translate` a `orient`. Rozmetané těleso má od toho snímku `visibility = invisible`. |
| drť | Body se šířkou podle velikosti zrnka, v barvě řezu o odstín tmavší, s rychlostí (`velocities`) a číslem (`ids`). Číslo dostane zrnko při vyhození a drží ho, dokud je ve scéně, takže ho renderer sleduje ze snímku na snímek a rozmaže pohybem. Cache starší než formát 4 čísla ani rychlosti nemá. |
| prach | Každý snímek zapíše jeden VDB soubor do složky `<jméno>_gas/` vedle scény. Cesty jsou relativní, takže složka jde přesunout spolu se scénou. |
| kamera | Poloha a otočení jako v uzlu Camera (`translate`, `rotateXYZ`: stupně kolem x, pak y, pak z), ohnisko, clona a `exposure` v EV. |
| světla | Slunce svítí ze směru, který má Look, a obloha má barvu a sílu z Looku. Intenzita je relativní jako v Looku, ne ve fyzikálních jednotkách. |
| podlaha | Čtverec kolem scény v barvě země z Outputu, je-li podlaha zapnutá. |

## 3. Konvence

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
  - Geometrie, která se hýbe a nemění topologii, má vzorek bodů pro každý snímek, ale plochy jen jednou.
- **Materiál:** jeden `UsdPreviewSurface`, který bere barvu z `displayColor`
  (`UsdPrimvarReader_float3`). Plochy řezu jsou `GeomSubset` s rodinou
  `materialBind`, takže jim jde v Houdini nebo Blenderu přiřadit vlastní materiál (beton, cihla).

## 4. Ověření

Scény jsme ověřili Pixarovou knihovnou `usd-core` 26.08. Ta je jen v prostředí,
kde se ověřovalo, prototype ji nepotřebuje.

- **Validátory USD:** pro `wall_collapse` i `demolition` hlásí všech 28 validátorů 0 nálezů
  (schémata, metadata stage, rodiny GeomSubset, vazba materiálu, stínovače).
- **Tělesa:** body těles přepočítané knihovnou do světa (`ComputeLocalToWorldTransform`)
  sedí na kusy, které vrací `posedPieces`.
  - Medián odchylky je 1e-6 m, tedy přesnost floatu.
  - Nejvíc je 0,13 mm, a to u těles, jejichž bod sdílí sousední kus: náhled
    ho posune s prvním z nich, USD dá každému tělesu vlastní kopii.
- **Kamera:** osy odpovídají rotaci Rz·Ry·Rx uzlu Camera, ohnisko je 22 mm a clona 42,67 × 24 mm.
- **Prach:** cesty k souborům VDB se vyřeší relativně ke scéně.

| Záběr | Snímky | Tělesa | `.usda` | VDB | Export z cache | Otevření |
|---|---|---|---|---|---|---|
| `wall_collapse` | 120 | 274 | 17 MB | 148 MB | 27 s | 0,2 s |
| `demolition` | 180 | 710 (421 rozmetaných) | 25 MB | 215 MB | 39 s | 0,4 s |

Testy jsou v `tests/test_usd.cpp` (8):
- hodnoty jako je píše USD (čísla, quaternion `(w, x, y, z)`, řetězce, jména primů);
- scéna zapsaná prim po primu s časovými vzorky, přesně daný text;
- geometrie do `.usda` podle přípony;
- tělesa posunutá a otočená přesně jako `posedPieces`;
- drť s čísly a rychlostmi ze snímku;
- prach jako soubory VDB vedle scény;
- ohnisko v desetinách jednotky a slunce tam, kde ho má Look;
- to, co se nemění, zapsané jednou, a jméno geometrie, které se nesrazí s jiným primem.

## 5. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/io/Usda.h` | Zapisovač USDA: hodnoty jako text, prim, stage, časové vzorky. Geometrie jako Mesh, BasisCurves a Points (`geometryPrim`, `geometryStage`). |
| `src/pg/io/Export.cpp` | `.usda` ve `writeGeometry`: geometrie jako samostatná scéna |
| `src/pg/sim/UsdExport.h` | Záběr simulace: snímky přicházejí po jednom (`add`), scéna se zapíše na konci (`finish`) |
| `tools/prototype/Commands.cpp` | `prototype sim --export ….usda` |
| `tools/prototype/SimWorkspace.cpp` | File › Export USD Scene… |

## 6. Omezení

- **Jen textový `.usda`.** Binární `.usdc` se nezapisuje, takže velké scény
  jsou větší a načítají se pomaleji než z Houdini.
- **Voda a déšť do USD zatím nejdou,** jen jako body do PLY s `v` a `id`
  (Liquid Points, Rain Points; [cache.md](cache.md)). Scéna se staví
  v paměti, a statisíce částic vody ve stovkách snímků by v textu zabraly
  gigabajty. Patří do souborů po snímcích, které scéna skládá (USD value
  clips), a to je další bod kroku 3.
- **Objekty z uzlu Object** (koule, kvádry) se neexportují. Jsou to překážky
  simulace, ne to, co se renderuje. Kdo je chce vidět, zobrazí je jako geometrii.
- **USD se zatím jen zapisuje.** Čtení (kamera z matchmove, modely z jiných
  programů) je další bod kroku 3.
- **Světla nemají fyzikální jednotky.** Sílu slunce a oblohy je potřeba
  v rendereru doladit.
