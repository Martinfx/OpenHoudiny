# Kouř a oheň: simulace z uzlů (Pyro)

Skutečná simulace plynu na 3D mřížce, stejný princip jako Pyro v Houdini a
EmberGen. Kouř a oheň se tu hýbou, protože to vyplývá z rovnic proudění:
teplo stoupá, víry se stáčejí, palivo hoří, plyn se rozpíná a obtéká
překážky. Simulace se skládá z **uzlů**: zdroje, síly a překážky vedou do
řešiče, ten do vzhledu a vzhled na výstup. Shaderové efekty
z [shader-graph.md §6](shader-graph.md#6-efekty-oheň-a-kouř) pohyb jen
předstírají šumem na jedné ploše.

![Táborák, exploze a tornádo, rozlišení 96](img/pyro.gif)

![Editor: síť Simulation s tornádem, vybraný uzel Vortex a jeho vodítka ve viewportu](img/editor-sim.png)

Všechno je v programu `pgshader`. Editor se otevře rovnou na síti
**Simulation** s táborákem; bez okna simulaci spočítá a vykreslí příkaz
`pgshader sim`.

Obsah:
[1. Rychlý start](#1-rychlý-start) ·
[2. Editor](#2-editor) ·
[3. Síť simulace](#3-síť-simulace) ·
[4. Jak simulace funguje](#4-jak-simulace-funguje) ·
[5. Jak se kreslí](#5-jak-se-kreslí) ·
[6. Výkon a determinismus](#6-výkon-a-determinismus) ·
[7. Ověřování](#7-ověřování) ·
[8. Jak přidat uzel](#8-jak-přidat-uzel) ·
[9. Co je potřeba znát](#9-co-je-potřeba-znát) ·
[10. Omezení a co dělá produkce](#10-omezení-a-co-dělá-produkce) ·
[11. Odkazy](#11-odkazy)

---

## 1. Rychlý start

```bash
./build/pgshader                          # editor: síť Simulation s táborákem
./build/pgshader --example tornado        # jiný vestavěný příklad
./build/pgshader moje.pgsim               # vlastní síť

# bez okna: poslední snímek, nebo každý k-tý jako očíslovanou sekvenci
./build/pgshader sim campfire fire.png
./build/pgshader sim examples/sim/explosion.pgsim out/boom.png --every 2
./build/pgshader sim tornado t.png --set vortex.speed=3 --set solver.resolution=128
./build/pgshader sim --list               # vestavěné příklady

# sekvence do videa
ffmpeg -framerate 15 -pattern_type glob -i 'out/boom_*.png' boom.mp4
```

`sim` síť načte ze souboru, nebo vezme vestavěný příklad podle jména.
`--set UZEL.PARAMETR=HODNOTA` změní parametr před simulací; když má
parametr toho jména jediný uzel, stačí `--set PARAMETR=HODNOTA`. Počet
snímků dává uzel Output, `--frames N` ho přepíše, `--resolution` přepíše
rozlišení řešiče. Příkaz vypíše, kolik trval krok simulace a kolik jeden
obrázek. Vykresluje přes EGL bez okna, takže funguje i na serveru; bez GPU
stačí softwarový ovladač, třeba Mesa llvmpipe. Se `--every K` uloží snímky
K, 2K, 3K… a pojmenuje je číslem snímku (`boom_0002.png`, `boom_0004.png`,
…). Proto je v příkladu pro ffmpeg `glob`: čísla netvoří souvislou řadu.
`--guides` do obrázku nakreslí vodítka: doménu, zdroje a síly.

Starší `pgshader pyro OUT.png --preset fire` pořád funguje: spustí příklad
(`fire` je táborák).

## 2. Editor

Editor má pro obě sítě, simulaci i shadery, stejné rozložení. Mezi sítěmi
se přepíná přepínačem uprostřed horní lišty.

| panel | co ukazuje |
|---|---|
| **Viewport** (vlevo nahoře) | scéna: plyn na podlaze se stínem, objekty, vodítka; výběr kliknutím a gizmo |
| **Časová osa** (vlevo dole) | přehrávání, snímky v cache, přehrávací hlava |
| **Parameters** (vpravo nahoře) | parametry vybraného uzlu; bez výběru přehled sítě |
| **Network** (vpravo dole) | síť uzlů |

Rozhraní mezi panely jdou táhnout.

### Síť

| akce | jak |
|---|---|
| přidat uzel | **Tab** nebo pravé tlačítko do prázdna: nabídka s hledáním, Enter vezme první |
| spojit | táhnout z pinu na pin; zelená = pasuje, červená = nepasuje a tooltip řekne proč |
| přidat rovnou spojený uzel | táhnout z pinu do prázdna: nabídne jen uzly, které k pinu pasují |
| přesunout nebo zrušit spoj | táhnout za připojený vstup; puštěný do prázdna zanikne. Ctrl+klik na spoj ho zruší |
| posun, zoom | prostřední tlačítko nebo Alt+levé; kolečko zvětšuje kolem myši |
| výběr | klik, rámeček tažením, Shift přidává, Ctrl přepíná, Ctrl+A vše |
| zarámovat / uspořádat | **F** výběr (nebo vše), **L** automatické rozložení do sloupců |
| smazat / duplikovat | Del nebo X / Ctrl+D |
| bypass | **B**: uzel zůstane v síti, ale simulace ho vynechá |

Klávesy patří panelu pod myší, jako v Houdini. Hlavička uzlu má barvu
kategorie, pod názvem je shrnutí toho, co uzel dělá (`fuel 14 · heat 1`).
Uzel, který nevede na výstup, je tlumený. Problém ukáže červený nebo žlutý
odznak; tooltip nad ním řekne, co je špatně. Při velkém oddálení se texty
schovají a zůstanou jen tvary a barvy.

### Parametry

Parametry jsou rozdělené do sekcí, které jdou sbalit. Popisek se rozsvítí,
když se hodnota liší od výchozí; ikona ↺ vpravo ji vrátí. Tooltip nad
popiskem vysvětlí, co parametr dělá, a uvede jeho jméno pro `--set` a
rozsah posuvníku. Posuvník má rozsah, kde je parametr užitečný. Ctrl+klik
dovolí napsat číslo i mimo něj, meze drží jen fyzikální smysl (třeba žádné
záporné palivo). Vektory mají osy barevně, x červeně, y zeleně, z modře, a
volby po několika tlačítkách vedle sebe.

### Viewport: objekty a gizmo

Viewport se ovládá jako ve 3D programech. Kliknutí vybere, co je pod myší:
objekt, zdroj, vodítko síly nebo hranu domény (ta vybere řešič). Výběr je
jeden pro viewport i síť: uzel vybraný kliknutím ve scéně svítí i v síti a
jeho parametry ukáže panel Parameters. Objekt pod myší se jemně rozsvítí,
vybraný má oranžový okraj a obrys.

![Viewport: vybraný objekt, gizmo pro posun v lokálních osách, lišta nástrojů vlevo](img/editor-objects.png)

| akce | jak |
|---|---|
| vybrat | klik; **Shift** nebo **Ctrl** přidá / ubere; klik do prázdna výběr zruší |
| nástroj | **Q** výběr, **W** posun, **E** rotace, **R** měřítko (lišta vlevo) |
| posun | táhnout šipku osy, čtverec roviny mezi dvěma osami, nebo tečku uprostřed (v rovině obrazovky) |
| rotace | táhnout kružnici osy, nebo vnější kružnici (kolem směru pohledu) |
| měřítko | táhnout kostičku na konci osy; prostřední kostička mění všechny tři |
| přichytávání | tlačítko s magnetem, nebo držet **Ctrl** při tažení: 5 cm, 15°, ×0,1 |
| lokální / světové osy | tlačítko na liště; měřítko je vždy v osách objektu |
| přidat | **Shift+A**, pravé tlačítko › Add, tlačítko + nebo menu **Add** |
| smazat, duplikovat | **Del** / **X**, **Ctrl+D** (kopie stojí vedle originálu a kolidují jako on) |
| zaostřit | **F** na výběr (nebo na všechno), dvojklik na objekt |
| zrušit tažení | **Esc** vrátí, co gizmo posunulo |
| kamera | levé tažení mimo gizmo obíhá, prostřední nebo Shift+levé posouvá, pravé tažení a kolečko přibližují |

Gizmo je stejně velké v každé vzdálenosti, osy mají barvy x červeně,
y zeleně, z modře a úchyt pod myší zežloutne. Během tažení ukazuje u myši,
o kolik se posunulo, pootočilo nebo zvětšilo. Tažení pracuje s hodnotami,
které uzel měl na začátku, takže se zaokrouhlování nesčítá, a v historii je
to jeden krok (**Ctrl+Z**). Dokud se táhne, simulace čeká: začne znovu až
s tím, kam objekt dopadl. Vybraných uzlů může být víc najednou, posouvají se
spolu a rotace je točí kolem společného středu.

Gizmo zná parametry, které uzel má (`NodeType::handles`): objekt a zdroj
mají polohu, rotaci a velikost, vír polohu, osu, poloměr a výšku, atraktor
polohu a poloměr, vítr jen směr (rotace ho otáčí).

**Add** (Shift+A, pravé tlačítko) přidá objekt (koule, kvádr, válec, kužel,
prstenec), zdroj (oheň, kouř) nebo sílu (vítr, vír, turbulence, atraktor,
odpor) tam, kam míří myš na podlaze. Objekt stojí na podlaze, dostane
vlastní barvu a rovnou se připojí do Colliders všech řešičů. Oheň a kouř se
připojí do řešiče, a když žádný není, vznikne i s vzhledem a výstupem.
Síly se připojí do Forces.

**G** zapne vodítka: obrys domény, zdroje (oranžově, se šipkou rychlosti a
drahou pohybu), víry a atraktory, šipky větru, obrys vybraných objektů.
Ikona fotoaparátu uloží snímek jako PNG ve dvojnásobném rozlišení.

### Časová osa a cache

Simulace běží ve vlastním vlákně a každý snímek si pamatuje. Oranžový pruh
na ose ukazuje, kam už je spočítaná. Přehrávání a posun po ose (klik,
tažení, **Space**, **Home**, **End**, šipky) pak nic nepočítají znovu.

- Změna, která mění simulaci (zdroj, síla, řešič), ji spustí znovu od
  snímku 1. Přehrávací hlava zůstane, kde byla, a viewport ukazuje poslední
  hotový snímek před ní, dokud ji simulace nedožene („simulating… 12 of 45“).
- Změna vzhledu (uzel Volume Look) snímek jen znovu vykreslí.
- Snímky se drží v poloviční přesnosti (half float): 6 B na buňku, při
  64 × 96 × 64 buňkách 2,4 MB na snímek, 150 snímků 350 MB. Paměť pro cache
  je 1,5 GB (menu **Simulation › Cache Size**); když dojde, simulace se
  zastaví a stavový řádek ukáže „full“.
- **Simulation › Simulate Ahead** vypne počítání dopředu,
  **Simulate Again** zahodí cache a začne znovu.

### Soubory, undo

**Ctrl+N/O/S**, **Ctrl+Shift+S**, příklady v **File › Examples**. Otevřený
příklad se ukládá přes Save As. **File › Render Image** uloží snímek,
**Render Frames** všechny spočítané snímky do složky. Dialog souborů ukazuje
složky a soubory dané přípony a cestu jde napsat.

**Ctrl+Z / Ctrl+Shift+Z** vrací celé stavy sítě. Tah posuvníkem nebo uzlem
je jeden krok, ne sto.

## 3. Síť simulace

```
[Pyro Source] ──Source───┐
[Turbulence] ───Force────┼──▶ [Pyro Solver] ──Gas──▶ [Volume Look] ──Look──▶ [Output]
[Object] ───────Collider─┘
```

Piny mají typ a barvu: **Source** oranžová, **Force** tyrkysová,
**Collider** modrá, **Gas** fialová, **Look** zelená. Výstup jde jen do
vstupu stejného typu. Vstupy řešiče Sources, Forces a Colliders berou
libovolný počet spojů (kreslí se jako obdélníček místo kolečka). Síly se
použijí v pořadí, v jakém byly připojené. Simuluje se jen to, co vede na
uzel Output.

Objekty jsou výjimka: patří do scény vždycky, kreslí se a vrhají stín, i
když nejsou nikam připojené. Připojené do Colliders řešiče mu navíc stojí
v cestě. Kulisa, která s ničím nekoliduje, je tedy objekt bez spoje.

Síť se překládá do popisu scény pro řešič
([`src/pg/sim/Network.h`](../src/pg/sim/Network.h)) a zároveň hlídá, co by
nedělalo, co uživatel čeká:

- chybí Output, vzhled nebo řešič (chyba, simulovat není co);
- řešič nemá zdroj; zdroj nic nepřidává; zdroj je mimo doménu nebo uvnitř
  překážky; překážka je mimo doménu (varování);
- neznámý typ uzlu ze souboru z novější verze se zachová a nahlásí.

### Uzly

**Objekty** (Objects) — pevná tělesa scény.

| uzel | parametry |
|---|---|
| Object | `shape` (sphere, box, cylinder, cone, torus, mesh); `file` (soubor OBJ, když je tvar mesh); `center` (poloha středu), `rotation` (stupně kolem x, pak y, pak z), `size` (šířka, výška, hloubka ve vlastních osách: průměr koule, hrany kvádru, šířka a tloušťka prstence, rozměry modelu); `color` |

Tvar ve vlastních osách vyplní `[-size/2, size/2]`: válec a kužel stojí
podél své osy y (kužel má špičku nahoře), prstenec leží v rovině xz a je
tlustý `size.y`, modelu se na `size` natáhne jeho obalový kvádr. Různé
velikosti tvar protáhnou, z koule je elipsoid. Změna barvy simulaci
nespouští znovu.

#### Modely z OBJ

![Příklad arch: kamenný oblouk a kámen ze souborů OBJ, vybraný oblouk s gizmem a cestou k souboru](img/editor-mesh.png)

Objekt i zdroj mohou mít tvar **mesh**: model ze souboru OBJ (kámen,
socha, auto, komín). **Add › Mesh…** (Shift+A) otevře dialog, model postaví
na podlahu tam, kam míří myš, a připojí ho do Colliders jako ostatní
objekty. Pokud je model v souboru obrovský nebo drobný (přes 3 m nebo pod
5 cm), zmenší nebo zvětší se na 80 cm. Jinak si nechá své rozměry.
V parametrech je cesta a tlačítko **…**. Relativní cesta se čte ze složky
souboru sítě (u vestavěných příkladů z `examples/sim`), takže síť a modely
jdou kopírovat spolu. V `.pgsim` je cesta v uvozovkách:
`param file "../models/arch.obj"`.

Z OBJ se čtou vrcholy a stěny ve všech zápisech (`f 1 2 3`, `1/1`, `1//1`,
`1/1/1`, záporné indexy). Mnohoúhelníky se rozdělí na trojúhelníky, normály,
textury a materiály se přeskočí. Čísla se čtou nezávisle na locale.

Pro simulaci se z trojúhelníků jednou upeče **pole vzdáleností** (SDF,
[`src/pg/sim/Mesh.h`](../src/pg/sim/Mesh.h)) na mřížce 48 buněk podél
nejdelší strany modelu. Postup je jako Bridsonův makelevelset3:

1. přesná vzdálenost k nejbližšímu trojúhelníku v úzkém pásu kolem stěn;
2. rychlé zametání (fast sweeping) ji rozšíří do celé mřížky;
3. vnitřek a vnějšek se určí sčítáním průsečíků paprsků podél x, y a z
   (lichý počet znamená uvnitř) a hlasováním dvou ze tří. Model s malou
   dírou se tak nepřevrátí naruby.

Z pole se pak rychle zjistí, jestli je bod uvnitř a jak je daleko od
povrchu: řešič podle toho označí pevné buňky a zdroj z modelu (hořící
auto) emituje v pásu pod povrchem. Stejný soubor (cesta, velikost, čas
změny) se čte a peče jen jednou, dokud ho něco používá.

Renderer kreslí skutečné trojúhelníky: rasterizuje je do bufferu (normála,
který objekt, vzdálenost), ze kterého hlavní průchod stínuje jako ostatní
tělesa. Normály se vyhladí mezi stěnami, které se lomí o méně než 60°, takže
kulaté zůstane kulaté a hrany kvádru ostré. Stíny na podlahu, na ostatní
tělesa i na kouř vrhá model paprskem pochodujícím polem vzdáleností
(nejvýš 4 modely se stínem, další se jen nakreslí).

**Zdroje** (Sources) — kde plyn vzniká.

| uzel | parametry |
|---|---|
| Pyro Source | `shape`, `file`, `center`, `rotation`, `size` jako u objektu; `fuel`, `smoke`, `heat` za sekundu; `velocity` (plyn opouští zdroj aspoň takhle rychle, ve vlastních osách zdroje, takže pootočený zdroj míří jinam); `flicker`, `flicker_size`, `seed` (blikotání šumem, který se zdrojem stoupá); `start`, `end` (časové okno: záblesk exploze); `motion` (static, circle, sway), `motion_size`, `motion_period` |

Koule je táborák, kvádr hořící poleno nebo průduch, prstenec plynový hořák.

**Síly** (Forces) — každá má `mask`: působí všude, jen kde je teplo (nebo
palivo), nebo jen kde je kouř.

| uzel | co dělá | parametry |
|---|---|---|
| Turbulence | náhodné víry, které se v čase mění | `strength`, `scale` (velikost vírů), `speed` (kolikrát za sekundu se mění), `seed` |
| Wind | vítr, stálý nebo v nárazech | `direction`, `speed`, `strength` (jak rychle plyn převezme rychlost větru), `gusts` |
| Vortex | točí plynem kolem osy, nese ho podél osy a nasává k ní | `center`, `axis`, `radius`, `height` (0 = celou doménou), `speed`, `lift`, `suction`, `strength` |
| Attractor | přitahuje k bodu, záporná síla odpuzuje | `center`, `radius`, `strength` |
| Drag | zpomaluje: hustý, klidný vzduch | `strength` |

**Pyro Solver** (Simulation):

| sekce | parametry |
|---|---|
| Domain | `size` (šířka, výška, hloubka v metrech; stojí na podlaze), `resolution` (buněk podél nejdelší strany, 16–256), `closed_floor` |
| Time | `fps`, `substeps`, `pressure_cycles`, `seed` |
| Motion | `buoyancy`, `weight` (tíha kouře), `vorticity` (víry, které hrubá mřížka rozmaže) |
| Combustion | `burn_rate`, `heat_release`, `soot_release`, `expansion`, `flame_life` |
| Dissipation | `cooling`, `smoke_decay` |

**Volume Look** (Render): barva a hustota kouře, `occlusion`; jas ohně,
teplota, kde začne žhnout (`flame_start`) a kde žhne do běla
(`flame_range`), `fire_light` (jak oheň svítí na kouř, podlahu a
překážky); slunce (`light_azimuth`, `light_elevation`, barva, jas),
obloha, `exposure`, `floor`. Jeho změna simulaci nespouští znovu.

**Output** (Render): `frames`, délka časové osy.

### Soubor .pgsim

Prostý text, jeden fakt na řádek, stabilní pořadí, takže se sítě dobře
porovnávají v gitu. Parametry se ukládají jen ty, které se liší od výchozích:

```
pgsim 1
# A campfire: a flickering ball of fuel just above the floor.
node 1 pyro_source 2 fire 0 0        # id, typ, verze typu, jméno, x y v editoru
  param fuel 14
  param heat 1
  param velocity 0 0.4 0
  param flicker 0.7
node 2 turbulence 1 turbulence 0 96
  param strength 3.5
node 3 pyro_solver 1 solver 250 28
  param buoyancy 0.9
node 4 volume_look 1 look 490 28
node 5 output 1 output 720 28
link 1.source -> 3.sources
link 2.force -> 3.forces
link 3.gas -> 4.gas
link 4.look -> 5.look
```

Volby se píšou jménem (`param motion circle`), přepínače `on`/`off`,
vektory třemi čísly. `bypass` na vlastním řádku uzel vynechá. Soubor
s neznámým parametrem nebo spojem, který nepasuje, se načte a výhrada se
vypíše; neznámý typ uzlu se zachová i s parametry, aby ho novější program
přečetl.

Soubory starších verzí se načtou: uzel si nese verzi svého typu a zastaralý
typ se při načtení převede na nástupce. Sphere Source a Box Source se stanou
Pyro Source s tvarem koule nebo kvádru (poloměr se převede na velikost),
Sphere Collider a Box Collider se stanou Object. Uložený soubor už má nové
typy. Test hlídá, že příklady jsou přesně v tom tvaru, v jakém je program
uloží.

### Příklady

![Příklady: táborák, pochodeň, oheň ve větru, exploze, kouř, kouř kolem koule, tornádo](img/sim-examples.png)

| příklad | co ukazuje |
|---|---|
| `campfire` | blikotající palivo, turbulence jen v teple, tmavé saze |
| `torch` | zdroj, který krouží: plamen se táhne za ním |
| `windy_fire` | vítr v nárazech, plameny se sklánějí, kouř odchází bokem |
| `explosion` | 0,2 s paliva s velkým rozpínáním: ohnivá koule, pak hřib sazí |
| `smoke` | teplý kouř ve slunci |
| `smoke_sphere` | kouř narazí do koule, rozlije se po ní a obteče ji |
| `tornado` | vír s nasáváním a zdvihem sbírá kouř z podlahy |
| `obstacles` | kouř mezi objekty: narazí do šikmé desky, vyteče po ní a stoupá k prstenci; koule na podlaze je kulisa bez kolizí |
| `arch` | modely z OBJ: vítr žene kouř kamenným obloukem a kolem kamene ([`examples/models`](../examples/models)) |

Soubory jsou v [`examples/sim`](../examples/sim) a CMake je zkompiluje do
programu. `pgshader sim campfire` proto funguje bez souborů vedle.
Táborák a kouř jsou zároveň scény, na kterých stojí testy řešiče: test
hlídá, že síť dává přesně `Scene::fire()` a `Scene::smoke()`.

## 4. Jak simulace funguje

Doména je krabice na podlaze rozdělená na krychlové buňky. Hrana buňky je
nejdelší strana domény dělená rozlišením a počty buněk se zaokrouhlí nahoru
na násobek 8, aby je multigrid mohl několikrát rozpůlit. V každé buňce jsou
pole:

| pole | význam |
|---|---|
| `density` | kouř, saze: to, co pohlcuje a rozptyluje světlo |
| `temperature` | teplo: zvedá plyn vzhůru |
| `fuel` | palivo, které ještě neshořelo |
| `flame` | palivo spálené během poslední `flame_life`: kde je plamen |
| `velocity` | rychlost proudění, tři složky |

Jeden krok ([`src/pg/sim/Pyro.h`](../src/pg/sim/Pyro.h)) má šest fází:

1. **emit** — zdroje přidávají palivo, kouř a teplo a tlačí plyn svým
   směrem. Váha klesá k okraji zdroje plynule (smoothstep), výkon kolísá
   podle šumu, který se zdrojem stoupá: plamen blikotá. Rychlost zdroje
   je „aspoň tolik“: plyn, který už letí rychleji, zdroj nebrzdí.
   Pohyblivý zdroj plyn strhává s sebou.
2. **advect** — proudění unáší všechna pole, včetně rychlosti samotné.
3. **combust** — část paliva shoří (`burn_rate` za sekundu) a změní se
   v teplo, saze a plamen. Hořící plyn se rozpíná.
4. **forces** — teplo stoupá, saze klesají, vorticity confinement vrací
   víry, pak síly sítě v pořadí spojů.
5. **project** — tlak zajistí, že plyn je nestlačitelný, kromě míst, kde se
   hořením rozpíná. Podlahou ani překážkou nic neproteče.
6. **dissipate** — kouř řídne, teplo chladne, plameny hasnou; co plyn nese,
   se zředí tam, kde se rozpíná.

### Unášení (advekce)

Nejjednodušší metoda je vzít hodnotu z buňky a posunout ji podle rychlosti.
Při velkém kroku je ale nestabilní a simulace vybuchne. Místo toho se
používá **semi-Lagrangeova metoda** (Stam, *Stable Fluids*, 1999): pro každou
buňku se zjistí, *odkud* sem plyn za krok přitekl, a hodnota se tam přečte
trilineární interpolací. Taková metoda je stabilní při libovolném kroku. Cesta
zpátky se počítá metodou Runge-Kutta 2. řádu: nejdřív půl kroku, pak celý krok
s rychlostí z poloviny cesty. Víry se díky tomu nerozplývají do spirál.

Interpolace ale rozmazává. Kouř, teplo, palivo a plamen se proto unášejí
metodou **MacCormack** (Selle a kol., 2008): výsledek se unese ještě jednou,
tentokrát proti proudu, a porovná se s tím, co v buňce bylo na začátku.
Tahle zkouška ukáže chybu tam a zpátky a výsledek se opraví o její
polovinu. Aby oprava nevytvořila nové extrémy, ořízne se na rozsah osmi
hodnot, ze kterých se interpolovalo. Na otevřených hranách se oprava
vypíná: cesta tam vede ven z domény, odkud se vrací nula, a „oprava“
by kouř u hranice přidávala.

### Mřížka MAC

Kouř, teplo, palivo a plamen jsou uprostřed buněk. Rychlost je
**posunutá** (MAC grid, Harlow a Welch, 1965): složka x leží na stěnách mezi
buňkami ve směru x, složka y na stěnách ve směru y a tak dál. Mřížka o `n`
buňkách má tedy v daném směru `n + 1` stěn. Divergence buňky, tedy kolik
plynu z ní vytéká, se pak spočítá přesně ze šesti stěn:

```
div = (u[i+1] − u[i] + v[j+1] − v[j] + w[k+1] − w[k]) / h
```

Na mřížce se vším uprostřed by se divergence počítala ob buňku. Sudé a liché
buňky by se pak navzájem „neviděly“ a tlak by v nich vytvářel šachovnici,
kterou projekce neodstraní. Posunutá mřížka navíc dělá překážky snadnými:
stěna mezi plynem a pevnou buňkou má rychlost nula a hotovo.

### Projekce a tlak

Plyn v simulaci je nestlačitelný: kolik do buňky přiteče, tolik z ní odteče.
Síly a advekce tuhle vlastnost porušují, a tak se na konci kroku opraví.
Najde se tlak `p`, jehož spád (gradient) přesně vyrovná divergenci, a
odečte se od rychlosti. Hledání tlaku je Poissonova rovnice:

```
součet přes otevřené stěny (p[soused] − p) / h² = div − rozpínání
```

Kde hoří palivo, má divergence odpovídat rozpínání plynu (`expansion`).
Tam plyn doopravdy přibývá.

Rovnice se řeší **geometrickým multigridem**
([`Poisson.h`](../src/pg/sim/Poisson.h)). Jednoduché iterace (Jacobi,
Gauss-Seidel) rychle srovnají chybu mezi sousedními buňkami, ale chyba,
která se táhne hladce přes celou doménu, se každým průchodem zmenší jen
nepatrně. Čím jemnější mřížka, tím hůř. Multigrid pošle hladkou část chyby
na mřížku s polovičním rozlišením, kde už tak hladká není, a opakuje to až
na pár buněk. Pak opravy cestou nahoru přičte. Takový V-cyklus ubere asi
90 % chyby **při jakémkoli rozlišení**. Stačí dva V-cykly na krok
(`pressure_cycles`), protože se začíná od tlaku z minulého kroku. Hladí se
red-black Gauss-Seidelem: buňky jsou obarvené jako šachovnice a každá
půlka čte jen buňky druhé barvy. Každá půlka tedy jde paralelně, a výsledek
přesto nezávisí na počtu vláken.

Hranice a překážky jsou v operátoru přímo:

- **Otevřené stěny** (boky, strop, podlaha, když není zavřená) drží na
  stěně tlak nula, jako atmosféra venku: buňka za stěnou má minus tlak
  buňky uvnitř.
- **Zavřená podlaha** je zeď: přes ni se nesčítá (Neumannova podmínka).
- **Překážky:** každá stěna buňky má koeficient 1 (otevřená) nebo 0 (vede
  do pevné buňky). Na hrubší mřížce multigridu je koeficient průměr čtyř
  jemných stěn, které hrubá stěna pokrývá. Pevné buňky mají tlak nula a
  jejich stěny rychlost nula. Tak se multigrid s překážkou sbíhá skoro
  stejně rychle jako bez ní.

### Otevřené hranice: vzduch venku stojí

Plyn, který přiteče otevřenou stěnou, přitéká z okolního klidného vzduchu:
jeho rychlost je nula a tlak mu teprve dá rychlost. První verze místo toho
vzala rychlost, kterou měl plyn u stěny (extrapolace, jak ji dělá řada
řešičů). Vír, který sahal ke stropu, pak nasával vzduch podél osy, ten
přitékal s rychlostí, kterou mu dal vír, vír ho zrychlil a stěna tu
rychlost poslala dovnitř znovu. Rychlost rostla bez konce (20 m/s po dvou
sekundách u víru, který točí rychlostí 1,2 m/s), až se kouř rozpadl na
jednotlivé body. Se stojícím vzduchem venku drží vír rychlost pod svou
(test `pyro_a_vortex_through_the_open_top_stays_bounded`). Stejná chyba
předtím potichu udržovala výstupný proud i po dohoření exploze: kouř pak
odcházel stropem rychleji, než měl.

### Rozpínání ředí

Semi-Lagrangeova advekce nese hodnotu tak, jak je: kus plynu, který se
nerozpíná ani nestlačuje, má po přesunu stejnou hustotu kouře. Tam, kde
hořící plyn bobtná, je to ale špatně: stejné množství paliva se rozprostře
do většího objemu. Bez ředění nabobtnalé palivo zůstalo stejně husté,
hořelo dál a znovu bobtnalo. Exploze tak během pár snímků vyplnila celou
doménu (100 % buněk v plameni, v testu
`pyro_burning_gas_thins_out_as_it_swells`). Kouř, palivo a plamen se proto
v buňce, která se za krok rozepne o `expansion × dt` svého objemu, zředí
dělením `1 + expansion × dt`. Teplota zůstává: plyn, který se ohřál,
zůstane horký.

### Síly

- **Vztlak:** `buoyancy × teplota − weight × kouř`. Působí na svislou složku
  rychlosti, tedy na stěny mezi buňkami nad sebou.
- **Vorticity confinement** (Fedkiw a kol., 2001): numerická difuze
  semi-Lagrangeovy metody rozmazává malé víry, a bez nich kouř vypadá jako
  vata. Síla najde místa, kde se plyn točí (rotace rychlosti `ω`), a točí jím
  dál: `ε · h · (N × ω)`, kde `N` míří k silnějším vírům.
- **Turbulence:** náhodná síla na hrubé mřížce (uzel každých `scale`),
  která se `speed`krát za sekundu plynule mění. Sama o sobě by plyn jen
  stlačovala a roztahovala; projekce z ní ponechá jen vířivou část. Díky ní
  se kouř trhá do vírů a plameny olizují.
- **Vítr** a **vír** plyn netlačí, ale **táhnou k rychlosti**: rychlost se
  každou sekundu přiblíží cíli o podíl daný `strength`
  (`v += (1 − e^(−strength·dt)) · (cíl − v)`). Jakkoli dlouho působí, nic
  nezrychlí nad svou rychlost. Vír má cíl složený ze tří částí: kolem osy
  `speed` (nejrychleji v půli poloměru, profil `4x(1 − x)`), podél osy
  `lift` a k ose `suction`. Na okraji válce plynule slábne. Tornádo
  vznikne, když vír s nasáváním a zdvihem sbírá kouř z podlahy.
- **Atraktor** zrychluje k bodu tím víc, čím je blíž (`(1 − d/r)²`), záporný
  odpuzuje. **Drag** rychlost exponenciálně tlumí.
- **Maska** omezí sílu na teplo (nebo palivo), nebo na kouř; podíl se
  odvodí z hodnot v buňkách u stěny, na které rychlost leží.

### Teplo a plamen zvlášť

Teplo musí vydržet dlouho, aby unášelo kouř vzhůru. Kdyby se oheň kreslil
podle teploty, vypadal by jako svítící sloup vysoký jako celý kouř. Houdini
to řeší stejně jako tahle simulace: **teplo** zvedá plyn a chladne pomalu,
**plamen** je čerstvě hořící palivo a vydrží zlomek sekundy (`flame_life`).
Oheň se kreslí z plamene a barvu dostane podle teploty.

## 5. Jak se kreslí

Renderer ([`src/pg/gl/Volume.h`](../src/pg/gl/Volume.h)) kreslí scénu ve
světových souřadnicích: doménu, jak stojí na podlaze, podlahu s mřížkou,
překážky a vodítka. Snímek dostane v poloviční přesnosti
([`sim::Frame`](../src/pg/sim/Frame.h)) a nahraje ho do 3D textury RGB16F
tak, jak je, bez převodu. Fragment shader pak pro každý pixel:

1. najde první pevné těleso na paprsku: překážku (koule a kvádry analyticky)
   nebo podlahu;
2. osvětlí ho sluncem (ve stínu kouře i ostatních těles), oblohou a září
   ohně;
3. projde plyn zepředu dozadu až k tělesu. Každý krok ubere světlo podle
   Beerova-Lambertova zákona a přidá světlo, které krok sám vyzáří nebo
   rozptýlí. V rámci kroku se to integruje analyticky: světlo, které krok
   přidá, ztlumí i on sám;
4. zapíše hloubku tělesa, aby vodítka kreslená potom zmizela za překážkou
   a pod podlahou.

Co je v obraze:

- **Stíny kouře** na sebe samý: paprsek z každé buňky ke slunci, na GPU
  v polovičním rozlišení. Každá vrstva 3D textury je jeden průchod
  fragment shaderu do této vrstvy (`glFramebufferTextureLayer`). Počítá se
  znovu jen při novém snímku, novém světle, hustotě nebo překážce.
  Překážky do stínu počítají taky.
- **Stín na podlaze:** paprsek z bodu podlahy ke slunci přes kouř a
  překážky.
- **Záře ohně** na podlaze a překážkách: doména se rozdělí na bloky (šest
  podél nejdelší strany) a každý blok je lampa se sečteným světlem svých
  plamenů. Sčítá se na GPU, znovu jen při novém snímku nebo novém vzhledu
  ohně.
- **Rozptyl:** Henyey-Greensteinova fázová funkce, převážně dopředná. Kouř
  proti slunci proto svítí na okrajích. Světlo oblohy se ztlumí tam, kde je
  v okolí hustý kouř (z mipmapy stejné textury).
- **Oheň** září jako černé těleso: barva podle Planckova zákona (aproximace
  Tannera Hellanda) od 1000 K do 3000 K, jas se čtvrtou mocninou teploty
  (Stefanův-Boltzmannův zákon). Saze v plameni pohlcují méně světla než
  vychladlé; žlutou barvu plameni doopravdy dávají právě rozžhavené saze.
- **Tone mapping** ACES (Narkowiczova aproximace), pak sRGB.
- **Proti artefaktům:** začátek paprsku i každý vzorek se posouvá o šum
  (interleaved gradient noise), jinak by vznikaly pruhy. Hustota u
  otevřených stěn a u stropu plynule mizí, jinak by hlava kouřového sloupce
  u stropu vypadala jako useknutá poklicí.

Viewport editoru kreslí znovu, jen když se něco změní: snímek, vzhled,
kamera, velikost, vodítka.

## 6. Výkon a determinismus

Krok řešiče na 4 jádrech (Xeon 2,1 GHz), táborák, bez vykreslování:

| rozlišení | buněk | ms na krok |
|---|---|---|
| 48 (32 × 48 × 32) | 49 tisíc | 15 |
| 72 (48 × 72 × 48) | 166 tisíc | 33 |
| 96 (64 × 96 × 64) | 393 tisíc | 63 |
| 144 (96 × 144 × 96) | 1,3 milionu | 190 |
| 192 (128 × 192 × 128) | 3,1 milionu | 404 |

Na jednom vlákně trvá krok při rozlišení 96 asi 218 ms, na čtyřech je tedy
3,4× rychlejší. Tlaková rovnice (dva V-cykly) z toho zabere 10 ms. Scéna bez
překážek se počítá jednodušším operátorem bez vah stěn, součty sousedů
překladač vloží přímo do smyčky hladiče a prodlužování z hrubé mřížky na
jemnou se dělá odděleně po osách. Předchozí verze bez překážek potřebovala
17 ms.

Simulace dodržuje invariant I5 jádra: stejná scéna dá **bitově stejná**
pole na libovolném počtu vláken. Každá smyčka je `pg::parallelFor` přes řádky
buněk, každou buňku zapisuje právě jeden kus práce a mezi buňkami se nic
nesčítá. Test to ověřuje se všemi prvky naráz: dva zdroje (jeden pohyblivý),
všechny síly a překážka, porovnání všech polí na 1 a na 4 vláknech.

## 7. Ověřování

[`tests/test_pyro.cpp`](../tests/test_pyro.cpp) (21 testů):

- interpolace mřížky; doména z násobků 8 buněk;
- projekce odstraní divergenci; výchozí dva cykly jí uberou přes 97 %;
  multigrid konverguje stejně rychle při každém rozlišení, i se zavřenou
  podlahou a s koulí uprostřed;
- horký kouř stoupá; palivo hoří na teplo a saze;
- zdroj přidává jen ve svém časovém okně, pohyblivý zdroj jde po své dráze,
  kvádrový zdroj plní kvádr;
- překážka drží plyn venku a proud ji obtéká; zavřenou podlahou nic
  neproteče; vítr odnáší kouř po větru;
- vír točí kolem osy, zdvih a nasávání působí, výška ho omezí; vír sahající
  ke stropu zůstane omezený; rozpínání ředí;
- bitově stejný výsledek na 1 a na 4 vláknech;
- nesmyslné vstupy (NaN, nulový krok, záporné rychlosti) řešič opraví;
- stíny; half float: přesné, zaokrouhlení k sudé, nekonečno, NaN.

[`tests/test_sim_network.cpp`](../tests/test_sim_network.cpp) (14 testů):
tabulka typů uzlů je konzistentní (a každá výchozí hodnota se zapíše a
přečte zpět stejně), jména a spoje, meze parametrů včetně čísel, která
se čtou stejně s každou standardní knihovnou, soubory tam a zpět, co se ze
souboru zachová, kompilace do scény a vzhledu, problémy sítě, příklady
dávají `Scene::fire()` a `Scene::smoke()`, všechny příklady běží a jsou
přesně v uloženém tvaru, soubory starších verzí se převedou, objekty jsou
ve scéně připojené i nepřipojené a nová barva nic nesimuluje znovu.

[`tests/test_shapes.cpp`](../tests/test_shapes.cpp) (5 testů): rotace na
úhly a zpět (i přes 180° a v gimbal locku), uvnitř a vně, vzdálenosti, kde
paprsek potká každý tvar (i pootočený a protažený) a s jakou normálou,
útlum zdroje ke kraji tvaru.

[`tests/test_mesh.cpp`](../tests/test_mesh.cpp) (6 testů): OBJ ve všech
zápisech stěn (i záporné indexy, mnohoúhelníky, CRLF, čísla nezávislá na
locale) a chybné soubory; pole vzdáleností proti přesné vzdálenosti
krychle; model s dírou je pořád správně uvnitř i vně; paprsky; model
umístěný ve světě (posunutý, pootočený, protažený); soubor se čte jednou a
po změně znovu. V testech sítě: cesta s mezerou a uvozovkami tam a zpět,
relativní cesta ze složky sítě, chybějící soubor nahlásí a zastoupí ho
kvádr.

Vše čisté pod AddressSanitizerem, UBSanem i ThreadSanitizerem. Pod TSanem a
ASanem běžel i editor s vláknem simulace a skriptovaným vstupem (`pgshader
--script`: myš, klávesy a snímky obrazovky ze souboru): přidání uzlu přes
Tab, spojení tažením, rychlé změny parametrů, které simulaci opakovaně
restartují, undo a redo, posun po časové ose, přepnutí sítí a konec
uprostřed kroku; výběr kliknutím, tažení gizma pro posun, rotaci i
měřítko, výběr více objektů, duplikace, mazání, přidání přes Shift+A a
kontextovou nabídku. Žádný data race ani chyba paměti v našem kódu;
hlášení zbyla jen uvnitř X11, GLX a Mesy, které pro sanitizery nejsou
instrumentované.

## 8. Jak přidat uzel

Editor, soubory i příkazová řádka berou uzly z jedné tabulky
([`src/pg/sim/Network.cpp`](../src/pg/sim/Network.cpp), `buildTypes()`).
Nový uzel se tam zapíše a všude se objeví sám: v nabídce Tab, v panelu
parametrů, v souborech i v `--set`.

1. **Fyzika**, pokud je nová: data do [`Scene.h`](../src/pg/sim/Scene.h)
   (třeba nový `ForceKind`), výpočet do [`Pyro.cpp`](../src/pg/sim/Pyro.cpp)
   (`addForce`), meze do `Scene::sanitized()`.
2. **Typ uzlu:** jméno, popisek, kategorie, nápověda, piny (typ určuje, co
   se smí spojit) a parametry. Každý parametr má sekci, druh (číslo, celé
   číslo, přepínač, vektor, barva, volba), výchozí hodnotu, rozsah
   posuvníku, fyzikální meze, jednotku a nápovědu.
3. **Kompilace:** v `Network::compile()` převést parametry na scénu.
4. **Vodítko** ve viewportu, pokud má uzel tvar:
   `gl::sceneGuides()` v [`Volume.cpp`](../src/pg/gl/Volume.cpp).
5. **Test:** test tabulky ho zkontroluje sám; přidat test fyziky.

## 9. Co je potřeba znát

- **Vektorový počet:** gradient, divergence, rotace (curl). Divergence říká,
  kolik z bodu vytéká, rotace jak moc se točí. Celá simulace se dá číst jako
  „posuň, přidej síly, odstraň divergenci“.
- **Navierovy-Stokesovy rovnice** pro nestlačitelnou tekutinu, bez
  viskozity: Eulerovy rovnice. Numerická difuze tu viskozitu stejně dodá.
- **Okrajové podmínky:** Dirichletova (daná hodnota, tady tlak nula na
  otevřené stěně) a Neumannova (daný tok, tady nulový tok zdí). Z §4 je
  vidět, že i „samozřejmá“ volba, co přitéká otevřenou stěnou, rozhoduje
  o stabilitě.
- **Stam, *Stable Fluids* (1999)** — základ celé třídy metod: semi-Lagrangeova
  advekce plus projekce.
- **Bridson, *Fluid Simulation for Computer Graphics*** — nejlepší kniha na
  začátek: MAC mřížka, projekce, hranice, pevná tělesa.
- **Fedkiw, Stam, Jensen, *Visual Simulation of Smoke* (2001)** — vorticity
  confinement.
- **Selle a kol., *An Unconditionally Stable MacCormack Method* (2008).**
- **Briggs, *A Multigrid Tutorial*** — multigrid srozumitelně.
- **Wrenninge, *Production Volume Rendering*** a **PBRT, kapitola o
  objemech** — raymarching, fázové funkce, stíny.
- **Dokumentace Houdini Pyro** — co z toho produkce používá a jak se to
  nastavuje: Pyro Solver, pole `flame`, `temperature`, `density`, `fuel`,
  POP Axis Force (předloha uzlu Vortex).

## 10. Omezení a co dělá produkce

Tahle simulace je prototyp, který ukazuje, jak Pyro funguje, a měří, kolik
to stojí. Oproti produkci:

1. **Hustá mřížka.** Počítá se každá buňka, i prázdná. Produkce ukládá jen
   buňky u kouře (OpenVDB, na GPU NanoVDB) a doména roste s kouřem. Rozhraní
   `Grid` je tu malé právě proto, aby se dalo vyměnit.
2. **CPU.** Houdini má řešiče v OpenCL, EmberGen simuluje v reálném čase na
   GPU. Každá fáze tu je smyčka přes buňky bez sdíleného stavu, takže se dá
   přímo převést na compute shader.
3. **Rychlost se unáší semi-Lagrangeovou metodou**, která rozmazává.
   MacCormack nebo BFECC i pro rychlost, případně FLIP, drží víry déle.
4. **Překážky stojí na místě.** Jsou to tvary (koule, kvádr, válec, kužel,
   prstenec) i modely z OBJ převedené na pole vzdáleností, ale pole má jen
   48 buněk podél modelu a jemné detaily v kolizích zmizí. V produkci se
   pole peče jemněji (a řídce, OpenVDB) a pohyblivé překážky předávají
   plynu svou rychlost.
5. **Jednoduchý rozptyl.** Produkční renderery (Karma, Arnold) počítají
   mnohonásobný rozptyl, díky kterému je hustý kouř uvnitř světlejší.
6. **Cache je v paměti.** Chybí zápis snímků na disk a export do `.vdb`.
7. **Simulace není uzel geometrické sítě.** Další krok je uzel
   `pyrosolver`: cook engine jádra už zná časovou závislost a cache
   snímků ([ARCHITECTURE.md §4.3](../ARCHITECTURE.md#43-čas-jako-dimenze-závislosti)).

## 11. Odkazy

- J. Stam: *Stable Fluids*, SIGGRAPH 1999.
- R. Fedkiw, J. Stam, H. W. Jensen: *Visual Simulation of Smoke*, SIGGRAPH 2001.
- A. Selle, R. Fedkiw, B. Kim, Y. Liu, J. Rossignac: *An Unconditionally
  Stable MacCormack Method*, J. Sci. Comput. 2008.
- R. Bridson: *Fluid Simulation for Computer Graphics*, 2. vyd., CRC Press 2015.
- W. L. Briggs, V. E. Henson, S. F. McCormick: *A Multigrid Tutorial*, SIAM 2000.
- M. Wrenninge: *Production Volume Rendering*, CRC Press 2012.
- M. Pharr, W. Jakob, G. Humphreys: *Physically Based Rendering*, 4. vyd.,
  kap. 11 a 14.
- J. Jimenez: *Next Generation Post Processing in Call of Duty: Advanced
  Warfare*, SIGGRAPH 2014 (interleaved gradient noise).
- SideFX: dokumentace Houdini, *Pyro* a *POP Axis Force*.
