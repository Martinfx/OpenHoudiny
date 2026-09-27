# Destrukce: Voronoi Fracture, tuhá tělesa, lepidlo a prach

Jak se v Prototype něco rozbije: uzavřené těleso se rozřeže na kusy
(**Voronoi Fracture**), kusy dostanou hmotu a slepí se k sobě (**RBD
Solver** nad knihovnou [Jolt Physics](https://github.com/jrouwe/JoltPhysics)).
Nálože přetrhnou lepidlo v daný čas, nárazy ho lámou dál, padající patra
drtí stěny na prach, úlomky sypou drť a vzduch, který zřícení vytlačí, žene
oblak prachu do ulic (**Pyro Solver**). Všechno je deterministické: stejná
síť dá stejné snímky na jednom i na čtyřech vláknech a při každém dalším
spuštění.

![Odstřel věžáku: oblak prachu se valí mezi domy](img/demolition.png)

```
./build/prototype --example demolition            # v editoru: Play
./build/prototype sim demolition odstrel.mp4      # 180 snímků (6 s) jako video
./build/prototype sim demolition odstrel.png --frames 120
```

Příklad **demolition** ([examples/sim/demolition.pgsim](../examples/sim/demolition.pgsim))
je odstřel čtrnáctipatrového věžáku v bloku domů za zlatého světla,
podle skutečných odstřelů:

- **Věž** postaví wrangle `tower` z kvádrů: stropní desky, stěny kolem
  oken, sloupy, atika a střecha, každý kvádr s barvou `Cd` a atributy
  `floor` a `kind`. Wrangle `seeds` dá body pro Voronoi Fracture — hustší
  v nabitých patrech, aby se tam věž drobila jemněji.
- **Nálože** nastaví wrangle `charges` jako atributy kusů: deska přízemí
  je základ (`active 0`), sloupy a stěny dvou nejnižších pater v první
  sekundě zmizí v prachu (`release 1`, `vanish 1`) a zbytek dostane
  šťouchnutí dovnitř (`kick`). Kusy od třetího patra nahoru mají `crush`:
  pod padajícími patry se rozdrtí na prach.
- **RBD Solver**: beton 2400 kg/m³, lepidlo 150 kPa, prach z přetržených
  spojů, z nárazů a z drcení, drť, `air 3`. Věž se v půdorysu sesune
  asi 13 m/s a necelé čtyři sekundy po odpálení z ní zbude hromada vysoká
  asi šest metrů.
- **Město**: devatenáct domů z assetu Building (jiné výšky a barvy) na
  chodnících s obrubníky. Osm nejbližších je v plynu překážkami (Object
  box), takže prach teče ulicemi a přes nižší střechy; další řada domů
  za nimi dělá z bloku město až k okraji záběru.
- **Prach**: Pyro Solver nad celým blokem (90 × 48 × 90 m, 176 buněk),
  kusy jsou v něm pohyblivé překážky, prach je těžší než vzduch (`weight`)
  a Turbulence ho rozvíří; Volume Look ho barví do okrova se silným
  vlastním stínem.
- **Obraz**: nízké teplé slunce s dlouhými stíny domů i kusů, šedá
  zem bez mřížky, kamera nad blokem pomalu najíždí.

```
[tower] ─┬─────────────────▶ [Voronoi Fracture] ─▶ [charges] ─Pieces─▶ [RBD Solver] ─Look───────────▶ [Output]
         └─▶ [seeds] ─Points─┘                                          │ Dust ─────▶ Sources ─┐         ▲   ▲
                                                                        │ Collider ─▶ Colliders ─┤         │   │
[Building]×19 ─▶ [Transform]×19 ─┐        [Object]×8 (domy) ─▶ Colliders ─┤                      │   │
[Box]×20 (obrubníky) ─▶ [Color] ──┴▶ [Merge city] (zobrazená) [Turbulence] ─▶ Forces ─▶ [Pyro Solver] ─▶ [Volume Look]
```

### Druhý příklad: trosky u země

![Zřícení zdi: cihly se kutálejí ulicí v prachu proti slunci](img/wall-collapse.png)

```
./build/prototype sim wall_collapse zed.mp4       # 120 snímků (4 s)
```

Příklad **wall_collapse** ([examples/sim/wall_collapse.pgsim](../examples/sim/wall_collapse.pgsim))
je destrukce zblízka, jak ji točí kamera těsně nad asfaltem:

- **Průčelí** cihlového domu postaví wrangle `facade`: pás kamene u každého
  stropu, pilíře mezi okny, zeď pod okny a nad nimi, atika. Voronoi
  Fracture ho rozřeže na kusy velké jako pár cihel.
- **Nálože** jdou od země nahoru, řada po řadě: pata zdi vyletí do ulice
  a co stálo nad ní, letí za ní — tím méně daleko, čím výš bylo — takže
  se zeď přeloží a spadne vlnou kusů, které se kutálejí ke kameře. Část
  nejnižších kvádrů se rozpráší.
- **Prach** z lomů a nárazů žene vytlačený vzduch ulicí mezi domy; nízké
  slunce za zdí ho prosvítí. Output má zapnutou oblohu za scénou
  (`sky_behind`): opar nejsvětlejší u obzoru a záři kolem slunce.
- **Kamera** je pár centimetrů nad asfaltem a pomalu jede bokem, se
  širokým objektivem.

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
hran. Tělesa s dutinami a z více uzavřených částí (věž z kvádrů, budova
s balkony) jdou také — víčka mají díry, kde je řez protne vícekrát (viz
Clip).

---

## 2. RBD Solver

Uzel **RBD Solver** (Simulation) dělá z kusů tuhá tělesa. Vstup **Pieces**
je geometrie s atributem `piece` (Voronoi Fracture; bez atributu je kusem
každá souvislá část), **Colliders** jsou objekty, do kterých kusy narážejí —
stojící i klíčované. Výstupy:

| Výstup | Typ | Kam |
|---|---|---|
| **Look** | Look | Do Outputu: solver se simuluje a kusy se kreslí tam, kam dopadly |
| **Rigid** | Rigid | Do uzlu **RBD Pieces**: kusy zpátky jako geometrie |
| **Collider** | Collider | Do Colliders Liquid Solveru, Pyro Solveru nebo Rain: voda, plyn a déšť jdou kolem kusů, kde právě jsou |
| **Dust** | Source | Do Sources Pyro Solveru: prach z přetržených spojů, z nárazů a z drcení |

Parametry:

| Sekce | Parametr | Význam |
|---|---|---|
| Pieces | `attribute` | Co říká, ke kterému kusu primitivum patří |
| Physics | `density` | kg/m³: 2400 beton, 700 dřevo, 7800 ocel; hmota kusu je hustota krát objem jeho obalu |
| | `friction`, `bounce` | Tření a odraz při dopadu (0 žuchnutí, 1 gumový míč) |
| | `gravity` | m/s², dolů |
| | `floor` | Podlaha v nule; bez ní kusy padají pořád |
| Glue | `glue` | Pevnost lepidla v kPa (kilonewtonech na metr čtvereční plochy spoje); 0 znamená bez lepidla |
| Time | `substeps` | Kroky řešiče na snímek: víc pro rychlé kusy a vysoké stavby |
| Dust | `dust` | Kolik prachu dá přetržený spoj |
| | `impact_dust` | … tvrdý náraz a rozdrcený kus |
| | `dust_size` | Jak velký je obláček (m) |
| | `debris` | Kolik drti nárazy a lomy sypou; 0 žádná |
| | `air` | Kolik vzduchu kusy vytlačí, když se drtí a narážejí: rozpíná obláčky a žene prach po zemi; 1 kolik by vytlačily, 0 nic |
| Look | `color`, `inside_color`, `inside_group` | Barva kusů bez vlastního `Cd`, barva řezných ploch a jejich skupina |

Atributy kusů (na primitivech, jinak na bodech; tělo bere atributy svého
prvního primitiva) řeknou, čím se kus liší:

| Atribut | Typ | Význam |
|---|---|---|
| `density` | f | kg/m³ místo hustoty solveru |
| `v`, `w` | v | Rychlost a otáčení (rad/s), se kterými začíná |
| `active` | i | 0: nehýbe se — základ; je slepený a stojí v cestě |
| `glue` | f | Spoje kusu jsou tolikrát pevnější (platí slabší ze dvou); 0: žádné |
| `release` | f | Sekundy, větší než 0: tehdy se jeho spoje přetrhnou — odpálí se nálož |
| `kick` | v | … a přičte se mu tahle rychlost |
| `vanish` | i | 1: při odpálení zmizí, rozmetaný na prach a drť, jako nálož rozdrtí sloup |
| `crush` | f | Náraz víc než tolikrát silnější, než kolik drží jeho lepidlo, ho rozdrtí na prach: stěny, na které dopadne patro. 0: nikdy |

Jako v Houdini doplní Merge atribut, který jedné geometrii chybí, nulou:
`active` a `glue` je proto potřeba nastavit všem kusům, ne jen některým.

### Jak to funguje

[`src/pg/sim/Rigid.h`](../src/pg/sim/Rigid.h) obaluje Jolt Physics 5.6
(MIT). Jolt je sestaven s `CROSS_PLATFORM_DETERMINISTIC` a bez AVX a
solver běží v jednom vlákně (`JobSystemSingleThreaded`): stejný krok dá
stejné bity na každém stroji a při každém spuštění — to je pro cache
snímků, pro testy a pro střih videa důležitější než rychlost. Věž
z příkladu (593 kusů v 710 tělech, přes dva tisíce spojů) se krokuje
v průměru za 6 ms na snímek.

- **Kusy a jejich části.** Kus je jedno tělo — nebo víc, když je
  z částí, které se nedotýkají. Každá část (uzavřený kus povrchu) naráží
  jako svůj konvexní obal (`ConvexHullShape`), takže kus z víc částí
  (roh stěny a stropu, shluk buněk) drží svůj tvar. Části, které se
  dotýkají jakoukoli plochou, jsou jedno tělo. Hmota a setrvačnost jsou
  z objemu obalů a hustoty.
- **Spoje.** Dvě těla se lepí tam, kde se dotýkají plochou: stěny, které
  leží v jedné rovině proti sobě a překrývají se (průnik mnohoúhelníků
  Sutherland–Hodgman, plocha triangulací). Spoj je tak pevný, jak velká
  je společná plocha: `glue` × plocha, krát atribut `glue` slabšího kusu.
  Plošky menší než (10⁻⁴ × úhlopříčka)² se nelepí.
- **Slepené kusy jsou jedno těleso.** Kusy spojené neporušenými spoji
  tvoří shluk (cluster) a ten je v Joltu jedno těleso složené z obalů
  všech svých kusů (`StaticCompoundShape`) — tuhé jako jeden kus, takže
  postavená stavba stojí, neprohýbá se a nekmitá. Kusy s `active 0` jsou
  statická tělesa a shluk k nim přilepený drží pevný spoj
  (`FixedConstraint`). Tak to dělá i Houdini (Bullet) s lepidlem: slepené
  kusy simuluje jako jedno těleso a rozdělí je až při nárazu.
- **Nárazy.** Z každého kontaktu (nového i trvajícího) solver pozná,
  jak silně to bouchlo: co bylo třeba ke změně pohybu tělesa (hybnost
  po kroku proti hybnosti před ním, bez gravitace, rozdělená mezi místa
  nárazu), a aspoň co je třeba k zastavení obou věcí proti sobě — jako
  síla za krok řešiče. Síla přetrhne spoje zasaženého kusu, které drží
  méně, a polovina jde dál na kusy za nimi (i přes spoje, které právě
  praskly), kde zase přetrhne, co drží méně než ona, a tak dál do
  ztracena. Kus s `crush`, do kterého narazí víc než `crush`-krát tolik,
  kolik drží jeho lepidlo, se rozdrtí: zmizí v obláčku prachu a drti.
- **Rozpad.** Když ve shluku prasknou spoje, rozpadne se na skupiny, které
  drží pohromadě, a každá jde dál jako samostatné těleso. Skupina, která
  se odlomila, si ponechá 98,5 % rychlosti, kterou měla před nárazem —
  drtila to, co se ulomilo, nestála na tom; skupina, do které převážně
  narazilo, se zastaví. Proto se věž v příkladu hroutí patro po patru
  rychlostí asi 13 m/s a nezůstane stát na první hromadě.
- **Nálože.** V čase `release` se přetrhnou všechny spoje kusu. Kus
  s `vanish` zmizí ve výbuchu prachu a drti, ostatní dostanou `kick`.
- **Drť.** Nárazy, lomy a drcení sypou drobné kamínky: letí balisticky,
  odrazí se od podlahy, kloužou a zůstanou ležet (nejvýš 40 000 kusů).
  Velikost roste s `dust_size`, počet s `debris`.
- **Prach a vytlačený vzduch.** Každý lom, náraz a rozdrcený kus vyfoukne
  obláček, který osm snímků slábne a letí polovinou rychlosti kusu;
  blízké obláčky téhož kroku se sloučí. Tvrdý náraz a drcení navíc
  vytlačí vzduch — tolik, kolik kus o své úhlopříčce *d* zastavený
  rychlostí *v* vytlačí: `air` × 0,35 × *d*² × *v* m³/s. Obláček se tím
  rozpíná (m³/s na svůj objem, nejvýš 20 za sekundu) a Pyro Solver tu
  expanzi započte do tlaku: prach se od hromady valí po zemi do stran,
  jako při skutečném odstřelu, kde padající patra vytlačí vzduch z celé
  budovy.
- **Překážky.** Objekty ze vstupu Colliders jsou kinematická tělesa (koule,
  kvádr, válec; kužel a síť jako konvexní obal): jdou přesně tam, kam je
  animace klíčuje, a odstrčí, co jim stojí v cestě. Podlaha je statický
  kvádr pod nulou. Rychlost kusů je omezená (40 m/s, 30 rad/s), aby je
  nekonečně těžká překážka nevystřelila.
- **Kroky.** Svět (`WorldSolver`) krokuje tuhá tělesa jako první; voda,
  plyn a déšť pak dostanou kusy tam, kde právě jsou, a plyn obláčky
  prachu jako zdroje.

---

## 3. Úlomky dál: kreslení, geometrie, voda, plyn, prach

**Kreslení.** Solver zapojený do Outputu se kreslí sám: každý snímek se
kusy posunou a otočí tam, kam dopadly (`drawnPieces`), s barvou `Cd`, kterou
si nesou, řezné plochy ze skupiny `inside_group` v barvě `inside_color`,
kusy bez barvy v `color`; rozdrcené a rozmetané kusy zmizí. Drť se kreslí
jako hranaté úlomky kamene své velikosti v barvě řezu, o odstín tmavší:
každý úlomek vyřízne pět až sedm lomů kolem vršku mimo střed, každý lom je
plocha, která se od oka odklání, a úlomek má svůj tvar i trochu jiný
odstín (šedší, světlejší, tmavší). Tvar se odvodí z velikosti zrnka, která
se za letu nemění, takže úlomek zůstane týž; v letu se otáčí, ležící je
v klidu. Body zobrazené geometrie zůstávají kulaté tečky. V prachu drť
zakryje prach mezi okem a zrnkem a zastíní prach mezi ním a sluncem, takže
drť v oblaku proti světlu tmavne. Geometrie i kusy vrhají stíny na sebe,
na zem i do kouře (stínová mapa slunce, 2048², měkké okraje) a Output má
barvu země, vypínač mřížky a oblohu za scénou (`sky_behind`).

**Do jiného rendereru.** `prototype sim demolition - --export
demolition.usda` zapíše celý záběr jako scénu USD: každé těleso jednou
jako tvar a pak jen jeho poloha a otočení v každém snímku, rozmetaná tělesa
zneviditelněná, drť jako body, prach jako soubory VDB vedle, kamera,
slunce a obloha. Blender, Houdini nebo Karma ho vyrenderují s vlastním
světlem, rozmazáním pohybem a materiály; plochy řezu jsou `GeomSubset`
`inside`, aby dostaly jiný materiál ([usd.md](usd.md)).

**RBD Pieces** (Geometry) vrátí kusy daného snímku jako geometrii: body
posunuté a otočené, normály otočené a rychlost každého bodu v `v` — pro
další uzly, pro export snímek po snímku (`prototype sim --export`), pro
scatter jisker z hran. Se zapnutým `grit` přidá i drť jako body: `pscale`
je polovina velikosti zrnka, `v` jeho rychlost a `id` jeho číslo — každé
zrnko dostane při vyhození své a drží ho, dokud je ve scéně, takže renderer
podle něj zrnko sleduje a rozmaže pohybem. Bez snímku (před simulací) je
prázdný.

**Voda, plyn a déšť.** Výstup Collider dá každý kus jako překážku typu
síť (`MeshShape` z jeho trojúhelníků), posunutou a otočenou tam, kde kus
je, s jeho rychlostí a otáčením: voda se před kusem hrne a za ním táhne
brázdu, kouř kusy obtéká a padající kusy ho strhávají s sebou, déšť od
nich odstřikuje. Vazba je jednosměrná — voda ani plyn kusy nebrzdí.

**Prach.** Výstup Dust do Sources Pyro Solveru: každý obláček je koule
velikosti obláčku, která dává kouř `4 × síla`, jen trochu tepla (prach
se valí víc, než stoupá), rychlost obláčku a jeho expanzi; kouř vychází
v chuchvalcích (šum jako u plamenů), ze kterých se oblak nadouvá. Pyro
Solver bez jiných zdrojů se nehlásí, že nic neuvidí — prach je zdroj.

**Expanze zdroje.** Stejnou věc umí i obyčejný zdroj: parametr
**Expansion** (1/s) uzlu Pyro Source říká, jak rychle se plyn ve zdroji
rozpíná — tlačí ho do všech stran, jako výbuch nebo vzduch vytlačený
zřícením. Pyro Solver ji sečte s expanzí hořícího paliva, započte do
tlaku a plyn, který se rozpíná, ředí.

---

## 4. Snímky a cache

Snímek (`sim::Frame`) nese k plynu, vodě a dešti i `RigidFrame`: polohy,
rychlosti a otáčení kusů, seznam kusů, které zmizely, drť (poloha a
velikost), počet spojů a kolik jich prasklo. Soubor `.pgframe` je od toho
verze 3 ([cache.md](cache.md)); starší snímky se čtou dál. Klidová
geometrie kusů v souborech není — je v síti, která ji uvaří při překladu
— a snímek načtený z disku ji dostane od světa, ve kterém se přehrává
(`adoptPieces`), pokud sedí počet kusů; rozložení kusů do těl se přitom
spočítá jednou pro celou sekvenci.

---

## 5. Ověřování

`tests/test_rigid.cpp` (15 testů), `tests/test_topology.cpp` (fracture)
a testy expanze v `tests/test_pyro.cpp`:

- kusy krychle jsou uzavřené a jejich objemy dají objem krychle; stejný hash
  na 1 i 4 vláknech; budova z assetu se rozřeže beze zbytku a stěny si
  nesou barvy;
- kusy padají a dosednou na podlahu, v klidu, a každý kus si drží tvar;
- slepená stavba stojí, jak je postavená, a pod závažím shozeným shora se
  lepidlo zlomí; klíčovaný objekt povalí slepenou zeď;
- těla jsou části, které se dotýkají, a spoje jsou tam, kde se potkají
  plochy (plocha a normála spoje dvou kvádrů);
- nálože přetrhnou lepidlo v čas `release`, `kick` kusy postrčí,
  `active 0` stojí; nárazy vyfouknou prach a sypou drť, která dopadne
  na zem; těžší kus podle atributu `density` přetáhne lehčí;
- tvrdý náraz vytlačí vzduch: obláček se rozpíná (nejvýš 20/s), s `air 0`
  ne; zdroj s expanzí tlačí plyn do stran a po zemi a studený kouř bez ní
  zůstane, kde byl;
- stejné snímky na 1 a 4 vláknech i mezi dvěma běhy;
- posun, otočení a `v` na bodech; barvy kreslení (vlastní `Cd`, barva
  řezu, barva kusů, drť jako body); kusy bez atributu podle souvislosti;
- RBD Solver v síti: překlad do světa a vzhledu (`glue` v kPa, `air`),
  aktivní uzly, RBD Pieces ze snímku, snímek přes cache a `adoptPieces`,
  soubor tam a zpět; kusy do vody, plynu a deště a prach jako zdroj;
  chyby (bez kusů, druhý solver, kusy ze simulace).

Sanitizery (ASan/UBSan) a libc++ běží na celé sadě jako u ostatních
kroků ([pyro.md §9](pyro.md#9-ověřování)).

---

## 6. Omezení a co dělá produkce

- **Konvexní obaly.** Každá část kusu naráží svým konvexním obalem:
  prohnutá část (rám okna) naráží větším tvarem, než vypadá. Houdini dělá
  totéž ve výchozím nastavení (Bullet, convex hull) a pro duté kusy nabízí
  konkávní rozklad.
- **Slepené je tuhé.** Shluk slepených kusů se neprohýbá; zlomí se, nebo
  drží. Ohyb ocelové výztuže (Houdini: soft constraints, plasticita) tu
  není.
- **Síla nárazu je odhad.** Kolik nárazu jde přes spoje dál (polovina) a
  kolik rychlosti si odlomená skupina ponechá, jsou pravidla, ne řešení
  napětí v konstrukci — věž padá jako skutečná, ale ne každý spoj praskne
  tam, kde by praskl beton.
- **Drcení na prach.** Rozdrcený kus zmizí najednou; drobení na menší
  kusy za běhu (Houdini RBD Material Fracture s omezeními) tu není —
  kusy jsou hotové předem.
- **Detail prachu** je daný mřížkou plynu: v příkladu je buňka půl
  metru, takže oblak má tvar a stíny, ale ne jemné „květákové“ chuchvalce
  produkčních simulací s desetinásobným rozlišením.
- **Jednosměrné vazby.** Kusy tlačí vodu a plyn, ale voda je nenadnáší a
  kouř je nebrzdí; kinematické překážky mají nekonečnou hmotu.
- **Jeden RBD Solver** v síti; kusy dvou solverů do sebe nenarážejí.

---

## 7. Odkazy

- J. Rouwe: [Jolt Physics](https://jrouwe.github.io/JoltPhysics/) —
  architektura, determinismus, vazby.
- Z. P. Bažant, M. Verdure: *Mechanics of Progressive Collapse* (Journal
  of Engineering Mechanics, 2007) — proč se hroucení šíří patro po patru a
  jak rychle.
- SideFX: [RBD Bullet Solver](https://www.sidefx.com/docs/houdini/nodes/dop/rbdbulletsolver.html),
  [Voronoi Fracture](https://www.sidefx.com/docs/houdini/nodes/sop/voronoifracture.html),
  [RBD Constraints](https://www.sidefx.com/docs/houdini/nodes/sop/rbdconstraintsfromrules.html)
  — jak destrukce vypadá v produkci: kusy, vazby s pevností, prach a
  drobky.
