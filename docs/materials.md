# Materiály a textury

Každá plocha může říct, **z čeho je**: beton, omítka, cihla, okno, ocel,
dřevo, kůra, tráva… Řekne to řetězcovým atributem primitiv `material`.
Renderery podle něj kreslí povrch: Cycles fotografií, pokud ji knihovna
má, jinak procedurálním vzorem, a vždy s drsností a kovovostí toho
materiálu. Path tracer klade stejné fotografie, ale bez reliéfu, a bere
drsnost a kovovost.

![Vzorník materiálů v Cycles](img/materials.jpg)

*Vzorník (Cycles). Přední řada fotografie: beton, omítka, cihlová zeď, dřevo,
kůra, půda, a procedurální lom betonu. Prostřední řada: cihla, malta, okno,
ocel, kov, asfalt, kámen. Zadní řada: střecha (fotka betonu), list, tráva,
sklo, bez materiálu.*

## 1. Rychlý start

```bash
# Ulice: omítka, okna s místnostmi za sklem, dřevěné dveře, střechy, asfalt.
./build/prototype sim street ulice.png --renderer cycles
# Demolice: věž z betonu a omítky, lom betonu na kusech, které padají.
./build/prototype sim demolition odstrel.png --renderer cycles
# Bez fotografií (jen vzory a barvy):
./build/prototype sim street ulice.png --renderer cycles --set output.render_textures=0
```

Ve wrangle stačí jeden řádek:

```c
s@material = "plaster";                 // Primitive Wrangle
if (@group_glass) s@material = "window";
```

## 2. Materiály

| Jméno | Co Cycles nakreslí | Drsnost / kov |
|---|---|---|
| `concrete` | **fotka** betonu se skvrnami, nahoře šmouhy stékající vody | 0,85 / 0 |
| `broken_concrete` | lom betonu: ostrohranné kamínky kameniva v cementu, rozervaný reliéf, tmavší prohlubně | 0,95 / 0 |
| `brick` | líc jedné cihly: skvrny, tmavší zrnka, drsný pálený povrch | 0,85 / 0 |
| `brick_wall` | **fotka** cihlové zdi i s maltou (na zeď, která je jedna plocha) | 0,85 / 0 |
| `mortar` | malta: zrnitý písek, místy špinavější | 0,95 / 0 |
| `plaster` | **fotka** omítky, šmouhy pod okny, skvrny po celé fasádě | 0,8 / 0 |
| `window` | sklo s místností za ním: každé okno jinak tmavé nebo světlé (záclony, světlo), teplejší či studenější, odráží oblohu | 0,04 / 0 |
| `glass` | sklo (průhledné, jako Glass Fracture) | 0 |
| `steel` | ocel výztuže, místy rezavá (rez je drsná a nekovová) | 0,45 / 0,8 |
| `metal` | plech: místy hladší, místy drsnější | 0,3 / 1 |
| `asphalt` | asfalt: dehet a kamínky, světlejší vyjeté pruhy, tmavší záplaty | 0,9 / 0 |
| `wood` | **fotka** dřeva s kresbou | 0,65 / 0 |
| `stone` | kámen: zrna několika barev, skvrny, nerovný | 0,75 / 0 |
| `roof` | plochá střecha: **fotka** betonu v šestimetrové dlaždici, skvrny po vodě | 0,8 / 0 |
| `bark` | **fotka** kůry (svislé rýhy, hluboký reliéf) | 0,9 / 0 |
| `leaf` | list: některé žlutší, některé tmavší, po skupinách v koruně | 0,5 / 0 |
| `grass` | tráva: sušší v několikametrových skvrnách | 0,6 / 0 |
| `soil` | **fotka** hlíny | 0,95 / 0 |

Neznámé jméno nebo `""` znamená bez materiálu: povrch má barvu `Cd`
a „detail“ z [cycles.md](cycles.md) (§3). Atributy `roughness`
a `metallic`, pokud je geometrie má, mají přednost před drsností a kovovostí
materiálu.

**Barva zůstává vaše.** Fotka i vzor kreslí kolem barvy `Cd`: průměrná
barva povrchu je pořád `Cd` a fotka ji jen zesvětluje a ztmavuje. Fasáda
tak má barvu, jakou jí dal asset, a každá cihla Brick Wall svůj odstín. Výjimkou je
cihlová zeď (`brick_wall`). Malta je světlejší než cihly, takže by se jasnou
`Cd` přepálila do bílé, a proto se kreslí ve vlastních barvách fotky.

### Kdo materiál nastaví sám

| Uzel | Materiál |
|---|---|
| **Brick Wall** | `brick`, `mortar`, `plaster` |
| **Concrete Fracture** | `concrete`, na plochách lomu `broken_concrete` |
| **Glass Fracture** | `glass` |
| **Rebar** | `steel` (i trubky prutů, které kreslí RBD Solver) |
| **Tree** | `bark`, `leaf` |
| **Grass** | `grass` |
| **Building** (asset) | `plaster`, `window`, dveře `wood`, střecha `roof` |

**Uzel Material** nastaví materiál skupině ploch (Group: jméno skupiny,
čísla a rozsahy `0-9 12`, `*`; prázdné znamená všechny). Volba None materiál
odebere.

### Kusy, které letí

RBD Solver kreslí kusy tam, kde zrovna jsou. Kdyby se fotka nebo vzor
počítaly z té polohy, po kusu by „tekly“. Každý bod si proto nese `rest`,
tedy kde byl, než se pohnul, a textura se kreslí podle `rest`. Kus si tak
svoji kresbu nese s sebou, i když se otáčí.

Plochy lomu (skupina `inside`) dostanou při kreslení `broken_concrete`.
Víčko řezu totiž zdědí materiál vedlejší plochy (omítku, nátěr, okno), ale
uvnitř zdi omítka není. Výjimkou jsou materiály, které jsou stejné skrz
naskrz: cihla, kámen, dřevo, kov a sklo zůstanou samy sebou.

## 3. Fotografie (textury)

Knihovna, která je součástí programu, leží v
[examples/textures](../examples/textures/README.md): `concrete`, `plaster`,
`brick_wall`, `wood`, `bark`, `soil` a `roof`. Každá sada obsahuje
`color.jpg`, `height.jpg` a `texture.txt` (velikost dlaždice v metrech,
hloubku reliéfu, průměrnou barvu a `tint`). Fotky pocházejí z Bistro od
Amazon Lumberyard a šíří se pod licencí CC-BY 4.0; autoři jsou uvedeni
v README knihovny.

**Jak se fotka klade.** Geometrie nemá UV, takže se fotka promítá ze tří
stran najednou (triplanárně). Z každé osy přispívá tolik, kolik se tím směrem
plocha dívala, umocněno na čtvrtou. Stěna má tedy fotku zepředu, podlaha
shora a šikmá plocha prolnutí obou. Promítá se podle `rest` a normály v
`rest`, takže letící kus nese svou kresbu. Každá ze tří projekcí je posunutá,
aby se dlaždice na hranách nepotkaly ve stejném místě. Na fasádách navíc
leží velké skvrny a šmouhy, takže opakování dlaždic není vidět.

**Výška** dělá v Cycles reliéf (Bump): spáry mezi cihlami, rýhy v kůře,
póry v betonu. **Drsnost** se bere z mapy, pokud ji sada má, jinak z
materiálu a v prohlubních je o něco vyšší.

### Vlastní textura na objekt

Uzel **Material** má sekci Texture:

| Parametr | Co dělá |
|---|---|
| **Texture** | obrázek barvy (`.jpg`, `.png`, `.exr`) ze sady, např. z Poly Haven nebo ambientCG; ostatní mapy se najdou vedle něj podle jmen: `_diff_`/`_rough_`/`_disp_` (Poly Haven), `_Color`/`_Roughness`/`_Displacement` (ambientCG); funguje i složka s `texture.txt` |
| **Texture Size** | kolik metrů pokryje jedna dlaždice (0: podle `texture.txt`, jinak 2 m) |
| **Tint by Color** | zapnuto: barva `Cd` místo barvy fotky (fotka kolem ní světlá a tmavne); vypnuto: fotka, jak je |

Zapíše `s@texture`, `f@texture_size` a `i@texture_tint`, takže totéž jde
udělat i ve wrangle. Relativní cesta se čte od složky sítě.

### Na uzlu Output

| Parametr | Co dělá |
|---|---|
| **Textures** | fotografie zapnuté nebo vypnuté (vypnuté: jen vzory a barvy) |
| **Texture Folder** | jiná knihovna: složka se složkami pojmenovanými jako materiály (prázdné: ta, která je součástí programu; také proměnná `PG_TEXTURES`) |
| **Surface Detail** | jak silné jsou vzory a skvrny přes fotky; 0 je vypne |

Z příkazové řádky: `--set output.render_textures=0`,
`--set output.render_texture_folder=/cesta/k/texturam`.

## 4. Před a po

![Demolice bez materiálů a s nimi](img/materials-demolition.jpg)

*Stejný snímek demolice v Cycles. Vlevo bez materiálů, vpravo s nimi:
okna s místnostmi za sklem, skvrnité střechy a omítka.*

![Věž zblízka](img/materials-close.jpg)

*Věž, když se začíná hroutit: omítka a betonové stropy z fotografií, okna
okolních domů odrážejí oblohu.*

## 5. Jak to funguje

- `src/pg/core/Material.h`: jména materiálů (`MaterialPreset`,
  `kMaterialNames`). Nový řetězcový atribut má na začátku tabulky `""`,
  takže primitivy, kterým nikdo nic nezapsal, nemají materiál. To platí pro
  wrangle, `setPrimitiveString` i spojení geometrií (Merge).
- `src/pg/nodes/Surface.cpp`: uzel Material.
- `src/pg/sim/Rigid.cpp`: `posedPieces` zapíše `rest` a `drawnPieces` dá
  plochám lomu `broken_concrete` a trubkám prutů `steel`.
- `src/pg/render/Scene.cpp`: `meshOf` čte `material`, `texture*` a `rest`
  a oknům dá číslo podle polohy jejich první plochy v `rest`. Číslo je
  stejné snímek za snímkem, i když kusy mizí.
- `src/pg/render/Textures.cpp`: hledání sad (knihovna, `texture.txt`,
  jména souborů z Poly Haven a ambientCG), průměrná barva a triplanární
  vyhledání pro path tracer.
- `src/pg/render/Cycles.cpp`: `patternOf` (procedurální vzory ve třech
  měřítkách), `laidOn` a `sampled` (triplanární fotky), `weathered`
  (skvrny a šmouhy přes fotky). Sítě nesou atributy `pg_rest`,
  `pg_rest_normal` a `pg_random`.
- `tools/textures/prepare.py`: výroba knihovny z fotek pbrt-v4-scenes.
