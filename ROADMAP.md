# Roadmapa

> Stav dokumentu: **návrh v1** · Poslední aktualizace: 2026-09-21
>
> Tohle je živý dokument. Každá fáze má měřitelná kritéria hotovosti a každá
> etapa končí rozhodovací branou, kde se projekt může otočit nebo zastavit.
>
> Související: [ARCHITECTURE.md](ARCHITECTURE.md) — datový model, cook engine
> a to, co z toho už ověřuje prototyp v `src/`.

---

## 1. Cíl

Postavit open-source **procedurální systém pro zpracování geometrie** — node-based,
nedestruktivní, škálující na produkční data, ovladatelný z Pythonu i z příkazové řádky.

Inspirace v Houdini je koncepční. Implementace je **clean room**: žádný HDK,
žádný reverse engineering, žádné přebírání dokumentace.

### Co stavíme

| | |
|---|---|
| Jádro | Líný závislostní cook engine + atributový geometrický model |
| Jazyk | JIT-kompilovaný per-element jazyk pro vlastní výpočty |
| Rozhraní | Headless knihovna + CLI **jako primární produkt**, GUI jako klient |
| Interop | USD, Alembic, OpenVDB, MaterialX — nativně, od začátku |

### Co vědomě NEstavíme (do 1.0)

- **Simulace** — FLIP, Pyro, Vellum, MPM, rigid bodies. Každý z nich je sám o sobě
  větší projekt než celé jádro. Viz [§9 Za horizontem](#9-za-horizontem-po-10).
- **Vlastní renderer** — od toho je Hydra a existující render delegates.
- **Compositing, editace zvuku, animační nástroje, modelovací nástroje** — jiné kontexty.
- **Kompletní paritu s Houdini.** Není to cíl a nikdy nebude.

Hodnota není v počtu nodů. Je v tom, že 30 dobře navržených nodů nad správným
datovým modelem pokryje většinu produkčních procedurálních úloh.

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

## 3. Fáze 0 — Rozhodnutí a základy

**Rozsah: týdny 1–8** · Cíl: uzavřít otázky, které se později mění draho.

### 0.1 Právní a identita

- [ ] **Zvolit jméno a ověřit ochranné známky.** Pracovní název `OpenHoudiny` je
      zaměnitelně podobný registrované známce SideFX. Přejmenovat teď = nic.
      Přejmenovat za tři roky = ztráta veškeré nabyté viditelnosti. **Blokující úkol.**
- [ ] Licence **Apache 2.0** (patentový grant, ASWF standard). Vyloučit GPL — studia
      musí smět psát proprietární nody.
- [ ] `DCO` nebo `CLA`, `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`.
- [ ] Písemně zaznamenat clean-room politiku pro přispěvatele.

### 0.2 Technické spike (časově omezené, výstup = ADR)

| Spike | Otázka | Limit |
|---|---|---|
| **S1** | Stavět nad **Gafferem** (BSD, už má vícevláknový deferred eval engine), nebo od nuly? | 3 týdny |
| **S2** | **C++20** vs **Rust** pro jádro | 1 týden |
| **S3** | Vlastní DSL + LLVM ORC JIT vs **NVIDIA Warp** (Apache 2.0) jako kernel vrstva | 2 týdny |
| **S4** | Rozsah nasazení **USD/Hydra** — jen I/O, nebo i jako scénový model? | 2 týdny |

Každý spike končí **ADR** (Architecture Decision Record) v `docs/adr/`. ADR obsahuje
zvolenou variantu, zamítnuté varianty a *důvod*. Zpětně měnitelné, ale ne mlčky.

> **S1 je nejdůležitější rozhodnutí celého projektu.** Gaffer už má velkou část
> toho, co by se psalo rok. Poctivě zvážit, jestli chybějící část (atributový
> geometrický model, wrangle jazyk) nejde dodat jako nadstavba.

### 0.3 Infrastruktura

- [ ] CMake (nebo Cargo) + správa závislostí
- [ ] CI: Linux / Windows / macOS, cíl **VFX Reference Platform CY2026**
      (Python 3.13, Qt 6.8, glibc 2.34 / EL9 baseline — přesná tabulka na vfxplatform.com)
- [ ] Sanitizery (ASan, UBSan, TSan) v CI od prvního commitu
- [ ] Benchmark harness — výkon je feature, měří se průběžně, ne na konci

**🚦 Brána A — konec fáze 0:** Je zvolené jméno, licence a odpověď na S1?
Bez toho se nepokračuje.

---

## 4. Fáze 1 — Jádro (headless)

**Rozsah: měsíce 2–8** · Cíl: použitelná knihovna bez jediného řádku GUI.

### M1 · Geometrický model

- Typované atributové pole: `int / float / vec2,3,4 / mat3,4 / string / array`
- Čtyři třídy atributů: `detail / point / vertex / primitive`
- Copy-on-write s refcountem na úrovni pole
- Topologie: primitive → vertex → point indirekce
- Grupy: point / prim / vertex / edge, bool i ordered
- String atributy jako indexy do sdílené tabulky
- Serializace + stabilní hash geometrie (pro testy)

**Hotovo, když:**
- 20 M bodů se načte a projde 50-nodovým řetězcem bez OOM na 32 GB stroji
- Node měnící jediný atribut alokuje **O(1)** nových polí, ne O(počet atributů)
- Benchmark tohle ověřuje automaticky v CI

> **Prototyp:** splněno v malém — 50 uzlů nad 2 M bodů alokuje 50 polí místo 250
> (`bench/bench_main.cpp`, blok 1). Zbývá ověřit na 20 M bodů a zapojit do CI.

### M2 · Cook engine

- Model nodu a parametru, DAG s validací cyklů
- **Pull evaluace** s rekurzivním vyžádáním vstupů
- **Dirty propagation** — změna parametru značí jen dotčený podstrom
- Cook cache: LRU + tvrdý paměťový rozpočet
- **Čas jako dimenze závislosti** — time-dependent nody se cachují per-frame
- Kontext evaluace (frame, iterace smyčky, instance)
- Paralelní vyhodnocení nezávislých větví (TBB)

**Hotovo, když:**
- Řetěz 100 nodů; změna parametru na nodu 50 přepočítá právě 50 nodů — ověřeno testem
- Cook je volatelný ze dvou vláken současně nad stejnou scénou bez race (TSan čistý)
- Cache respektuje rozpočet a nepřeteče ani při cooku 1000 snímků

> **Prototyp:** splněno. Editace uprostřed 100-uzlového řetězce stojí 48 % času
> studeného cooku, recook beze změny 0.025 ms, testy čisté pod TSan.

### M3 · Prvních 10 nodů + I/O

`file in` · `file out` · `null` · `switch` · `transform` · `merge` · `blast` ·
`group` · `attribute create` · `scatter`

- I/O: **Alembic**, **USD**, **OpenVDB**
- CLI: `<tool> cook scene.<ext> -n /out/render -f 1-240`

**Hotovo, když:** první end-to-end pipeline proběhne bez GUI a bez Pythonu.

### M4 · Python API

- Vazby přes `nanobind`
- Stavba a cook scény, čtení a zápis parametrů
- **Přístup k atributům jako zero-copy numpy views** — ne kopie
- Callbacky, introspekce typu nodu

**Hotovo, když:** celá scéna z M3 jde postavit, cooknout a přečíst čistě z Pythonu.

**🚦 Brána B — konec M2:** Splňuje cook engine benchmark? Pokud ne, je to
architektonický problém, ne optimalizační. Zastavit a přepracovat.

---

## 5. Fáze 2 — Jazyk a šířka nodů

**Rozsah: měsíce 8–14** · Cíl: nástroj, kterým jde vyjádřit vlastní myšlenka, ne jen
poskládat hotové kostky.

### M5 · Wrangle jazyk v1

- Lexer → parser → typechecker → vlastní IR → **LLVM ORC JIT**
- Kompilace na **SIMD batche** (8/16 elementů), ne skalárně
- Čtení a zápis atributů, tvorba a mazání elementů
- Vektorová matematika, noise, geometrické dotazy (`nearest point`, `raycast`, `xyzdist`)
- Chybové hlášky s pozicí ve zdroji — TD tráví v tomhle editoru hodiny denně

**Hotovo, když:**
- Aritmeticky vázaný snippet na 10 M bodech: **< 120 ms na 4 jádrech**
- Nejméně **8×** rychlejší než interpret prototypu na aritmeticky vázaném
  snippetu, měřeno blokem 3 v `bench/bench_main.cpp`
- Kompilační cache — druhý cook stejného snippetu nekompiluje znovu

> **Kritérium revidováno 2026-09-21 podle měření prototypu.** Původní znění
> („< 150 ms na 10 M bodech na 16 jádrech" a „50× proti interpretu") stálo na
> odhadu, že interpret zvládne řádově 1 Mbod/s. Skutečnost je **38 Mbodů/s**,
> takže původní absolutní cíl by splnil i interpret a původní násobek byl
> nedosažitelný. Přesně kvůli tomuhle se prototyp staví před plánem, ne po něm.

### M6 · Geometrické operace

Napojení knihoven: **Manifold** (booleany, Apache 2.0), **OpenSubdiv**, **Embree** (BVH a raycast).

Nové nody: `copy to points` · `for-each` (compile block) · `resample` · `polyextrude` ·
`subdivide` · `attribute promote / transfer / delete` · `sort` · `fuse` · `boolean` ·
`vdb from polygons` · `convert vdb` · `ray` · `measure` · `attribute wrangle`

**Hotovo, když:** demo „procedurální budova" — parametrizovaná scéna, která z několika
sliderů generuje geometrii — běží reprodukovatelně z CLI.

> **Prototyp:** v malém existuje — `pgbuilding` staví blok 32 budov shape gramatikou
> ([docs/shape-grammar.md](docs/shape-grammar.md)), reprodukovatelně z CLI, a změna
> jednoho pravidla přepočítá 5 z 25 uzlů. Tvary jsou ale jen kvádry (scope), ne
> obecné polygony — `polyextrude` ani booleany to nenahrazuje.

### M7 · Determinismus a regresní síť

- Deterministické paralelní redukce (žádný naivní `parallel_reduce` nad floaty)
- Stabilní řazení, fixní paralelní partitioning nezávislý na počtu vláken
- **Golden-file regresní suita** nad hashem atributů
- Fuzzing parseru wrangle jazyka

**Hotovo, když:**
- Bit-identický výstup při 1 vs. 32 vláknech
- Bit-identický výstup na Linux / Windows / macOS
- Regresní suita běží na každý PR

**🚦 Brána C — konec M7:** Máme headless nástroj, který dělá reálnou práci.
Tohle je první bod, kdy má smysl ukázat to ven a hledat první uživatele —
**ještě před GUI**. Pokud o to nikdo nejeví zájem, GUI to nezachrání.

---

## 6. Fáze 3 — Grafické rozhraní

**Rozsah: měsíce 14–22** · Qt 6.8 (LGPL, dynamické linkování) + PySide6.

| Milník | Obsah | Hotovo, když |
|---|---|---|
| **M8** · Viewport | **Hydra** + Storm; selekce, manipulátory, zobrazovací režimy | 20 M bodů se otáčí plynule (> 30 fps) |
| **M9** · Node editor | Graf, drag & drop, boxy, barvy, poznámky, level-of-detail vykreslování | 10 000 nodů v grafu bez zpomalení UI |
| **M10** · Parametry + **geometry spreadsheet** | Panel parametrů, výrazy, kanálové reference; tabulka všech atributů | Spreadsheet zvládne 20 M řádků (virtualizované) |
| **M11** · Session | Timeline, undo/redo, autosave, crash recovery, layouty | Pád neztratí víc než 2 minuty práce |

> **Geometry spreadsheet není „nice to have".** Bez něj je procedurální graf černá
> skříňka a uživatelé to vzdají. Patří do stejné úrovně priority jako viewport.

---

## 7. Fáze 4 — Pipeline a produkční nasazení

**Rozsah: měsíce 22–30** · Cíl: přechod z „funguje to" na „jde to nasadit".

### M12 · Digital assets

Zabalení podgrafu do parametrizovaného znovupoužitelného nodu.

- Promotované parametry a jejich UI
- Verzování assetu, namespace, závislosti
- Knihovna a vyhledávání assetů

> Datový model tohle musí předpokládat **od M1**. Dolepit to později jde velmi špatně.

### M13 · Kompatibilita scén

- Verze typu nodu + migrační funkce
- **Textový, diffovatelný formát scény** (studia scény verzují v gitu)
- Test: scéna uložená v M3 se musí načíst v 1.0

### M14 · Škálování na produkční data

- **Packed primitives** — primitive jako odkaz na geometrii + transformace
- Delayed load / streaming z disku
- Out-of-core cook, tvrdý paměťový strop
- Profilovací nástroj: kde se tráví čas a paměť v grafu

**Hotovo, když:** scéna s 10 000 instancemi se otevře a cookne v rozumné paměti.

### M15 · Farma a integrace

- Distribuovaný cook po snímcích
- Render submission přes Hydra delegates
- Balíčky pro `rez` / `spack`
- Stabilní ABI plugin API pro vlastní nody (C++ i Python)

---

## 8. Fáze 5 — Cesta k 1.0

**Rozsah: měsíce 30–36**

- [ ] Plná shoda s **VFX Reference Platform** aktuálního CY
- [ ] Dokumentace a tutoriály — u procedurálního nástroje je to ~30 % hodnoty produktu, ne dodatek
- [ ] Beta program: **minimálně 3 studia** na reálném projektu
- [ ] Bezpečnostní a stabilitní audit
- [ ] Instalační balíčky pro Linux / Windows / macOS
- [ ] Přihláška do **ASWF** — to je signál, který studiím dává důvěru
- [ ] Veřejný plán podpory verzí a politika breaking changes

---

## 9. Za horizontem (po 1.0)

Seřazeno podle poměru hodnota / náklad:

1. **XPBD solver** — cloth, měkká tělesa, granuláty. Dobře popsáno v literatuře, jeden solver pokryje široké spektrum.
2. **Rigid bodies** — napojení Jolt / PhysX 5, relativně levné.
3. **GPU backend** — kernel abstrakce navržená už ve fázi 2 se zaplatí až tady.
4. **FLIP / APIC kapaliny** nad VDB mřížkami.
5. **Pyro** — řídké mřížky, MacCormack advekce, multigrid tlakový solver. Nejdražší položka.
6. **Compositing kontext** (2D nody).

Každá z položek 1–5 je **vícečlověkoroční projekt**. Nezačínat žádný z nich před 1.0.

---

## 10. Rizika a rozhodovací brány

| # | Riziko | Dopad | Mitigace |
|---|---|---|---|
| R1 | Ochranná známka „Houdini" | Nucené přejmenování, ztráta viditelnosti | Vyřešit ve fázi 0, blokující |
| R2 | Cook engine nezvládne interaktivní rychlost | Fatální — nedá se opravit optimalizací | Brána B, benchmark od M2 |
| R3 | JIT jazyk se protáhne | Zdržení 6–12 měsíců | Fallback na Warp (S3), rozhodnuto předem |
| R4 | Nedeterminismus objevený pozdě | Studia nenasadí | M7 jako samostatný milník, ne úklid |
| R5 | Projekt zůstane „one man show" | Zánik při vyhoření | Brána C — hledat přispěvatele **před** GUI |
| R6 | Blender Geometry Nodes pokryje cílovou skupinu dřív | Ztráta smyslu projektu | Diferenciace: headless-first, out-of-core, Apache licence |
| R7 | Financování | Zastavení ve fázi 3–4 | Řešit od brány C, ne až když dojdou peníze |

### Brány

| Brána | Kdy | Otázka |
|---|---|---|
| **A** | Konec fáze 0 | Je jméno, licence a odpověď na S1 (Gaffer vs. od nuly)? |
| **B** | Konec M2 | Splňuje cook engine výkonnostní cíl? |
| **C** | Konec M7 | Používá headless verzi někdo kromě nás? Máme ≥ 3 externí přispěvatele? |
| **D** | Konec fáze 3 | Chce alespoň jedno studio beta-testovat na reálném projektu? |

Brána, u které se nikdy neodpoví „ne", není brána. Pokud odpověď na C nebo D je „ne",
správný krok je **pivot na knihovnu nebo plugin do existující aplikace**, ne pokračovat dál.

---

## 11. Předpoklady o zdrojích

Časové odhady výše předpokládají:

- **2–3 seniorní C++ vývojáře na plný úvazek** od fáze 1
- **+1 UI vývojáře** od fáze 3
- **+1 technical writera** od fáze 4

Při jednom člověku na částečný úvazek přenásob odhady **3× až 5×** — a rozsah
1.0 zřež na fáze 0–2 (headless knihovna + CLI + Python). To je pořád užitečný
produkt a je to reálně dosažitelné.

---

## 12. Metriky úspěchu

Ne „počet hvězdiček". Tyhle:

| Metrika | Cíl u 1.0 |
|---|---|
| Externí přispěvatelé s ≥ 3 mergnutými PR | ≥ 10 |
| Studia používající v produkci | ≥ 3 |
| Pokrytí regresními testy nad jádrem | ≥ 80 % |
| Průměrný cook 30-nodové scény, 5 M bodů | < 2 s |
| Čas od `git clone` k prvnímu buildu | < 30 min |
| Otevřené P0 chyby | 0 |

---

## Příloha · Rychlý přehled časové osy

```
Fáze 0   Rozhodnutí             ├──┤                                      M0–M2
Fáze 1   Jádro (headless)          ├──────────┤                           M2–M8
Fáze 2   Jazyk + nody                        ├────────┤                   M8–M14
Fáze 3   GUI                                          ├──────────┤        M14–M22
Fáze 4   Pipeline                                               ├──────┤  M22–M30
Fáze 5   1.0                                                        ├───┤ M30–M36

Brány        A       B              C              D
```
