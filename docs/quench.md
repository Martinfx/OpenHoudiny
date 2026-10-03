# Voda a oheň: hašení, pára a déšť, který plní vodu

Řešiče v jedné síti o sobě vědí. Voda Liquid Solveru a kapky uzlu Rain,
které se dostanou do plynu Pyro Solveru, oheň hasí: chladí ho, promáčí
palivo, zhasnou plamen a teplo, které vezmou, stoupá jako pára. Zdroj
ohně, na který voda padá, se promočí a dává čím dál méně — táborák v lijáku
uhasne a už se nerozhoří. A déšť, který padá do vody, ji doplňuje:
hladina bazénu v lijáku stoupá.

Nic se k tomu nezapojuje: stačí mít Pyro Solver a Liquid Solver nebo Rain
v téže síti. Jak silně voda hasí, říká Pyro Solver (`quench`, `steam`),
jak rychle déšť plní vodu, říká Rain (`fill`).

![Táborák v lijáku: snímky 30, 60, 90 a 150](img/quench-rain.jpg)

## Příklady

| příklad | co ukazuje |
|---|---|
| `campfire_rain` | táborák hoří vteřinu, pak přijde liják (2 500 kapek za sekundu na m²): kapky, které propadnou plameny, je chladí, ty, které padnou na oheň, ho promočí. Za čtyři sekundy zbude z plamenů 1,5 % a z polen stoupá pára |
| `fire_douse` | na táborák spadne v 1,5 s koule vody o průměru 32 cm: plameny zmizí během deseti snímků, vyvalí se oblak páry a voda odteče po zemi |
| `rain_fill` | liják plní kamennou nádrž: kapky, které padnou do vody, ji rozvlní a přidají do ní 15 mm za sekundu — hladina stoupne za pět sekund o 7 cm a zaleje schod |

```
./build/prototype sim campfire_rain out.png --frames 150 --every 30
./build/prototype sim fire_douse out.png --frames 90 --renderer cycles
```

![Táborák uhašený kbelíkem vody: snímky 45, 60, 75 a 90](img/quench-douse.jpg)

![Liják plní nádrž: na začátku a po pěti sekundách](img/quench-fill.jpg)

V editoru je táborák po 75 snímcích uhašený; uzel Rain ve shrnutí
ukazuje, odkdy prší (a s `fill`, jak rychle plní vodu):

![Editor: campfire_rain na snímku 75](img/editor-quench.jpg)

Co zbývá z plamene (součet pole `flame`) v `campfire_rain` a `fire_douse`:

| snímek | 30 | 50 | 60 | 70 | 90 | 110 | 150 |
|---|---|---|---|---|---|---|---|
| liják od 1 s | 984 | 574 | — | 226 | 118 | 49 | 15 |
| kbelík v 1,5 s | 984 | 739 | 35 | 13 | 3,8 | — | — |

## Parametry

**Pyro Solver**, sekce Water:

| parametr | výchozí | co dělá |
|---|---|---|
| `quench` | 1 | jak silně voda a kapky hasí oheň, do kterého se dostanou: chladí plyn, promáčí palivo a zdroje, na které padají. 0: voda oheň nehasí a plyn je bit po bitu týž jako bez ní |
| `steam` | 1 | kolik kouře (páry) udělá každá jednotka tepla, kterou voda vezme |

**Rain**, sekce Rain:

| parametr | výchozí | co dělá |
|---|---|---|
| `fill` | 0 | o kolik milimetrů za sekundu stoupá voda, do které kapky padají, jako by do ní padal všechen déšť pod mrakem. 0: nic — skutečná kapka je na to příliš málo vody (i prudký liják dá 0,02 mm/s); 5 až 20 naplní bazén během záběru |

Pára je kouř plynu: kreslí se barvou kouře z Volume Look. Příklady s vodou
proto mají kouř světlejší (`smoke_color 0.5 0.49 0.47`) než suchý táborák.

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
Desetina vzatého tepla zůstane v páře, která proto trochu stoupá, a kouř
přibude o `steam` × vzaté teplo. Palivo se namočí dřív, než shoří: zdroj,
který dál přidává palivo do mokrých buněk, nehoří.

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
(verze 5) nese promočení zdrojů plynu a vodu nalitou deštěm, která ještě
není částicí; běh obnovený z checkpointu dává tytéž bajty.

## Kód

| soubor | co tam je |
|---|---|
| [`src/pg/sim/Pyro.h`](../src/pg/sim/Pyro.h), `.cpp` | `PyroSolver::Water`, `setWater`, `quench` (chlazení, pára, promočení zdrojů), `soaked`; `detail::emitScalars` s promočením |
| [`src/pg/sim/Scene.h`](../src/pg/sim/Scene.h) | `SolverSettings::quench`, `steam` |
| [`src/pg/sim/Rain.h`](../src/pg/sim/Rain.h), `.cpp` | `RainSettings::fill`, `intoWater`, `dropVolume`; dopad do mělké vody |
| [`src/pg/sim/Liquid.h`](../src/pg/sim/Liquid.h), `.cpp` | `LiquidSolver::pour`, korekce objemu v `project` |
| [`src/pg/sim/World.cpp`](../src/pg/sim/World.cpp) | voda a kapky do plynu v `prepare`, kapky do vody po kroku deště; stav verze 5 |
| [`src/pg/sim/Network.cpp`](../src/pg/sim/Network.cpp) | parametry `quench`, `steam` (Pyro Solver) a `fill` (Rain) |

## Testy

[`tests/test_quench.cpp`](../tests/test_quench.cpp):

- `water_in_the_gas_cools_it_soaks_the_fuel_and_makes_steam` — voda nad
  ohněm: teplo pod 60 %, plamen a palivo pod 10 %, kouře o 10 % víc,
  zdroj promočený;
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
  do řešičů a zpět ze souboru;

a v [`tests/test_state.cpp`](../tests/test_state.cpp)
`state_resumes_the_quenched_fire_and_the_filling_water_to_the_bit`.

## Omezení

- Pára je kouř: má jeho barvu, hustotu a rozpad. Vlastní pole páry (bílé,
  řídnoucí, kondenzující) zatím není.
- Oheň vodu neodpařuje: částice vody a kapky žárem projdou beze změny
  a voda nedostane teplo.
- Pyro Upres bere promočené zdroje, ale své jemné pole nehasí: drobné
  plameny, které upres nese, dohoří samy.
- Zdroj neschne; mokré dřevo, které se po chvíli znovu chytí, chce čas
  schnutí.
- Kapky hasí, jen dokud jsou kapkami: odstřiky od země a kapičky z hladiny
  plyn nechladí.
