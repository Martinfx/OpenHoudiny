# Voda a oheň: hašení, pára, odpařování a déšť, který plní vodu

Řešiče v jedné síti o sobě vědí. Voda Liquid Solveru a kapky uzlu Rain,
které se dostanou do plynu Pyro Solveru, oheň hasí: chladí ho, promáčí
palivo a zhasnou plamen. Z tepla, které vezmou, je pára — vlastní pole
plynu vedle kouře: bílá, lehčí než vzduch, stoupá a cestou řídne do
čistého vzduchu. Zdroj ohně, na který voda padá, se promočí a dává čím dál
méně — táborák v lijáku uhasne a už se nerozhoří. Plameny zase vodu, která
jimi prochází, odpařují: hrst vody hozená do ohně přijde cestou dolů
o třetinu. A déšť, který padá do vody, ji doplňuje: hladina bazénu
v lijáku stoupá.

Nic se k tomu nezapojuje: stačí mít Pyro Solver a Liquid Solver nebo Rain
v téže síti. Jak silně voda hasí, kolik dělá páry a jak rychle se odpařuje,
říká Pyro Solver (sekce Water), jak pára vypadá, říká Volume Look (sekce
Steam) a jak rychle déšť plní vodu, říká Rain (`fill`).

![Táborák v lijáku: snímky 30, 60, 90 a 150](img/quench-rain.jpg)

## Příklady

| příklad | co ukazuje |
|---|---|
| `campfire_rain` | táborák hoří vteřinu, pak přijde liják (2 500 kapek za sekundu na m²): kapky, které propadnou plameny, je chladí, ty, které padnou na oheň, ho promočí. Za čtyři sekundy zbude z plamenů 1,5 % a z polen stoupá pára |
| `fire_douse` | na táborák spadne v 1,5 s koule vody o průměru 32 cm: plameny zmizí během deseti snímků, tmavým kouřem se vyvalí bílý oblak páry a voda odteče po zemi |
| `fire_hose` | do ohně z polen míří od 1,5 s do 3,5 s proud vody z hadice: kde vchází do plamenů, malá část se ho odpaří dřív, než dopadne, zbytek oheň postupně hasí; bílá pára stoupá tmavým kouřem a nahoře řídne |
| `rain_fill` | liják plní kamennou nádrž: kapky, které padnou do vody, ji rozvlní a přidají do ní 15 mm za sekundu — hladina stoupne za pět sekund o 7 cm a zaleje schod |

```
./build/prototype sim campfire_rain out.png --frames 150 --every 30
./build/prototype sim fire_douse out.png --frames 90 --renderer cycles
./build/prototype sim fire_hose out.png --frames 90 --renderer cycles
```

![Táborák uhašený kbelíkem vody: snímky 45, 60, 75 a 90](img/quench-douse.jpg)

![Hašení hadicí: snímky 60, 90, 120 a 150 — proud vody v plamenech, bílá pára, uhašený oheň a řídnoucí sloup páry (Cycles)](img/quench-hose.jpg)

![Liják plní nádrž: na začátku a po pěti sekundách](img/quench-fill.jpg)

V editoru je táborák po 75 snímcích uhašený; uzel Rain ve shrnutí
ukazuje, odkdy prší (a s `fill`, jak rychle plní vodu):

![Editor: campfire_rain na snímku 75](img/editor-quench.jpg)

Co zbývá z plamene (součet pole `flame`) v `campfire_rain` a `fire_douse`:

| snímek | 30 | 50 | 60 | 70 | 90 | 110 | 150 |
|---|---|---|---|---|---|---|---|
| liják od 1 s | 984 | 575 | 309 | 231 | 122 | 51 | 15 |
| kbelík v 1,5 s | 984 | 739 | 39 | 15 | 5,6 | — | — |

V `fire_hose` proud vody od snímku 45 (1,5 s) stáhne plameny na polovinu
za sekundu a na pětinu za půldruhé sekundy; půl sekundy po konci proudu
(snímek 120) jich zbývá 5 %: 728 na snímku 45, 362 na 75, 154 na 90, 36
na 120. Páry je nejvíc na snímku 90 (součet pole 8 990); sekundu po konci
proudu (snímek 135) z ní zbývá třetina.

## Parametry

**Pyro Solver**, sekce Water:

| parametr | výchozí | co dělá |
|---|---|---|
| `quench` | 1 | jak silně voda a kapky hasí oheň, do kterého se dostanou: chladí plyn, promáčí palivo a zdroje, na které padají. 0: voda oheň nehasí a plyn je bit po bitu týž jako bez ní |
| `steam` | 1 | kolik páry udělá každá jednotka tepla, kterou voda vezme. Pára je vlastní pole plynu (`steam`), kreslené bíle. 0: žádná |
| `steam_lift` | 1,5 | jak silně pára stoupá — je lehčí než vzduch a teplá — v jednotkách vztlaku, jakým zvedá plyn jednotka tepla |
| `steam_fade` | 0,7 1/s | jak rychle pára řídne do čistého vzduchu: za sekundu z ní zbude e^(−0,7), tedy polovina |
| `evaporate` | 1 | jak rychle oheň odpařuje vodu, která je v něm — částice Liquid Solveru, kapky deště. V plamenech zmizí kapka za zlomek sekundy, v teplém kouři vydrží. 0: voda zůstane, ať je jakkoli horko |

**Volume Look**, sekce Steam:

| parametr | výchozí | co dělá |
|---|---|---|
| `steam_color` | 0,92 0,93 0,95 | barva páry ve světle: bílá |
| `steam_density` | 8 | kolik světla pára zastaví: od chomáčku po bílý mrak |

**Rain**, sekce Rain:

| parametr | výchozí | co dělá |
|---|---|---|
| `fill` | 0 | o kolik milimetrů za sekundu stoupá voda, do které kapky padají, jako by do ní padal všechen déšť pod mrakem. 0: nic — skutečná kapka je na to příliš málo vody (i prudký liják dá 0,02 mm/s); 5 až 20 naplní bazén během záběru |

Pára se kreslí sama, svou barvou a hustotou, a kouř zůstává kouřem:
příklady s vodou proto mají kouř tmavý (`smoke_color 0.2 0.19 0.18`, saze
z mokrého dřeva) a bílá pára je v něm dobře vidět. Všechny tři renderery —
editor (OpenGL), path tracer i Cycles — berou obě pole: světlo zastaví
`smoke_density` × kouř + `steam_density` × pára a rozptýlí se v barvě
podle toho, čeho je v místě víc.

## Jak to funguje

### Voda v plynu

Na začátku každého kroku World předá plynu, kde je voda
(`PyroSolver::setWater`): částice Liquid Solveru v kvádru plynu a dráhy
kapek deště na tento krok, tak jak byly na konci minulého. Plyn z nich
udělá pro každou buňku rychlost hašení *r* (1/s):

- **částice vody** zaplní osminu buňky vody (h³/8). Buňka plynu plná vody
  má *r* = 90/s: za snímek z ní zbude dvacetina tepla;
- **kapka** chladí sloupec vzduchu, kterým padá, o průřezu 1 cm² (sprška,
  na kterou se tříští, vzduch, který strhává), každou buňku na své dráze
  rychlostí 12/s × průřez × délka dráhy v buňce / objem buňky / krok.
  V průměru tak každá buňka dostane tolik, kolik naprší, nezávisle na
  velikosti buněk: liják 2 500 kapek za sekundu na m² chladí plyn zhruba
  třikrát za sekundu. Jednotlivá kapka je v buňce, kterou proletí, prudká:
  plamen trhá.

Po unášení a před hořením (`PyroSolver::quench`) ztratí každá mokrá buňka
podíl *q* = 1 − e^(−quench · *r* · dt) svého tepla, paliva i plamene.
Desetina vzatého tepla zůstane v plynu (pára je teplá) a pára v buňce
přibude o `steam` × vzaté teplo. Palivo se namočí dřív, než shoří: zdroj,
který dál přidává palivo do mokrých buněk, nehoří.

### Pára

Pára je vlastní pole plynu (`PyroSolver::steam`) vedle kouře, tepla,
paliva a plamene, na stejné mřížce a ve stejných dlaždicích. Proudění ji unáší stejně jako kouř (MacCormack)
a v pevných tělesech je nula. Ve vztlaku zvedá plyn jako teplo:
svislá rychlost dostane za krok

dt × (buoyancy × teplo − weight × kouř + `steam_lift` × pára),

takže pára stoupá, i když vychladne. Při rozpadu ubude o e^(−`steam_fade`
· dt) a tam, kde se hořící plyn rozpíná, zředí se s ním jako kouř.
Dlaždici, ve které nějaká pára je, řídký řešič nepustí.

Dokud voda do plynu nepřišla (nebo je `steam 0`), řešič pole páry vůbec
neunáší ani nerozpouští, takže plyn bez vody nic nestojí. Pyro
Upres páru sám nepočítá: do jemného snímku ji vzorkuje z hrubého řešiče
(`addCoarseSteam`) v dlaždicích, kde nějaká je.

Renderery berou páru vedle kouře. Editor ji má v alfa kanálu 3D textury
plynu (RGBA16F), path tracer a Cycles ve čtvrté složce mřížky: světlo
zastaví `smoke_density` × kouř + `steam_density` × pára a rozptýlená barva
je průměr `smoke_color` a `steam_color` vážený tím, kolik světla které
pole zastaví. Cycles dostane tuto barvu jako atribut voxelů (`pg_albedo`)
do Principled Volume. Uzel Gas Volume vrací páru jako čtvrtý objem
(`steam`), Python ji vrací jako `frame.gas("steam")`.

### Odpařování

Po kroku plynu a před krokem vody World projde částice vody a kapky deště.
Tam, kde je plyn teplejší než bod varu (`kBoil` = 0,5, teplota mezi
středy buněk, `PyroSolver::heatAt`), se každá odpaří s pravděpodobností

1 − e^(−`evaporate` × (*T* − 0,5) × dt).

V plamenech příkladů (teplota 4 až 5, nejvýš 8) tak kapka při `evaporate
1` vydrží v průměru čtvrt sekundy, v teplém kouři nad nimi déle a ve
vzduchu chladnějším než 0,5 věčně. Los je hash
čísla částice (kapky), čísla snímku a semínka (`detail::boiledAway`), ne
pořadí ve vláknech: odpaří se tytéž částice na jakémkoli počtu vláken
i po obnově z checkpointu.

Odpařená voda páru nepřidává: tu udělalo teplo, které voda vzala při
hašení — a hasí jen voda, která v plynu je. Odpaření tak bere vodu, která
by jinak oheň hasila dál: hrst vody hozená do plamenů přijde cestou dolů
o třetinu (test). Kapky deště proletí plameny za dvacetinu sekundy, takže
se jich při `evaporate 1` odpaří jen pár; s `evaporate 8` jich pod plameny
dopadne o 30 % méně. Hustý proud se ubrání: plyn kolem sebe hned ochladí
pod bod varu, takže v `fire_hose` je po 75 snímcích vody jen o 4 % méně
než s `evaporate 0` a plamenů o 5 % víc.

### Promočený zdroj

Zdroj ohně (palivo nebo teplo nad 0), na který voda padá, se promáčí:
jeho promočení *s* roste o dt × quench × 0,4 × průměrné *r* přes buňky
zdroje a zdroj dává e^(−*s*) toho, co by dal — palivo, kouř, teplo
i rozpínání. Pod vodou je oheň pryč za pár snímků, v lijáku za pár
sekund. Zdroj neschne: když voda odteče nebo přestane pršet, oheň se
znovu nerozhoří. Pyro Upres bere zdroje promočené stejně jako řešič.

### Déšť plní vodu

Kapka nese `fill` / `rate` m³ vody (`RainSolver::dropVolume`). Kapky, které
v kroku dopadnou do vody, dá World vodě (`LiquidSolver::pour`): ta si
objem střádá, a jakmile je ho na částici (osmina buňky), dá ji do volné
osminy buňky poblíž místa dopadu — první od horní vrstvy vody nahoru, tedy
na vodu, ne do ní. Co na celou částici nestačí, počká na další kapky
a jde i do checkpointu.

Rychlá kapka urazí za snímek přes 20 cm, víc, než je hluboká mělká voda.
Dopad proto déšť zkouší i v místě, kde by kapka prošla podlahou — dřív
takové kapky „dopadly na podlahu" pod vodou a do vody se nepočítaly.

### Korekce objemu ve FLIPu

FLIP drží nestlačitelnou vodu na mřížce, ne částice v ní: částice, které
se nahrnou do buňky, se už nerozestoupí. Voda nalitá navrch by tak zapadla
do vody pod ní a stlačila ji, místo aby zvedla hladinu (v testu 25,9 l na
1 m² zvedlo hladinu o 8 mm místo 26). Buňka s víc než 10 částicemi (plná
jich má 8) se proto při projekci nechá rozpínat: každý podkrok z ní
odteče 0,3 přebytku (`kPacked`, `kSwell` v `Liquid.cpp`). Stejných 25,9 l
teď hladinu zvedne o 25 mm. Totéž drží objem i jinde, kde FLIP vodu
stlačí — při dopadu a v proudu do nádrže.

### Determinismus a checkpoint

Mokré buňky jsou seřazené podle čísla buňky a sčítané v pořadí, v jakém
voda přišla; každá buňka plynu se mění jen ve vlastním vlákně. Bez vody
nebo s `quench 0` je plyn bit po bitu stejný jako dřív. Stav simulace
(verze 6) nese promočení zdrojů plynu, pole páry a vodu nalitou deštěm,
která ještě není částicí; běh obnovený z checkpointu dává tytéž bajty.
Snímek cache (verze 17) nese páru v poloviční přesnosti, starší snímky se
čtou dál bez ní.

## Kód

| soubor | co tam je |
|---|---|
| [`src/pg/sim/Pyro.h`](../src/pg/sim/Pyro.h), `.cpp` | `PyroSolver::Water`, `setWater`, `quench` (chlazení, pára, promočení zdrojů), `soaked`; pole `steam` (unášení, vztlak, rozpad), `steamy`, `heatAt`, `kBoil`; `detail::emitScalars` s promočením |
| [`src/pg/sim/Scene.h`](../src/pg/sim/Scene.h) | `SolverSettings::quench`, `steam`, `steamLift`, `steamFade`, `evaporate` |
| [`src/pg/sim/Shared.h`](../src/pg/sim/Shared.h) | `detail::boiledAway`: los odpaření podle čísla částice a snímku |
| [`src/pg/sim/Rain.h`](../src/pg/sim/Rain.h), `.cpp` | `RainSettings::fill`, `intoWater`, `dropVolume`; dopad do mělké vody; `RainSolver::evaporate` |
| [`src/pg/sim/Liquid.h`](../src/pg/sim/Liquid.h), `.cpp` | `LiquidSolver::pour`, korekce objemu v `project`; `LiquidSolver::evaporate` |
| [`src/pg/sim/World.cpp`](../src/pg/sim/World.cpp) | voda a kapky do plynu v `prepare`, odpaření po kroku plynu, kapky do vody po kroku deště; stav verze 6 |
| [`src/pg/sim/Frame.h`](../src/pg/sim/Frame.h), `.cpp` | `Frame::steam` (poloviční přesnost, v dlaždicích plynu), `denseSteam`, `addCoarseSteam` (pára pro Pyro Upres) |
| [`src/pg/sim/Look.h`](../src/pg/sim/Look.h) | `Look::steamColor`, `steamDensity` |
| [`src/pg/gl/Volume.cpp`](../src/pg/gl/Volume.cpp), [`src/pg/render/Gas.cpp`](../src/pg/render/Gas.cpp), [`src/pg/render/Cycles.cpp`](../src/pg/render/Cycles.cpp) | pára v editoru (alfa kanál textury plynu), v path traceru a v Cycles (`pg_albedo`) |
| [`src/pg/sim/Network.cpp`](../src/pg/sim/Network.cpp) | parametry `quench`, `steam`, `steam_lift`, `steam_fade`, `evaporate` (Pyro Solver), `steam_color`, `steam_density` (Volume Look) a `fill` (Rain) |

## Testy

[`tests/test_quench.cpp`](../tests/test_quench.cpp):

- `water_in_the_gas_cools_it_soaks_the_fuel_and_makes_steam` — voda nad
  ohněm: teplo pod 60 %, plamen a palivo pod 10 %, páry aspoň 0,3 vzatého
  tepla a kouře ne víc než bez vody, pára ve snímku (suchý oheň žádnou),
  zdroj promočený;
- `steam_rises_and_thins_out` — pára z uhašeného ohně za sekundu vystoupá
  o víc než půl metru a zbude z ní 5 až 70 %; se `steam 0` žádná;
- `the_fire_boils_away_the_water_in_it` — hrst vody do plamenů přijde
  o víc než 30 % částic, s `evaporate 0` o žádnou, na 1 i 4 vláknech tytéž
  částice; kapek deště, které plameny proletí, je s `evaporate 8` aspoň
  o pětinu méně než s `evaporate 0`;
- `a_soaked_fire_stays_out_when_the_water_has_gone` — po odtečení vody se
  oheň nerozhoří (pod 5 % plamene ohně, který vodu nedostal);
- `no_water_or_quench_0_change_nothing_to_the_bit` — bez vody, s prázdnou
  vodou, s `quench 0` i s deštěm mimo plyn bit po bitu týž plyn;
- `a_downpour_puts_out_a_campfire_and_a_bucket_douses_it` — `campfire_rain`
  pod 20 % plamene suchého ohně, `fire_douse` pod 10 % plamene před
  kbelíkem;
- `rain_fills_the_water_as_fast_as_fill_says` — přidaný objem 80 až 110 %
  toho, co říká `fill`, hladina stoupne; bez `fill` voda nepřibude;
- `fast_drops_land_in_shallow_water_not_through_it` — do 6 cm vody
  dopadne dvacetkrát víc kapek než na podlahu;
- `network_quench_steam_and_fill_reach_the_solvers` — parametry ze sítě
  (i páry a jejího vzhledu) do řešičů a zpět ze souboru, výchozí hodnoty;

dále `gas_steam_stops_light_and_scatters_it_white` v
[`tests/test_gas.cpp`](../tests/test_gas.cpp) (pára v path traceru
zastaví světlo podle `steam_density` a rozptýlí ho v barvě páry),
`frames_keep_their_steam` a `frames_of_version_16_still_read_without_steam`
v [`tests/test_export.cpp`](../tests/test_export.cpp), čtvrtý objem
`steam` uzlu Gas Volume v [`tests/test_sim_geometry.cpp`](../tests/test_sim_geometry.cpp)
a v [`tests/test_state.cpp`](../tests/test_state.cpp)
`state_resumes_the_quenched_fire_and_the_filling_water_to_the_bit`.

## Omezení

- Pára nekondenzuje: jen řídne (`steam_fade`), nesráží se na studených
  plochách ani nedělá mlhu a kapky.
- Odpaření je los po celých částicích a kapkách, ne ubývání objemu.
  Odpařená voda páru nepřidá a plyn už dál nechladí: páru dělá jen teplo,
  které voda vezme při hašení.
- Pyro Upres páru nesimuluje: jemný snímek ji bere vzorkovanou z hrubého
  řešiče, takže je měkčí než kouř upresu.
- Pyro Upres bere promočené zdroje, ale své jemné pole nehasí: drobné
  plameny, které upres nese, dohoří samy.
- Zdroj neschne; mokré dřevo, které se po chvíli znovu chytí, chce čas
  schnutí.
- Kapky hasí, jen dokud jsou kapkami: odstřiky od země a kapičky z hladiny
  plyn nechladí a neodpařují se.
