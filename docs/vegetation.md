# Vegetace: tráva, keře a stromy jako instance

Louka má miliony stébel a les tisíce stromů. Kdyby každé stéblo bylo
vlastní geometrií, nevešla by se do paměti a viewport by se nehnul. Proto
každá rostlina vyroste jen jednou a v krajině ji zastupuje bod. Takovému
bodu se říká **instance**. Bod říká, která rostlina na něm stojí, jak je
otočená, jak velká a jak odstíněná. Rostlině, kterou zastupuje, se říká
**prototyp**. Viewport kreslí instance přes GPU instancing: prototyp je na
grafické kartě jednou a nakreslí se tolikrát, kolik bodů ho zastupuje.
Do USD jde celek jako PointInstancer, do OBJ a PLY jako kopie.

Příklad **meadow** je louka u okraje lesa ve větru: 122 577 trsů trávy
(přes 1,9 milionu stébel), 84 stromů a 65 keřů. Všechno to drží 21
prototypů a 160 tisíc bodů.

![Louka u lesa: tráva z instancí, cesta, keře na okraji lesa, listnáče a smrky](img/vegetation-meadow.jpg)

```bash
./build/prototype --example meadow                  # louka u lesa ve větru: Play
./build/prototype cook meadow - --start 1 --end 3   # kolik bodů a jak dlouho
./build/prototype cook meadow louka.usda            # do USD jako PointInstancer
./build/prototype sim meadow - --frames 48 --export shot.usda   # záběr ve větru do USD
```

## 1. Instance

Bod s celočíselným atributem `instance` = k (0 a víc) zastupuje prototyp
číslo k své geometrie. Prototypy jsou geometrie, které geometrie drží
jednou. Kopie geometrie je sdílí a nekopíruje. Kde prototyp stojí, určují
atributy bodu stejně jako u **Copy to Points**:

| Atribut | Co dělá |
|---|---|
| `P` | kam jde počátek prototypu |
| `orient` | jak je prototyp otočený: kvaternion x, y, z, w. Bez něj se +y prototypu otočí do `N`, bez `N` se neotáčí |
| `pscale` | jak je velký; bez něj 1 |
| `tint` | čím se násobí barvy prototypu: jeden trs trávy o kousek žlutší než druhý |

Barva `Cd` bodu instanci nebarví. Body rozházené po barevném terénu totiž
nesou barvu terénu, a tráva na nich má zůstat zelená. Bod s `instance`
−1 nebo bez tohoto atributu je obyčejný bod.

Instance procházejí sítí jako ostatní geometrie:

- **Merge** spojí prototypy za sebe. Prototypy druhé geometrie jdou za
  prototypy první a čísla `instance` jejích bodů se posunou o jejich počet.
  Geometrie, která atribut `instance` nemá, dostane −1. Instance, kterým
  chybí `pscale` nebo `tint`, dostanou 1, ne nulu, takže nezmizí ani
  nezčernají.
- **Transform** instance posune, otočí a zvětší jako jejich kopie:
  `orient` otočí a `pscale` vynásobí měřítkem. Při nerovnoměrném měřítku
  použije nejbližší otočení a průměrné zvětšení, protože instance se
  nedeformují. Bod natočený jen podle `N` dostane `orient`, který ho tak
  natáčel. Kdyby se prototyp jen znovu natočil do otočeného `N`, pootočil
  by se kolem něj.
- **Unpack** udělá z instancí kopie, tedy geometrii, kterou mohou měnit
  všechny uzly. Nejdřív nechá, co instancí není, pak přidá kopie prototypů
  jednoho po druhém, každý na jeho body v jejich pořadí. Tint vynásobí
  barvy kopií.
- Wrangle vidí instance jako body. Změní `p@orient`, `@pscale` nebo
  `v@tint` a prototypy zůstanou sdílené (vítr, níže).

Viewport instance nekreslí jako tečky. Každý prototyp nahraje jednou
a nakreslí ho jedním voláním tolikrát, kolik bodů ho zastupuje, včetně
stínové mapy. Na instanci připadá 48 bajtů (poloha, velikost, otočení,
tint). Když se hýbou jen body, jako ve větru, posílá se na GPU jen tohle.
Rámeček pro stíny a zarámování počítá z rohů rámečku prototypu
umístěných na každý bod.

## 2. Tráva: uzel Grass

Uzel **Grass** pěstuje trsy trávy. Trs je skupina stébel z jednoho
kořene. Každé stéblo je pásek, který se zužuje do špičky. Od středu trsu
se naklání a pod svou vahou se ohýbá, tím víc, čím výš. Kolem sebe se
trochu stáčí. U kořene je tmavě zelené, ke špičce světlejší a sem tam je
nějaké suché.

![Tráva zblízka: stébla z trsů, suchá stébla, v pozadí les](img/vegetation-grass.jpg)

**Se vstupem povrchu** uzel rozhází trsy po jeho polygonech, **Density**
na metr čtvereční, podle pravidel uzlu Scatter (Density Attribute, Max
Slope, oddíl 3). Každý trs je instance jedné z **Variants** variant, které
uzel vypěstuje jednou. Kterou variantu bod dostane, určuje jeho `id`,
jinak jeho pořadí, a Seed. Bod dostane také:

- `orient`, tedy náhodné otočení kolem +y, protože tráva roste svisle
  i na svahu. S **Along Normal** roste ven z povrchu podél `N`, třeba
  mech na zdi.
- `pscale`: Size Variation víc či míň, krát vlastní `pscale` povrchu.
- `tint`: Variation, tedy trs o kousek světlejší, tmavší nebo žlutší.

**Jen s body na vstupu** (bez polygonů) vyroste trs na každém bodě. **Bez
vstupu** je výstupem jeden trs v **Center**, jako geometrie. S vypnutým
**Instances** jsou výstupem kopie. Ty může měnit každý uzel, ale jsou tak
těžké jako všechna jejich stébla.

| Parametr | Co dělá |
|---|---|
| **Density** | trsů na m² povrchu (výchozí 50) |
| **Seed** | jiné číslo, jiná místa a jiné trsy |
| **Size Variation** | jak moc se trsy liší velikostí |
| **Along Normal** | růst podél normály povrchu místo svisle |
| **Density Attribute** | atribut bodů povrchu 0 až 1: jaký podíl trsů na místě roste. 0 znamená cestu, 1 plnou louku |
| **Max Slope** | na plochách strmějších než tento úhel od vodorovné tráva neroste (výchozí 45°) |
| **Blades** | stébel v trsu (16) |
| **Height**, **Height Variation** | délka stébla (0,4 m; trávník 0,08, vysoká tráva 1) a jak se liší |
| **Width** | šířka stébla u kořene (6 mm) |
| **Bend** | jak moc se stébla ohýbají: 0 rovně, 1 špička vodorovně |
| **Lean** | největší náklon od středu trsu (30°) |
| **Spread** | jak daleko od středu trsu jsou kořeny (8 cm) |
| **Segments** | kousků podél stébla: víc je hladší oblouk, míň je lehčí pole v dálce |
| **Root Color**, **Tip Color** | barva u kořene a na špičce |
| **Dry**, **Dry Color** | podíl suchých stébel (jaro 0, pozdní léto 0,5) a jejich barva |
| **Variation** | jak moc se stébla i trsy liší odstínem |
| **Variants** | kolik různých trsů se vypěstuje |
| **Instances** | body s prototypy (zapnuto), nebo kopie |

Prototyp trsu má bodové `Cd` a `flex`, tedy jak daleko po stéble bod je
(0 u kořene, 1 na špičce), vrcholové `uv` (u jednou přes šířku stébla, v od
kořene 0 po špičku 1) a primitivní `blade`. Podle `uv` na stéblo jde
obrázek trávy z knihovny (`examples/textures/grass`: střední žilka
a proužky podél), v Cycles i v path traceru. Kořeny sedí kousek pod
zemí (0,6 Spread, nejvýš pětina výšky). Trs na svahu totiž stojí svisle
a jeho kořeny do kopce nesmí viset ve vzduchu.

## 3. Scatter: pravidla

**Scatter** rozhazuje body úměrně ploše, deterministicky podle Seed
a stejně na jakémkoli počtu vláken. Nová pravidla:

| Parametr | Co dělá |
|---|---|
| **Mode** | Count: Count bodů po celé ploše. Density: Density bodů na m², takže víc plochy dá víc bodů |
| **Density Attribute** | atribut bodů vstupu 0 až 1, namalovaný (Attribute Paint) nebo z wranglu: jaký podíl bodů na místě zůstane |
| **Max Slope** | žádné body na plochách skloněných víc než tento úhel od vodorovné (180 = kdekoli) |
| **Min Distance** | žádný bod blíž než tato vzdálenost k bodu, který zůstal před ním: stromy, které si drží odstup |

Min Distance se počítá bod po bodu v pořadí, s mřížkou buněk velkých
jako vzdálenost. Proto je deterministická. Uzel Grass používá Scatter se
stejnými pravidly.

## 4. Copy to Points: instance a varianty

**Copy to Points** má dva nové parametry:

- **Instance**: výstupem nejsou kopie, ale body, z nichž každý zastupuje
  to, co by se na něj zkopírovalo. Unpack z nich udělá přesně ty kopie.
- **Piece Attribute**: primitivní atribut geometrie (celé číslo nebo
  text) ji rozdělí na kusy, každá hodnota jeden kus. Bod dostane kus
  podle svého atributu stejného jména. Bod bez něj dostane kus podle
  svého pořadí. Tak jde na body osm různých kamenů nebo trsů.

`tint` bodu násobí barvy kopie. `Cd` bodu jako dřív nahrazuje barvy kopie
(jen v kopiích; instanci nebarví).

## 5. Stromy a keře jako instance

Tree má výstup **Instances**. Vypěstuje **Variants** stromů (výchozí 8),
a to stromy, které by vyrostly na prvních bodech. Každý bod pak jeden
z nich zastupuje: vybraný podle `id`, jinak podle pořadí, otočený kolem +y,
velký podle `pscale` a Size Variation a s vlastním odstínem (`tint` podle
Variation). Les tisíců stromů tak stojí tolik, kolik stojí osm stromů
a tisíc bodů. Bez bodů je výstupem jedna instance v Center, stejný strom
jako výstup Mesh.

Keř je Tree, který se rozvětví hned u země: Forks 5, Fork Height 0,03,
Fork Angle 38°, Crown 0,02, dvě úrovně větví a malé listy (příklad
meadow, uzel `shrubs`).

## 6. Vítr na instancích

Instance se ve větru nedeformují. Celý trs se ohne od kořene a strom se
zakývá od paty. Stačí k tomu otočit `orient` bodů. Wrangle `wind`
z příkladu meadow:

```c
// Nárazy větru běží přes louku a ohýbají trsy od kořene.
float gust = 0.5 + 0.5 * sin(@Time * 1.6 - @P.x * 0.3 + 3.0 * noise(@P * 0.12));
float bow = 0.08 + 0.22 * gust * gust + 0.05 * sin(@Time * 5.0 + @ptnum * 0.37);
p@orient = qmultiply(quaternion(ch("strength") * bow, set(0, 0, -1)), p@orient);
```

`quaternion(úhel, osa)` je otočení kolem osy −z, takže vítr fouká
ve směru +x. `qmultiply` ho přidá k otočení, které trs měl. Fáze závisí
na poloze a šumu, takže nárazy běží přes louku jako vlny. Každý trs má
navíc vlastní rychlé chvění podle `@ptnum`. Stromy a keře kývá wrangle
`sway` s dvanáctkrát menším úhlem.

Wrangle mění jen `orient`. Polohy, prototypy a ostatní atributy zůstávají
sdílené se vstupem. Snímek louky se proto uvaří za 31–39 ms a viewport
posílá na GPU jen nová umístění. Ohyb stébla podél jeho délky (podle
`flex`) by potřeboval stébla jako geometrii nebo vítr ve shaderu, viz
oddíl 10.

## 7. Příklad meadow

[examples/sim/meadow.pgsim](../examples/sim/meadow.pgsim):

- **Grid** `land` 80 × 60 m (220 × 170 bodů) zvedne Point Wrangle
  `terrain` šumem do mírných vln, které stoupají k lesu vzadu. Wrangle
  terén i obarví a namaluje mu tři atributy 0 až 1: `grass` (0 na cestě
  vinoucí se přes louku, tráva řídne k jejím okrajům i v lese), `trees`
  (les za 7 až 13 m od louky a sem tam strom v louce) a `shrubs` (pás
  podél okraje lesa).
- **Grass** `grass` rozhází po terénu 40 trsů na m² podle `grass`, ne na
  svazích přes 40°. Vznikne 122 577 trsů v osmi variantách, přes
  1,9 milionu stébel. Wrangle `wind` je ohýbá ve větru.
- **Scatter** `tree_spots` (Density 0,08 na m² podle `trees`, Min Distance
  3,5 m) dá místa stromům. Point Wrangle `kinds` a dva **Blasty** je
  rozdělí na listnáče a smrky: **Tree** `broadleaves` (5 variant)
  a `spruces` (4 varianty), oba s Output Instances. Vznikne 45 listnáčů
  a 39 smrků.
- **Scatter** `shrub_spots` podle `shrubs` a **Tree** `shrubs` (keře,
  4 varianty): 65 keřů.
- **Merge** `trees` spojí stromy s keři a Point Wrangle `sway` je kývá.
  **Merge** `meadow` spojí terén, trávu a stromy. Výsledek má 21
  prototypů a zobrazuje se.

Příklad je model bez kamery a uzlu Output. Obrázky nahoře jsou z jeho
kopie s přidanou kamerou a výstupem se sluncem a oblohou (Sky Behind).

![Editor s příkladem meadow: krajina ve viewportu, parametry uzlu Grass, síť](img/vegetation-editor.jpg)

## 8. Export

- **OBJ a PLY** (`prototype cook`, `geo.save`, Export Geometry
  v editoru) dostanou instance jako kopie, stejně jako z Unpacku. Pozor
  na velikost: tráva z příkladu meadow je rozbalená 17,7 milionu bodů
  a 7,8 milionu polygonů, zatímco jako instance jen 122 577 bodů.
- **USD** (`.usda`) dostane **PointInstancer** `instances` vedle meshe
  zbytku geometrie. Pod ním je scope `Prototypes`, v něm každý prototyp
  (`proto_0`, `proto_1`…) jako geometrie, včetně vnořených instancí. Na
  instancer jde vztah `prototypes` a pole `protoIndices`, `positions`,
  `orientations` (quath), `scales`, `primvars:tint` a `ids` (z `id`)
  a `extent` přes umístěné prototypy. V záběru (`prototype sim
  --export shot.usda`) jsou prototypy ve stage jednou. Co se mění, tedy
  natočení ve větru, jde do vrstvy za každý snímek (value clips, jako
  ostatní geometrie). Knihovna USD (pxr) umístí instance tam, kde jsou
  kopie z Unpacku, s odchylkou do 0,23 mm, protože `orientations` jsou
  v poloviční přesnosti (test `tests/python/test_instances.py`).
- Nulové normály, `orient`, `pscale` a `tint`, které Merge doplnil
  terénu od bodů instancí, se nezapisují. Renderer by podle nich terén
  začernil.
- **Python**: `geo.prototypes` (seznam `pg.Geometry`), `geo.instance_count`,
  `geo.add_prototype(g)` (vrací číslo pro `instance`),
  `geo.clear_prototypes()` a `geo.unpack()` ([python.md](python.md)).

## 9. Výkon

Na tomto stroji (release, všechna vlákna):

| Co | Čas |
|---|---|
| celá louka, první snímek (terén, 122 577 trsů, 13 variant stromů a keřů) | 148 ms |
| další snímek ve větru (jen wrangle `wind`, `sway` a Merge) | 31–39 ms |
| Grass: rozházet 192 000 kandidátů, prořídit, 8 trsů | 35 ms |
| Unpack trávy na 17,7 milionu bodů | 2,6 s |

Render snímku 1600 × 900 přes softwarový OpenGL (llvmpipe, bez grafické
karty) trvá i s vařením sítě 7,6 s. S obrázky listů a trávy (alfa výřez
a mipmapy na procesoru) a s focením billboardů je to víc než 5,4 s bez
obrázků, ale stále méně než 8,1 s bez úrovní detailu. Na grafické kartě je
to zlomek.

**Úrovně detailu (LOD).** Viewport kreslí každou kopii rostliny podle
toho, jak velká se jeví: poloměr krabice prototypu krát `pscale` děleno
vzdáleností od oka.

| Jeví se | Kreslí se |
|---|---|
| nad 0,04 | celá |
| 0,012–0,04 | třetina listů a stébel (0,35) |
| 0,005–0,012 | osmina (0,12), bez větviček |
| 0,0015–0,005 | billboard |
| pod 0,0015 | vůbec |

Kolem každé hranice (±20 %) je kopie v obou úrovních najednou. Každá
nakreslí jen část pixelů podle ditheringu v obraze, dohromady všechny.
Rostlina tak z jedné úrovně do druhé přechází plynule, nepřeskočí.
V dálce stejně tak mizí.

Řidší rostlinu dělá `plantDetail` (`src/pg/core/Lod.h`), jak to dělá
SpeedTree. Rovnoměrně vybere listy a stébla (plochy s `translucency` nad
0; stéblo jsou všechny plochy jednoho `blade`). Každý ponechaný list
zvětší 1/√podíl kolem jeho paty a každé stéblo rozšíří 1/podíl, takže
listí pokryje stejnou plochu jako předtím (strom: 2133 listů 11,98 m²,
746 listů 12,02 m²). Pod polovinou zmizí větvičky (`level` 2 a víc).

**Billboardy.** Poslední úroveň je karta otočená k oku kolem svislé osy.
Ukazuje obrázek rostliny z té strany, ze které ji oko vidí: osm pohledů
kolem dokola, 128 × 128 texelů každý. Viewport si je vyfotí sám, když
prototyp poprvé dostane. Obrázek není barva, ale G-buffer viewportu:
normála, barva, průsvitnost. Billboard se tedy osvětlí jako geometrie,
normály se otočí s kopií a barva se tónuje jejím `tint`. Stín vrhá osmina
rostliny, ne karta.

Kopie se mezi úrovně rozdělí znovu, když se oko posune o 10 cm. Louka
z 50 m od okraje: 22,1 milionu trojúhelníků v plné podobě, nakreslí se
11,1 milionu (50 %); 86 kopií celých, 63 790 třetinových, 111 933
osminových a 6 762 billboardů (prolínající se počítány dvakrát).

![Louka ve viewportu: nahoře úrovně detailu, jak jsou; dole pro srovnání všechny rostliny jako billboardy](img/viewport-billboards.jpg)

Cycles a path tracer kreslí vše v plné podobě, instance je nestojí paměť.

## 10. Co zatím chybí

- Ohyb stébel ve větru podél délky (ve shaderu podle `flex`), ne jen
  otočení trsu.
- Šlapání a interakce (tráva ohnutá tělesem nebo postavou).
- Ekosystém: druhy, které si konkurují o místo a světlo, a jejich rozšíření
  podle vlhkosti a stínu.
