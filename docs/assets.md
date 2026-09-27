# Digital assets

Asset je síť geometrických uzlů zabalená do jednoho uzlu. Má vlastní jméno,
vlastní parametry a verzi a dá se použít v kterékoli síti jako kterýkoli
jiný uzel. Odpovídá digital assetům (HDA) v Houdini.

Příklad: plot. Uvnitř je krabička jako sloupek, Copy to Points ji rozmístí
na body cesty a Detail Wrangle natáhne mezi body břevna. Zvenku je to jeden
uzel **Fence** se vstupem pro cestu a posuvníky Height, Post Width, Rails,
Rail Size a Color. Když se změní definice plotu, změní se každý plot ve
všech scénách.

![Scéna se dvěma assety: Fence podél čáry a Spiral Stairs, v dešti](img/assets-garden.png)

Příklad **garden_assets** (File › Examples) ukazuje schodiště a plot
v dešti. Oba uzly jsou assety, které program nosí s sebou.

## 1. Jak asset vznikne

1. Vyberte v síti geometrické uzly, ze kterých má být asset.
2. Zvolte **Edit › Make Asset…**, nebo klikněte pravým tlačítkem na uzel
   a zvolte **Make Asset…**.
3. Zadejte popisek (*Label*, např. „Blue Lift“). Jméno typu (*Name*,
   `blue_lift`) se z něj odvodí samo a dá se přepsat.
4. Potvrďte **Make**.

Co se stane:

- Vybrané uzly se přesunou do definice assetu. Na jejich místo přijde jeden
  uzel nového typu a zůstanou na něm všechna propojení.
- Každá geometrie, která do výběru vedla zvenku, se uvnitř stane uzlem
  **Asset Input** (index 0, 1, …). Asset tak dostane odpovídající vstupy.
- Asset vydává geometrii uzlu, ze kterého vede výstup ven. Když ven nevede
  nic, vydává geometrii uzlu s display flagem, případně jediného uzlu,
  který nekrmí žádný jiný. Tento uzel dostane uvnitř display flag.
- Definice se uloží do `~/.local/share/prototype/assets/JMÉNO.pgasset`
  (nebo do `$XDG_DATA_HOME/prototype/assets`) a přidá se do knihovny. Tab
  menu ji hned nabízí v kategorii **Assets**.

Asset nelze udělat ze simulačních uzlů (zdroje, řešiče, Output…). Dovnitř
patří jen geometrie. Asset má nejvýš čtyři vstupy a vydává geometrii jen
jednoho uzlu. Když výběr tyto podmínky nesplní, dialog napíše proč.

## 2. Uvnitř assetu

Do assetu se vstoupí dvojklikem na jeho uzel, klávesou **I** nad sítí,
položkou **Edit Contents** v menu uzlu nebo tlačítkem **Edit Contents**
v parametrech instance. Zpátky se vrací klávesa **U**, šipka nahoru
v hlavičce sítě nebo File › Back Up.

![Uvnitř assetu Fence: vstup path, sloupek, Copy to Points, wrangle s břevny](img/assets-inside.png)

Uvnitř se edituje síť definice stejně jako každá jiná. Hlavička sítě ukazuje,
kde jste (`scene › fence`), a stavový řádek připomíná, že U vede zpátky.

- **Vstupy.** Do uzlů Asset Input teče to, co je zapojené do instance, ze
  které jste vstoupili, a to ve snímku na časové ose. Plot uvnitř tedy stojí
  na stejné čáře jako ve scéně.
- **Hodnoty.** Parametry uvnitř mají hodnoty definice, tedy výchozí hodnoty
  každé nové instance. Instance, ze které jste přišli, může mít vlastní
  hodnoty (plot ve scéně je nižší než uvnitř).
- **Scéna čeká.** Simulace scény se nepřepočítává a viewport ukazuje jen
  geometrii assetu. Po návratu je scéna zase celá, včetně nasimulovaných
  snímků, pokud se asset nepodílí na tom, co se simuluje.
- **Nová verze.** Při návratu nahoru (nebo Ctrl+S, File › Save Asset) se
  změny uloží jako nová verze. Číslo verze se zvýší, soubor `.pgasset` se
  zapíše a všechny instance ve všech otevřených sítích se přepočítají podle
  nové definice. Když se nic nezměnilo, verze zůstává.
- Když nebyl zatržený display flag nebo definice jinak neprojde, návrat se
  nepovede. Chyba je ve stavovém řádku a zůstanete uvnitř, abyste ji mohli
  opravit.

Když není vybraný žádný uzel, panel parametrů ukazuje vlastnosti assetu:
**Label**, **Help** (text do tooltipu a do parametrů instance), seznam
promotovaných parametrů s tlačítkem × pro zrušení a seznam vstupů.

Assety se dají vnořovat: uvnitř assetu lze použít jiný asset a vstoupit
i do něj. Hlavička pak ukazuje celou cestu, např. `scene › house1 ›
window2`.

## 3. Parametry assetu (promote)

Parametr uzlu uvnitř se stane parametrem assetu takto: klikněte pravým
tlačítkem na jméno parametru a zvolte **Promote to the Asset**. Promotovaný
parametr má u řádku oranžovou značku. Po návratu nahoru ho má každá instance
v sekci pojmenované podle assetu, s posuvníkem, fx a klíči jako kterýkoli
jiný parametr:

- hodnota uvnitř je výchozí hodnota,
- každá instance nastavuje vlastní hodnotu,
- instance může mít na parametru klíče i výraz (`$F`, `ch()`), protože se
  vyhodnocují v síti instance.

Promotovat jde i parametr ze snippetu wranglu (`ch("height")` → posuvník
Height). V assetu Fence řídí výrazy krabičky sloupku
(`ch("../rails/height")`) posuvníky wranglu, takže jeden promotovaný
parametr pohne sloupky i břevny.

**Unpromote** ve stejném menu nebo křížek v přehledu assetu parametr vrátí
jen uzlu uvnitř. Instance o něj přijdou při příští verzi. Hodnoty, které
instance ve scéně měly, se při načtení zahodí s varováním.

Položka **Copy Reference** ve stejném menu zkopíruje
`ch("../uzel/parametr")` pro výraz jiného parametru.

## 4. Soubory

Asset je obyčejná síť `.pgsim`, jen má na začátku řádky navíc:

```
pgsim 1
asset fence 3 "Fence"
help "A fence along a path: a post on each of its points, ..."
promote rails height height "Height"
promote paint color color "Color"
node 1 asset_input 1 path 0 130
node 2 box 1 post 0 0
  expr size.y "ch(\"../rails/height\")"
...
```

- `asset JMÉNO VERZE "POPISEK"` — jméno typu, verze a popisek.
- `help "…"` — nápověda (nepovinná).
- `promote UZEL PARAMETR JMÉNO "POPISEK"` — parametr uzlu uvnitř se stane
  parametrem assetu. Prázdný popisek znamená popisek parametru.

Soubor `.pgasset` jde otevřít i přímo (File › Open Asset… nebo
`prototype cesta/k/fence.pgasset`) a editovat bez scény. Ctrl+S pak uloží
novou verzi.

**Kde program assety hledá**, v tomto pořadí (pozdější přepíše dřívější
stejného jména):

1. assety, které program nosí s sebou (`examples/assets`, zakompilované):
   **Fence** a **Spiral Stairs**,
2. složky v proměnné `PROTOTYPE_ASSETS` (oddělené dvojtečkou),
3. uživatelská složka `$XDG_DATA_HOME/prototype/assets`, jinak
   `~/.local/share/prototype/assets` — sem ukládá editor.

**Síť nese své assety s sebou.** Když se ukládá síť, která asset používá,
zapíše se na její konec i jeho definice:

```
definition fence
| pgsim 1
| asset fence 3 "Fence"
| ...
end
```

Soubor se tak otevře i na jiném počítači, kde asset v knihovně není.
Novější verze vyhrává: definice ze souboru nahradí knihovní jen tehdy, když
je knihovní starší. Novější plot z vaší složky tedy nepřepíše starší kopie
v souborech a starší scéna dostane nový plot.

`prototype sim` a ostatní příkazy načítají knihovnu stejně jako editor.
`--set fence.height=0.8` nastaví promotovaný parametr instance.

## 5. Co asset odmítne

| Situace | Co se stane |
|---|---|
| asset uvnitř sebe, i přes jiný asset (A v B, B v A) | nová verze se odmítne: „it has itself inside (through B)“ |
| žádný uzel s display flagem | nová verze se odmítne |
| jméno, které má vestavěný uzel (`box`) | odmítne se |
| promotovaný parametr, který uvnitř není | definice se odmítne (v souboru se řádek zahodí s varováním) |
| hodnota nebo klíč parametru, který instance už nemá | při načtení se zahodí s varováním |
| instance assetu, který knihovna nezná (soubor bez definice) | síť se načte, uzel hlásí chybu, dokud se asset neobjeví |

## 6. Jak to funguje

- **Knihovna** (`pg/sim/Asset.h`, `AssetLibrary`) drží definice podle
  jména. Každá změna zvýší číslo revize knihovny. `GeometryGraph` ho
  sleduje, takže si instance novou definici všimnou při příští
  synchronizaci. Nahrazené definice knihovna nemaže, protože na typ jejich
  uzlu se ještě může dívat editor.
- **Typ instance** se skládá z definice: vstupy podle uzlů Asset Input,
  jeden výstup, parametry podle `promote` s výchozími hodnotami z definice.
  `findNodeType` hledá nejdřív vestavěné typy, potom knihovnu.
- **Vaření.** Uzel instance v jádře (`AssetNode`) drží kopii sítě definice
  a vlastní `GeometryGraph`. Před každým vařením zapíše do kopie hodnoty
  promotovaných parametrů v čase snímku (klíče a výrazy instance už jsou
  vyhodnocené) a předá vstupy uzlům Asset Input. Přepočítá se jen to, co se
  uvnitř změnilo, jako v každé jiné síti. Chyby uzlů uvnitř hlásí instance
  jako `fence/rails: …`.
- **Časová závislost.** Asset je časově závislý, když jeho výstupní uzel
  závisí na čase, např. wrangle čte `$F` nebo parametr má klíče. Odpověď se
  pamatuje pro revizi knihovny, protože asset uvnitř se mohl změnit.
- **Cykly.** Při přidání definice se pod zámkem knihovny kontroluje, jestli
  síť neobsahuje sama sebe, přímo ani přes jiné assety. Z definic, které by
  tvořily cyklus, by tu poslední kontrola odmítla, takže v knihovně cyklus
  nikdy není a vaření nemůže běžet donekonečna.

## 7. Omezení (zatím)

- Uvnitř jsou jen geometrické uzly. Simulace se do assetu nezabalí.
- Asset má nejvýš čtyři vstupy a jeden výstup.
- Uvnitř se ukazují výchozí hodnoty, ne hodnoty instance, ze které jste
  vstoupili.
- Asset nejde zamknout (Houdini má lock/unlock). Každá změna uvnitř je
  změnou definice pro všechny instance.
- Undo uvnitř vrací jen změny uvnitř. Verze uložené při návratu undo ve
  scéně nevrátí.
- Parametry assetu nemají vlastní rozvržení (složky, podmínky
  viditelnosti). Jsou v jedné sekci v pořadí promotování.
