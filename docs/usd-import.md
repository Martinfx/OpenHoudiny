# Čtení USD

Studio si záběr předává v USD: matchmove dodá kameru, oddělení layoutu
kulisu (set) a assety, jiné oddělení cache z Houdini nebo Mayi. Prototype
tyto soubory **čte** a skládá je do scény stejně jako knihovna USD:
- **kamera z matchmove** jde do Outputu jako kamera záběru, snímek po snímku;
- **geometrie** (kulisa, modely, cache) jde do sítě jako uzel, ze kterého
  jsou kolize pro simulaci, tvary pro fracture nebo cokoli dalšího.

Čtečka nepotřebuje knihovnu USD. Čte všechny tři podoby vrstvy:
- `.usda` (text),
- `.usdc` (binární „crate“),
- `.usdz` (balíček).

Binární formát jiná specifikace než zdrojové kódy OpenUSD nepopisuje, proto
je jeho rozložení zjištěné z nich. Kód čtečky je vlastní. Výsledky jsou
ověřené proti knihovně `usd-core` (§6).

## 1. Rychlý start

```bash
./build/prototype usd examples/usd/shot.usda                 # co soubor obsahuje: strom, kamery, geometrie
./build/prototype sim matchmove out/mm.png --every 24         # oheň v kulise z USD, přes kameru z USD
PYTHONPATH=build/python python3 examples/usd/make_plate.py    # plate záběru; pak je oheň v natočeném dvoře
```

V editoru:
- **Shift+A › Geometry › USD Import:** geometrie ze souboru.
- **Shift+A › Render › USD Camera:** kamera ze souboru. Zapojí se do vstupu
  Camera uzlu Output.

Z Pythonu:

```python
import pg
stage = pg.UsdStage("examples/usd/shot.usda")
stage.prims()                        # prim po primu, jak je skládá USD
cam = stage.camera(time=1010)        # matice ve světě (metry, Y nahoru), ohnisko, film
set_ = stage.geometry(prims=["/Set"])  # pg.Geometry: body, plochy, atributy
```

## 2. Uzly

### USD Import (geometrie)

Geometrie scény v daném snímku, ve světových souřadnicích:

| Z USD | Do geometrie |
|---|---|
| **Mesh** | polygony; `leftHanded` a zrcadlení transformací otočí pořadí rohů, díry (`holeIndices`) vypadnou |
| normály (`normals`, `primvars:normals`) | `N`: podle `interpolation` bodu, rohu nebo plochy; transformované a znormované |
| `primvars:st` | `uv` (vektor, z = 0), i s indexy (`primvars:st:indices`) |
| `primvars:displayColor`, `displayOpacity` | `Cd`, `Alpha` |
| ostatní primvars (čísla, vektory) | atribut stejného jména; `constant` a `uniform` na primitivech, `vertex` na bodech, `faceVarying` na rozích |
| `velocities` | `v`, transformované |
| **GeomSubset** ploch | skupina primitiv pojmenovaná po subsetu (`roof`, `glass`) |
| **Points** | volné body: `pscale` z `widths` (polovina, zvětšená transformací), `id` z `ids` |
| **BasisCurves** | otevřené lomené čáry (řídicí body; `periodic` uzavřené), `pscale` z `widths` |
| **Cube, Sphere, Cylinder, Cone, Capsule, Plane** | polygony podle rozměrů a osy |
| cesta primu | textový atribut primitiv `path` (`/Set/beam`) |

Parametry:
- **File:** `.usd`, `.usda`, `.usdc` nebo `.usdz`. Relativní cesta se čte ze
  složky sítě. Soubor, který se změní, se načte znovu.
- **Prims:** které primy číst, s tím, co je pod nimi. Jsou to cesty oddělené
  mezerou. Prázdné pole znamená celou scénu.
- **Frame Offset:** posun v čase (§3).
- **Render, Proxy, Guide:** které *purpose* číst. Výchozí je `default`
  a `render`, stejně jako renderer.
- **Metres, Y Up:** převod jednotek a osy nahoru (§4).
- **Subsets as Groups, Path Attribute:** skupiny ze subsetů a atribut `path`.

Neviditelné primy (`visibility = invisible` na nich nebo nad nimi) se
nečtou. Když se geometrie v souboru hýbe (časové vzorky, value clips,
animovaná transformace nebo viditelnost), uzel se vaří v každém snímku
znovu. Jinak jen jednou.

### USD Camera

Kamera ze souboru. Zapojená do vstupu Camera uzlu Output je to kamera, přes
kterou se renderuje, a kterou ukáže viewport při pohledu kamerou (0).

- **Poloha a otočení:** ze světové matice kamery v každém snímku. USD kamera
  se dívá po své −z, stejně jako Camera. Úhly se volí nejblíž předchozímu
  snímku, takže otočení nepřeskočí ze 180° na −180°.
- **Objektiv:** zorný úhel zleva doprava odpovídá horizontální cloně
  a ohnisku (`horizontalAperture`, `focalLength`), jako „fit horizontal“
  v Maye a Houdini.
- **Width:** šířka obrazu v pixelech.
- **Height:** výška obrazu. Hodnota 0 znamená, že se výška odvodí z poměru
  stran filmu, tedy Width × vertikální / horizontální clona. Super 35
  (24,89 × 14 mm) při šířce 1280 dá 720.
- **Prim:** cesta ke kameře. Prázdné pole znamená první kameru ve scéně.
- **Plate**, **Plate Frame:** obraz záběru za CG; jeho snímky jdou podle
  time codes záběru, `….1001.…` ve snímku, který čte time code 1001
  ([plate.md](plate.md)).

Co kreslení prototypu neumí, nahlásí uzel jako varování:
- ortografickou kameru nakreslí perspektivně;
- posun filmu (`horizontalApertureOffset`) vynechá.

## 3. Čas

Snímky prototypu začínají 1, časy USD bývají čísla snímků plate (1001…).
- **Snímek 1** čte `startTimeCode` scény. Když ho scéna neuvádí, čte time code 1.
- **Další snímky** jdou po `timeCodesPerSecond` scény ku frame rate Outputu.
  Při 24 a 24 je snímek jeden time code; když má Output 12 fps, jsou to dva.
- **Frame Offset** posune čtení o tolik snímků.

Mezi vzorky USD interpoluje, a čtečka stejně:
- čísla a vektory lineárně, quaterniony po kratším oblouku (slerp);
- pole jen tehdy, když mají obě stejně prvků. Jinak drží dřívější hodnotu.

Takže i podsnímky pro motion blur vycházejí jako v USD.

## 4. Jednotky a osy

Prototype počítá v metrech s osou Y nahoru. USD scéna může mít cokoli:
- **Jednotky:** `metersPerUnit` říká, kolik metrů je jednotka. Když scéna nic
  neuvádí, platí 0,01, tedy centimetry, jak má USD ve výchozím stavu.
- **Osa nahoru:** `upAxis` je Y, nebo Z (Maya, 3ds Max). Scéna se Z nahoru
  se otočí o −90° kolem X.

Obojí se bere z **kořenové vrstvy**, stejně jako v USD. Vrstvy, které
načítá, své jednotky nemají; pipeline je musí mít všude stejné. Parametr
**Metres, Y Up** převod vypne a geometrie zůstane v jednotkách souboru.

## 5. Co se skládá

Scéna se skládá jako v knihovně USD (Pcp), včetně pořadí síly názorů
(LIVRPS):

| | |
|---|---|
| **sublayers** | vrstvy pod kořenovou, s posunem a měřítkem času; když má vrstva jiné `timeCodesPerSecond`, čas se přepočítá |
| **references** | do jiného souboru (na default prim nebo pojmenovaný prim), uvnitř vrstev (interní), s posunem času |
| **payloads** | jako reference; načtou se vždy |
| **variant sets** | vybraná varianta: nejsilnější názor kdekoli v indexu (výběr záběru přebije výchozí volbu assetu), varianty ve variantách |
| **inherits, specializes** | třídy (`class`) i obyčejné primy; co se specializuje, je nejslabší v celém indexu, i když to přišlo z assetu v referenci |
| **def / over / class, active** | co je definované, co jen doplňuje, abstraktní třídy; třída, kterou prim dědí, z něj třídu nedělá; neaktivní prim nemá potomky |
| **value clips** | hodnoty z vrstev po snímcích (i vlastní export, [usd.md](usd.md) §3) – viz níže |
| **list edits** | `prepend`, `append`, `delete`, `add`, `reorder` a explicitní seznamy, složené od nejslabší vrstvy |

Reference na prim hlouběji ve stromu (`@asset.usd@</Tree/crown>`) přinese i
to, co ten prim dostává od předků, třeba jejich varianty a reference.

**Value clips** se čtou jako v USD:
- **Síla:** clips jsou hned za vrstvou, která je pojmenovala. Názor silnější
  vrstvy (třeba přepsání od lightingu) je přebije, názor slabší vrstvy
  (třeba výchozí hodnota v topologii) přebijí ony.
- **Primy pod primem** s clips berou hodnoty z odpovídajících primů v clips,
  mají-li vlastní clips, tak navíc k nim.
- **Pole sady** (`assetPaths`, `active`, `times`, `primPath`, manifest) se
  skládají přes vrstvy: každé z nejsilnější vrstvy, která ho má. Časy
  `active` a `times` jdou přes posun a měřítko vrstvy, která je zapsala.
- **Šablony:** `templateAssetPath` (`fx/sim.###.usd`, i `###.###` pro
  podsnímky) s `templateStartTime`, `templateEndTime`, `templateStride`
  a `templateActiveOffset`; soubor, který chybí, se přeskočí.
- **Čas:** `times` může jít zpátky i skokem (dva záznamy v jednom čase),
  třeba pro smyčku. Mezi vzorky se interpoluje i přes hranici clipů.
- **Manifest** říká, které atributy clips dávají. Když chybí, jsou to ty,
  které mají v některém clipu vzorky. Clip bez vzorků atributu dá výchozí
  hodnotu manifestu, jinak nic; s `interpolateMissingClipValues` se
  interpoluje ze sousedních clipů.
Soubor, který nejde přečíst, je varování (`prototype usd` ho vypíše), ne
chyba: zbytek scény se složí.

Nepodporuje se:
- relocates;
- implied inherits přes reference;
- instancing jako takový: instanceable primy se rozbalí jako obyčejné;
- session layer.

## 6. Ověření

Čtečka se porovnávala s Pixarovou knihovnou `usd-core` 26.08. Ta je jen
v testovacím prostředí, prototype ji nepotřebuje. Testy v Pythonu se bez ní
přeskočí.

- **Formáty:**
  - stejná scéna jako `.usda` a `.usdc` dá stejné hodnoty, časové vzorky,
    list edits, varianty, slovníky a vztahy;
  - crate verzí 0.4.0 až 0.12.0 (tu knihovna zapíše, když má scéna spline)
    se čte stejně, včetně komprimovaných polí (celá čísla, tabulky hodnot,
    halfy, 64bitová čísla);
  - `.usdz` se čte i s referencemi na soubory uvnitř balíčku.
- **Transformace:**
  - šest náhodných scén s operacemi všech druhů: posun, měřítko, rotace
    kolem jedné osy i tří os v šesti pořadích, `orient`, `transform`,
    inverze (`!invert!`), `!resetXformStack!`;
  - s časovými vzorky, v obou formátech;
  - matice ve světě se od `ComputeLocalToWorldTransform` liší nejvýš
    o 6e-7 relativně, tedy na úrovni přesnosti floatu.
- **Skládání:**
  - záběr z pěti souborů: sublayer s posunem, měřítkem a jiným FPS,
    reference s posunem, výběr varianty v záběru, třída, specialize na prim
    uvnitř reference, interní reference, payload, instanceable prim,
    neaktivní prim, value clips;
  - všech 25 primů, jejich typy a všechny zapsané hodnoty v 8 časech (i mezi
    vzorky) a matice ve světě sedí s knihovnou;
  - totéž platí pro vlastní export vody a deště (value clips po snímcích)
    ve snímcích i mezi nimi;
  - specializes v assetu za referencí a specifier primu, který dědí třídu,
    vycházejí jako v knihovně.
- **Value clips:**
  - 1300 náhodných záběrů: clips pojmenované v sublayeru mezi silnější
    a slabší vrstvou, s posunem a měřítkem času, 1–4 clipy, `times` se
    skoky a návraty, manifest s výchozími hodnotami i bez, šablony,
    `interpolateMissingClipValues`, clips předka na potomkovi;
  - hodnota v každé čtvrtině snímku, časové vzorky a „může se měnit“ sedí
    s knihovnou v 1295 záběrech;
  - zbylých 5 narazí na nedefinované chování samotné knihovny: když se
    vzorek clipu nedá přes `times` převést zpět do času scény, USD čte
    prázdný `std::optional`. Čtečka takový vzorek vynechá. Když místo
    toho vezme 0 (co v té situaci přečte tahle build knihovny), sedí
    všech 1300;
  - 2000 dalších náhodných záběrů, jejichž `times` pokrývají všechny vzorky,
    sedí všechny.
- **Geometrie:**
  - body ve světě po převodu jednotek a os sedí s body z knihovny
    přepočítanými její maticí (odchylka pod 1e-4);
  - `leftHanded`, díry, `faceVarying` uv s indexy, `uniform` barvy,
    subsety, purpose a viditelnost dopadnou tak, jak je čte renderer.
- **Kamera:** poloha, směr pohledu i horizontální zorný úhel USD Camera sedí
  s kamerou z knihovny.
- **Rychlost:** `.usdc` s 200 meshi (22 MB, milion bodů) se otevře za
  0,07 s a geometrie se z něj přečte za 0,12 s.
- **Ukázkový záběr:** `examples/usd` hlásí všech 28 validátorů USD 0 nálezů.

Testy:
- **`tests/test_usd_read.cpp` (17):**
  - text s hodnotami všech druhů a chyba s řádkem;
  - crate proti textu téže scény z `tests/data/usd`, jak je zapsalo USD;
  - crate verze 0.4.0 a `.usdz`;
  - skládání, specializes a specifier;
  - value clips: interpolace přes hranici, síla vůči vrstvám nad a pod
    nimi, pole ze dvou vrstev, šablony s posunem, smyčka skokem (hodnoty
    z knihovny);
  - transformace, import geometrie, uzly USD Camera a USD Import;
  - zpětné čtení vlastního exportu;
  - prototypy PointInstanceru zůstanou, kde jsou: nejsou geometrií scény;
  - 500 poškozených souborů odmítnutých bez pádu (i pod ASan).
- **`tests/python/test_usd.py`:**
  - ukázkový záběr;
  - s knihovnou `usd-core` náhodné transformace, skládání, geometrie a value
    clips proti ní, z toho 80 náhodných záběrů s clips.

## 7. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/usd/Layer.h` | Vrstva: hodnoty, prim a property specy, list edits, varianty; cesty; čtení souboru i z balíčku (`readLayer`, `resolveAsset`) |
| `src/pg/usd/Text.cpp` | Parser `.usda` |
| `src/pg/usd/Crate.cpp` | Čtečka `.usdc`: LZ4, celočíselné kódování USD, tabulky tokenů, cest, polí a speců, hodnoty všech typů |
| `src/pg/usd/Stage.h` | Skládání scény: indexy primů, síla názorů, hodnoty v čase, interpolace, value clips, cache otevřených scén (`openCached`) |
| `src/pg/usd/Geom.h` | Transformace z xformOps, jednotky a osa, geometrie (`importGeometry`), kamera (`cameraAt`), čas (`timeCodeAt`) |
| `src/pg/nodes/Usd.cpp` | Uzel USD Import (`usdimport`) |
| `src/pg/sim/Camera.cpp` | `cameraFromUsd`: kamera USD jako kamera záběru |
| `src/pg/sim/Network.cpp` | Uzly `usd_import` a `usd_camera`; kamera ze souboru v každém snímku |
| `src/python/PyUsd.cpp`, `pg.UsdStage` | Čtení z Pythonu |
| `tools/prototype/Commands.cpp` | `prototype usd` |
| `examples/usd/make_shot.py` | Jak vznikly soubory ukázkového záběru (knihovnou USD) |

## 8. Omezení

- **Materiály a textury** se nečtou. Barva je jen `displayColor`.
- **PointInstancer** (rozmístěné kopie, typicky vegetace, drť a zrna
  z vlastního exportu), **NURBS** a **Volume** (VDB) se nečtou. `prototype
  usd` je vypíše jako přeskočené. Prototypy pod PointInstancerem se
  nečtou ani jako samostatná geometrie: nestojí tam, kde jsou v souboru,
  ale tam, kam je instancer rozmístí.
- **Subdivize:** mesh se čte jako řídicí síť, bez vyhlazení.
- **Spliny** (animace křivkou, `x.spline`, USD 25 a novější) se nečtou:
  atribut, který má jen spline, nemá hodnotu. Časové vzorky a zbytek
  souboru se čtou dál.
- **Kolize z geometrie:** Object s tvarem z USD Import bere tvar ze snímku 1,
  ani když se geometrie v souboru hýbe. Kamera se hýbe po snímcích.
- **Objekty s barvou ze souboru:** Object se kreslí jednou barvou svého
  parametru, ne `Cd` z USD.
- **Kamera:** posun filmu, deformace objektivu a ortografická projekce se
  nekreslí.
- **Plate:** obraz záběru za CG, holdouty a shadow catchery popisuje
  [plate.md](plate.md); plate musí být bez zkreslení objektivu.
