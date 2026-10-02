# Obraz záběru (plate)

Efekt se ve studiu kreslí do natočeného záběru. **Plate** je sekvence
snímků z kamery na place, matchmove z ní spočítá pohyb kamery (uzel USD
Camera, [usd-import.md](usd-import.md)) a CG se kreslí touž kamerou přes
plate. Prototype to umí celé:

- čte sekvence **PNG, JPEG a OpenEXR** vlastními čtečkami, bez knihoven;
- kreslí plate **za CG**, když se díváte kamerou záběru, v editoru i v renderu;
- objekty a podlaha mohou být skutečné věci ze záběru:
  - **holdout** schová CG, které je za ním, a ukáže tam plate;
  - **shadow catcher** udělá totéž a navíc na sebe vezme stíny CG a světlo ohně;
- do **EXR** zapíše CG zvlášť s alfou a vedle ní průchod catcheru, pro compositing;
- totéž umí finální render, **Cycles** i path tracer ([§4](#ve-finálním-renderu-cycles-a-path-tracer)).

Kde CG nic nemění, vyjde plate z renderu pixel po pixelu, jak do něj
vešel.

![Vlevo plate (courtyard.1072.jpg), vpravo tentýž snímek s ohněm a kouřem ze simulace: stěny, trám a bedny jsou shadow catchery](img/plate.jpg)

## 1. Rychlý start

```bash
PYTHONPATH=build/python python3 examples/usd/make_plate.py   # „natočí“ plate příkladu (numpy)
./build/prototype sim matchmove mm.mp4                       # oheň v natočeném dvoře, celý záběr
./build/prototype sim matchmove mm.exr --every 24            # CG s alfou a průchod catcheru pro compositing
./build/prototype examples/sim/matchmove.pgsim               # editor: klávesa 0 = pohled kamerou, s plate
```

Ukázkový záběr nemá skutečnou kameru ani dvůr, a tak si plate program
natočí sám. `examples/usd/make_plate.py` vykreslí kulisu ze `shot.usda`
kamerou z matchmove, snímek po snímku. Pak jí dodá, co by jí dala kamera
a sken:

- objektiv trochu měkký a do rohů tmavší;
- světla, která se teple rozlévají (halace);
- barevné ladění;
- zrno, které je v každém snímku jiné.

Vznikne 72 JPEGů `examples/usd/plate/courtyard.1001.jpg` až `.1072.jpg`,
1280 × 720, asi 16 MB. Git je vynechává: dají se kdykoli vyrobit znovu.

V síti `matchmove` pak:
- USD Camera má `plate` = `../usd/plate/courtyard.####.jpg`;
- objekt `walls` (stěny, trám, sloup a bedny z USD) je shadow catcher;
- podlaha je výchozí shadow catcher.

Bez plate se síť kreslí jako dřív: bez obrazu vzadu a se vším jako CG
(kompilace varuje `no plate at frame 1`).

## 2. Plate na kameře

Uzly **Camera** i **USD Camera** mají sekci **Plate**:

| parametr | co dělá |
|---|---|
| **Plate** (`plate`) | soubor, nebo sekvence s číslem snímku ve jméně: `plate.####.exr` (tolik číslic, kolik `#`), `plate.$F4.exr` (`$F` bez doplnění nulami), `plate.%04d.exr` (`%d` bez doplnění). Relativní cesta se čte ze složky sítě. Jméno bez čísla je tentýž obrázek ve všech snímcích |
| **Plate Frame** (`plate_frame`) | číslo snímku plate ve snímku 1. U Camera je výchozí 1 (plate pro 1001… potřebuje 1001). U USD Camera je výchozí 0: plate jde podle time codes záběru, snímek s time code 1001 čte `….1001.…`. Posun kamery (Frame Offset) posune i plate |

- **Formáty:** PNG, JPEG a OpenEXR, poznané podle obsahu souboru (ne podle přípony).
- **Velikost:** plate stejně velký jako obraz kamery (Width × Height) se kreslí
  pixel po pixelu. Jinak se roztáhne na celý rámeček kamery a filtruje se.
- **Barvy:** PNG a JPEG jsou obrázky, jak je vidět na obrazovce. Proto
  jdou do světla rendereru zpátky přes jeho view transform (inverze ACES
  a gamy 2,2), a kde je jen plate, vyjdou znovu stejné. EXR je lineární
  světlo a bere se, jak je.
- **Kdy se kreslí:** jen pohledem kamery. V editoru přepne pohled klávesa **0**
  (nebo oko v záhlaví viewportu), render kamerou jde vždy, když ji síť má.
- **Čtení:** snímek se čte, když se změní. Chybějící nebo vadný soubor
  napíše editor do viewportu a `prototype sim` do terminálu, s důvodem.
  Obraz se pak kreslí bez plate.

## 3. Co je nad plate: holdout a shadow catcher

Uzel **Object** má v sekci Look parametr **Over the Plate** (`matte`).
Output má pro podlahu **Floor over the Plate** (`floor_matte`).

| volba | objekt nad plate | výchozí |
|---|---|---|
| **Solid** | CG: kreslí se sám sebou a zakryje plate | objekty |
| **Holdout** | skutečná věc ze záběru: CG za ní zmizí a ukáže se tam plate, jak je | |
| **Shadow Catcher** | holdout, který CG přesvětlí: stíny CG (objektů, zobrazené geometrie, kouře) plate ztmaví, oheň ho rozsvítí | podlaha |

Simulace se tím nemění. Holdout i catcher jsou dál kolize, kouř je obtéká
a voda o ně naráží. Bez plate se všechno kreslí jako Solid.

Jak catcher počítá. Pro bod na catcheru se spočítá světlo:
- **s CG:** slunce za vším (i za kouřem a CG objekty), obloha a světlo ohně;
- **bez CG:** slunce jen za skutečnými věcmi (holdouty a catchery) a obloha.

Plate se vynásobí jejich poměrem. Kde CG nic nemění, je poměr přesně 1 a
plate zůstane, jak je. Stín kouře ho ztmaví, světlo ohně rozsvítí, a to
víc tam, kde bylo ve skutečnosti jen světlo oblohy (ve stínu zdí).
Skutečné stíny, které plate už má, se nepřidají podruhé.

## 4. EXR pro compositing nad plate

Nad plate zapíše `prototype sim … OUT.exr` jen CG, aby šlo do compositingu:

| kanál | co v něm je |
|---|---|
| `R`, `G`, `B` | CG v lineárním světle: kouř, oheň, CG objekty; plate v nich není |
| `A` | kolik z pixelu CG zakrývá: 1 na CG objektu, neprůhlednost kouře, 0 tam, kde je jen plate (i na holdoutech a catcherech) |
| `catcher.R`, `catcher.G`, `catcher.B` | čím plate vynásobit: 1, kde CG nic nemění; méně ve stínech CG; víc, kde svítí oheň |

Záběr je pak `plate × catcher × (1 − A) + RGB`. V Nuke: plate (lineární)
vynásobit kanály `catcher` (Merge multiply nebo Shuffle a Multiply) a přes
to dát render operací `over`. Plate z PNG nebo JPEG musí jít do lineárního
světla stejnou inverzí view transformu (§2). Ostatní průchody (`Z`,
`forward.u/v`, masky) jsou jako bez plate ([render.md](render.md#4-exr-pro-compositing)).

Ověřeno na příkladu `matchmove`: složení podle vzorce, protažené tónovou
křivkou, dá PNG z rendereru. U 99,7 % pixelů na úroveň z 255, rozdíly
zbývají jen na hranách, kde se vyhlazení 2 × 2 průměruje jinde.

### Ve finálním renderu: Cycles a path tracer

![Snímek 72 příkladu matchmove: vlevo plate, uprostřed Cycles (64 vzorků na pixel), vpravo path tracer (128 vzorků). Oheň svítí na podlahu a na bok bedny, kouř stíní zeď, stěny, trám a bedny zůstaly skutečné](img/plate-render.jpg)

Plate jde i do finálního renderu, když se renderuje kamerou záběru:
v `prototype sim … --renderer cycles` nebo `--renderer path` a v záložce
**Render** editoru (pohled kamerou, klávesa **0**).

```bash
./build/prototype sim matchmove mm.png --renderer cycles             # snímek 72 nad plate
./build/prototype sim matchmove mm.exr --renderer cycles --every 24  # CG, A a catcher pro compositing
./build/prototype sim matchmove mm.png --renderer path --samples 128 # totéž přes path tracer
```

Renderer dá jen CG, kolik z pixelu zakrývá (alfa) a čím plate vynásobit
(catcher). PNG a záložka Render ukážou `plate × catcher × (1 − A) + CG`
v lineárním světle. EXR má kanály jako v tabulce výše (a k tomu `Z`,
`albedo.*` a `N.*`, [cycles.md](cycles.md)). PNG a JPEG jdou do světla
rendereru zpátky inverzí jeho view transformu (AgX, AgX Punchy nebo ACES,
`unshown`), a tak kde CG nic nemění, vyjde plate pixel po pixelu stejný.
Jedinou výjimkou je čistá bílá: AgX Punchy ukáže nejvýš 254,5 z 255,
takže 255 vyjde jako 254.

- **Cycles** renderuje na průhledný film, i přes sklo (Transparent Glass
  jako v Blenderu). Holdouty a catchery, objekty i podlaha, jsou jeho
  vlastní holdouty a shadow catchery. Slunce a obloha jsou skutečná
  světla, která plate osvětlila (v Blenderu „shadow catcher“ u světla),
  a proto svítí i ve světle bez CG. Násobitel plate je průchod Shadow
  Catcher: světlo na catcheru s CG lomeno světlem bez CG, obojí sledováním
  cest. Stíny CG, kouře i oheň v něm jsou celé a odšumí se spolu s obrazem.
- **Path tracer:** kamerový paprsek, který skončí na plate, holdoutu nebo
  catcheru, nechá pixel plate. Na catcheru spočítá světlo bílé matné
  plochy s CG a bez něj ze stejných paprsků. S CG je to slunce za vším
  (i za kouřem) a jeden směr oblohy, na kterém je obloha, nebo co pošle
  CG: jeho světlo, světlo ohně. Bez CG je to slunce jen za skutečnými
  věcmi a obloha mezi nimi. Kde CG nic nemění, jsou obě stejná a poměr
  je přesně 1. Násobitel odšumí Open Image Denoise spolu s obrazem. Sklem
  a vodou vidí kamera plate po lomeném paprsku. Ten je pak už v CG
  a alfa skla je 1, kdežto v Cycles je sklo průhledné.
- **Bez plate**, pohledem mimo kameru záběru nebo s kamerou bez plate, se
  holdouty i catchery kreslí jako obyčejné objekty.

Cycles a path tracer dávají nad plate podobný obraz. Stíny jsou v obou
sledované paprsky, takže světlo ohně zastaví i zeď, kterou ve viewportu
projde. Snímek 1280 × 720 na čtyřech jádrech trvá v Cycles se 64 vzorky
7,6 min, v path traceru se 128 vzorky 2,2 min. EXR z Cycles složené
podle vzorce a protažené tónovou křivkou dá PNG z rendereru: 98 % pixelů
přesně, zbytek o 1 úroveň z 255, protože EXR drží hodnoty jako half
float.

## 5. Obrázky bez knihoven

Plate čtou vlastní čtečky v `src/pg/io`. Tytéž čtečky i zapisovače jsou
v Pythonu jako `pg.read_picture` a `pg.write_picture`
([python.md](python.md#obrázky)).

| formát | čte | ověřeno |
|---|---|---|
| PNG | 1, 2, 4, 8 a 16 bitů; šedá, šedá s alfou, RGB, RGBA a paleta (i s průhledností `tRNS`); všech pět filtrů; prokládání Adam7 | na hodnotu proti Pillow: fixtures a 40 náhodných souborů |
| JPEG | baseline i progresivní, šedý i YCbCr, libovolné podvzorkování (4:2:0, 4:2:2, 4:4:0, 4:4:4), restart markery | bit po bitu jako libjpeg: 8 fixtures a 60 náhodných souborů různé kvality. Stejná celočíselná IDCT (`islow`), „fancy“ převzorkování barev a převod YCbCr |
| OpenEXR | řádky pixelů; bez komprese, RLE, ZIPS, ZIP, PIZ, PXR24, B44 a B44A; kanály half, float i uint | hodnota po hodnotě proti knihovně OpenEXR: soubor každé komprese a 48 náhodných. Včetně PIZ s 16bitovou vlnkou a datového okna menšího než obraz |

Z EXR se berou `R`, `G`, `B` (`A`). Když chybí, bere se první vrstva, která
je má (`beauty.R`…), jinak `Y` jako šedá, jinak první kanál. Deflate (pro
PNG a ZIP) je vlastní: bloky stored, pevné i dynamické Huffmanovy kódy,
tabulky na 10 bitů. Snímek 2K se přečte za 50–190 ms.

Zapisují se PNG (8 bitů, RGB nebo RGBA, deflate bez komprese), JPEG
(baseline 4:2:0, [render.md](render.md)) a EXR (half, RLE).

## 6. Render jen z geometrie

Síť bez simulace, jen se zobrazenou geometrií, se dřív kreslila jako jeden
obrázek z pohledu na geometrii. Když má Output kameru, kreslí se teď touž
kamerou po všech snímcích Outputu (`--every 1`, video), s animovanou
kamerou i geometrií. Tak `make_plate.py` natočí plate z kulisy: layout nebo
previz bez simulace.

## 7. Ověřeno

- **Plate beze změny:** v testu `ThroughAPlate` jde JPEG přes renderer se
  vším možným nad sebou, ale bez CG. Vyjde pixel po pixelu stejný:
  - samotný;
  - s podlahou jako catcher i jako holdout;
  - s objektem jako holdout;
  - s objektem jako catcher.
- **CG objekt** nad plate zakryje, co zakrývá, a jeho stín na podlaze
  (catcheru) plate ztmaví. V EXR je jeho `A` 1, jinde 0 a RGB tam je 0.
  `catcher` je pod 1 ve stínu a jinde přesně 1.
- **Příklad `matchmove`:** nebe je v každém snímku přesně plate. Stěny
  a zem se mění jen tam, kam dopadne světlo ohně nebo stín kouře. Světlo
  dopadá na strany obrácené k ohni, spodní plocha trámu svítí, přední
  strany beden ne.
- **Stínová mapa geometrie:** stěny, na které slunce svítí pod ostrým
  úhlem, měly ve stínech zobrazené geometrie pruhy. Stín se teď hledá kousek
  od povrchu po normále (půl druhého texelu mapy) a pruhy zmizely.

- **Finální render nad plate** (`tests/test_render.cpp`): záběr s CG
  kvádrem, jeho stínem na podlaze (catcher), holdoutem před druhým
  kvádrem a sklem. Path tracer i Cycles:
  - nad horizontem dají plate pixel po pixelu (320 z 320 pixelů);
  - kvádr zakryje svůj pixel (alfa 1,00), holdout ho odkryje (0,00) a za
    ním je plate na úroveň;
  - stín kvádru vynásobí plate 0,17 (path tracer) a 0,22 (Cycles);
  - přes sklo je plate o 7 až 8 % tmavší, o odraz na skle;
  - EXR (z path traceru) má `A` a `catcher.R/G/B`;
  - bez plate jsou holdout a catcher obyčejné objekty: pixel holdoutu má
    v path traceru jeho světlo (0,13), nad plate z CG nic.
- **Inverze view transformu** (`render_unshown_gives_back_the_light_a_picture_shows`):
  všech 256 šedých se vrátí na úroveň v AgX, AgX Punchy (bílá na 254)
  i ACES; barvy fotografie v AgX a ACES všechny, v AgX Punchy 3998 ze
  4000, zbylé o jednu úroveň.
- **Příklad `matchmove` bez ohně** v Cycles: násobitel 0,997 až 1,003
  (1. a 99. percentil), obraz se od plate liší v průměru o 0,9 úrovně
  z 255 (plate zmenšený na 640 × 360).

## 8. V kódu

| soubor | co dělá |
|---|---|
| `src/pg/io/Picture.h` | `Picture`, `readPicture`, `decodePicture`, `decodePng`, `decodeJpeg`, `encodePng`, `writePicture`, `sequenceFile`, `isSequence` |
| `src/pg/io/Inflate.h` | `inflate`, `zlibInflate`: deflate a zlib |
| `src/pg/io/Png.cpp`, `JpegDecode.cpp`, `ExrRead.cpp` | čtečky (a zápis PNG) |
| `src/pg/sim/Camera.h` | `Camera::plate`, `plateFrame`, `plateFile(frame)` |
| `src/pg/sim/Look.h`, `Scene.h` | `Matte` (None, Holdout, Catcher), `Solid::matte`, `Look::floorMatte` |
| `src/pg/gl/Volume.h` | `setPlate`, `clearPlate`: plate jako textura RGBA16F; v shaderu `plateAt`, `realSun`, `catcher`; průchod `catcher.*` v `writePassesExr` |
| `src/pg/render/Plate.h` | finální render: `Plate`, `loadPlate`, `plateLight` (plate ve světle rendereru), `plateSeen` (v pixelech obrazu), `overPlate` (složení) |
| `src/pg/render/PathTracer.h`, `Scene.h` | `unshown` (inverze view transformu); `PathTracer::alpha`, `catcher`; `Scene::plate`, `matteOf`, `realBlocks` |
| `src/pg/render/Cycles.cpp` | průhledný film, holdouty a shadow catchery Cyclesu, slunce a obloha jako skutečná světla, průchod `catcher` |
| `tools/prototype/SimViewport.cpp`, `Commands.cpp` | plate v editoru a v `prototype sim` |
| `examples/usd/make_plate.py` | plate příkladu: kulisa kamerou z matchmove a „film“ |
| `tests/test_picture.cpp` | 7 testů: PNG, JPEG a EXR proti knihovnám, jména sekvencí, vadné soubory, zápis a čtení zpět, plate a matte v síti |
| `tests/python/test_picture.py` | 12 testů: proti Pillow a OpenEXR (čtení i zápis), zápis a chyby, plate přes renderer |

## 9. Omezení

- **Zkreslení objektivu:** plate musí být bez zkreslení (undistorted),
  jak ho dodá matchmove. STMap ani overscan program nečte.
- **Posun filmu** (`horizontalApertureOffset`) kamera nekreslí, takže ho
  plate nesmí potřebovat.
- **Skutečné stíny catcheru** počítá program z těles objektů. U tvarů
  (koule, kvádr…) přesně, u objektů z geometrie z pole vzdáleností o 64
  buňkách podél nejdelší strany. To je hrubší než stíny, které má plate. Na
  hraně skutečného stínu, kam svítí oheň, proto může zůstat šev široký pixel
  nebo dva.
- **Světlo ohně** prochází ve viewportu zdmi: viewport ho nestíní. Cycles
  a path tracer ano.
- **Zrno:** CG zrno plate nemá. Doladit ho je práce compositingu z EXR.
- **Odrazy a osvětlení z plate:** CG nevidí plate jinak než jako pozadí.
  Voda v něm plate neodráží a HDRI z něj nesvítí.
- **Formáty:**
  - JPEG bez aritmetického kódování, 12 bitů a bezztrátového režimu;
  - PNG bez ohledu na `gAMA` a `iCCP`, bez APNG;
  - EXR bez dlaždic, deep dat, více částí, DWAA/DWAB a podvzorkovaných kanálů;
  - hodnoty nad 65504 se na GPU (half float) oříznou.
- **Průchod `catcher`** se průměruje z vyhlazení 2 × 2 zvlášť od alfy, a tak
  složení podle vzorce se na hranách CG od renderu liší (§4).
