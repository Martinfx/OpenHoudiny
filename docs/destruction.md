# Destrukce: Voronoi Fracture, tuhá tělesa a lepidlo

Jak se v Prototype něco rozbije: uzavřené těleso se rozřeže na kusy
(**Voronoi Fracture**), kusy dostanou hmotu a slepí se k sobě (**RBD
Solver** nad knihovnou [Jolt Physics](https://github.com/jrouwe/JoltPhysics)),
a když do nich něco narazí, lepidlo se utrhne, kusy padají, kutálejí se,
padají do vody a zvedají prach. Všechno je deterministické: stejná síť dá
stejné snímky na jednom i na čtyřech vláknech a při každém dalším spuštění.

![Demolice: budova z assetu Building, koule a bazén](img/demolition.png)

```
./build/prototype --example demolition            # v editoru: Play
./build/prototype sim demolition demolice.mp4     # 110 snímků jako video
./build/prototype sim demolition demolice.png --frames 60
```

Příklad **demolition** ([examples/sim/demolition.pgsim](../examples/sim/demolition.pgsim)):
budova z assetu [Building](assets.md#5-příklad-budova-z-posuvníků) stojí u
bazénu, Voronoi Fracture ji rozřeže na 36 kusů, RBD Solver je slepí, a
koule s klíčovanou polohou (viz [animation.md](animation.md)) se do ní
rozhoupe. Kusy, které vyletí, padají do vody (Liquid Solver je má jako
překážky) a každý přetržený spoj vyfoukne prach do Pyro Solveru.

```
[Building] ─▶ [Transform] ─▶ [Voronoi Fracture] ─Geometry─▶ [RBD Solver] ─Look────▶ [Output]
                                                    [Object] ─Collider─┘  │ Collider     ▲  ▲
                                                     (koule, klíče)       ├──────▶ [Liquid Solver] ─▶ [Water Look]
                                                                          │ Dust  (Colliders)
                                                                          └──────▶ [Pyro Solver] ─▶ [Volume Look]
                                                                                  (Sources)
```

---

## 1. Voronoi Fracture

Uzel **Voronoi Fracture** (Geometry) vezme uzavřenou síť polygonů a rozřeže
ji na buňky bodů: každá buňka je ta část tělesa, která je k svému bodu blíž
než ke kterémukoli jinému. Body přijdou buď z druhého vstupu **Points**
(scatter, body z wrangle, cokoli), nebo si je uzel vyrobí sám: **Count**
bodů náhodně uvnitř tělesa, pro stejný **Seed** vždy stejných.

| Parametr | Význam |
|---|---|
| `count` | Kolik kusů, když nepřijdou body: tolik náhodných bodů uvnitř tělesa |
| `seed` | Jiné číslo, jiné body |
| `attribute` | Jméno atributu s číslem kusu (výchozí `piece`), na primitivech i bodech |
| `insidegroup` | Skupina primitiv řezných ploch (výchozí `inside`) |

Jak to počítá ([`src/pg/nodes/Fracture.cpp`](../src/pg/nodes/Fracture.cpp)):
buňka bodu *s* je průnik polorovin „blíž k *s* než k *t*“ pro všechny
ostatní body *t*. Uzel proto těleso postupně ořezává rovinou v polovině
mezi *s* a *t* (uzel [Clip](geometry.md) s uzavřením řezu), od nejbližšího
*t* k nejvzdálenějšímu, a rovinu, která už nic neuřízne, přeskočí — po
několika prvních řezech je kus malý a zbytek rovin ho mine. Každý řez
uzavře víčkem, takže kus je uzavřený jako bylo těleso, a kusy dohromady
jsou přesně původní těleso (test to ověřuje na objemu: součet objemů kusů
se rovná objemu tělesa na 1e-4). Víčka nesou atributy primitiva, ze
kterého vznikla, a jsou ve skupině `inside` — vzhled ji obarví jinak než
povrch. Buňky se počítají paralelně a skládají v pořadí bodů, takže výsledek
je stejný na libovolném počtu vláken.

Body uvnitř tělesa pozná uzel paritou průsečíků paprsku s polygony
(Möller–Trumbore po vějířích), s paprskem mírně mimo osy, aby neběžel podél
hran. Tělesa s dutinami a z více uzavřených částí (budova s balkony) jdou
také — víčka mají díry, kde je řez protne vícekrát (viz Clip).

---

## 2. RBD Solver

Uzel **RBD Solver** (Simulation) dělá z kusů tuhá tělesa. Vstup **Pieces**
je geometrie s atributem `piece` (Voronoi Fracture; bez atributu je kusem
každá souvislá část), **Colliders** jsou objekty, do kterých kusy narážejí —
tiché i klíčované (demoliční koule). Výstupy:

| Výstup | Typ | Kam |
|---|---|---|
| **Look** | Look | Do Outputu: solver se simuluje a kusy se kreslí tam, kam dopadly |
| **Rigid** | Rigid | Do uzlu **RBD Pieces**: kusy zpátky jako geometrie |
| **Collider** | Collider | Do Colliders Liquid Solveru, Pyro Solveru nebo Rain: voda, plyn a déšť jdou kolem kusů, kde právě jsou |
| **Dust** | Source | Do Sources Pyro Solveru: každý přetržený spoj vyfoukne prach (kouř) |

Parametry:

| Sekce | Parametr | Význam |
|---|---|---|
| Pieces | `attribute` | Co říká, ke kterému kusu primitivum patří |
| Physics | `density` | kg/m³: 2400 beton, 700 dřevo, 7800 ocel; hmota kusu je hustota krát objem jeho obalu |
| | `friction`, `bounce` | Tření a odraz při dopadu (0 žuchnutí, 1 gumový míč) |
| | `gravity` | m/s², dolů |
| | `floor` | Podlaha v nule; bez ní kusy padají pořád |
| Glue | `glue` | Pevnost lepidla v newtonech: spoj tažený větší silou se utrhne; 0 znamená bez lepidla. Kus o tuně váží zhruba 10 000 N |
| Time | `substeps` | Kroky řešiče na snímek: víc pro rychlé kusy a vysoké stavby |
| Dust | `dust`, `dust_size` | Kolik kouře a jak velký obláček dá přetržený spoj |
| Look | `color`, `inside_color`, `inside_group` | Barva kusů bez vlastního `Cd`, barva řezných ploch a jejich skupina |

### Jak to funguje

[`src/pg/sim/Rigid.h`](../src/pg/sim/Rigid.h) obaluje Jolt Physics 5.6
(MIT). Jolt je sestaven s `CROSS_PLATFORM_DETERMINISTIC` a bez AVX a
solver běží v jednom vlákně (`JobSystemSingleThreaded`): stejný krok dá
stejné bity na každém stroji a při každém spuštění — to je pro cache
snímků, pro test a pro střih videa důležitější než rychlost, a desítky až
stovky kusů jsou i tak rychlé.

- **Kusy.** Každý kus je konvexní obal svých bodů (`ConvexHullShape`)
  s hustotou; Jolt z něj spočítá hmotu a setrvačnost. Kus, který je příliš
  plochý na obal, se nepohybuje. Kusy začínají tam, kde je vyrobila síť
  (pozice těla je počátek, body jsou v jeho prostoru), takže snímek
  drží pro každý kus jen posun, kvaternion, rychlost a otáčení
  (`RigidPose`).
- **Lepidlo.** Dva kusy, které sdílejí aspoň tři shodné body (řeznou
  plochu, ne jen hranu nebo roh), dostanou pevný spoj (`FixedConstraint`).
  Po každém kroku se přečte impuls spoje: síla `|λ| / dt` větší než
  `glue` spoj natrvalo přetrhne a poznamená si místo — obláček prachu, který
  osm snímků slábne. Řetězy pevných spojů řeší iterační solver pružně,
  proto RBD Solver nechá Jolt jít po vazbách víckrát než hra (24 rychlostních
  a 6 polohových iterací) a trpí menší průnik (5 mm): slepený trám
  přesahující přes hranu stolu se prohne o centimetry, ne o decimetry, a
  slepená stavba stojí.
- **Překážky.** Objekty ze vstupu Colliders jsou kinematická tělesa (koule,
  kvádr, válec; kužel a síť jako konvexní obal): jdou přesně tam, kam je
  animace klíčuje (`MoveKinematic` na polohu dalšího snímku), a odstrčí,
  co jim stojí v cestě — nekonečně těžké, kusy je nezastaví. Podlaha je
  statický kvádr pod nulou.
- **Kroky.** Svět (`WorldSolver`) krokuje tuhá tělesa jako první; voda,
  plyn a déšť pak dostanou kusy tam, kde právě jsou.

---

## 3. Úlomky dál: kreslení, geometrie, voda, plyn

**Kreslení.** Solver zapojený do Outputu se kreslí sám: každý snímek se
kusy posunou a otočí tam, kam dopadly (`drawnPieces`), s barvou `Cd`, kterou
si nesou (budova má obarvené stěny, sklo a ostění), řezné plochy ze skupiny
`inside_group` v barvě `inside_color`, a kusy bez barvy v `color`. Kreslí
se stejně jako zobrazená geometrie (polygony s normálami, stíny slunce).

**RBD Pieces** (Geometry) vrátí kusy daného snímku jako geometrii: body
posunuté a otočené, normály otočené a rychlost každého bodu v `v` — pro
další uzly, pro export snímek po snímku (`prototype sim --export`), pro
scatter jisker z hran. Bez snímku (před simulací) je prázdný.

**Voda, plyn a déšť.** Výstup Collider dá každý kus jako překážku typu
síť (`MeshShape` z jeho trojúhelníků, pole vzdáleností 16³), posunutou a
otočenou tam, kde kus je, s jeho rychlostí a otáčením: voda se před kusem
hrne a za ním táhne brázdu jako za [klíčovanou koulí](animation.md), kouř
kusy obtéká, déšť od nich odstřikuje. Vazba je jednosměrná — voda kusy
nenadnáší ani nebrzdí.

**Prach.** Výstup Dust do Sources Pyro Solveru: každý přetržený spoj je
osm snímků koulí `dust_size`, která dává kouř `dust · 4 · zbytek` a trochu
tepla, aby stoupal. Pyro Solver bez jiných zdrojů se nehlásí, že nic
neuvidí — prach je zdroj.

---

## 4. Snímky a cache

Snímek (`sim::Frame`) nese k plynu, vodě a dešti i `RigidFrame`: polohy,
rychlosti a otáčení kusů, počet spojů a kolik jich prasklo. Soubor
`.pgframe` je od toho verze 2 ([cache.md](cache.md)); starší snímky se čtou
dál. Klidová geometrie kusů v souborech není — je sítě, která ji uvaří při
překladu — a snímek načtený z disku ji dostane od světa, ve kterém se
přehrává (`adoptPieces`), pokud sedí počet kusů; jinak se z tuhých těles
nekreslí nic.

---

## 5. Ověřování

`tests/test_rigid.cpp` (10 testů) a `tests/test_topology.cpp` (fracture):

- kusy krychle jsou uzavřené a jejich objemy dají objem krychle; stejný hash
  na 1 i 4 vláknech; budova z assetu se rozřeže beze zbytku a stěny si
  nesou barvy;
- kusy padají a dosednou na podlahu, v klidu, a každý kus si drží tvar
  (vzdálenosti bodů);
- slepený trám přes hranu stolu drží (žádný spoj neprasknul, prohyb pod
  deset centimetrů), bez lepidla přesah spadne, se slabým lepidlem se
  utrhne a vyfoukne prach;
- koule s klíčovanou polohou prorazí slepenou zeď: spoje praskají, kusy
  letí jejím směrem; kusy jako překážky vody jsou sítě s pohybem;
- stejné snímky na 1 a 4 vláknech i mezi dvěma běhy;
- posun, otočení a `v` na bodech; barvy kreslení (vlastní `Cd`, barva
  řezu, barva kusů); kusy bez atributu podle souvislosti;
- RBD Solver v síti: překlad do světa a vzhledu, aktivní uzly, RBD Pieces
  ze snímku, snímek přes cache a `adoptPieces`, soubor tam a zpět; kusy do
  vody, plynu a deště a prach jako zdroj; chyby (bez kusů, druhý solver,
  kusy ze simulace).

Sanitizery (ASan/UBSan, TSan) a libc++ běží na celé sadě jako u ostatních
kroků ([pyro.md §9](pyro.md#9-ověřování)).

---

## 6. Omezení a co dělá produkce

- **Konvexní obaly.** Kus je pro srážky svým konvexním obalem: dutý nebo
  prohnutý kus (rám okna, roh budovy) naráží větším tvarem, než vypadá.
  Houdini dělá totéž ve výchozím nastavení (Bullet, convex hull) a pro
  duté kusy nabízí konkávní rozklad; Jolt umí `MeshShape` jen pro
  statická tělesa, takže by bylo třeba rozkládat na víc obalů.
- **Lepidlo je pružné.** Iterační řešič vazeb nedrží dlouhé řetězy tuho —
  vysoká stavba se maličko prohýbá a slabě kmitá. Produkce slepené kusy
  slučuje do jednoho tělesa (compound) a rozděluje je až při nárazu, což
  drží dokonale, ale hůř se řídí pevnost jednotlivých spojů.
- **Jednosměrné vazby.** Kusy tlačí vodu a plyn, ale voda je nenadnáší a
  kouř je nebrzdí; kinematické překážky mají nekonečnou hmotu.
- **Kusy jsou hotové předem.** Těleso se rozřeže před simulací; dynamické
  praskání podle místa nárazu (jako Houdini RBD Material Fracture s
  omezeními) tu není.
- **Jeden RBD Solver** v síti; kusy dvou solverů do sebe nenarážejí.
- **Rychlost.** Jolt v jednom vlákně zvládne stovky kusů v reálném čase;
  víc kusů vede spíš k méně, větším kusům a k menšímu `substeps`.

---

## 7. Odkazy

- J. Rouwe: [Jolt Physics](https://jrouwe.github.io/JoltPhysics/) —
  architektura, determinismus, vazby.
- E. Catto: *Soft Constraints* (GDC 2011) — proč iterační řešiče vazeb
  pruží a co s tím.
- SideFX: [RBD Bullet Solver](https://www.sidefx.com/docs/houdini/nodes/dop/rbdbulletsolver.html),
  [Voronoi Fracture](https://www.sidefx.com/docs/houdini/nodes/sop/voronoifracture.html),
  [RBD Constraints](https://www.sidefx.com/docs/houdini/nodes/sop/rbdconstraintsfromrules.html)
  — jak destrukce vypadá v produkci: kusy, vazby s pevností, prach a
  drobky.
