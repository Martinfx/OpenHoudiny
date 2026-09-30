# Architektura

> Stav: **návrh v1 + ověřovací prototyp** · Poslední aktualizace: 2026-09-21
>
> Dokument popisuje cílovou architekturu. Část je už postavená a změřená
> (viz [§9](#9-co-prototyp-skutečně-umí)), zbytek je návrh. U každé kapitoly je
> uvedeno, co platí dnes a co je plán.

Související dokumenty: [ROADMAP.md](ROADMAP.md) — cíl, co je hotové a další kroky.

---

## 1. Tvar systému

```
┌─────────────────────────────────────────────────────────────┐
│  Editor (Dear ImGui)  uzly · viewport · parametry · tabulka │   hotovo
├─────────────────────────────────────────────────────────────┤
│  CLI / headless cook               Python API (pybind11)    │   hotovo
├─────────────────────────────────────────────────────────────┤
│  Uzly: geometrie · simulace · I/O · digital assets          │   hotovo · kroky 1–2
├─────────────────────────────────────────────────────────────┤
│  Jazyk: parser → interpret (později JIT → SIMD kernely)     │   krok 1 · §5
├─────────────────────────────────────────────────────────────┤
│  JÁDRO   cook engine · geometrie · atributy · paralelismus  │   hotovo
├─────────────────────────────────────────────────────────────┤
│  Externí: Jolt (hotovo) · USD · (OpenVDB · Embree · OpenSubdiv) │   kroky 2–3 · §5
└─────────────────────────────────────────────────────────────┘
```

Zásadní vlastnost tohohle uspořádání: **GUI je klient knihovny, ne naopak.**
Kroky vpravo odkazují do [ROADMAP.md §4](ROADMAP.md#4-další-kroky).
Každá schopnost musí být dosažitelná voláním knihovny dřív, než pro ni vznikne
tlačítko. Render farma nemá viewport a testy nemají myš.

---

## 2. Invarianty

Osm pravidel, která platí napříč celým systémem. Nejsou to preference —
je to výběr věcí, které **se nedají dodělat zpětně**, protože prostupují
každou datovou strukturou a každým uzlem.

| # | Invariant | Co se stane, když se poruší |
|---|---|---|
| **I1** | Copy-on-write per atributové pole | Paměť roste jako O(uzlů × atributů). Dlouhé řetězce přestanou být použitelné. |
| **I2** | Struct-of-arrays | Ztráta vektorizace, 3–10× pomalejší per-element operace. |
| **I3** | Jeden geometrický kontejner pro vše | Ztráta kompozicionality — uzly přestanou jít libovolně spojovat. |
| **I4** | Líná pull evaluace, jen špinavé | Každá změna parametru přepočítá celý graf. Konec interaktivity. |
| **I5** | Determinismus nezávislý na počtu vláken | Snímky se liší mezi stroji farmy. Nalezeno měsíce po nasazení. |
| **I6** | Headless-first | Nic nejde spustit na farmě ani otestovat v CI. |
| **I7** | Žádný globální mutovatelný stav | Cook nejde volat paralelně ani reentrantně. |
| **I8** | Verzovaný typ uzlu + migrace | První breaking change znehodnotí všechny existující scény. |

Porušení kteréhokoliv z nich je důvod k zamítnutí PR.

---

## 3. Datový model

### 3.1 Atributové pole

Atribut je **jedno souvislé typované pole** (`AttributeArray`). Ne pole struktur.

```cpp
class AttributeArray {
    std::shared_ptr<Buffer>      buffer_;   // data prvků
    std::shared_ptr<StringTable> strings_;  // jen pro String, také COW
    AttrType type_;
    size_t   count_;
};
```

Celé COW se odehrává na jednom místě:

```cpp
std::byte* AttributeArray::rawWrite() {
    if (!buffer_)                       buffer_ = make(byteSize());
    else if (buffer_.use_count() > 1)   buffer_ = make(*buffer_);  // klon
    return buffer_->bytes.data();
}
```

Čtení (`rawRead`) nikdy neklonuje ani nealokuje. Důsledek: uzel, který mění
jen `P`, **sdílí všechna ostatní pole** s výstupem předchozího uzlu.

**Typy prvků:** `int32`, `float32`, `vec2/3/4`, `string`.
String atributy ukládají `int32` index do sdílené tabulky — prvky zůstávají
pevné šířky, takže SoA rozvržení a všechny hromadné operace (`gather`, `append`,
`resize`, hash) fungují beze změny.

> **Plán:** array atributy (per-prvek pole proměnné délky) a `mat3/mat4`.
> Návrh: samostatné pole offsetů + jedno ploché datové pole, aby prvky
> zůstaly pevné šířky.

### 3.2 Čtyři třídy atributů

`detail` (1 hodnota na celou geometrii) · `point` · `vertex` (roh primitiva) ·
`primitive`

Rozdělení point vs. vertex je to, co umožňuje švy v UV a tvrdé hrany
v normálách, aniž by se musely duplikovat body.

### 3.3 Geometrie

```cpp
class Geometry {
    AttributeSet detail_, points_, vertices_, primitives_;
    std::map<std::string, Group> groups_;   // masky, také COW
    std::shared_ptr<Topology> topo_;        // také COW
};
```

Topologie je plochá, indexová:

```
primitive p  ──▶ primStart[p], primCount[p]  ──▶  vertexPoint[v]  ──▶  point
```

**Kopie `Geometry` nekopíruje ani jeden prvek dat.** Atributy, topologie
i skupiny jsou `shared_ptr` a klonují se až při zápisu. Proto každý uzel
začíná cook takhle:

```cpp
auto geo = editableCopy(input);   // O(počet atributů), ne O(počet prvků)
```

`GeometryPtr` je `shared_ptr<const Geometry>` — co vyšlo z uzlu, je neměnné.

> **Plán:** packed primitives (primitive jako odkaz na geometrii + transformace)
> a delayed load. Bez nich nejde postavit scéna s 10 000 instancemi. Patří do M14
> a datový model s nimi počítá — proto je topologie oddělená od atributů.

### 3.4 Hash geometrie

`Geometry::hash()` prochází atributy **v setříděném pořadí jmen** (proto
`std::map`, ne `unordered_map`), pak topologii a skupiny. Je to základ zlaté
regresní sady: dva výsledky se stejným hashem jsou obsahově bitově totožné.

U string atributů se hashují **samotné řetězce**, ne indexy — hash tak nezávisí
na tom, v jakém pořadí se tabulka náhodou naplnila.

> **Známý nedostatek:** FNV-1a po bajtech je pomalý (~1 GB/s) a je serializovaný.
> Produkce potřebuje xxHash3 nebo podobný, paralelně po chuncích se stromovou
> kombinací.

---

## 4. Cook engine

### 4.1 Verze místo dirty flagu

Špinavost se nesleduje booleanem, ale **čítačem verzí**:

```
změna parametru na uzlu N
    ├─▶ N.version++
    └─▶ pro každý uzel po proudu (BFS, se seen-setem): version++
```

Klíč cache je `(uzel, verze, snímek)`. Stará položka se prostě přestane
nacházet. **Není potřeba žádný invalidační průchod** — a není ho tedy ani jak
zapomenout napsat. To je hlavní důvod pro tohle řešení.

Nastavení parametru na hodnotu, kterou už má, verzi nezvedá.

### 4.2 Pull evaluace

```cpp
GeometryPtr cookRecursive(Node& n, const CookContext& ctx, const TimeDepMap& td) {
    key = { &n, n.version(), timeDependent(n) ? ctx.frame : kAnyFrame };
    if (auto hit = cache_.find(key)) return hit;       // ← nic se nepočítá

    inputs = [rekurzivně cookRecursive pro každý vstup]
    out = n.cookNode(ctx, inputs);
    cache_.insert(key, out);
    return out;
}
```

Uzly, jejichž verze se nezměnila, se **vůbec nezavolají**.

### 4.3 Čas jako dimenze závislosti

Uzel je časově závislý, pokud jím je sám, **nebo** je jím cokoliv nad ním.
Časově nezávislé uzly mají jednu položku cache pro všechny snímky; časově
závislé mají položku per snímek.

Mapa časové závislosti se počítá **jednou, předem, sériově** — počítat ji
líně uvnitř rekurze by se rozbilo, jakmile se větve začnou cookovat paralelně.

Uzel `pointwrangle` hlásí časovou závislost podle toho, jestli jeho snippet
čte `@Time` nebo `@Frame` — odvozeno z naparsovaného programu, ne ze zaškrtávátka,
na které uživatel zapomene.

### 4.4 Cache

LRU s tvrdým paměťovým stropem (výchozí 2 GB). Položka větší než celý rozpočet
se stejně vrátí (jinak by velký výsledek nešel nikdy dodat). Když je cache plná,
jdou nejdřív výsledky, které potřebuje jen jeden snímek (časově závislé), od
nejstaršího; časově nezávislé až po nich. Jinak by animovaný uzel pod těžkou
geometrií (milion bodů rozbitého betonu) v každém snímku vytlačil tu geometrii
a každý snímek by ji cookoval znovu — osmdesátkrát pomaleji.

> **Známý nedostatek:** položce se účtuje `Geometry::memoryUsage()`, což počítá
> i buffery sdílené s jinou položkou cache. Je to tedy **horní odhad**. Přesné
> účtování vyžaduje registr bufferů s deduplikací podle identity — patří to
> k M14 (out-of-core), kde na tom začne záležet.

### 4.5 Paralelismus větví

Nezávislé vstupy vícevstupého uzlu se cookují souběžně. Cache je chráněná
mutexem.

> **Známý nedostatek:** uzel dosažitelný přes dvě větve může být cooknut
> dvakrát (stejný výsledek, zbytečná práce). Odstranění vyžaduje evidenci
> „právě se počítá" s čekáním — jednoduché, ale zatím neudělané.
> Test, který počítá přesné počty cooků na diamantovém grafu, si proto
> paralelismus větví vypíná.

### 4.6 Přerušení

`CookContext::interrupt` je příznak toho, kdo o vaření požádal: „už
nechci“. Engine ho čte před každým uzlem a po něm. Když je nastavený,
nevaří dál, vrátí null a **nic, co vzniklo po nastavení, neuloží do
cache** — přerušený výsledek může být poloviční. Uzly, které trvají
dlouho, se na příznak ptají samy: wrangle každých 1024 kroků
interpretu, smyčka For-Each mezi průchody, asset předá příznak svému
vnitřnímu grafu. Editor tak vaří zobrazenou geometrii na vlastním
vlákně (`pg/sim/Cooker.h`) a každý nový požadavek (tah posuvníkem)
přeruší ten rozpracovaný.

---

## 5. Per-element jazyk

Pipeline, kterou se sem míří:

```
zdroj → lexer → parser → typová kontrola a vazba → vlastní IR → LLVM ORC JIT → SIMD kernel
                                                                    ↑
                                                    sem se připojí JIT (ROADMAP §5)
```

**Dnes** je místo posledních dvou kroků interpret typovaného stromu
(`src/pg/lang`, popis jazyka v [docs/wrangle.md](docs/wrangle.md)). Švem,
kde JIT nahradí interpret, je `lang::Program::run()`:

1. **Parse jednou** (`Parse.cpp`) — celý program: proměnné, `if`, `for`,
   `foreach`, `while`, vlastní funkce, pole, řetězce. Už parser zná jména
   a počty argumentů vestavěných funkcí, takže překlep v názvu je chyba
   s řádkem a sloupcem hned při psaní.
2. **Typová kontrola a vazba při každém běhu** (`Check.cpp`) — typy atributů
   přicházejí z geometrie, nad kterou program běží, a ta se mezi cooky může
   změnit. Kontrola z nich udělá typovaný strom: každé `@jméno` je navázané
   na ukazatel do atributového pole, každé volání na konkrétní přetížení,
   každý převod (int → float, float → vektor) je explicitní uzel. Za běhu
   se už nic nevyhledává podle jména a nic se nerozhoduje podle typu.
   Parametry `ch("x")` a `$F` jsou po celý běh stejné, takže se tu
   dosadí jako konstanty.
3. **Běh** (`Eval.cpp`, `Run.cpp`) — vyhodnocení je šablona podle typu
   výsledku (`ev<float>`, `ev<Vec3>` …), lokální proměnné jsou sloty
   v polích podle typu. Program, který sahá jen na svůj prvek, běží po
   deterministických chuncích paralelně. Program, který vyrábí nebo maže
   geometrii, zapisuje cizí prvky nebo řetězce, běží **popořadě**: změny se
   řadí do fronty a provedou se po běhu v pořadí, v jakém je program
   požádal. Výsledek tedy nikdy nezávisí na počtu vláken — test
   `lang_results_do_not_depend_on_the_thread_count`.

JIT zamění krok 3 za volání zkompilovaného kernelu. Kroky 1 a 2 zůstanou.

**Proč vlastní jazyk a ne embedovaný Python/Lua:** per-element kód musí běžet
desetimilionkrát za cook. To vylučuje jakýkoliv jazyk s GIL, s dynamickou
alokací na hodnotu nebo bez statických typů.

---

## 6. Paralelismus a determinismus

Pravidlo, které dělá výsledky reprodukovatelné:

> Práce se dělí na počet chunků odvozený **pouze z počtu prvků**, nikdy
> z počtu vláken.

Z toho plyne:

- `parallelFor` — chunky píšou do disjunktních rozsahů, na pořadí nezáleží.
- `parallelReduce` — jeden mezivýsledek na chunk, **skládají se sériově
  v pořadí chunků**. Sčítání floatů není asociativní, takže tohle je jediný
  způsob, jak dostat bitově stejný výsledek na 1 i 32 vláknech.

Naivní `#pragma omp parallel for reduction(+:x)` nad floaty tenhle invariant
porušuje a tiše produkuje výsledky závislé na počtu vláken.

Ze stejného důvodu je generátor bodů **hashovaný z indexu**, ne sekvenční RNG:
bod *i* závisí jen na *i* a seedu.

Thread pool: vlákno, které čeká na dokončení dávky, **zároveň vykonává čekající
úkoly**. Proto vnořený `parallelFor` uvnitř paralelně cookované větve nemůže
uváznout.

> **Známý nedostatek:** pool má jednu sdílenou frontu pod mutexem, bez
> work-stealingu a per-thread deque. Na jemných úkolech to znamená kontenci.
> Produkce použije **TBB** (standard VFX platformy); tenhle pool existuje proto,
> aby prototyp neměl žádné externí závislosti.

---

## 7. Volba závislostí

Rozhodující kritérium je licence: **Apache 2.0 / BSD / MIT ano, GPL ne** —
studia musí smět psát proprietární uzly.

| Oblast | Volba | Licence |
|---|---|---|
| Řídké objemy | OpenVDB / NanoVDB | MPL 2.0 |
| Subdivize | OpenSubdiv | Apache 2.0 |
| Booleany | **Manifold** | Apache 2.0 |
| BVH, raycast | Embree | Apache 2.0 |
| Scéna, viewport, render | **OpenUSD + Hydra** | Apache 2.0 (mod.) |
| Obrázky, barvy | OpenImageIO, OpenColorIO, OpenEXR | BSD / Apache 2.0 |
| Materiály | MaterialX, OSL | Apache 2.0 / BSD |
| Threading | Intel TBB | Apache 2.0 |
| Rigid body | **Jolt Physics** (krok 2, hotovo: [docs/destruction.md](docs/destruction.md)) | MIT |
| Python vazby | **pybind11** (krok 3, hotovo: [docs/python.md](docs/python.md)) | BSD |
| JIT | LLVM ORC | Apache 2.0 + LLVM ex. |
| GUI | Qt 6 | LGPL (dynamicky linkovat) |

**CGAL se nepoužije** — je GPL/komerční. Proto Manifold na booleany.

**OpenUSD + Hydra** stojí za zdůraznění: dává viewport (Storm), napojení na
libovolný produkční renderer přes render delegates, a hlavně okamžitou
interoperabilitu se studii. Je to největší jednotlivá úspora práce v celém
projektu.

---

## 8. Rozvržení zdrojů

```
src/pg/core/     Types      vektory, matice, typy atributů
                 Attribute  AttributeArray (COW), AttributeSet
                 Geometry   kontejner, topologie, skupiny, objemy, hash
                 Parallel   deterministické chunkování, thread pool
                 Node       uzel, parametry, verzování, registry
                 Graph      vlastnictví uzlů
                 CookEngine pull evaluace, LRU cache
                 Spatial    k-d strom bodů (deterministické pořadí), sousedé přes hrany
                            (Adjacency: dva průchody do polí, bez vektoru na bod)
                 Selection  vzory prvků: čísla, rozsahy, skupiny, hrany (p3-4), * a ^
                 Soft       měkký výběr: podíl pohybu bodů kolem vybraného, vzdálenost
                            přímo nebo po povrchu (přes hrany), tvary útlumu
                 Sculpt     kapky štětce (push/pull, smooth, grab, flatten) na bodech:
                            mřížka, která jde s body, uhlazování s okraji; Sculptor
                            počítá tah přírůstkově, do bitu jako od začátku
                 Pick       co je pod myší, v obdélníku, lasu, tahu štětce: BVH nad
                            polygony, jen viditelné nebo i skryté; refit, když se body
                            jen posunou
src/pg/lang/     Parse      lexer a parser do AST, kontrola jmen a počtu argumentů
                 Check      typová kontrola proti geometrii: vazby atributů, přetížení
                 Eval       interpret typovaného stromu
                 Run        běh nad prvky: paralelně po kusech, nebo v pořadí s odloženými
                            změnami geometrie; výrazy parametrů (Expression)
                 Builtins   matematika, šum, matice, řetězce, pole; BuiltinsGeo: geometrie
src/pg/nodes/    Generators grid, line, pointcloud
                 Primitives box, sphere, tube (uzavřené, stěny ven)
                 Modifiers  transform, merge, switch, null,
                            attribcreate, groupbox, blast
                 Edit       groupcreate, edit (s měkkým poloměrem), attribpaint, sculpt:
                            co udělá výběr, úchyt a štětec ve viewportu
                 Surface    file (OBJ), scatter, normal, copytopoints, color
                 Wrangle    uzly pointwrangle a attribwrangle (body, primitivy, rohy, detail)
                 Topology   connectivity, fuse, polyextrude, subdivide (Catmull-Clark),
                            clip s uzavřením řezu, attribtransfer; nové body jako váhy starých
                 Rebuild    nové prvky z vah starých (Blends): atributy, skupiny, rebuild
                 Fracture   voronoifracture: buňky bodů jako řezy s víčky
                 Concrete   concretefracture: nestejné buňky, hrubé lomy stejné z obou
                            stran trhliny, odprýsklé rohy, rovný řez v atributu proxy
                 Cluster    rbdcluster: kusy do ker (k-means++, Lloyd), lepidlo uvnitř pevnější
                 Rebar      rebar: pruty do bloku natočeného podle největší stěny a rotujících
                            třmenů — síť ve zdi, armokoš s třmínky v trámu; width průměr
                 Glass      glassfracture: tabule jako sklo — radiální a soustředné trhliny
                            (výseče, rozvětvení), buňky vyříznuté rovinami kolmými na tabuli;
                            glass 1 plochy, 2 trhliny, Cd barva skla
                 Bricks     brickwall: zeď z cihel ve vazbě (běhounová, anglická, vlámská,
                            stack), vrstvy podél úseků uvnitř vstupu (otvory), každá cihla
                            s maltou a omítkou jeden kus; rozlomené cihly jako dvě poloviny
                            jedné kry (cluster, clusterglue)
src/pg/io/       Obj        čtení a zápis OBJ (body, polygony, čáry)
                 Ply        body s atributy a polygony do PLY a zpátky (ASCII i binárně)
                 Vdb        objemy do OpenVDB bez knihovny: řídký strom 5-4-3, soubor verze 224
                 Export     geometrie podle přípony (.ply, .obj, .vdb), sekvence ($F4)
                 Jpeg       baseline JPEG: YCbCr 4:2:0, tabulky normy, AAN DCT
                 Picture    obrázky dovnitř bez knihoven: PNG (Png), JPEG baseline
                            i progresivní jako libjpeg (JpegDecode), OpenEXR všech
                            kompresí kromě DWA (ExrRead); deflate (Inflate); sekvence
                            snímků (####, $F4, %04d); zápis PNG, JPEG a EXR
                 Video      video po snímcích: AVI s Motion JPEG sám, .mp4/.webm/.gif rourou do ffmpeg
src/pg/usd/      Layer      vrstva USD: hodnoty, specy primů a vlastností, list edits, varianty;
                            soubor i soubor v balíčku .usdz
                 Text       parser .usda
                 Crate      čtečka .usdc: LZ4, celočíselné kódování USD, tabulky, hodnoty všech typů
                 Stage      skládání scény (indexy primů, síla názorů LIVRPS), hodnoty v čase,
                            interpolace, value clips
                 Geom       transformace z xformOps, jednotky a osa, geometrie a kamery ze scény
src/pg/shader/   Types      typy shader grafu a jejich převody
                 NodeLibrary definice uzlů z textu (builtin.pgnodes)
                 ShaderGraph instance uzlů, spoje, formát .pgsg
                 Generator  graf → příkazy, typy, mrtvý kód
                 Target     GLSL 330, GLSL ES 300, Vulkan, HLSL; registr
src/pg/sim/      Grid       hustá 3D mřížka hodnot, trilineární vzorkování
                 SparseGrid řídká mřížka: dlaždice 8 × 8 × 8, uložené jen ty aktivní;
                            stěny MAC s první vrstvou další dlaždice
                 Poisson    tlaková rovnice: geometrický multigrid se stěnami a překážkami,
                            na aktivních dlaždicích (mimo ně p = 0)
                 Shape      tvary umístěné ve světě: poloha, rotace, velikost; uvnitř,
                            vzdálenost, průsečík s paprskem
                 Mesh       modely z OBJ: pole vzdáleností (SDF), paprsky, cache souborů
                 Scene      co se simuluje: doména, zdroje, síly, překážky, objekty
                 Pyro       simulace kouře a ohně, řídce: jen dlaždice s plynem a kolem
                 Liquid     voda: FLIP -- částice nesou vodu, mřížka drží její objem
                 FreeSurface tlak kapaliny s volnou hladinou: CG s multigridem, ghost fluid
                 Rain       déšť: kapky z mraku ve větru, odstřiky, vlnky na hladině
                 Rigid      tuhá tělesa nad Jolt: tělesa a dotyky kusů (rigidLayout), lepidlo
                            jako síť vazeb (rigidGlue, rigidNetwork), výztuž, drť jako
                            částice (narážejí, leží, jedou s kusy), prach a jeho stopy,
                            Guide jako póza každého kusu (rigidGuide), ke které solver
                            vede slepená tělesa; Jolt na vláknech, nárazy seřazené
                 Cloth      látky, lana a měkká tělesa (XPBD, malé kroky): vazby délky,
                            smyku a ohybu, objem balonů, přišpendlené body, kolize
                            s objekty, kusy RBD (obousměrně) i sebou samou, vzduch,
                            trhání dělením bodů; vazby v barvách
                            na vláknech, bitově stejně na 1 i 4 vláknech
                 Camera     kamera záběru: poloha, rotace, objektiv, rozlišení
                 Shared     co řešiče sdílejí: paralelní smyčky, šum, vítr v nárazech, síly na MAC mřížce
                 World      všechny řešiče sítě jednou snímkovou frekvencí, snímek po snímku;
                            checkpoint (saveState/loadState: plyn, voda, déšť a látky se načtou,
                            tělesa se spočítají znovu) a náhled na hrubších mřížkách
                 State      stav řešiče jako bajty a zpět; čtení hlídá každou délku
                 Network    síť uzlů simulace i geometrie, formát .pgsim, klíčové snímky,
                            překlad na World + Look (snímek po snímku, když je co animovat)
                 GeometryGraph  geometrické uzly sítě jako graf jádra: synchronizace,
                            inkrementální vaření, simulace zpátky jako body a objemy,
                            síť vazeb z kusů (RBD Constraints)
                 Cooker     geometrie vařená na vlastním vlákně: požadavek (síť, snímek, uzly),
                            přerušení rozpracovaného, úrovně assetů se vstupy instance
                 ForEach    smyčky For-Each: kusy, primitivy, body, počet, zpětná vazba;
                            tělo smyčky jako vlastní síť ve vlastním GeometryGraph
                 Asset      digital assets: knihovna definic s verzemi, typ instance
                            z definice, instance vařená ve vlastním GeometryGraph,
                            asset z vybraných uzlů (collapseToAsset)
                 Display    geometrie pro viewport: trojúhelníky s barvou, tečky, čáry
                 Frame      snímek: plyn v poloviční přesnosti, hladina vody po bajtech, kapky
                 Cache      snímky na disku: složka, cache.txt, .pgframe s běhy nul; hash sítě
src/pg/gl/       Gl, Camera, Png, HeadlessContext — OpenGL bez závislostí
                 Preview    náhled shaderu na tělese
                 Volume     objemové vykreslování simulace: podlaha, objekty, voda, déšť,
                            zobrazená geometrie, vodítka; sklo jako dvě odloupnuté vrstvy
                            přivrácených ploch (depth peeling), složené s plynem na paprsku
tests/           59 testů jádra (invarianty, SOP uzly) + 27 pro jazyk a výrazy + 7 pro
                 digital assets + 10 pro topologii, smyčky a vaření na pozadí + 25 pro shader graf + 86 pro simulaci, vodu, déšť, objekty,
                 modely, geometrii v síti a animaci + 11 pro cache a export + 11 pro checkpointy,
                 náhled a profil + 5 pro JPEG a video
bench/           měření tvrzení, o která se architektura opírá
cli/             headless demo, export OBJ
tools/prototype/  prototype — editor se dvěma sítěmi, simulací (výchozí) a shadery,
                 na společném plátně uzlů; viewport s výběrem a gizmem
                 (SimViewport, Gizmo), zobrazená geometrie a tabulka atributů
                 (SimGeometry); úpravy zobrazené geometrie -- body, hrany a plochy
                 myší, úchyt, skupina, mazání, štětec jako uzly sítě (SimElements);
                 digital assets: vstup dovnitř a zpět, Make Asset,
                 promote (SimAssets); cache na disk a export (SimRunner: snímky v paměti
                 i čtené z disku podle potřeby, menu); bake na pozadí jako vlastní proces
                 s průběhem, zrušením a pokračováním z checkpointu (Bake); wedge -- varianty
                 parametru, bake po bake (Wedge); profil kroku v přehledu; render
                 sekvencí a videa po snímcích s průběhem (RenderJob); kontext bez okna
                 pro příkazy (Offscreen: EGL, jinak skryté okno GLFW);
                 příkazy list/gen/check/render/sim (sim --cache/--from-cache/--export, video,
                 --checkpoint/--resume/--preview)
                 a cook (geometrie bez simulace do souboru, hash pro determinismus)
examples/        grafy shaderů, ukázková uživatelská knihovna, sítě simulace, assety (assets/)
```

Shader graf je popsaný zvlášť v [docs/shader-graph.md](docs/shader-graph.md),
simulace kouře a ohně v [docs/pyro.md](docs/pyro.md), geometrie v síti
editoru (uzly jako SOP, display flag, tabulka atributů, geometrie jako tvar
simulací) v [docs/geometry.md](docs/geometry.md), klíčové snímky a
pohyblivé překážky v [docs/animation.md](docs/animation.md), digital assets
v [docs/assets.md](docs/assets.md), cache na disku
a export do PLY, OpenVDB a OBJ v [docs/cache.md](docs/cache.md), obrázky
a video v [docs/render.md](docs/render.md).

Jmenný prostor `pg` zůstává i po přejmenování projektu na Prototype.

---

## 9. Co prototyp skutečně umí

Prototyp existuje, aby **ověřil invarianty měřením**, ne aby byl produktem.

### Hotovo a otestováno

| | |
|---|---|
| ✅ | COW atributy, topologie i skupiny — s testy na identitu bufferů |
| ✅ | Čtyři třídy atributů, skupiny, string tabulka, `gather` |
| ✅ | Cook engine: pull evaluace, verzování, LRU cache s rozpočtem |
| ✅ | Časová závislost včetně tranzitivního šíření |
| ✅ | Detekce cyklů při zapojování |
| ✅ | Deterministický `parallelFor` / `parallelReduce`, thread pool |
| ✅ | Wrangle jazyk: proměnné, řízení toku, funkce, pole, řetězce, matice a kvaterniony; běh nad body, primitivy, rohy i detailem; čtení libovolných prvků a vstupů, hledání sousedů (k-d strom), tvorba a mazání geometrie; parametry z `ch()`; výsledek nezávislý na počtu vláken |
| ✅ | 28 typů uzlů (box, sphere, tube, scatter, copy to points, file, polyextrude, subdivide, clip, objem na polygony…), obsahový hash, čtení i zápis OBJ, headless CLI |
| ✅ | Objemy v geometrii (husté mřížky hodnot, COW) |
| ✅ | 271 testů · čisté pod ASan, UBSan i **ThreadSanitizerem** |
| ✅ | Shader graf: uzly z textu, 4 cíle, editor; každý uzel ověřený glslangem a spirv-val |
| ✅ | Simulace kouře a ohně z uzlů: zdroje, síly, překážky; MAC mřížka, multigrid, bitově stejná na 1 i 4 vláknech; editor a `prototype sim` |
| ✅ | Voda (FLIP): tlak s volnou hladinou (CG s multigridem, ghost fluid, stěny zakryté tělesy), bitově stejná na 1 i 4 vláknech; hladina s odrazy a lomem |
| ✅ | Déšť a vítr: kapky z mraku, nárazy větru putující s větrem, odstřiky od objektů, vlnky na vodě (vlnová rovnice), mokrá podlaha; bitově stejné na 1 i 4 vláknech |
| ✅ | Kamera záběru: pohled kamerou v editoru s rámečkem obrazu, kamera z pohledu, render a sekvence kamerou (editor i `prototype sim`) |
| ✅ | Geometrie v síti editoru: SOP uzly vařené jádrem inkrementálně, display flag, viewport, **geometry spreadsheet**; geometrie jako tvar překážek a zdrojů, simulace zpátky jako body a objemy |
| ✅ | Výrazy v parametrech (`$F`, `$T`, `ch("../uzel/parametr")`) se sledováním závislostí a detekcí smyček |
| ✅ | Digital assets: knihovna `.pgasset` s verzemi, instance vařené ve vlastním grafu, promotované parametry, definice nesené v souboru sítě, odmítnuté cykly; v editoru Make Asset, vstup dovnitř a zpět |
| ✅ | Smyčky For-Each (kusy, primitivy, body, počet, zpětná vazba) a uzly topologie: PolyExtrude, Subdivide, Clip s uzavřením řezu (i nekonvexního), Fuse, Connectivity, Attribute Transfer |
| ✅ | Animace: klíče na libovolném parametru (Smooth/Linear/Step), síť snímek po snímku, pohyblivé překážky s rychlostí i rotací v okrajových podmínkách plynu i vody, animované parametry geometrie jako výrazy jádra |
| ✅ | Cache simulace na disku (editor i `prototype sim`), export geometrie snímek po snímku: PLY s atributy, **OpenVDB** (ověřeno čtením v OpenVDB 10: voxely i součty sedí s mřížkou simulace), OBJ |
| ✅ | Destrukce: Voronoi Fracture, tuhá tělesa nad Jolt, slepené kusy jako jedno těleso, nálože, drcení na prach, drť, vytlačený vzduch žene prach ([docs/destruction.md](docs/destruction.md)) |
| ✅ | Beton: Concrete Fracture — nestejné kusy, nejmenší kolem nárazu, odprýsklé rohy, hrubé lomy lícující z obou stran; RBD Solver simuluje rovný řez (`proxy`) a kreslí detail; `spread` a `rings` drží škodu u místa nárazu, kusy přilepené k základu stojí; sekundární lámání: RBD Cluster seskupí kusy do ker, které se rozpadnou až při tvrdém dopadu ([docs/destruction.md §2](docs/destruction.md#2-concrete-fracture)) |
| ✅ | Výztuž: uzel Rebar (síť ve zdi, armokoš s třmínky v trámu, natočené podle bloku); RBD Solver drží kusy na prutech plastickými vazbami Joltu (tření v šesti směrech), prut povolí podle oceli nebo kotvení, vytahuje se, ohýbá a trhá; stav prutů ve snímku a cache (verze 6), kreslení trubkami, USD `/World/rebar` ([docs/destruction.md §2](docs/destruction.md#výztuž-rebar)) |
| ✅ | Sklo: Glass Fracture (radiální a soustředné trhliny kolem místa úderu); tabule je celá, dokud jí nepraskne spoj; skleněná drť a desetina prachu (cache verze 7); renderer kreslí sklo průhledné — dvě vrstvy, Fresnel obou stěn, odraz oblohy a slunce, zabarvení podle cesty sklem; USD materiál skla a trhliny viditelné od prasknutí ([docs/destruction.md §2](docs/destruction.md#sklo-glass-fracture)) |
| ✅ | Síť vazeb jako geometrie: RBD Constraints udělá z kusů bod na těleso a čáru na spoj (`strength` jako násobek Glue, `area`, barva podle pevnosti); zeslabená, smazaná nebo nakreslená síť zapojená do Constraints RBD Solveru je lepidlem; RBD Pieces vrátí síť snímku s `broken` a `time`, stav spojů ve snímku a cache (verze 8) ([docs/destruction.md §3](docs/destruction.md#síť-vazeb-rbd-constraints)) |
| ✅ | Cihly: Brick Wall vyzdí zeď z cihel ve vazbě (běhounová, anglická, vlámská, stack) s maltou, omítkou a otvory s rovným ostěním; každá cihla jeden kus, rozlomené cihly jako dvě poloviny jedné kry; RBD Solver má maltu jako lepidlo, zeď se rozpadá ve spárách; příklady `brick_wall` (koule proti cihlové zdi s oknem) a `concrete_column` (odstřel železobetonového sloupu, holý armokoš) ([docs/destruction.md §2](docs/destruction.md#cihly-brick-wall)) |
| ✅ | Drť jako částice: vylétá z okraje plochy prasklého spoje, vzduch ji brzdí, točí se, naráží do kusů, překážek i podlahy (paprsky v Joltu), zůstává ležet a jede s kusem, na kterém leží; stopy prachu za utrženými kusy (`trail`); natočení (`orient`) ve snímcích, cache verze 9, RBD Pieces, Pythonu a USD, Copy to Points podle něj natáčí ([docs/destruction.md §3](docs/destruction.md#drť-jako-částice)) |
| ✅ | Tuhá tělesa na vláknech: Jolt na vlastním poolu, nárazy z jeho vláken seřazené, drť na vláknech; bitově stejné snímky na 1 i 4 vláknech; Voronoi Fracture řeže buňku jen z blízkých částí blízkými body (bitově stejně, věž z 5 628 buněk 13× rychleji); `pgbench_rigid` ([docs/destruction.md §3](docs/destruction.md#jak-to-funguje)) |
| ✅ | **Řídký plyn**: Pyro Solver počítá a drží jen dlaždice 8 × 8 × 8 buněk, kde je plyn, a kolem, kam za krok doletí; tlak multigridem jen na nich; se všemi dlaždicemi bitově stejně jako hustá mřížka; snímky a cache (verze 10) jen s dlaždicemi s plynem; rozlišení až 1024; prach odstřelu ve 103,5 M voxelů za 19 minut ([docs/pyro.md §4](docs/pyro.md#řídká-mřížka-počítá-se-jen-tam-kde-je-plyn), `pgbench_pyro`) |
| ✅ | Usměrněná simulace: Guide RBD Solveru (kusy posunuté a natočené, třeba klíčovaný Transform kolem Pivotu) vede slepená tělesa do pózy, která jejich body nejlépe položí na body Guide; síla, doba, dosah a puštění při prasknutí lepidla; atribut `guide`; z Guide se v každém snímku bere jen póza kusu; příklad `guided_fall` ([docs/destruction.md §3](docs/destruction.md#usměrněná-simulace-guide)) |
| ✅ | Povrch vody jako uzavřená síť s rychlostí a pěnou (surface nets, uzel Liquid Surface) a objem na polygony (Convert Volume), bitově stejné na 1 i 4 vláknech |
| ✅ | Celý záběr do **USD** bez knihovny: tělesa jako transformace, drť, povrch vody, déšť, prach jako VDB, kamera, světla; co se mění, v souboru pro každý snímek (value clips); ověřeno Pixarovou knihovnou, 28 validátorů bez nálezu ([docs/usd.md](docs/usd.md)) |
| ✅ | **Čtení USD** bez knihovny: `.usda`, `.usdc` (verze 0.4.0–0.10.0), `.usdz`; scéna složená jako v USD (sublayers, reference, payloady, varianty, třídy, value clips); kamera z matchmove (USD Camera) a geometrie (USD Import) v metrech s Y nahoru; transformace, skládání i geometrie sedí s knihovnou USD ([docs/usd-import.md](docs/usd-import.md)) |
| ✅ | **Python API** `import pg` (pybind11): sítě, parametry, výrazy a klíče, vaření a simulace ze skriptu; atributy a data snímků jako pole numpy bez kopie (buffer protocol nad sdílenou pamětí jádra); cache, USD, render přes `prototype`; síť jako Python (`as_code()`) ([docs/python.md](docs/python.md)) |
| ✅ | Render do **EXR** bez knihovny: lineární světlo, hloubka, vektory pohybu, masky; čte ho OpenEXR 3.5 ([docs/render.md](docs/render.md)) |
| ✅ | **Plate**: obraz záběru (PNG, JPEG, EXR, čtené bez knihoven, JPEG bit po bitu jako libjpeg) za CG kamerou záběru; holdout a shadow catcher (objekty i podlaha); do EXR CG s alfou a průchod `catcher`; kde CG nic nemění, vyjde plate pixel po pixelu ([docs/plate.md](docs/plate.md)) |
| ✅ | **Látky, lana a měkká tělesa** (XPBD, obdoba Vellum): Cloth Solver z polygonů, polyčar a uzavřených sítí s tlakem; přišpendlené body nesené animací; kolize s podlahou, objekty, kusy RBD i sebou samou, tření; vzduch, vítr a proud plynu na plochách; snímky a cache (verze 11, roztržená látka verze 12), checkpoint, Cloth Geometry; trhání (Tear, atribut `tear`); kusy RBD jako tělesa v kroku látky, látka je brzdí a nese; příklady `tablecloth`, `flag` a `tarp` ([docs/cloth.md](docs/cloth.md)) |
| ✅ | Video: AVI s Motion JPEG bez závislostí (vlastní kodér JPEG), MP4/MOV/MKV (H.264), WebM (VP9) a GIF přes ffmpeg; v editoru render na pozadí s průběhem, z příkazové řádky `sim OUT.mp4` a `render OUT.mp4`; ověřeno dekódováním v ffmpeg |

### Změřeno (4 jádra, g++ 13.3, RelWithDebInfo)

| Tvrzení | Měření |
|---|---|
| COW: 50 uzlů, 2M bodů, 5 atributů | **50 alokací** místo 250; 3.7× méně paměti |
| Editace uprostřed 100-uzlového řetězce | **48 %** času studeného cooku, 51 ze 101 uzlů |
| Recook beze změny | **0.025 ms**, 0 uzlů |
| Škálování na 4 vláknech | **2.99×**, hash bitově identický na 1/2/4 vláknech |
| Interpret jazyka, aritmetika | 38 Mbodů/s (1M bodů za 26 ms) — první, jednoduchý jazyk |
| Interpret jazyka, noise | 63 Mbodů/s — první, jednoduchý jazyk |
| Wrangle v2 proti prvnímu jazyku, týž stroj (4 jádra) | aritmetika 26,6 proti 29,0 Mbodů/s, noise 38 proti 50 Mbodů/s |
| 240 snímků s cache 256 MB | 1.9 ms/snímek, zdroj cooknut **1×** |
| Tuhá tělesa, věž odstřelu (593 kusů, 710 těles), `pgbench_rigid` | 5,3 ms/snímek na 1 vláknu, **3,2 ms** na 4; snímky bitově stejné |
| … desetkrát víc kusů (5 628) | 78 ms/snímek na 1 vláknu, **31 ms** na 4; řez Voronoi 1,0 s (dřív 13,5 s) |
| Prach odstřelu, rozlišení 96 (0,5 M voxelů), `pgbench_pyro` | hustě 249 ms/snímek, **řídce 55 ms** (hustý řešič dřív 164 ms); 119 → 75 MB |
| … rozlišení 576 (103,5 M voxelů), 180 snímků | **6,2 s/snímek** i s tuhými tělesy, 19 minut; nejvýš 3,4 GB; počítá se nejvýš 23 % domény |

### Není v prototypu (vědomě)

I/O (materiály a PointInstancer z USD, Alembic; VDB jen zápis hustých mřížek) · JIT · packed primitives a out-of-core ·
Python uvnitř sítě (Python SOP) · booleany, geometrické dotazy (xyzdist, primuv) · simulace
látek · řídká voda a simulace na GPU

---

## 10. Přehled známých zjednodušení

Souhrn toho, co je v textu rozeseté — každá položka je vědomá, ne přehlédnutá:

1. **Účtování paměti cache** je horní odhad (nepočítá sdílení mezi položkami).
2. **Duplicitní cook** uzlu dosažitelného přes dvě paralelně cookované větve.
3. **Thread pool** má jednu frontu pod mutexem, bez work-stealingu → TBB.
4. **Hash** je sériový FNV-1a po bajtech → xxHash3 paralelně.
5. **Jazyk** je interpret typovaného stromu, ne JIT; program, který mění
   geometrii nebo píše řetězce, běží na jednom vlákně.
6. **Bez array a matice atributů.** Jazyk pole i matice má, jen jako
   lokální proměnné.
7. **`gather` nekomprimuje string tabulku** — po velkém mazání v ní zůstávají
   nepoužité položky.
8. **Detekce cyklů** je O(V) na každé zapojení; u velmi velkých grafů bude
   potřeba inkrementální varianta.
