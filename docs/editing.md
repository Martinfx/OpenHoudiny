# Úpravy geometrie ve viewportu: body, hrany, plochy a štětec

Zobrazenou geometrii (uzel s display flagem, [geometry.md](geometry.md))
jde upravovat přímo ve viewportu, jako v Houdini: myší vybrat body, hrany
nebo plochy, posunout je, otočit a zvětšit úchytem, udělat z nich skupinu,
smazat je, a štětcem namalovat atribut — třeba `pin` nebo `tear` látce
([cloth.md](cloth.md)).

Nic z toho není skryté kouzlo. Každá úprava je **obyčejný uzel sítě**,
který editor vloží za zobrazený uzel: **Edit**, **Group**, **Blast**,
**Attribute Paint**. V jeho parametrech je to, co udělala myš — vybrané
prvky jako vzor (`339 363-364 387-390`), tahy štětce jako seznam kapek.
Síť zůstává procedurální: undo funguje jako u každé jiné změny, uzel jde
vypnout (bypass), přesunout, upravit v parametrech, uložit se sítí, a co
je před ním, jde dál měnit.

![Body vybrané obdélníkem, zvednuté úchytem: uzel Edit s měkkým poloměrem 0,7 m udělal z mřížky kopec](img/edit-points.jpg)

![Štětec maluje atribut pin podél dvou okrajů látky; s Ctrl kus zase smazal](img/edit-paint.jpg)

## 1. Rychlý start

```bash
./build/prototype --example shade_sail    # plachta: rohy zvednuté Editem, piny namalované štětcem
```

1. Zobrazte uzel s geometrií (praporek na pravém konci uzlu, nebo **R**
   nad ním v síti) — třeba Grid.
2. Myš nad viewport, **2**: body. Klik vybere bod, tažení levým tlačítkem
   obdélník.
3. **W** a tažení šipkou úchytu: body se posunou a za zobrazeným uzlem
   přibude uzel **Edit**. V jeho parametrech nastavte **Soft Radius**
   a body kolem půjdou s nimi — z mřížky je kopec.
4. **Ctrl+G** udělá z vybraného skupinu (uzel **Group**), **Delete** ho
   smaže (uzel **Blast**).
5. **P** a tažení po geometrii maluje atribut `pin` (uzel **Attribute
   Paint**); s **Ctrl** maže. **P** znovu malování ukončí.

## 2. Co se vybírá

| Klávesa | Tlačítko v liště | Co klik vybírá |
|---|---|---|
| **1** | krychle | objekty scény — uzly (jako dosud) |
| **2** | čtyřúhelník s tečkami v rozích | body zobrazené geometrie |
| **3** | čtyřúhelník se zvýrazněnou stranou | hrany |
| **4** | vyplněný čtyřúhelník | primitivy: polygony a křivky |
| **P** | štětec | nic — maluje |

V režimech 2–4 viewport ukáže drátěný model geometrie, v režimu bodů
i všechny body. Prvek pod myší svítí **tyrkysově**, vybrané jsou
**žluté**. Vpravo dole je napsáno, co je pod myší (`point 446`,
`edge 6-7`, `primitive 17`) a kolik je vybráno.

Přepnutí režimu výběr **převede**: z bodů na primitivy (ty, jejichž
všechny body byly vybrané), z primitiv na body (jejich rohy), z primitiv
na hrany (jejich strany), z bodů na hrany (hrany mezi vybranými body).

Vybírá se jen to, co je **vidět**: bod, hranu či plochu, kterou zakrývá
vlastní povrch geometrie, klik ani obdélník nevezme a značky za povrchem
nejsou vidět. Výběr drží čísla bodů a primitiv zobrazené geometrie;
zůstane i po undo a po vložení dalšího uzlu, dokud má geometrie stejně
bodů a primitiv.

## 3. Myš a klávesy

| Vstup | Co udělá |
|---|---|
| klik | vybere prvek pod myší (a nic jiného); klik do prázdna výběr zruší |
| **Shift**+klik, **Ctrl**+klik | přidá, ubere |
| tažení levým | obdélník: bod, který v něm leží, hranu, jejíž oba konce v něm leží, primitivum, jehož střed v něm leží; se Shift přidá, s Ctrl ubere |
| **Alt** nebo **mezerník** + tažení levým | otáčí pohledem — levé tlačítko v těchto režimech vybírá |
| prostřední tažení, pravé tažení, kolečko | posun pohledu, přiblížení (jako vždy) |
| **Ctrl+A**, **Ctrl+I**, **Esc** | vybere vše, obrátí výběr, zruší výběr |
| **F** | zarámuje vybrané |
| **W**, **E**, **R** | posun, otočení, měřítko vybraného úchytem; **Q** úchyt skryje |
| **Ctrl+G** | skupina z vybraného (Group) |
| **Delete**, **X** | smaže vybrané (Blast) |
| **P** | štětec zapnout / vypnout |
| **[** , **]**, **Shift**+kolečko | menší / větší štětec |
| **Ctrl** při malování | maluje hodnotou Erase Value (maže) |
| **Esc** při tažení | vrátí, co tažení udělalo |

Stejné položky jsou v pravém kliku do viewportu a v nabídce Help.
Mezerník přehrává a zastavuje, až když ho pustíte, a jen pokud jste
s ním netočili pohledem.

## 4. Posun, otočení, měřítko: uzel Edit

První tah úchytem na vybraném vloží za zobrazený uzel uzel **Edit** —
dostane vstup zobrazeného uzlu, jeho výstup vede tam, kam vedl výstup
zobrazeného, a převezme display flag — a nastaví mu:

| Parametr | Co v něm je |
|---|---|
| Elements | vybrané prvky jako vzor (§7) |
| Class | Points, nebo Primitives (pak se hýbou body vybraných primitiv) |
| Translate, Rotate, Scale | co udělal úchyt; rotace ve stupních kolem x, pak y, pak z |
| Pivot | střed vybraného při prvním tahu: kolem něj se otáčí a zvětšuje |
| Soft Radius | jak daleko kolem vybraného body jdou s ním: úplně u něj, vůbec ve vzdálenosti Soft Radius, mezi tím `(1 − (d/r)²)²` |

Další tahy se **stejným výběrem** nastavují tentýž Edit: otočení
a měřítko se skládají přesně (otočení kolem středu úchytu, měřítko podél
os Editu — úchyt pak má jeho osy; jinak by se tvar zkosil). Jiný výběr
dostane nový Edit za tím prvním. Klik na úchyt bez pohnutí žádný uzel
neudělá; **Esc** během tahu vrátí hodnoty, a pokud tah Edit teprve
vytvořil, vrátí síť, jak byla. **Ctrl** během tahu přichytává po krocích
(5 cm, 15°, ×0,1), jako u objektů.

Hrany se posouvají svými body: Edit dostane vzor hran (`p3-4`)
a třídu Points.

## 5. Skupina a mazání

**Ctrl+G** vloží uzel **Group** se jménem `group1` (první, které
geometrie ještě nemá), třídou a vzorem vybraného. Jméno přepište
v parametrech. Skupina hran je skupinou jejich bodů — geometrie jádra
skupiny hran nemá.

**Delete** vloží uzel **Blast**:

| Vybráno | Co zmizí |
|---|---|
| body | body a primitivy, které o bod přijdou |
| primitivy | primitivy a body, které používaly jen ony |
| hrany | primitivy, jejichž jsou stranou (jako *Delete Edges* v Blenderu) |

Blast umí i obráceně (Keep): nechat jen vybrané.

## 6. Štětec: Attribute Paint

**P** nad viewportem maluje do zobrazeného uzlu **Attribute Paint**;
pokud zobrazený uzel žádný Attribute Paint není, vloží se za něj nový
a vybere se, takže jeho parametry jsou hned po ruce:

| Parametr | Co dělá |
|---|---|
| Attribute | co se maluje — `pin`, `tear`, `mass`… (`@pin` ve wranglu) |
| Value | co štětec nanáší |
| Erase Value | co nanáší s **Ctrl** |
| Radius | velikost štětce (**[** **]**, **Shift**+kolečko) |
| Strength | kolik z hodnoty kapka nanese uprostřed |
| Default | kde body začínají, když atribut ještě nemají |
| Strokes | namalované kapky: kolik jich je, a **Clear** |

Tah klade **kapky** po čtvrtině poloměru. Každá kapka je koule: bod
uvnitř se posune k hodnotě kapky o `Strength × (1 − d²/r²)²`, uprostřed
nejvíc, na okraji vůbec; kapky se nanášejí v pořadí, v jakém byly
namalované. Tah smí začít mimo geometrii — maluje, kde štětec leží na
povrchu, a když z něj sjede a vrátí se, nezačne malovat přes díru.

Kapky jsou **místa, ne čísla bodů**: zjemněte mřížku před Attribute
Paint a barva zůstane, kde byla. Celočíselný atribut zůstane celočíselný
(zaokrouhlený).

Během malování je geometrie obarvená podle malovaného atributu: modrá
0, přes tyrkysovou a žlutou do červené 1 (větší hodnoty než 1 se
zmenšují podle největší). Kroužek štětce leží na povrchu pod myší.
**P** znovu (nebo Q, W, E, R, 1–4) malování ukončí; uzel, který P
vložilo a do kterého se nic nenamalovalo, zase zmizí.

Pro látku ([cloth.md](cloth.md)): bod s `pin` nad 0,5 je přišpendlený,
`tear` násobí mez trhání (0,5 se trhá dvakrát dřív — perforace), `mass`
je hmotnost bodu v kg. Příklad **shade_sail**: plachta napnutá mezi čtyři
sloupy, dva rohy zvednuté Editem s měkkým poloměrem, rohy přišpendlené
čtyřmi kapkami `pin`; vítr ji nafukuje.

![Příklad shade_sail: plachta mezi čtyřmi sloupy ve větru](img/shade-sail.jpg)

## 7. Vzory prvků

Parametry Group, Edit a Blast berou prvky jako **vzor**, jako skupinová
pole uzlů v Houdini. Viewport je tak píše a dají se psát i ručně:

| Vzor | Co vybere |
|---|---|
| `0-9 12 20-30` | čísla a rozsahy (rozsah i obráceně: `9-0`) |
| `*` | vše |
| `pin_group` | skupinu podle jména; skupina druhé třídy se převede (body primitiv, primitivy se všemi body ve skupině) |
| `p3-4` | hranu mezi body 3 a 4; pro body oba body, pro primitivy ta, jejichž je stranou |
| `p0-1-2-3` | cestu tří hran |
| `^…` | ubere: `* ^0-9` je vše kromě prvních deseti |

Položky se oddělují mezerami nebo čárkami a platí popořadě. Co nic
nepojmenuje (číslo za posledním, skupina, která není, hrana, kterou
geometrie nemá), nevybere nic. Viewport píše nejkratší vzor: rozsahy čísel
a hrany spojené do cest (`p0-1-2-3-4 p9-10`).

## 8. Jak to funguje

- **Vzory** — `src/pg/core/Selection.h`: `selectElements` (vzor → maska
  bodů nebo primitiv), `patternOf`, `edgesOf`, `selectEdges`,
  `edgePatternOf`.
- **Co je pod myší** — `src/pg/core/Pick.h`, `ElementPicker`: polygony
  rozložené na trojúhelníky (vějíř), strom obálek (BVH, dělení mediánem,
  listy po čtyřech). Paprsek najde nejbližší plochu; stejně daleké plochy
  rozhoduje pořadí trojúhelníků, takže výsledek nezávisí na tom, jak se
  strom rozdělil. Prvek je vidět, když paprsek od oka k němu nepotká
  plochu blíž než 0,1 % vzdálenosti před ním. Obdélník se vyhodnocuje na
  více vláknech, prvek po prvku. Strom se staví jen pro nové body nebo
  topologii: malováním se geometrie mění, ale sdílí body (copy-on-write),
  a strom zůstává.
- **Značky** — overlay rendereru (`gl::Overlay`): drát, body, výběr,
  zvýraznění pod myší (vlastní vrstva, aby pohyb myši nepřestavoval
  zbytek) a barvy malování, kreslené s testem hloubky proti scéně, kousek
  blíž k oku než povrch. Tečka se přitáhne o tolik, kolik z povrchu kolem
  sebe pokrývá — víc, když je povrch vidět šikmo — takže ji povrch
  neusekne. Široké čáry jsou dva trojúhelníky (core profil OpenGL širší
  čáry než pixel nemá). Rendery záběru značky nemají.
- **Skládání úprav** — `sim::EditTransform` (`src/pg/sim/Shape.h`):
  Edit dělá `x → R S (x − p) + p + t`; tah úchytu kolem středu `c`
  složený za něj je znovu Edit: otočení předřazené `R`, měřítko násobí
  `S`, `t` vezme `p + t` tam, kam ho vezme tah. Testy ověřují, že to
  platí bod po bodu a že Edit v uzlu počítá totéž.
- **Uzly** — `src/pg/nodes/Edit.cpp`: `groupcreate`, `edit`,
  `attribpaint`; Blast v `Modifiers.cpp`. Attribute Paint nanáší kapku po
  kapce jen na body v jejím dosahu (strom bodů), takže tisíce kapek na
  jemné síti jsou rychlé.
- **Editor** — `tools/prototype/SimElements.cpp`.

Geometrie ležící na podlaze (mřížka v y = 0) se s podlahou už nebije:
plocha geometrie vyhrává, když je stejně daleko jako podlaha.

## 9. Omezení

- Upravuje se **zobrazená geometrie**, ne výsledek simulace: piny se malují
  na vstup Cloth Solveru, ne na spočítanou látku.
- Výběr, Edit, Group a Blast drží **čísla** bodů a primitiv. Změna před
  nimi, která body přečísluje (jiný počet řad mřížky), posune, co
  pojmenovávají. Malování to nepostihne — kapky jsou místa.
- Zakrývání bere v úvahu jen vlastní povrch zobrazené geometrie, ne
  objekty ani kusy před ní.
- Zatím chybí výběr lasem a štětcem, režim vrcholů (rohů), *Dissolve* hran,
  symetrie a čísla bodů ve viewportu.
- Drát a všechny body se kreslí do 400 000 hran či bodů; ve větší geometrii
  jen výběr. První výběr v síti milionů trojúhelníků postaví strom obálek
  (řádově sekunda).
