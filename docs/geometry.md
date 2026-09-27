# Geometrie v síti: uzly jako SOP v Houdini

Síť simulace (editor `pgshader`, soubory `.pgsim`) má kategorii
**Geometry**: uzly, které geometrii vyrábějí a upravují — krychle, koule,
mřížka, rozházené body, transformace, kopie na body, wrangle… Počítá je
geometrické jádro (`src/pg/core`, viz [ARCHITECTURE.md](../ARCHITECTURE.md)),
stejné, na kterém stojí `pgdemo`. Geometrie se:

- **ukazuje ve viewportu** — uzel s *display flagem* (modrý praporek na
  pravém konci uzlu) — a v **tabulce atributů** (Geometry Spreadsheet);
- **stává tvarem simulací** — vstup *Shape* objektu (překážky), zdroje
  kouře a zdroje vody;
- **vrací ze simulací** — částice vody, kapky deště a mřížky plynu jako body
  a objemy, které jdou dál upravovat dalšími uzly.

![Editor: částice vody jako body obarvené wranglem podle rychlosti, tabulka jejich atributů a síť s display flagem na uzlu speed_color](img/editor-geometry.png)

![Oheň z rozházených bodů (scatter_fire), částice vody obarvené podle rychlosti (liquid_points) a déšť na kamenech z kopií koule (rock_garden)](img/geometry.png)

## 1. Rychlý start

```bash
./build/pgshader --example liquid_points      # částice vody jako body, wrangle je barví
./build/pgshader --example scatter_fire       # oheň z bodů rozházených po mřížce
./build/pgshader --example rock_garden        # kameny z kopií koule, déšť na nich
./build/pgshader sim rock_garden rocks.png    # bez okna: poslední snímek do PNG
./build/pgshader sim liquid_points out/p.png --every 5 --set look.surface=on
```

`pgshader sim` kreslí i zobrazenou geometrii. Síť, která nic nesimuluje
(jen geometrie, bez uzlu Output), vykreslí zobrazenou geometrii s pohledem
nastaveným na ni.

## 2. Uzly

| Uzel | Co dělá |
|---|---|
| **Box**, **Sphere**, **Tube** | Uzavřené tělo z polygonů, stěny otočené ven (normála podle Newella); krychle s dělením stěn, koule z pásů, válec s víky |
| **Grid** | Mřížka čtyřúhelníků v rovině xz, stěny nahoru (+y) |
| **Line** | Otevřená lomená čára bodů |
| **Point Cloud** | Volné body v krychli, stejné pro stejné seed |
| **File** | Body, polygony a čáry ze souboru OBJ; relativní cesta od složky sítě; soubor, který se změní, se načte znovu |
| **Transform** | Posun, rotace, měřítko po osách a celkové |
| **Merge** | Spojí geometrie ve vstupu, který bere libovolně spojů — v pořadí spojů |
| **Switch** | Pustí dál jeden ze vstupů podle indexu |
| **Attribute Create** | Atribut jedné hodnoty (číslo nebo vektor) na bodech, rozích, primitivech nebo celé geometrii |
| **Color** | Barva `Cd` bodů nebo primitiv |
| **Group Box** | Skupina bodů uvnitř krabice |
| **Blast** | Smaže body skupiny — nebo naopak nechá jen je (Keep) — i s primitivy, které ztratí bod |
| **Point Wrangle** | Snippet nad každým bodem: posouvá body, barví je, vyrábí atributy (jazyk níže) |
| **Normal** | Normály bodů `N`, průměr stěn kolem bodu vážený plochou |
| **Scatter** | Body rozházené po polygonech úměrně ploše, deterministicky podle seed; barvy a další atributy se interpolují z rohů, `N` ze stěny |
| **Copy to Points** | Kopie geometrie na každý bod druhého vstupu: velikost `pscale` × Scale, natočená +y do `N` (Align), s atributy bodu (kromě P, N, pscale) |
| **Null** | Nic nemění: jméno, na které se dá ukázat, konec řetězce |
| **Liquid Points** | Částice vody z Liquid Solveru: `P`, rychlost `v`, pěna `foam` |
| **Rain Points** | Kapky deště a kapičky odstřiků: `P`, `v`, `droplet` (1 u kapičky) |
| **Gas Volume** | Plyn z Pyro Solveru jako tři objemy: `density` (kouř), `temperature`, `flame` |

Geometrické uzly se dají **obejít** (bypass, B): obejitý uzel pustí dál,
co do něj vstupuje. Síť odmítne spoj, který by udělal smyčku.

### Wrangle

Snippet Point Wrangle je **kód** — víceřádkové pole s neproporcionálním
písmem; použije se, když se klikne jinam. Chyba v kódu je u uzlu vidět
(červený odznak, text v parametrech i v tabulce).

```c
@Cd = vec3(0.05, 0.2, 0.6) + vec3(0.9, 0.75, 0.4) * clamp(length(@v) / 2.5, 0, 1)
@pscale = 0.6 + 0.9 * abs(noise(@P * 2.5))
```

Příkazy se oddělují středníkem. Čte `@P`, `@ptnum`, `@numpt`, `@Time`,
`@Frame` a atributy bodů; funkce `sin cos abs sqrt floor pow min max clamp
length noise fit vec3`. Typ nového atributu se odvodí z pravé strany.

## 3. Display flag a viewport

Každý geometrický uzel má na pravém konci praporek. Klik na něj (nebo **R**
nad sítí) z uzlu udělá **zobrazený**: jeho geometrie je ve viewportu,
v renderech a v `pgshader sim`. Klik na praporek zobrazeného uzlu zobrazení
vypne. Zobrazený je vždy nanejvýš jeden uzel; nově přidaný geometrický uzel
dostane praporek, když není zobrazené nic — nebo když navazuje na ten
zobrazený (přidaný tažením spoje z jeho výstupu), jako další krok řetězce.
Praporek se ukládá do souboru.

Jak se geometrie kreslí:

- **polygony** — trojúhelníky (vějíř přes každý uzavřený polygon), osvětlené
  sluncem a oblohou jako objekty scény, se stínem kouře a objektů; barva
  z `Cd` rohu, jinak bodu, primitiva, celé geometrie, jinak světle šedá;
  normály z `N` bodů, jinak z plošek kolem rohu, které se od něj ohýbají
  méně než o 60° (koule vypadá kulatě, krychle má hrany);
- **otevřené čáry** — úsečky v barvě `Cd`;
- **body, které nepoužívá žádný polygon** — kulaté tečky stínované jako
  kuličky; s `pscale` mají poloměr `pscale`, jinak pár pixelů;
- **objemy** — rámeček kolem a tečka v každém neprázdném voxelu, od modré
  přes purpurovou k žluté podle hodnoty; u velkých objemů jen každý druhý,
  třetí… voxel (tečky jsou pak větší), z objemů nejvýš 400 tisíc teček.

Klávesa **F** bez výběru zarámuje i zobrazenou geometrii; když síť nic
nesimuluje, kamera ji zarámuje sama.

Water Look má přepínač **Surface**: vypnutý hladinu nekreslí — voda se
simuluje dál a je vidět jen to, co z ní ukazuje síť (částice přes Liquid
Points).

## 4. Tabulka atributů

Tlačítko s tabulkou v záhlaví panelu parametrů přepne na **Geometry
Spreadsheet** — geometrii vybraného geometrického uzlu, jinak zobrazeného.
Nahoře počty (body, rohy, primitiva, objemy); pod nimi třídy:

- **Points** — `P` první, pak atributy podle jména, vektory po složkách
  (`P[x]`, `P[y]`, `P[z]`), a skupiny bodů jako sloupce 0/1;
- **Vertices** — bod každého rohu a atributy rohů;
- **Primitives** — uzavřené/otevřené, body primitiva, atributy;
- **Detail** — atributy celé geometrie;
- **Volumes** — jméno, rozlišení, velikost voxelu, počátek, minimum,
  maximum a průměr hodnot.

Řádky se kreslí jen viditelné (virtualizace), takže tabulka zvládne i
stovky tisíc bodů: částice vody se dají procházet za běhu simulace.

## 5. Geometrie jako tvar simulací

Objekt (Object), zdroj kouře (Pyro Source) a zdroj vody (Water Source) mají
vstup **Shape**. Když je v něm geometrie, je jejich tvarem místo vlastního:

- geometrie se vezme **ve snímku 1** (stejně jako tvary ze souborů: tvar
  během simulace nemění — pohyb přijde s animací);
- z uzavřených polygonů se udělá trojúhelníková síť a z ní pole
  vzdáleností (SDF, 64 buněk na nejdelší straně) — stejné jako u modelu
  z OBJ, takže objekt vrhá stín, kreslí se a srážejí se s ním plyn, voda
  i déšť;
- geometrie **bez polygonů** (jen body) dá kuličku kolem každého bodu:
  dvacetistěn o poloměru `pscale`, jinak 5 cm; nejvýš 20 000 bodů;
- vnitřek se určuje paprsky podél os a parita průsečíků se počítá **pro
  každou slupku zvlášť** (trojúhelníky spojené rohy na stejném místě);
  bod je uvnitř, je-li uvnitř kterékoli slupky. Překrývající se tvary —
  kuličky kolem bodů, kopie, sloučená tělesa — tak vyplní i svůj průnik.
  Slupka vnořená do jiné (dutina) se tím vyplní;
- stejná geometrie (podle obsahu, `hash`) dává stejnou síť — upéct se
  jednou, dokud ji někdo drží;
- prázdná geometrie → varování a místo ní vlastní tvar uzlu; geometrie ze
  simulace (Liquid Points…) tvarem být nemůže — simulace v té chvíli ještě
  neproběhla — a řekne to varování.

Parametry vlastního tvaru (tvar, poloha, rotace, velikost) pak platí jen
jako náhrada; gizmo ve viewportu takový uzel nehýbe — hýbe se uzly
geometrie (třeba Transform, nebo střed krychle). Souhrn uzlu ukáže „shape of
*jméno*“.

## 6. Simulace zpátky jako geometrie

**Liquid Points**, **Rain Points** a **Gas Volume** mají vstup ze
simulace (Liquid, Rain, Gas) a na výstupu geometrii snímku, který je právě
vidět: v editoru z cache snímků, v `pgshader sim` ze snímku právě
spočítaného. Za nimi jdou libovolné geometrické uzly — wrangle, color,
blast… — a výsledek se zobrazí nebo prohlíží v tabulce.

- Snímky simulace drží částice vody jen tehdy, když je nějaký Liquid
  Points napojený na simulovaný Liquid Solver (jinak by se jejich pozice
  a rychlosti ukládaly zbytečně: 19 bajtů na částici a snímek).
- Uzel napojený na řešič, který nevede do Output (a tedy se nesimuluje),
  dostane varování a je prázdný.
- Ze snímku, který ještě není spočítaný, je geometrie prázdná.

## 7. Jak to funguje

Síť editoru a graf jádra jsou dvě různé věci: síť (`sim::Network`) je
model pro editor a soubory, jádro (`pg::Graph` + `CookEngine`) počítá.
Most mezi nimi je **`sim::GeometryGraph`** (`src/pg/sim/GeometryGraph.h`):

```
[Box] -> [Transform] -> [Scatter] ...      síť (Network.h)
  n3 ------> n4 -------> n7                 graf jádra, uzly "n<id>"
```

- `sync(net)` graf přizpůsobí síti: uzly, které zmizely nebo změnily typ,
  smaže (`Graph::remove`), nové vyrobí (`NodeType::core` říká typ jádra),
  nastaví parametry a propojí vstupy. **Parametr se nastaví jen tehdy,
  když se změnil** — nezměněný nic neznehodnotí, takže tah posuvníkem na
  konci řetězce padesáti uzlů přepočítá jeden uzel. Beze změny sítě je
  `sync` levný (porovná revizi), jen znovu zkontroluje velikost a čas změny
  souborů, které čtou File uzly.
- Obejitý uzel se ve spojích vynechá: co ho krmí, krmí to, co krmil on.
- Přepojování jde ve dvou krocích (nejdřív odpojit, pak zapojit), aby otočení
  A → B na B → A nevypadalo cestou jako smyčka.
- `cook(id, frame)` vyhodnotí uzel líně (pull): přepočítá se jen to, co je
  zastaralé. Cache jádra je klíčovaná (uzel, verze, snímek). **Verze jsou
  globální čítač**, takže uzel smazaný a vyrobený znovu na stejné adrese
  nemůže trefit starou položku cache.
- Uzly, které čtou simulaci (`FrameNode`), dostanou před vařením snímek
  pro dané číslo snímku. Co z kterého snímku udělaly, zůstává v cache
  jádra, dokud je snímek tentýž — přehrávání tam a zpátky se nepočítá
  znovu. Snímek simulovaný znovu (jiný objekt se stejným číslem) uzel
  znehodnotí.
- Chyby vaření (`Node::cookError()`) — soubor, který nejde přečíst, chyba
  ve wrangle — se ukazují u uzlů.

Editor drží jeden `GeometryGraph` po celou dobu: `compile()` z něj bere
tvary (a nevaří znovu, co je hotové) a viewport z něj každý snímek bere
zobrazenou geometrii. Pro GPU ji `sim::displayOf()` (`src/pg/sim/Display.h`)
převede na ploché pole trojúhelníků, teček a čar — na CPU a testovaně.
Trojúhelníky jdou do stejného G-bufferu jako modely z OBJ: normála, index
tělesa a vzdálenost na pixel; zobrazená geometrie má místo indexu barvu
zakódovanou jako záporné číslo (8 bitů na kanál), takže ji hlavní shader
nasvítí stejně jako objekty. Tečky jsou `GL_POINTS` s velikostí podle
`pscale`, stínované jako kulička, a čáry jdou stejným programem jako
vodítka.

## 8. Soubor .pgsim

Text parametru (Text, Code i cesta k souboru) se zapisuje v uvozovkách;
nové řádky, tabulátory, uvozovky a zpětná lomítka escapované (`\n`, `\t`,
`\"`, `\\`), takže `#` v kódu není komentář. Zobrazený uzel má řádek
`display`:

```
node 7 point_wrangle 1 speed_color 720 170
  param snippet "@Cd = vec3(0.05, 0.2, 0.6) + vec3(0.9, 0.75, 0.4) * clamp(length(@v) / 2.5, 0, 1)"
  display
link 6.geometry -> 7.geometry
```

## 9. Omezení

- Tvar z geometrie je statický (snímek 1); pohyblivé tvary přinese animace.
- Vnořené slupky (dutina uvnitř tělesa) se vyplní; otevřená plocha (grid)
  nemá vnitřek — jako překážka je tenká.
- Kulička kolem bodu je dvacetistěn a pole vzdáleností má 64 buněk na
  nejdelší stranu: malé body ve velkém mračnu jsou hrubé.
- Zobrazená geometrie nevrhá stín, neodráží se ve vodě a nedá se kliknutím
  vybrat ve viewportu.
- Objemy se kreslí jako tečky, ne jako kouř.
- Jazyk wrangle nemá řídicí struktury ani lokální proměnné (viz ARCHITECTURE).
- Vaří se na vlákně okna: velmi těžká geometrie (miliony bodů ve Scatter)
  editor na chvíli zastaví.
