# Wrangle: jazyk pro výpočty nad geometrií

Wrangle je uzel, do kterého se píše kód. Ten běží pro každý bod, primitivum
nebo roh geometrie, nebo jednou pro celou geometrii. Jazyk se podobá VEX
z Houdini: C-like syntaxe, typy, proměnné, cykly, vlastní funkce a stovka
vestavěných funkcí. Čte a zapisuje atributy, dívá se na sousedy, sahá do
dalších vstupů a umí geometrii stavět i mazat.

![Točité schodiště z Detail Wrangle -- vlastní funkce staví schod, cyklus je otáčí -- a voda, která po něm stéká](img/wrangle-stairs.png)

```c
// Bod po bodu: zvednout podle šumu, obarvit podle výšky.
float h = 0, amp = 1;
vector p = @P * ch("frequency");
for (int i = 0; i < 5; i++) {      // fraktální šum: pět oktáv
    h += amp * (noise(p) - 0.5);
    p *= 2;
    amp *= 0.5;
}
@P.y = h * ch("height");
@Cd = lerp({0.2, 0.35, 0.1}, {0.9, 0.9, 0.95}, fit(@P.y, 0, 0.3, 0, 1));
```

## 1. Rychlý start

- **V editoru:** Tab (v síti) nebo Shift+A (ve viewportu) → *Point
  Wrangle*, *Primitive Wrangle* nebo *Detail Wrangle*. Kód se píše do pole
  *Snippet* a použije se, když se klikne jinam. Chyba se ukáže u uzlu
  červeně i s řádkem a sloupcem, varování žlutě a výstup `printf()` pod
  parametry.
- **Parametry z kódu:** `ch("height")` v kódu udělá z *height* posuvník pod
  snippetem (`chi` celé číslo, `chv` vektor, `chs` text). Hodnoty se ukládají
  do souboru sítě a dají se animovat klíči jako každý jiný parametr.
- **Příklad:** `./build/prototype --example spiral_stairs` — Detail Wrangle
  postaví točité schodiště (vlastní funkce `stair()` pro jeden schod, cyklus
  je otáčí kolem středu, `ch()` dává posuvníky *steps*, *rise*, *turn*)
  a voda po něm stéká.

## 2. Uzly

| Uzel | Běží | Typické použití |
|---|---|---|
| **Point Wrangle** | pro každý bod | posun, barva, atributy, sousedé, mazání bodů |
| **Primitive Wrangle** | pro každé primitivum; `@P` je jeho střed | barva ploch, mazání ploch, výpočty nad rohy |
| **Detail Wrangle** | jednou pro celou geometrii | stavba geometrie (`addpoint`, `addprim`), součty, detail atributy |

Všechny tři jsou tentýž uzel s jiným výchozím **Run Over** (Points,
Primitives, Vertices, Detail), který jde přepnout. **Group** omezí běh na
prvky jedné skupiny — nebo na prvky vzoru: čísla a rozsahy `0-9 12`, hrany
`p3-4`, `*`, `^` ubírá ([editing.md](editing.md#7-vzory-prvků)); Tab ve
viewportu ho vyplní vybranými prvky. Uzel má **čtyři vstupy**: první je geometrie, nad kterou
běží a kterou mění, další tři jsou jen ke čtení (`point(1, "P", i)`,
`@opinput1_P`, `npoints(2)` …).

## 3. Jazyk

### Typy

| Typ | Příklad |
|---|---|
| `int` | `int n = 3;` — 32 bitů, přetečení se zalomí |
| `float` | `float t = 0.5;` |
| `vector2`, `vector`, `vector4` | `vector c = {1, 0.5, 0};`, `set(x, y, z)`, `vector4 q = quaternion(M_PI, {0, 1, 0});` |
| `matrix3`, `matrix` | `matrix3 m = ident(); rotate(m, M_PI / 4, {0, 1, 0});` |
| `string` | `string s = sprintf("piece%d", @ptnum);` |
| pole | `int list[] = {3, 1, 2};`, `vector pts[];`, `string names[] = split("a b c");` |

Číslo s tečkou je `float`, bez tečky `int`. `int` se v aritmetice sám mění
na `float` a `float` na vektor (stejné číslo ve všech složkách):
`@P * 2`, `@Cd = 0.5`.

### Proměnné, podmínky, cykly, funkce

```c
int n = 0;
for (int i = 0; i < 10; i++) {
    if (i == 3) continue;
    if (i == 7) break;
    n += i;
}
foreach (int pt; neighbours(0, @ptnum)) n++;
foreach (int i; vector p; pts) { ... }      // i je index
while (n > 0) n--;
do { n++; } while (n < 5);

float twice(float x) { return x * 2; }   // funkce kdekoli v kódu
void bump(int k) { k += 5; }             // proměnnou, kterou dostane, změní -- jako VEX
```

`return;` v hlavním kódu ukončí běh pro tento prvek. Funkce nesmějí volat
samy sebe (rekurze). Parametry se píší jako v C (`float a, float b`), nebo
jako ve VEX (`float a, b; vector c`).

### Operátory

`+ - * / %`, porovnání `== != < <= > >=` (vektory a řetězce `==`, `!=`),
`&& || !`, bitové `& | ^ ~ << >>` na `int`, `?:`, `= += -= *= /= %=`,
`++ --`. Vektory se počítají po složkách; `vector * matrix3` otáčí,
`vector * matrix` transformuje bod; `+` spojuje řetězce.

**Dělení je vždy desetinné:** `7 / 2` je `3.5` (jako v Pythonu 3, ne jako
v C a VEX). Na `3` se zkrátí až při uložení do `int`: `int k = 7 / 2;`.
Dělení nulou dává nulu, ne nekonečno.

### Atributy

| Zápis | Co znamená |
|---|---|
| `@P`, `@Cd`, `@pscale` | atribut prvku, pro který kód běží |
| `i@id`, `f@mass`, `v@dir`, `u@st`, `p@orient`, `s@name` | typ, když atribut vzniká: int, float, vector, vector2, vector4, string |
| `@group_top` | členství ve skupině `top` (0 nebo 1); zápis skupinu vytvoří |
| `@opinput1_P` | P stejného prvku ze vstupu 1 |
| `@ptnum`, `@primnum`, `@vtxnum`, `@elemnum` | číslo prvku |
| `@numpt`, `@numprim`, `@numvtx`, `@numelem` | počty |
| `@Time`, `@Frame`, `@TimeInc` | čas v sekundách, snímek, délka snímku |

- Atribut, který ještě není, **vznikne zápisem**. Typ má podle předpony,
  jinak podle zvyklostí Houdini (`P N v Cd up` vektory, `orient` vector4,
  `id` int, `name` string), jinak podle toho, co se do něj poprvé zapíše
  (`@dir = {1, 0, 0}` je vektor). Celé číslo bez předpony se ukládá jako
  `float` (`@count = 1`); celočíselný atribut chce `i@`.
- Čtení atributu, který není, dá nulu. Point Wrangle přečte i atribut
  detailu; při běhu nad rohy (Run Over: Vertices) se čte i atribut bodu
  a primitiva, kterým roh patří.
- V Primitive Wrangle je `@P` střed primitiva (jen ke čtení).

### Parametry a čas

`ch("name")` (také `chf`), `chi`, `chv`, `chs` čtou parametr uzlu, který kód
sám vytvořil (viz výše). Tentýž jazyk počítá i **výrazy v parametrech**
libovolného uzlu — `$F * 0.1`, `ch("../box1/sizex") * 2` — viz
[animation.md §6](animation.md#6-výrazy). `$F` je číslo snímku, `$T` čas v sekundách, `$FPS`
snímková frekvence. Uzel, který čte `$F`, `@Time` nebo animovaný parametr,
se počítá znovu na každém snímku; ostatní jen při změně.

## 4. Vestavěné funkce

| Skupina | Funkce |
|---|---|
| Matematika | `sin cos tan asin acos atan atan2 sinh cosh tanh exp log log10 sqrt pow abs sign floor ceil round rint frac trunc min max clamp lerp fit fit01 fit10 fit11 efit smooth smoothstep step fmod radians degrees`, konstanty `M_PI M_TWO_PI M_PI_2 M_E M_SQRT2` |
| Vektory | `length length2 normalize dot cross distance distance2 reflect avg sum`, `set()`, `vec3()` |
| Šum a náhoda | `noise` (0 až 1; do vektoru dá tři různé), `curlnoise`, `rand`/`random` (stejné číslo pro stejné semínko, vždy a všude) |
| Řetězce | `sprintf itoa atoi atof strlen len concat toupper tolower startswith endswith find replace strip split join match` |
| Pole | `len append push pop insert removeindex removevalue resize find sort argsort reverse slice isvalidindex min max sum avg array()`; záporný index počítá od konce |
| Matice, kvaterniony | `ident transpose invert determinant rotate scale translate dihedral lookat quaternion qmultiply qrotate qinvert qconvert slerp eulertoquaternion` |
| Čtení geometrie | `point prim vertex detail npoints nprimitives nvertices primpoints primpoint primvertexcount primvertex vertexpoint vertexprim vertexprimindex pointprims pointvertices neighbours neighbourcount nearpoints nearpoint pcfind inpointgroup inprimgroup expandpointgroup expandprimgroup npointsgroup nprimitivesgroup haspointattrib hasprimattrib hasvertexattrib hasdetailattrib getbbox_min getbbox_max getbbox_center getbbox_size relbbox prim_normal primarea primcentroid` |
| Změny geometrie | `addpoint addprim addvertex removepoint removeprim setpointattrib setprimattrib setvertexattrib setdetailattrib setpointgroup setprimgroup geoself` |
| Výstup | `printf warning error` |

- `point(1, "P", i)` vrátí typ, jaký atribut má; jméno i vstup se hledají
  jednou za běh, ne pro každý prvek.
- `nearpoints(0, @P, 0.5)` vrací body do vzdálenosti 0,5 od nejbližšího
  (stejná vzdálenost: podle čísla bodu), `nearpoints(0, @P, 0.5, 8)` nejvýš
  osm. Stojí na k-d stromu, který se postaví jednou za běh.
- `addprim(0, "poly", a, b, c)` uzavřený polygon, `"polyline"` otevřená
  čára; body i jako pole: `addprim(0, "poly", pts)`.
- `setpointattrib(0, "mass", pt, 1.0, "add")` — režimy `set add min max mult`.

## 5. Jak to běží

- **Paralelně, nebo popořadě.** Kód, který sahá jen na svůj prvek, běží
  po blocích na všech jádrech. Kód, který volá `addpoint`, `removepoint`,
  `setpointattrib`… nebo zapisuje textový atribut, běží popořadě na jednom
  vlákně. Detail Wrangle běží vždy jednou.
- **Změny geometrie počkají na konec běhu.** Nové body a primitiva,
  `set…attrib` a mazání se provedou až po posledním prvku, v pořadí, v jakém
  o ně kód požádal. Každý prvek tedy vidí geometrii tak, jak přišla:
  `@numpt` se během běhu nemění a `point(0, …)` čte vstup, ne to, co kód už
  zapsal. Číslo, které vrátí `addpoint`, platí pro `addprim` a
  `setpointattrib` v témže běhu.
- **Výsledek nezávisí na počtu vláken.** `rand` a `noise` hashují hodnotu,
  ne pořadí výpočtu, bloky se dělí jen podle počtu prvků a změny se řadí
  podle prvků. Ověřuje to test na 1 a 4 vláknech.
- **Rychlost:** interpret typovaného stromu. Na 4 jádrech zvládne čistou
  aritmetiku nad milionem bodů za 37 ms (27 Mbodů/s), šum za 26 ms. První,
  jednoduchý jazyk bez proměnných a cyklů byl na stejném stroji o 10 % (aritmetika) až
  25 % (šum) rychlejší. Náhradou za interpret má být JIT
  ([ROADMAP.md §5](../ROADMAP.md#5-potom)).

## 6. Chyby

- Syntax: `line 3, col 12: expected ';'` — hned při psaní, uzel geometrii
  pustí dál beze změny.
- Typy: `cannot turn a string into a float`, `unknown variable 'hieght'`,
  `@ptnum is read only` — při vaření, s řádkem.
- `error("…")` běh zastaví se zprávou; `warning("…")` přidá varování;
  `printf("…")` píše pod parametry uzlu (prvních dvanáct řádků).
- `ch("x")` parametru, který neexistuje, dá nulu a varování.

## 7. Rozdíly proti VEX

- Dělení celých čísel je desetinné (viz výše).
- Bez rekurze, bez `#include`, `export` a `struct`.
- Atributy polí a matic zatím nejsou (pole a matice jen jako proměnné).
- `removepoint` smaže i primitiva, která bod používala; `addvertex` jen do
  primitiva, které kód sám vytvořil; `pcfind` hledá jen podle `P`.
- Chybí `xyzdist`, `primuv`, `intersect` a další dotazy na plochy (přijdou
  s uzly Ray a Attribute Transfer).
- Čísla z `rand` a `noise` jsou jiná než v Houdini (jiný hash, hodnotový
  šum místo Perlinova).

## 8. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/lang/Lang.h` | veřejné API: `Program` (snippet), `Expression` (výraz parametru), `Host` (`ch()`, `$F`) |
| `src/pg/lang/Parse.cpp` | lexer, parser, kontrola jmen a počtu argumentů funkcí |
| `src/pg/lang/Check.cpp` | typová kontrola a vazba na atributy pro konkrétní geometrii |
| `src/pg/lang/Eval.cpp`, `Run.cpp` | interpret, paralelní a uspořádaný běh, odložené změny geometrie |
| `src/pg/lang/Builtins.cpp`, `BuiltinsGeo.cpp` | vestavěné funkce |
| `src/pg/core/Spatial.h` | k-d strom bodů a sousednost (hrany, primitiva bodu) |
| `src/pg/nodes/Wrangle.cpp` | uzel: Run Over, Group, čtyři vstupy, `ch()` z parametrů uzlu |
| `tests/test_lang.cpp` | 27 testů: jazyk, uzel, parametry z `ch()` v síti, výrazy v parametrech |
