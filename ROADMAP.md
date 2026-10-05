# Roadmapa

> Stav dokumentu: **v3** · Poslední aktualizace: 2026-10-04
>
> Živý dokument. Verze 1 (2026-09-21) plánovala headless knihovnu pro
> geometrii, GUI až ve třetí fázi a simulace po verzi 1.0. Cíl se změnil:
> **prototyp profesionálního softwaru typu Houdini** — celá cesta od
> geometrie přes simulace po obrázek a export. Verze 1 je v historii gitu
> (`git show a8058b2:ROADMAP.md`); její měřená kritéria platí dál a prototyp
> je ověřuje ([§3](#3-co-je-hotové)). Verze 3 rozepsala, co chybí k použití
> ve VFX studiu: krok 3 je napojení do pipeline (USD, farma, EXR), krok 4
> destrukce pro produkci, krok 5 měřítko.
>
> Související: [ARCHITECTURE.md](ARCHITECTURE.md) — datový model, cook engine
> a to, co z toho prototyp ověřuje.

---

## 1. Cíl

Node-based procedurální software pro geometrii a vizuální efekty, po vzoru
Houdini:

| Vrstva | Co to znamená |
|---|---|
| Geometrie | Uzly jako SOP: atributy na bodech, rozích, primitivech i celé geometrii, skupiny, objemy; líné vaření jen toho, co se změnilo |
| Jazyk | Wrangle: vlastní výpočet nad každým prvkem, s proměnnými, podmínkami, cykly a dotazy na geometrii |
| Procedurálnost | Výrazy a odkazy v parametrech, subnety a digital assets, smyčky |
| Simulace | Kouř a oheň, voda (FLIP), déšť a vítr; tuhá tělesa a destrukce; látky, lana a měkká tělesa (XPBD) |
| Obraz | Viewport, kamera, render záběru do obrázků a videa |
| Pipeline | Příkazová řádka, cache na disku, export (PLY, OBJ, OpenVDB, USD, Alembic), čtení USD, Alembic a OpenVDB, Python API |

Inspirace v Houdini je koncepční. Implementace je **clean room**: žádný HDK,
žádný reverse engineering, žádné přebírání dokumentace.

### Co to (zatím) není

- **Produkční nástroj.** Simulace počítají na CPU, Cycles také, editor
  stojí na Dear ImGui a mnoho z toho, co studio potřebuje, chybí
  ([§5](#5-potom)). Prototyp má ukázat, že architektura drží pohromadě,
  a změřit, kolik co stojí.
- **Kompletní parita s Houdini.** Hodnota není v počtu uzlů, ale v tom, že
  pár dobře navržených uzlů nad správným datovým modelem pokryje většinu úloh.
- **Compositing, zvuk, rigging a animace postav** — jiné kontexty.

---

## 2. Architektonické invarianty

Tato pravidla platí ve všech fázích. Porušení kteréhokoliv z nich je důvod
k zamítnutí PR — ne proto, že jsou posvátná, ale protože se **nedají dodělat zpětně**.
Rozbor každého z nich je v [ARCHITECTURE.md §2](ARCHITECTURE.md#2-invarianty).

1. **Copy-on-write na úrovni jednotlivých atributových polí.**
   Node, který mění `P`, sdílí všechna ostatní pole přes refcount. Nikdy nekopíruje celou geometrii.
2. **Struct-of-arrays.** Atribut je souvislé typované pole. Žádné `struct Point { ... }`.
3. **Jeden geometrický kontejner pro vše.** Polygony, křivky, volumes, packed prims, instance.
   Oddělené typy geometrie = ztráta kompozicionality.
4. **Líná pull evaluace.** Data se počítají až na vyžádání a jen to, co je špinavé.
5. **Determinismus.** Stejná scéna → bit-identický výstup, nezávisle na počtu vláken a platformě.
6. **Headless-first.** Každá funkce musí jít použít bez GUI. GUI je klient knihovny, ne naopak.
7. **Žádná globální mutovatelná data.** Cook musí být volatelný z více vláken a reentrantní.
8. **Verzovaný typ nodu.** Každý node má verzi a migrační cestu od prvního commitu.

---

## 3. Co je hotové

| Oblast | Stav | Dokumentace |
|---|---|---|
| Jádro | COW atributy, cook engine s verzemi a cache, časová závislost, deterministický paralelismus, per-element jazyk (interpret); vektory, matice a kvaterniony z knihovny GLM | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Shader graf | Uzly z textu, čtyři cíle (GLSL, GLSL ES, Vulkan, HLSL), editor s náhledem | [docs/shader-graph.md](docs/shader-graph.md) |
| Simulace | Kouř a oheň, voda (FLIP), déšť a vítr; z uzlů, deterministicky na libovolném počtu vláken | [docs/pyro.md](docs/pyro.md) |
| Destrukce | Voronoi Fracture, tuhá tělesa nad Jolt, slepené kusy jako jedno těleso, nálože, drcení, drť, prach hnaný vytlačeným vzduchem; odstřel věžáku jako video | [docs/destruction.md](docs/destruction.md) |
| Úpravy ve viewportu | Body, hrany a plochy vybrané myší (klik, obdélník, laso, štětec; jen viditelné, nebo i skryté); úchyt je posune, otočí a zvětší (Edit s měkkým poloměrem), skupina a mazání z vybraného, štětec atributů (piny, trhání látky) — vše jako uzly sítě | [docs/editing.md](docs/editing.md) |
| Geometrie v editoru | 30 SOP uzlů (i PolyExtrude, Subdivide, Clip, Fuse, Connectivity, Attribute Transfer, Voronoi Fracture, Convert Volume, Liquid Surface), smyčky For-Each, display flag, tabulka atributů; vaření na vlastním vlákně s přerušením; geometrie jako tvar simulací a simulace zpátky jako geometrie | [docs/geometry.md](docs/geometry.md) |
| Stromy | Uzel Tree: kmen s vidlicí, tři úrovně větví, sedm tvarů koruny, listy a jehličí, les na bodech, `flex` pro vítr, kostra pro vlastní listy; deterministicky na libovolném počtu vláken | [docs/trees.md](docs/trees.md) |
| Vegetace | Instance: body, které zastupují prototypy (GPU instancing, USD PointInstancer, Unpack); uzel Grass (trsy trávy), stromy a keře jako varianty; Scatter s hustotou, maskou, sklonem a odstupem; louka u lesa ve větru | [docs/vegetation.md](docs/vegetation.md) |
| Render | Cycles z Blenderu jako knihovna (sítě a instance, Principled BSDF, sklo a voda, kouř a oheň, fyzikální obloha jako v Blenderu, převod barev AgX, detail povrchů, hloubka ostrosti, rozmazání pohybem i plynu a otáčejících se objektů, Open Image Denoise), výchozí v záložce Render, `--renderer cycles`; vlastní path tracer na procesoru přes Intel Embree 4 a NanoVDB se stejným rozmazáním pohybem jako druhá volba (`--renderer path`); pohledy AgX, ACES 1.0 a ACES 2.0 jako v OpenColorIO; PNG a EXR s průchody v Rec. 709, ACEScg nebo ACES2065-1; oba nad plate záběru s holdouty a shadow catchery | [docs/cycles.md](docs/cycles.md), [docs/pathtracer.md](docs/pathtracer.md), [docs/color.md](docs/color.md), [docs/plate.md](docs/plate.md) |
| Procedurálnost | Wrangle jako VEX, výrazy v parametrech (`$F`, `ch()`), digital assets s knihovnou a verzemi, `prototype cook` | [docs/wrangle.md](docs/wrangle.md), [docs/assets.md](docs/assets.md) |
| Animace | Klíče na libovolném parametru, pohyblivé překážky, jejichž pohyb převezme plyn i voda | [docs/animation.md](docs/animation.md) |
| Cache a export | Snímky na disk a zpátky; PLY, OBJ, OpenVDB; celý záběr do USD (geometrie, tělesa v pohybu, drť a zrna jako kamínky, povrch vody, déšť, prach a pára, kamera, světla; co se mění, v souboru pro každý snímek) a do Alembicu; čtení Alembicu a OpenVDB (kouř z jiných programů přehraný jako plyn, level set jako překážka) | [docs/cache.md](docs/cache.md), [docs/usd.md](docs/usd.md), [docs/alembic.md](docs/alembic.md), [docs/vdb.md](docs/vdb.md) |
| Obraz | Kamera záběru, render do PNG, sekvence a videa | [docs/render.md](docs/render.md) |

### Co měření změnilo

- **M1 · COW:** 50 uzlů nad 2 M bodů alokuje 50 polí místo 250
  (`bench/bench_main.cpp`, blok 1). Ověření na 20 M bodech zbývá.
- **M2 · Cook engine:** editace uprostřed 100-uzlového řetězce stojí 48 %
  času studeného cooku, recook beze změny 0.025 ms, testy čisté pod TSan.
- **M5 · Jazyk — kritérium revidováno 2026-09-21.** Původní znění („< 150 ms
  na 10 M bodech na 16 jádrech" a „50× proti interpretu") stálo na odhadu,
  že interpret zvládne řádově 1 Mbod/s. Skutečnost je **38 Mbodů/s**, takže
  absolutní cíl by splnil i interpret a násobek byl nedosažitelný. Nové
  znění: aritmeticky vázaný snippet na 10 M bodech pod 120 ms na 4 jádrech
  a nejméně 8× rychleji než interpret (blok 3 v `bench/bench_main.cpp`).
  Přesně kvůli tomuhle se prototyp staví před plánem, ne po něm.

---

## 4. Další kroky

Každý krok končí demem a kritériem „hotovo, když". Další krok začíná, až je
předchozí hotový, zdokumentovaný a otestovaný (ASan, UBSan, TSan, libc++).

### Krok 1 — Procedurální jádro naplno ✅

- ✅ **Wrangle v2** ([docs/wrangle.md](docs/wrangle.md)): lokální
  proměnné, `if`/`else`, `for`, `while`, vlastní funkce; běh nad body,
  primitivy, rohy i celou geometrií; čtení jiných prvků a hledání sousedů
  (`point()`, `nearpoints()`); tvorba a mazání bodů a primitiv;
  celočíselné atributy; pole jako výsledky dotazů; posuvníky z `ch()`.
- ✅ **Výrazy v parametrech** ([docs/animation.md §6](docs/animation.md#6-výrazy)):
  `$F`, `$T`, matematika a odkazy `ch("../uzel/parametr")`, se sledováním
  závislostí a času; tlačítko fx v editoru, `--set` na příkazové řádce.
- ✅ **Digital assets** ([docs/assets.md](docs/assets.md)): vybrané uzly
  jako jeden uzel (Make Asset) s parametry, které si vybere (promote);
  knihovna `.pgasset` (s programem, `$PROTOTYPE_ASSETS`, uživatelská
  složka), vstup dovnitř a zpět (I/U), každá změna je nová verze, kterou
  sledují všechny instance; síť nese definice svých assetů s sebou;
  vnořování, cykly odmítnuté. Subnet bez knihovny zatím není — asset
  ho zastoupí.
- ✅ **Smyčky a nové uzly** ([docs/geometry.md](docs/geometry.md#smyčky-for-each)):
  For-Each Begin/End po kusech, primitivech, bodech, počtem i se zpětnou
  vazbou (tělo vařené ve vlastním grafu, bitově stejně na 1 i 4 vláknech);
  PolyExtrude s inset, Subdivide (Catmull-Clark), Clip s uzavřením řezu,
  Fuse, Connectivity, Attribute Transfer. Boolean zatím ne — řezy rovinou
  (Clip) pokryjí, co potřebuje Voronoi Fracture v kroku 2.
- ✅ **Vaření na pozadí:** zobrazená geometrie se vaří na vlastním vlákně
  (`pg/sim/Cooker.h`), editor na ni nečeká; nový požadavek přeruší
  rozpracované vaření (`CookContext::interrupt` — wrangle, smyčky
  i assety se vzdají uprostřed) a nic přerušeného se neuloží do cache.

**Hotovo, když:** procedurální budova z několika posuvníků (digital asset)
jde v editoru i z příkazové řádky (`prototype cook`) a stejné hodnoty dají
bitově stejnou geometrii na 1 i 4 vláknech. ✅ Asset **Building**
([docs/assets.md §5](docs/assets.md#5-příklad-budova-z-posuvníků)) má
deset posuvníků; příklad **street** z něj staví ulici;
`prototype cook street - --hash --threads 1` i `--threads 4` vypíší týž
hash a hlídá to test.

### Krok 2 — Destrukce ✅

- ✅ **Voronoi Fracture** ([docs/destruction.md §1](docs/destruction.md#1-voronoi-fracture)):
  buňky bodů (daných, nebo náhodných uvnitř) jako postupné řezy rovinami
  s víčky (Clip), kusy uzavřené a dohromady přesně původní těleso; `piece`
  na primitivech i bodech, řezné plochy ve skupině `inside`; paralelně
  a bitově stejně na 1 i 4 vláknech.
- ✅ **RBD Solver** ([docs/destruction.md §3](docs/destruction.md#3-rbd-solver))
  nad Jolt Physics 5.6 (MIT, `CROSS_PLATFORM_DETERMINISTIC`, jedno vlákno):
  kusy jako konvexní obaly s hustotou; slepené kusy jsou jedno těleso
  (compound), které náraz silnější než `glue` (kPa krát plocha spoje)
  rozlomí, a síla jde dál na další spoje; nálože, drcení na prach, drť;
  klíčované objekty jako kinematické překážky; podlaha.
- ✅ **Úlomky dál:** solver v Outputu se kreslí sám (barvy `Cd`, barva
  řezu); uzel RBD Pieces je vrací jako geometrii s `v`; výstup Collider
  dá kusy jako pohyblivé síťové překážky vodě, plynu a dešti; výstup Dust
  je zdroj prachu, který se rozpíná vzduchem vytlačeným zřícením; stíny
  geometrie a kusů; snímky s polohami kusů a drtí jdou do cache (formát 3).

**Hotovo, když:** budova se zřítí a zvedne prach — celé jako video,
deterministicky. ✅ Příklad **demolition**: odstřel čtrnáctipatrového
věžáku v bloku domů — nálože v přízemí, věž se sesune do svého půdorysu,
patra se drtí a oblak prachu se valí ulicemi; `prototype sim demolition
out.mp4` dá video a snímky jsou bitově stejné při každém běhu (test na
1 a 4 vláknech i mezi dvěma řešiči).

### Krok 3 — Napojení do studia (pipeline)

Studio by prototyp používalo jako Houdini: jako FX nástroj uprostřed
pipeline, který dostane modely a kameru od ostatních oddělení a vydá
simulace, které vyrenderuje oddělení osvětlení (Karma, Arnold, RenderMan,
Cycles) a složí compositing. Bez výměny dat s ostatními programy ho proto
nepoužije nikdo, ať simuluje jakkoli dobře.

- ✅ **USD — zápis** (`.usda`, bez knihovny; [docs/usd.md](docs/usd.md)):
  celá scéna —
  zobrazená geometrie, kusy jako tělesa s pohybem (tvar jednou, pak jen
  poloha a otočení), drť jako body, povrch vody jako uzavřená síť
  s rychlostí a pěnou, déšť, prach jako objemy (VDB vedle),
  kamera s ohniskem podle konvence USD, slunce a obloha; časové vzorky jen
  tam, kde se něco mění. Co je velké a v každém snímku jiné, jde do
  souboru pro každý snímek (value clips), takže záběr libovolné délky se
  nemusí vejít do paměti.
- ✅ **USD — čtení** (bez knihovny; [docs/usd-import.md](docs/usd-import.md)):
  `.usda`, `.usdc` (verze 0.4.0 až 0.10.0) i `.usdz`, scéna složená jako
  v USD — sublayers, reference a payloady s posunem času, varianty, třídy,
  value clips; uzel USD Camera dá Outputu kameru z matchmove snímek po
  snímku, USD Import kulisu a modely jako geometrii v metrech s Y nahoru;
  `prototype usd`, `pg.UsdStage`. Ověřeno proti knihovně USD; ukázka
  [examples/sim/matchmove.pgsim](examples/sim/matchmove.pgsim).
- ✅ **Alembic** (bez knihovny; [docs/alembic.md](docs/alembic.md)): celý
  záběr jako jeden archiv `.abc` — zobrazená geometrie, kusy jako tělesa
  v pohybu (tvar jednou, pak Xform), drť, zrna, výztuž, látka, voda, déšť,
  kamera; plyn jako VDB vedle — a uzly Alembic Import a Alembic Camera.
  Ověřeno Blenderem 4.5 (Alembic 1.8.3) oběma směry; ukázka
  [examples/sim/alembic_shot.pgsim](examples/sim/alembic_shot.pgsim).
- ✅ **OpenVDB — čtení** (bez knihovny; [docs/vdb.md](docs/vdb.md)): zip,
  Blosc (LZ4, zlib, BloscLZ), half, neaktivní hodnoty, dlaždice, instance,
  proudy, otočené mřížky; VDB Gas přehraje kouř a oheň jiných programů
  jako plyn záběru, VDB Import dá objemy nebo polygony level setu.
  Ověřeno na souborech z OpenVDB 10 a 13; ukázky
  [examples/sim/vdb_fireball.pgsim](examples/sim/vdb_fireball.pgsim)
  a [examples/sim/vdb_rock.pgsim](examples/sim/vdb_rock.pgsim).
- ✅ **`v` a stabilní `id`** u všech částic (drť, voda, déšť): z nich
  renderery počítají rozmazání pohybem a instancování. Nesou je snímky
  (cache formát 4), uzly Liquid Points, Rain Points a RBD Pieces (`grit`)
  a drť, voda a déšť v USD.
- ✅ **Povrch vody jako síť** (Liquid Surface, jako Particle Fluid Surface
  v Houdini) a objem na polygony (Convert Volume): surface nets, uzavřená
  síť s rychlostí z mřížky řešiče (cache formát 5) a pěnou
  ([docs/geometry.md](docs/geometry.md#povrch-vody-liquid-surface-a-convert-volume)).
- ✅ **Python API** (`import pg`, [docs/python.md](docs/python.md)): stavba
  sítě, parametry, výrazy a klíče, vaření a simulace ze skriptu; atributy,
  topologie, objemy i data snímků jako pole numpy bez kopie; cache, USD,
  render přes `prototype`; `as_code()` napíše síť jako Python. Scéna
  z kroku 2 postavená čistě z Pythonu:
  [examples/python/demolition.py](examples/python/demolition.py).
- ✅ **Farma**: rozsah snímků (`--start`, `--end`) pro render i export
  z cache. Simulace přerušená uprostřed jde dopočítat z checkpointu
  (`--checkpoint K`, `--resume`, v Pythonu `save_state` / `load_state`)
  bitově stejně. V editoru je bake na pozadí s průběhem, zrušením
  a pokračováním a náhled v polovičním rozlišení
  ([docs/cache.md](docs/cache.md#3-bake-na-pozadí-checkpointy-a-náhled)).
- ✅ **EXR**: náhledový render do lineárního EXR s hloubkou, vektory pohybu
  a maskami — pro previs a compositing ([docs/render.md](docs/render.md#4-exr-pro-compositing)).
- ✅ **Plate** ([docs/plate.md](docs/plate.md)): obraz záběru (sekvence PNG,
  JPEG nebo EXR, čtená bez knihoven a ověřená proti libjpeg, Pillow
  a OpenEXR) za CG, když se díváte kamerou záběru; objekty a podlaha jako
  holdout nebo shadow catcher, který na sebe vezme stíny CG a světlo ohně;
  do EXR CG s alfou a průchod `catcher`. Kde CG nic nemění, vyjde plate
  z renderu pixel po pixelu, jak do něj vešel. Ve viewportu, v Cycles
  (průhledný film, jeho shadow catchery) i v path traceru.

**Hotovo, když:** scéna z kroku 2 jde postavit a spočítat čistě
z Pythonu; výsledek se otevře v Blenderu a v usdview jako USD (kusy, drť,
prach, kamera, světlo) a render v Cycles sedí na náš náhled; simulace
přerušená uprostřed jde dopočítat z cache bitově stejně. To poslední je
**splněno**. Bake zabitý na snímku 43 pokračoval od checkpointu na snímku
40 a všech 60 snímků vyšlo bajt po bajtu stejně jako u nepřerušeného běhu.
Bake zrušený v editoru na snímku 50 a obnovený dal 150 stejných snímků.
Testy to ověřují pro plyn, vodu, déšť i odstřel s prachem.

### Krok 4 — Destrukce pro produkci

- **Lámání podle materiálu:** ✅ beton — uzel **Concrete Fracture**
  ([docs/destruction.md §2](docs/destruction.md#2-concrete-fracture)):
  nestejné kusy, nejmenší kolem místa nárazu, odprýsklé rohy jako
  samostatné úlomky, hrubé lomové plochy (vektorový šum, na obou stranách
  trhliny týž, kusy dál přesně lícují) a pod nimi rovný řez v atributu
  `proxy`: RBD Solver simuluje proxy a kreslí detail. K tomu u lepidla
  `spread` a `rings` (jak daleko náraz láme — Houdini *Propagate Rate* a
  *Iterations*), shluky přilepené k základu stojí, kde byly postavené, a
  příklad **concrete_wall**: demoliční koule prorazí betonovou zeď na
  soklu. ✅ Dřevo — uzel **Wood Fracture**
  ([docs/destruction.md §2](docs/destruction.md#dřevo-wood-fracture)):
  Voronoi v prostoru stlačeném podél vláken dá dlouhé třísky a latě,
  lomy napříč vlákny roztřepené na třísky, podél nich rýhované; směr
  vláken jde s kusy (`grain`), takže se podél nich lámou i za běhu.
  Příklad **wood_beam**: ocelová koule prorazí dřevěný trám.
- ✅ **Cihly:** uzel **Brick Wall** ([docs/destruction.md §2](docs/destruction.md#cihly-brick-wall))
  vyzdí zeď z cihel ve vazbě (běhounová, anglická, vlámská, stack), každou
  cihlu s maltou a omítkou jako jeden kus, s rovným ostěním u otvorů;
  malta je lepidlo, takže zeď praská ve spárách, a rozlomené cihly jsou
  dvě poloviny jedné kry. Příklad **brick_wall**: demoliční koule prorazí
  cihlovou zeď domu vedle okna; příklad **concrete_column**: odstřel
  železobetonového sloupu, po kterém zůstane holý armokoš.
- ✅ **Sklo:** uzel **Glass Fracture** ([docs/destruction.md §2](docs/destruction.md#sklo-glass-fracture))
  rozláme tabule paprsky z místa úderu a oblouky kolem něj (pavučina:
  střípky uprostřed, dlouhé střepy dál, rozvětvení); tabule je celá, dokud
  jí nepraskne spoj — pak se objeví celá pavučina. Sklo dělá desetinu
  prachu a skleněnou drť. Renderer ho kreslí průhledné (dvě vrstvy ploch,
  Fresnel obou stěn tabule, odraz oblohy a slunce, zelené hrany střepů),
  do USD jde s materiálem skla a trhlinami viditelnými od prasknutí.
  Příklad **glass_window**: míč vyletí oknem, zpomaleně.
- ✅ **Výztuž:** uzel **Rebar** ([docs/destruction.md §2](docs/destruction.md#výztuž-rebar))
  položí do zdi síť a do trámu armokoš s třmínky, natočené, jak blok
  leží; RBD Solver pruty (i nakreslené, lomené čáry s `width`) projde
  kusy a spojí kusy, které už lepidlo nedrží, plastickými vazbami: prut
  drží, co unese ocel nebo kotvení betonem (`rebar_strength`, `bond`),
  pak povolí a zůstane ohnutý, vytahuje se z krátkých konců a přetrhne se
  protažený o `stretch`. Kreslí se jako ocelové trubky ohnuté mezi kusy
  s pahýly přetržených prutů, do USD jako křivky s tloušťkou. Oba betonové
  příklady mají výztuž: trám se přes kvádr přehne a visí na ní, ze zdi
  visí kusy kolem díry.
- ✅ **Síť vazeb jako geometrie:** uzel **RBD Constraints**
  ([docs/destruction.md §3](docs/destruction.md#síť-vazeb-rbd-constraints))
  udělá z lepidla bod na těleso a čáru na spoj s `strength` (násobek Glue),
  `area` a barvou podle pevnosti. Síť upravená běžnými uzly — zeslabená,
  smazaná, dokreslená mezi kusy, které se nedotýkají — jde do Constraints
  RBD Solveru a je lepidlem. RBD Pieces vrátí síť snímku s `broken` a
  `time` (stav spojů ve snímku a cache). Příklad **constraint_network**:
  zeď praskne po čáře, kterou chce záběr. Zbývají výztuž a klouby
  (*Hard*, *Cone Twist*, měkké vazby) jako primitiva sítě.
- ✅ **Sekundární lámání:** kus se rozpadne až při nárazu — uzel **RBD
  Cluster** ([docs/destruction.md §2](docs/destruction.md#kry-a-sekundární-lámání-rbd-cluster))
  seskupí jemné kusy do ker s pevnějším lepidlem uvnitř (`cluster`,
  `clusterglue`, k-means++ a Lloyd přes těžiště kusů): věc se rozpadne na
  kry a kra se rozbije, až když tvrdě dopadne. Příklad
  **concrete_drop**: trám se zlomí přes kvádr a poloviny se rozpadnou na
  kry, až dopadnou.
- ✅ **Lámání za běhu:** ([docs/destruction.md §3](docs/destruction.md#lámání-za-běhu))
  kus, do kterého něco narazí silněji, než unese jeho průřez (`fracture`
  RBD Solveru, `f@fracture` kusu), se rozlomí tam, kam rána přišla: na
  úlomky, nejmenší kolem rány, s hrubými lomy (dřevo podél vláken,
  s třískami), které letí dál, jak kus letěl, a s prachem a drtí. Co do
  něj narazilo, jde dál, zpomalené jen o to, co kus unesl. Úlomky se
  lámou dál do zadané hloubky a velikosti. Zlom je událost (těleso,
  místo, semínko, počet, čas): snímky a cache (formát 16) nesou jen ty
  a úlomky se z nich udělají znovu bit po bitu; USD má úlomky jako
  tělesa viditelná od zlomu. Příklady **shatter_blocks** (koule projede
  třemi celými betonovými kvádry) a **wood_beam**.
- ✅ **Úlomky jako částice:** drť ([docs/destruction.md §3](docs/destruction.md#drť-jako-částice))
  vylétá z okraje plochy, kde praskl spoj, v její rovině. Vzduch ji brzdí
  (malou víc) a točí se, naráží do kusů, překážek i podlahy, odráží se a
  zůstává ležet na schodech, na římsách i na kusech, a s kusem, na kterém
  leží, jede, dokud se nerozjede, nenakloní nebo nezmizí. Za utrženými
  kusy se táhne prach (`trail`). Natočení každého kousku (`orient`) jde
  do snímků, cache (verze 9), RBD Pieces, Pythonu a USD a Copy to Points
  podle něj natočí kamínky. Příklad **debris_stairs**: podetnutý sloup se
  skácí ze schodů a drť zůstane na stupních. ✅ USD nese drť jako
  `PointInstancer` s kamínky stejných tvarů, jaké kreslí renderery
  ([docs/usd.md](docs/usd.md)), a drť zapojená do vstupu Grit Grain
  Solveru je zrny, která do sebe narážejí a hromadí se (příklad
  **shatter_grit**, [docs/grains.md](docs/grains.md#drť-z-betonu-jako-zrna)).
- ✅ **Usměrněná simulace:** Guide RBD Solveru ([docs/destruction.md §3](docs/destruction.md#usměrněná-simulace-guide))
  je animace kusů, tytéž body posunuté a natočené (klíčovaný Transform
  kolem Pivotu, wrangle podle `@Time`). Solver v každém kroku vede každé
  slepené těleso do pózy, která jeho body nejlépe položí na body Guide:
  s `guide_strength` 1 přesně, s menší se opožďuje. Gravitaci vyruší,
  kusy dál narážejí. Pustí je po `guide_until`, když praskne lepidlo
  (`guide_let_go`) nebo když je něco zastaví dál než `guide_reach`.
  Atribut `guide` řekne, jak moc Guide vede který kus. Z Guide se
  v každém snímku bere jen póza každého kusu (pár bajtů na kus). Příklad
  **guided_fall**: odstřelený komín padne podle klíčů přesně do ulice mezi
  dva domy a na silnici se volně rozlomí. Zbývá vedení silou nebo pružnou
  vazbou místo rychlosti a Guide, který kusy deformuje.
- ✅ **Tuhá tělesa na více vláknech**, deterministicky ([docs/destruction.md §3](docs/destruction.md#jak-to-funguje)):
  Jolt na vlastním poolu vláken, nárazy z jeho vláken sebrané a seřazené
  podle podkroku, těles a jejich částí, drť na vláknech, paprsek s pevným
  pořadím stejně blízkých ploch; snímky na 1 i 4 vláknech bit po bitu
  stejné (testy, `prototype sim --threads`, `pgbench_rigid`). Voronoi
  Fracture řeže buňku jen z blízkých částí tělesa blízkými body, bit po
  bitu stejně: věž z 5 628 buněk za 1,0 s místo 13,5 s. Lepidlo hledá
  plochy zametáním místo každé s každou.
- ✅ **Tělesa v klidu zmrznou** (Freeze at Rest, [docs/destruction.md §3](docs/destruction.md#jak-to-funguje)):
  těleso, které se půl sekundy nepohnulo a leží na tom, co se nehýbe, je
  v Joltu statické a nestojí nic. Probudí ho náraz s dost velkou
  hybností, těleso, které k němu za podkrok doletí (zametené kvádry
  v broad phase Joltu), voda, plyn, nálož nebo klíčovaný objekt; s ním
  i to, co na něm leží. Usazená věž z 5 628 kusů se krokuje za 1,9 ms
  na snímek místo 27 ms (před levnějšími kontakty 2,1 místo 56 ms),
  snímky zůstávají na 1 i 4 vláknech bitově stejné. Parametr `rest`
  (výchozí zapnuto).
- ✅ **Levnější kontakty** ([docs/destruction.md §3](docs/destruction.md#jak-to-funguje)):
  Jolt hledá spekulativní kontakty 5 mm dopředu místo 2 cm a podrží
  kontakty dvojice, která se pohnula o méně než 5 mm a 5° (místo 1 mm
  a 2°). Velká věž padá o 30 % rychleji, hromada i všechny příklady
  vypadají stejně. Cestou opravena látka, která natažený okraj díry
  trhala znovu v každém podkroku: plachta o 23 % rychlejší, bitově stejná.

**Hotovo, když:** odstřel z kroku 2 má beton, sklo a výztuž, stopy prachu
a sekundární lámání a desetkrát víc kusů za stejný čas na snímek. Beton,
výztuž, sklo, sekundární lámání, stopy prachu a vlákna jsou. Rychlost
splněná není. Věž z příkladu (593 kusů) se během pádu krokuje za 2,1 ms
na snímek na 4 vláknech (3,2 ms na jednom), desetkrát víc kusů (5 628)
za 19,5 ms (44 ms na jednom), tedy za devětkrát delší čas. Trosky, které
se usadily, už nestojí skoro nic, protože zmrznou: usazená velká věž
1,9 ms na snímek, 360 snímků v průměru 27 ms místo 61 ms před zmrazením
a levnějšími kontakty. Během pádu je krok skoro celý v Joltu, ve
srážkách dvojic konvexních obalů, a roste s počtem těles, která se
hýbou. Kusy věže nejsou malé (0,4–0,75 m, v průměru 11 bodů v obalu),
takže zjednodušit nejde co. Desetinásobek za stejný čas potřebuje
řešič na GPU (krok 5).

### Krok 5 — Měřítko

- ✅ **Řídká mřížka pro kouř a oheň** ([docs/pyro.md §4](docs/pyro.md#řídká-mřížka-počítá-se-jen-tam-kde-je-plyn)):
  pole v dlaždicích 8 × 8 × 8 buněk, jen tam, kde je plyn, a kolem, kam za
  krok doletí; tlak multigridem jen na nich, mimo ně p = 0; snímky a cache
  (verze 10) jen s dlaždicemi, kde plyn je. Se všemi dlaždicemi počítá
  bitově stejně jako hustá mřížka. Buňky, které zabírají kusy, se hledají
  jen v jejich obalech (dřív 40 % času prachu). Rozlišení až 1024.
- ✅ **Řídká voda** ([docs/pyro.md §5](docs/pyro.md#velký-běh)): mřížky
  vody v dlaždicích 8 × 8 × 8 jen kolem částic, tlak s volnou hladinou na
  nich; snímky a cache (verze 13 a 14) jen s dlaždicemi u hladiny, dlaždice
  hluboko ve vodě jen číslem; povrch surface nets jen kolem nich. Se všemi
  dlaždicemi bitově stejně jako hustá voda. Rozlišení až 1024. Příklad
  `flood_crates_hd`: 512 × 128 × 256 buněk a 17,6 milionu částic, 30 s až
  2,5 minuty na snímek na 4 jádrech, 2,2 až 5,7 GB paměti.
- ✅ **Upres** ([docs/pyro.md §4](docs/pyro.md#upres-hrubá-simulace-jemný-obraz)):
  uzel Pyro Upres nese plyn řešiče na dvakrát až čtyřikrát jemnější řídké
  mřížce, zdroje a hoření v jemném rozlišení, víry z curl noise unášeného
  s prouděním podle vířivosti hrubé simulace. Příklad `campfire_upres`:
  řešič 64 s upresem ×3 za 177 ms a 167 MB na snímek proti 358 ms a 334 MB
  řešiče ve 192.
- ✅ **Viewport pro velké cache** ([docs/cache.md](docs/cache.md#velké-cache-ve-viewportu)):
  snímky v paměti v rozpočtu (Cache Size), za ním odložené na disk a čtené
  zpátky; cache z disku čtená dopředu před přehrávací hlavou na vlastním
  vlákně, časová osa nikdy nečeká; pásy paměti a disku na časové ose;
  zástupné mřížky plynu a vody při přehrávání (nejvýš ~4 miliony buněk),
  plné po zastavení. `flood_crates_hd` (24 MB na snímek) se z disku
  přehrává ~15 snímků/s; 150 snímků povodně s cache 64 MB doběhne celých.
- **GPU** pro řešiče.
- **Packed primitives, instance a out-of-core** — miliony kusů a data
  větší než paměť.

**Hotovo, když:** prach odstřelu má 100 milionů voxelů a spočítá se na
jednom stroji přes noc. **Splněno:** prach příkladu `demolition` s
`--resolution 576` má doménu 576 × 312 × 576 = **103,5 milionu voxelů**
(buňka 16 cm) a 180 snímků se spočítá za **19 minut** na 4 jádrech (6,2 s
na snímek i s tuhými tělesy, `pgbench_pyro 576 --frames 180`), v nejvýš
3,4 GB paměti, s renderem 5 GB. Počítá se přitom nejvýš 23 % domény
(24 milionů voxelů, v nejhustším okamžiku); zbytek je stojící vzduch.
Hustá mřížka by jen na pole potřebovala přes 10 GB.

![Prach odstřelu ve 103,5 milionu voxelů: snímky 60, 90, 120 a 150](docs/img/demolition-576.jpg)

---

## 5. Potom

Seřazeno podle poměru hodnota / náklad:

1. **XPBD solver** — ✅ látky, lana a měkká tělesa s tlakem (obdoba
   Vellum): přišpendlené body nesené animací, kolize s objekty, kusy RBD
   i sebou samou, vzduch, vítr a proud plynu, deterministicky na
   libovolném počtu vláken, trhání (body se dělí, lana se rozpojí, balony
   praskají) a obousměrná vazba s kusy RBD ([cloth.md](docs/cloth.md)).
   ✅ Plochy a hrany se srážejí s objekty i mezi sebou (strom plošek
   s kužely normál, látka visí přes tyč tenčí než vzdálenost bodů),
   měkká tělesa drží tvar (shape matching) a s plasticitou zůstanou
   promáčklá (příklad **soft_bodies**). Zbývá: spojitá detekce kolizí
   (CCD) s časem dotyku, tvar po shlucích. ✅ Granuláty: Grain
   Solver, písek a štěrk s třením a kohezí, sypání, obousměrná vazba s kusy
   RBD, drť RBD Solveru jako zrna ([grains.md](docs/grains.md)).
2. **Vazby mezi řešiči** — ✅ trosky ve vodě a v plynu: voda je nadnáší
   a unáší, proud plynu unáší drť, obousměrně s vodou i plynem, které jdou
   kolem kusů ([destruction.md](docs/destruction.md#jedenáctý-příklad-povodeň-na-dvoře)).
   ✅ Voda a oheň: voda a kapky deště hasí oheň, chladí plyn, promáčí
   palivo i zdroje a dělají páru; déšť plní vodu, do které padá; korekce
   objemu FLIPu ([quench.md](docs/quench.md)). ✅ Pára je vlastní pole
   plynu (bílá, stoupá, řídne; ve všech třech rendererech, v cache, VDB
   i USD) a plameny vodu i kapky odpařují (příklad **fire_hose**). Zbývá:
   kondenzace páry, hašení jemných polí upresu.
3. **Render pro finální obraz** — ✅ Cycles z Blenderu jako knihovna
   ([cycles.md](docs/cycles.md)) a vlastní path tracer na procesoru
   ([pathtracer.md](docs/pathtracer.md)): povrchy, sklo a voda, hloubka
   ostrosti, AOV do EXR; kouř, oheň a prach s vícenásobným rozptylem,
   stíny kouře a světlem plamenů; odšumění neuronovou sítí Intel Open
   Image Denoise; materiály podle toho, z čeho povrch je (`s@material`:
   beton, omítka, cihla, okno, ocel, dlažba, tašky, trávník…), fotografie
   povrchů z knihovny (patnáct sad) a vlastní textury kladené ze tří stran
   nebo podél plochy podle polohy kusu před pohybem, nebo podle UV (uzel
   UV Project, importy) s normálovými mapami
   ([materials.md](docs/materials.md)); drť jako hranaté úlomky kamene
   a skla, déšť jako čárky vody a mokrý povrch, kam prší
   ([pathtracer.md](docs/pathtracer.md#drť-déšť-a-mokrý-povrch)); v obou
   rendererech rozmazání pohybem podle `v` bodů, rychlosti plynu, pohybu
   a otáčení objektů a pohybu kamery
   ([cycles.md](docs/cycles.md#rozmazání-pohybem)); CG nad plate záběru,
   holdouty a shadow catchery v obou rendererech, do EXR s alfou
   a průchodem `catcher` ([plate.md](docs/plate.md#ve-finálním-renderu-cycles-a-path-tracer)).
   ✅ ACES: pohledy ACES 1.0 a 2.0 jako v konfiguracích OpenColorIO
   (od OpenColorIO 2.6 nejvýš o setinu stupně z 255), EXR v ACEScg
   a ACES2065-1 s chromaticities a jejich čtení ([color.md](docs/color.md)).
   ✅ Rozmazání plynu (rychlost ve snímcích, z VDB i do VDB) a objektů
   scény v Cycles i v path traceru, který rozmazává i vše ostatní.
   ✅ UV a normálové mapy: uzel UV Project, `vt` z OBJ, fotky podle UV
   a normálové mapy (OpenGL i DirectX) v obou rendererech, tečny jako
   MikkTSpace ([materials.md](docs/materials.md#podle-uv-a-normálové-mapy)).
   Zbývá: Cycles na GPU, čtení konfigurací OCIO. Do té doby renderují studia náročné záběry přes USD
   vlastními renderery.
4. **JIT pro wrangle** (LLVM ORC nebo Warp) — až bude interpret úzkým
   hrdlem (kritérium M5 výše).
5. **Výměna dat** — ✅ Alembic (zápis i čtení, [alembic.md](docs/alembic.md))
   a čtení OpenVDB ([vdb.md](docs/vdb.md)), rychlost plynu (`vel`) ve
   snímcích, VDB a USD, zápis VDB s kompresí Blosc nebo zip, materiály
   jako MaterialX v USD i v `.mtlx` a čtení `.mtlx` jako sady textur
   ([materialx.md](docs/materialx.md)).
6. **Build podle VFX Reference Platform** — Rocky Linux a knihovny ve
   verzích, se kterými počítají pipeline studií.
7. **Úpravy geometrie ve viewportu** — ✅ body, hrany a plochy vybrané
   myší, jen viditelné (strom obálek nad polygony); úchyt na vybraném
   nastavuje uzel Edit (tahy se přesně skládají, měkký poloměr), Ctrl+G
   skupina, Delete Blast, štětec maluje atribut kapkami jako místy
   (Attribute Paint); značky v rendereru s testem hloubky
   ([editing.md](docs/editing.md)); Tab vloží libovolný uzel na vybrané
   (PolyExtrude s úchytem na Distance, wrangle), N ukáže čísla prvků;
   výběr obdélníkem, lasem i štětcem (S), H vybírá i skryté a ukáže je
   průsvitně; úchyty geometrických uzlů (Transform a Edit v pivotu, Clip);
   měkký výběr (O) s náhledem podílu pohybu na geometrii, poloměr klávesami
   i kolečkem během tahu, vzdálenost přímá nebo po povrchu, pět tvarů útlumu;
   sculpt (U, uzel Sculpt): Push / Pull, Smooth s okraji, které drží čáru,
   Grab, Flatten, kapky jako místa, přírůstkový výpočet tahu do bitu shodný
   s výpočtem od začátku (pohyb myši na milionu bodů 12 ms), strom pro
   výběr jen přepočítá obálky (refit); viewport kreslí zobrazenou geometrii
   indexovaně a při posunu bodů nahraje jen polohy a normály vrcholů
   (milion bodů 62 ms místo sekund, na GPU 24 MB místo 215 MB).
   Zbývá: režim vrcholů, Dissolve hran, symetrie, dyntopo.
8. **Vegetace** — ✅ uzel Tree: strom roste jako rostlina podle modelu
   Webera a Penna — kmen (i rozdělený do vůdčích větví), až tři úrovně
   větví kolem rodiče o zlatý úhel, prohnuté vahou, stočené ke světlu
   a bloudící, sedm tvarů koruny, listy, úzké listy a jehličí; na každém
   bodě vstupu jeden strom (les, každý jiný podle `id`); síť s atributem
   `flex` pro vítr wranglem, nebo kostra s `orient` pro vlastní listy přes
   Copy to Points ([trees.md](docs/trees.md)). Les 34 stromů (3,5 milionu
   bodů) za 0,9 s; ve větru se ve viewportu nahrávají jen polohy.
   ✅ Instance: bod zastupuje prototyp geometrie (`instance`, `orient`,
   `pscale`, `tint`) — Merge, Transform a Unpack s nimi počítají, viewport
   je kreslí přes GPU instancing, USD dostane PointInstancer (i po
   snímcích), OBJ a PLY kopie; uzel Grass pěstuje trsy trávy a rozhází je
   po terénu jako instance; Tree s výstupem Instances (varianty) pro les
   a keře; Scatter s hustotou na m², maskou z atributu, sklonem
   a odstupem; Copy to Points s instancemi a kusy podle atributu. Příklad
   meadow: 122 577 trsů (1,9 milionu stébel), 84 stromů a 65 keřů
   za 148 ms, snímek ve větru 31–39 ms ([vegetation.md](docs/vegetation.md)).
   ✅ UV na kůře, listech a stéblech; kůra s normálovou mapou podle UV,
   listy z obrázku knihovny s alfa výřezem a tráva z obrázku stébla
   v Cycles i path traceru (příklad **foliage**, [trees.md](docs/trees.md)).
   ✅ Průsvitnost listů a stébel proti slunci i ve viewportu.
   ✅ Úrovně detailu ve viewportu: vzdálené stromy a tráva s méně,
   ale většími listy a stébly (louka z 50 m: 44 % trojúhelníků).
   ✅ Viewport klade fotky a obrázky listů jako renderery (UV, tři strany,
   normálové mapy, alfa výřez), vzdálené rostliny jako billboardy
   a úrovně detailu se prolínají.
   ✅ Uzel Plant Wind: ohyb od paty podle `flex` bez natahování, poryvy,
   třepetání listů, `v` pro rozmazání; instance předohnuté do několika
   tvarů.
   ✅ Prořezávání obálkou a kořeny (Tree), šlapání (Plant Trample),
   ekosystém tří druhů za roky (Ecosystem, příklad **ecosystem**).
   ✅ Vítr jako pružiny (Plant Wind, Dynamics): každý stonek tlumený
   oscilátor řešený přesně po krocích 1/120 s, větve nesené a švihané
   stonkem, ze kterého rostou, stavy uložené mezi snímky (stejný výsledek
   v jakémkoli pořadí snímků i počtu vláken); les 0,29 s na snímek.
   Cestou opraveny stromy ze dvou uzlů Tree po Merge, které se ohýbaly
   kolem cizí paty.
   Zbývá: ekosystém ve 3D (světlo podle výšky), vyhýbání se větví
   překážkám.

---

## 6. Rizika

| # | Riziko | Dopad | Mitigace |
|---|---|---|---|
| R1 | ~~Ochranná známka „Houdini"~~ | — | **Vyřešeno 2026-09-27:** projekt se jmenuje Prototype |
| R2 | Rozsah přeroste síly | Nic není dotažené | Každý krok končí demem a kritériem „hotovo, když"; další až po něm |
| R3 | Nedeterminismus objevený pozdě | Cache a render se rozejdou, testy nejdou | Bitová shoda na 1 a 4 vláknech je v testech každého řešiče |
| R4 | Výkon hustých mřížek | Scény zůstanou malé | Malé rozhraní `Grid`, výměna za řídké mřížky (krok 5) |
| R5 | Závislosti (Jolt, Python) | Build na FreeBSD, bez sítě | Každá závislost volitelná, jádro zůstává jen C++20; build ověřovaný i s libc++ |
| R6 | Projekt zůstane „one man show" | Zánik při vyhoření | Dokumentace a příklady ke každému kroku, testy jako specifikace |

---

## 7. Jméno, licence, clean room

- [x] **Jméno: Prototype** (2026-09-27). Pracovní název byl zaměnitelně
      podobný registrované ochranné známce SideFX. Jmenný prostor v kódu
      zůstává neutrální `pg`, formáty souborů se nemění.
- [ ] Licence **Apache 2.0** (patentový grant, standard ASWF). Vyloučit GPL —
      studia musí smět psát proprietární uzly.
- [ ] `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, DCO.
- [ ] Písemně zaznamenat clean-room politiku pro přispěvatele.
