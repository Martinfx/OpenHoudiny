# Python API

Modul `pg` zpřístupní prototyp Pythonu, podobně jako `hou` zpřístupní Houdini.
Skript v něm:
- postaví síť uzlů nebo otevře existující;
- nastaví parametry, výrazy a klíče;
- uvaří geometrii a čte ji jako pole numpy **bez kopírování**;
- simuluje po snímcích, ukládá cache a zapisuje záběr do USD;
- vykreslí obrázek nebo video.

Studio si tak prototyp zapojí do své pipeline: skripty na farmě, kontrola
simulací, generování variant, napojení na vlastní nástroje.

Modul nepotřebuje OpenGL, takže běží i na serveru bez grafiky. Obrázky
a videa kreslí program `prototype`, který modul spustí jako samostatný proces.

## 1. Sestavení

```bash
cmake -B build                      # PG_BUILD_PYTHON je zapnuté, když najde python3-dev
cmake --build build -j
PYTHONPATH=build/python python3 -c "import pg; print(pg.examples())"
ctest --test-dir build -R python    # testy modulu
```

- **Závislosti:** pybind11 (BSD) se stáhne při konfiguraci, stejně jako Jolt nebo ImGui.
  Offline stačí nastavit `FETCHCONTENT_SOURCE_DIR_PYBIND11` na lokální kopii.
  Když pybind11 není vůbec k dispozici, CMake modul s varováním vynechá a zbytek postaví.
- **Verze Pythonu:** modul je sestavený pro jednu verzi, tu, kterou CMake najde.
  Jinou vybere `-DPython3_EXECUTABLE=/cesta/k/python3`.
- **Kam se modul uloží:** `build/python/pg/` obsahuje `__init__.py` a rozšíření `_pg`.
  Tuhle složku stačí dát do `PYTHONPATH`.
- **numpy:** doporučené. Bez něj jsou pole `memoryview` nad stejnou pamětí.
- **Sanitizery:** `PG_SANITIZE` modul vypne, protože knihovna se sanitizerem se do běžného
  Pythonu nenačte. Knihovny se kvůli modulu staví jako position-independent code.

## 2. Rychlý start

```python
import pg

net = pg.Network()
column = net.add("box", "column", size=(0.6, 3.0, 0.6), center=(0, 1.5, 0))
pieces = net.add("voronoi_fracture", "pieces", count=25)
column.connect(pieces)                      # první výstup a vstup, které typem sedí

geo = pieces.geometry()                     # uvaří uzel na snímku 1
geo.P                                       # numpy (N, 3) float32, bez kopie
geo.prims["piece"]                          # kus každé plochy

rbd = net.add("rbd_solver", glue=0.0)
pieces.connect(rbd).connect(net.add("output", frames=60))
sim = net.simulate()
with pg.UsdExport("out/column.usda", sim) as usd:
    for frame in sim.run():                 # snímek po snímku
        usd.add()
print(sim.current.rigid.centres)            # kde jsou středy kusů

net.layout().save("out/column.pgsim")       # otevře se v editoru
net.render("out/column.mp4")                # video přes prototype
```

## 3. Síť a uzly

`pg.Network` je síť stejná jako v editoru a v souborech `.pgsim`.

| Co | Jak |
|---|---|
| nová, ze souboru, příklad | `pg.Network()`, `pg.Network.load("scene.pgsim")`, `pg.Network.example("demolition")`, `pg.examples()` |
| uložit | `net.save("scene.pgsim")`, `net.text()` |
| uzel | `net.add("box", "name", size=(1, 2, 1))` vrátí `pg.Node`; `net["box1"]`, `net.nodes("box")`, `"box1" in net`, `net.remove(node)` |
| typy uzlů | `pg.node_types()`, `pg.node_type("box")`: parametry, vstupy, výstupy a nápověda; `pg.assets()` pro digital assets, `pg.load_assets(složka)` |
| parametr | `node["size"]`, `node["size"] = (1, 2, 3)`, `node.set(count=40, seed=3)`, `node.param("size")` (druh, výchozí hodnota, rozsah, jednotka, nápověda), `node.reset("size")` |
| výraz | `node.expression("size.y", "$F * 0.1")`, `ch("../box1/sizex")` a další jako v editoru |
| klíče | `node.key("center", 1, (0, 0, 0)).key("center", 24, (0, 5, 0), "linear")`, `node.value("center", 12)`, `node.keys("center")`, `node.clear_keys("center")` |
| spoje | `a.connect(b)` (vrátí `b`, takže jde řetězit), `net.connect(a, b, output="look", input="look")`, `net.disconnect(a, b)`, `net.links()` |
| příznaky | `node.display()`, `net.displayed`, `node.bypass = True`, `node.position = (x, y)`, `net.layout()` rozmístí uzly pro editor |
| kontrola | `net.problems()`: `(úroveň, uzel, zpráva)`, tedy co by editor ukázal u uzlů |

- **Hodnoty podle druhu parametru:**
  - čísla jako `float`, celá čísla jako `int`, přepínače jako `bool`;
  - vektory a barvy jako trojice;
  - volba jako jméno (`clip["keep"] = "below"`), text a kód jako `str`.
- **Text jako v souborech:** hodnotu jde zadat i textem, jak ji píšou soubory a `--set`:
  `box["size"] = "1 2 3"`.
- **Chyby:** neznámý parametr, nesmyslná hodnota nebo spoj, který typem nesedí,
  vyhodí `pg.Error` se seznamem toho, co uzel má.

### `as_code()`: síť jako Python

`net.as_code()` vrátí zdrojový kód funkce `build()`, která síť postaví znovu, uzel po uzlu.
Je to obdoba `asCode()` v Houdini. Kód obsahuje:
- parametry, které nejsou výchozí;
- výrazy, klíče a příznaky;
- polohy uzlů a spoje.

Kód wrangle se zapíše jako `r'''…'''`. Test ověří na všech příkladech, že síť postavená tímto
kódem je stejná jako původní.

## 4. Geometrie jako numpy

`node.geometry(frame)` (nebo `net.cook(node, frame)`) vrátí `pg.Geometry`. Jádro vaří
inkrementálně: co se od minulého vaření nezměnilo, vezme z cache.

| Co | Jak |
|---|---|
| atributy | `geo.points`, `geo.vertices`, `geo.prims`, `geo.detail`: tabulky podle jména, `geo.points["Cd"]`, `geo.points.type("Cd")` (`int`, `float`, `vector3`…), `list(geo.points)` |
| polohy | `geo.P` je totéž co `geo.points["P"]` |
| topologie | `sizes, points = geo.topology()`: počet rohů každého primitiva a bod každého rohu, jako `faceVertexCounts` a `faceVertexIndices` v USD; `geo.primitive_starts()`, `geo.closed()`, `geo.primitive(i)` |
| skupiny | `geo.groups`, `cls, members = geo.group("inside")`, `geo.set_group(...)` |
| objemy | `geo.volumes`, `geo.volume("density").values[i, j, k]`, `geo.add_volume(name, values, origin, voxel)` |
| stavba | `pg.Geometry()`, `add_points(P)`, `add_polygons(sizes, points)`, `add_polylines(...)`, `append(other)` |
| nové hodnoty | `geo.points["Cd"] = colors`, `geo.points.set("mask", values, "int")`, `del geo.points["mask"]` |
| soubory | `geo.save("x.ply" \| ".obj" \| ".vdb" \| ".usda")`, `pg.Geometry.load("x.obj" \| "x.ply")` |

- **Bez kopie:** pole ukazují přímo do paměti jádra. Číst ho znovu dá stejnou adresu
  a uvařený uzel z cache také (test to ověřuje). Pole drží geometrii naživu, dokud existují.
- **Jen pro čtení:** jádro tu paměť sdílí mezi geometriemi (copy-on-write), takže
  zápis do pole vyhodí `ValueError`. Nové hodnoty přijdou přes přiřazení do tabulky:
  - to hodnoty zkopíruje;
  - geometrie se tím oddělí a dál má vlastní buffery, jako uzel v jádře;
  - uvařená geometrie v cache zůstane, jaká byla.
- **Typ nového atributu** se pozná z dat:
  - řádky po 2, 3 nebo 4 číslech jsou vektory;
  - celá čísla jsou `int`, ostatní `float`;
  - seznam řetězců je `string`.
- **Objemy** se indexují `[i, j, k]` jako voxely, přestože v paměti jde x nejrychleji
  (pohled s kroky, zase bez kopie).

## 5. Simulace

`net.simulate()` (nebo `pg.Simulation(net)`) přeloží síť tak, jak je v tu chvíli, a simuluje
po snímcích. Pozdější změny sítě ji už neovlivní. `net.simulate(cache="složka")` místo
simulace čte snímky z cache.

| Co | Jak |
|---|---|
| krok | `frame = sim.step()`, `for frame in sim.run(120): …`, `sim.frame`, `sim.frames`, `sim.fps`, `sim.current` |
| geometrie snímku | `sim.geometry()` zobrazeného uzlu, `sim.geometry(node)` jiného: Liquid Surface, RBD Pieces a další vidí právě tento snímek |
| plyn | `frame.gas("density" \| "temperature" \| "flame")[i, j, k]` (float16), `frame.gas_domain()` |
| voda | `frame.water.positions`, `.velocities` (float16), `.foam`, `.ids`, `.flow` (rychlost na mřížce, `[i, j, k, osa]`), `.surface()` (síť jako Liquid Surface), `.litres` |
| déšť | `frame.rain.positions`, `.velocities`, `.ids`, `.droplet_positions`…, `.ripples()` |
| tělesa | `frame.rigid.centres`, `.velocities` (středů), `.spins`, `.rotations` (x, y, z, w), `.translations`, `.vanished`, `.grit` (x, y, z, velikost), `.grit_ids`, `.pieces()` |
| kamera | `sim.camera()`: poloha, otočení, ohnisko, rozměry obrazu v tomto snímku |
| cache | `frame.save("cache")`, `sim.write_cache_info("cache")`, `sim.cache("cache", frames=120)`, `pg.Frame.read("cache", 7)` |
| USD | `with pg.UsdExport("shot.usda", sim) as usd:` a po každém kroku `usd.add()`, nebo `sim.export_usd("shot.usda")` ([usd.md](usd.md)) |
| obraz | `net.render("out.png" \| ".exr" \| ".mp4", frames=…, every=…, size="1920x1080", from_cache="cache")`, `pg.run("sim", …)` |

- **Póza tělesa:** bod `p` v klidové poloze je teď v `rotate(rotations[b], p) + translations[b]`.
  `centres` je kde je teď střed tělesa (středu kvádru kolem jeho tvaru v klidu).
- **Vlákna:** simulace i vaření uvolní GIL, takže jiná vlákna Pythonu mezitím běží. Jednu síť
  ale nemají používat dvě vlákna naráz.
- **Render:** `net.render` uloží síť do dočasného souboru a spustí `prototype sim`.
  Relativní cesty sítě (meshe) čte z `net.folder` přes novou volbu `--folder`. Program se najde
  podle sestavení, nebo podle proměnné `PG_PROTOTYPE`, jinak na `PATH`.

## 6. Příklady

- **[`examples/python/demolition.py`](../examples/python/demolition.py)** — scéna z kroku 2
  (odstřel věžáku ve městě) postavená čistě z Pythonu:
  - `build()` napsalo `as_code()` z `examples/sim/demolition.pgsim`;
  - skript simuluje, každý snímek zapíše do cache a záběr do USD;
  - s `--video` ho vykreslí z cache;
  - test ověřuje, že `build()` dá přesně síť příkladu.

  ```bash
  PYTHONPATH=build/python python3 examples/python/demolition.py --frames 60 --video out/demolition.mp4
  ```

- **[`examples/python/fracture_stats.py`](../examples/python/fracture_stats.py)** — sloup
  rozbitý na kusy:
  - objem každého kusu spočítaný z polí numpy (dohromady přesně objem sloupu);
  - pád kusů a jejich středy, když dopadnou;
  - záběr do USD a síť pro editor.

## 7. Jak to funguje

```
pg/__init__.py      třídy Network, Node, Geometry, Simulation, Frame, UsdExport (Python)
_pg (C++)           pybind11 nad sim::Network, GeometryGraph, WorldSolver, UsdExport
```

- **Pole bez kopie:** `_pg.Array` je objekt s buffer protocolem (PEP 3118). Nese ukazatel
  do paměti, formát (`f`, `e`, `i`, `I`, `B`), tvar, kroky a vlastníka, sdílený ukazatel
  na geometrii nebo snímek. `numpy.asarray(array)` nad ním udělá pole bez kopie.
- **Snímek geometrie:** každý pohled vznikne nad kopií geometrie `Geometry`, která sdílí
  buffery (copy-on-write jako v jádře). Když se geometrie potom změní, pohled drží starou
  paměť a nikdy neukazuje do uvolněné.
- **Síť a vaření:** `pg.Network` drží `sim::Network` a vlastní `GeometryGraph`. Vaření je tedy
  inkrementální jako v editoru.
- **Simulace:** `pg.Simulation` drží kopii sítě, `Compiled`, `WorldSolver` (nebo čte cache)
  a vlastní graf, jehož uzly simulace zpátky na geometrii vidí aktuální snímek.
- **Assety:** při importu se načtou digital assets jako při startu programu: ty, které
  program nese, a ty ze složek `$PROTOTYPE_ASSETS` a uživatele.

## 8. Testy

`tests/python/test_pg.py` (16 testů; `ctest -R python`):
- **Sítě:**
  - typy uzlů a příklady;
  - parametry všech druhů (vektor, volba podle jména, přepínač, text, kód) a chyby;
  - spoje podle typu, příznaky a kontrola sítě;
  - výrazy a klíče;
  - uložení a načtení beze změny;
  - `as_code()` postaví znovu každý příklad.
- **Příklady:** `build()` z `demolition.py` je síť příkladu demolition.
- **Geometrie:**
  - pole numpy nad pamětí jádra, stejná adresa i z cache, jen pro čtení;
  - změny kopie nechají cache na pokoji;
  - geometrie postavená v Pythonu do PLY a OBJ a zpátky, s řetězci a skupinami;
  - objem indexovaný `[i, j, k]`.
- **Simulace:**
  - rain_pond po snímcích: voda, déšť, povrch, kamera;
  - cache a čtení z ní;
  - kusy RBD: body v klidu posunuté pózou sedí na `pieces()`;
  - záběr do USD, ověřený knihovnou `pxr`, když je nainstalovaná.
- **Obraz:** obrázek přes `prototype` (přeskočí se, když tu OpenGL není).

## 9. Omezení

- **Python uvnitř sítě zatím není.** Uzel, jehož geometrii počítá skript (Python SOP
  v Houdini), chybí. Geometrie z Pythonu jde do sítě jen přes soubor a uzel File.
- **Render mimo proces:** obraz kreslí `prototype`, takže znovu simuluje, pokud nedostane
  `from_cache`.
- **Vlákna:** jednu síť nebo simulaci smí používat jen jedno vlákno Pythonu naráz.
- **Velikost modulu:** v sestavení RelWithDebInfo má kolem 100 MB, protože nese ladicí
  informace všech knihoven. V sestavení Release je mnohem menší.
- **float16:** pole vody a plynu jsou v poloviční přesnosti, jak je drží snímek.
  `astype("float32")` z nich udělá kopii ve float.
