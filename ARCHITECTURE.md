# Architektura

> Stav: **návrh v1 + ověřovací prototyp** · Poslední aktualizace: 2026-09-21
>
> Dokument popisuje cílovou architekturu. Část je už postavená a změřená
> (viz [§9](#9-co-prototyp-skutečně-umí)), zbytek je návrh. U každé kapitoly je
> uvedeno, co platí dnes a co je plán.

Související dokumenty: [ROADMAP.md](ROADMAP.md) — fáze, milníky a rozhodovací brány.

---

## 1. Tvar systému

```
┌─────────────────────────────────────────────────────────────┐
│  GUI (Qt)   node editor · viewport · parametry · spreadsheet │   fáze 3
├─────────────────────────────────────────────────────────────┤
│  Python API (nanobind)          CLI / headless cook          │   M4
├─────────────────────────────────────────────────────────────┤
│  Uzly: generátory · modifikátory · I/O · digital assets      │   M3, M6, M12
├─────────────────────────────────────────────────────────────┤
│  Jazyk: parser → IR → LLVM ORC JIT → SIMD kernely            │   M5
├─────────────────────────────────────────────────────────────┤
│  JÁDRO   cook engine · geometrie · atributy · paralelismus   │   M1, M2
├─────────────────────────────────────────────────────────────┤
│  Externí: OpenVDB · USD/Hydra · Embree · OpenSubdiv · OIIO   │   M3, M6, M8
└─────────────────────────────────────────────────────────────┘
```

Zásadní vlastnost tohohle uspořádání: **GUI je klient knihovny, ne naopak.**
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

LRU s tvrdým paměťovým stropem. Položka větší než celý rozpočet se stejně vrátí
(jinak by velký výsledek nešel nikdy dodat).

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

---

## 5. Per-element jazyk

Pipeline, kterou se sem míří:

```
zdroj → lexer → parser → typová inference → vlastní IR → LLVM ORC JIT → SIMD kernel
                                                             ↑
                                             sem se připojuje M5
```

**Dnes** je místo posledních dvou kroků strom procházející interpret. Švem,
kde JIT nahradí interpret, je `Program::run()`:

1. **Typová inference** — typy z existujících atributů, výraz se odvodí zdola
   nahoru, přiřazení určuje typ vytvářeného atributu. `@v = @P * 2.0` vytvoří
   `vec3`, `@d = length(@P)` vytvoří `float`.
2. **Vazba na sloty** — každé `@jméno` dostane při parsování číslo slotu.
   Před během se sloty **jednou** naváží na ukazatele do atributových polí.
   Per-point se tedy nevyhledává podle jména.
3. **Běh po deterministických chuncích** — jazyk sahá jen na aktuální bod,
   takže chunky píšou do disjunktních rozsahů a není co synchronizovat.

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
| Rigid body (po 1.0) | Jolt / PhysX 5 | MIT / BSD-3 |
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
                 Geometry   kontejner, topologie, skupiny, hash
                 Parallel   deterministické chunkování, thread pool
                 Node       uzel, parametry, verzování, registry
                 Graph      vlastnictví uzlů
                 CookEngine pull evaluace, LRU cache
src/pg/nodes/    Generators grid, line, pointcloud
                 Modifiers  transform, merge, switch, null,
                            attribcreate, groupbox, blast
                 Expression per-element jazyk
                 Wrangle    uzel pointwrangle
src/pg/shader/   Types      typy shader grafu a jejich převody
                 NodeLibrary definice uzlů z textu (builtin.pgnodes)
                 ShaderGraph instance uzlů, spoje, formát .pgsg
                 Generator  graf → příkazy, typy, mrtvý kód
                 Target     GLSL 330, GLSL ES 300, Vulkan, HLSL; registr
src/pg/gl/       Gl, Preview, Png, HeadlessContext — náhled přes OpenGL
tests/           50 testů proti invariantům + 21 pro shader graf
bench/           měření tvrzení, o která se architektura opírá
cli/             headless demo, export OBJ; pgshader
tools/           pgshadered — node editor shaderů (Dear ImGui, imnodes)
examples/        grafy shaderů a ukázková uživatelská knihovna
```

Shader graf je popsaný zvlášť v [docs/shader-graph.md](docs/shader-graph.md).

Jmenný prostor `pg` je placeholder — jméno je výstup fáze 0 roadmapy.

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
| ✅ | Per-element jazyk: parser, typová inference, vazba na sloty |
| ✅ | 10 typů uzlů, obsahový hash, export OBJ, headless CLI |
| ✅ | 50 testů · čisté pod ASan, UBSan i **ThreadSanitizerem** |
| ✅ | Shader graf: uzly z textu, 4 cíle, editor; každý uzel ověřený glslangem a spirv-val |

### Změřeno (4 jádra, g++ 13.3, RelWithDebInfo)

| Tvrzení | Měření |
|---|---|
| COW: 50 uzlů, 2M bodů, 5 atributů | **50 alokací** místo 250; 3.7× méně paměti |
| Editace uprostřed 100-uzlového řetězce | **48 %** času studeného cooku, 51 ze 101 uzlů |
| Recook beze změny | **0.025 ms**, 0 uzlů |
| Škálování na 4 vláknech | **2.99×**, hash bitově identický na 1/2/4 vláknech |
| Interpret jazyka, aritmetika | 38 Mbodů/s (1M bodů za 26 ms) |
| Interpret jazyka, noise | 63 Mbodů/s |
| 240 snímků s cache 256 MB | 1.9 ms/snímek, zdroj cooknut **1×** |

### Není v prototypu (vědomě)

I/O (USD, Alembic, VDB) · JIT · packed primitives a out-of-core · digital
assets · serializace scény a migrace verzí · Python vazby · GUI pro geometrii ·
booleany, subdivize, geometrické dotazy · simulace

---

## 10. Přehled známých zjednodušení

Souhrn toho, co je v textu rozeseté — každá položka je vědomá, ne přehlédnutá:

1. **Účtování paměti cache** je horní odhad (nepočítá sdílení mezi položkami).
2. **Duplicitní cook** uzlu dosažitelného přes dvě paralelně cookované větve.
3. **Thread pool** má jednu frontu pod mutexem, bez work-stealingu → TBB.
4. **Hash** je sériový FNV-1a po bajtech → xxHash3 paralelně.
5. **Jazyk** je interpret, bez řídicích struktur, lokálních proměnných a
   zápisu do int atributů.
6. **Bez array a matice atributů.**
7. **`gather` nekomprimuje string tabulku** — po velkém mazání v ní zůstávají
   nepoužité položky.
8. **Detekce cyklů** je O(V) na každé zapojení; u velmi velkých grafů bude
   potřeba inkrementální varianta.
