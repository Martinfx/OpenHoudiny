# Alembic

Alembic (`.abc`) je formát, ve kterém si studia předávají cache: geometrii,
která se hýbe, snímek po snímku. Píše a čte ho Houdini, Maya, Blender,
Nuke, Katana i renderery. Prototype ho **zapisuje** (celý záběr jako jeden
archiv) a **čte** (geometrii a kameru jako uzly sítě), obojí bez knihovny
Alembic: kontejner Ogawa, vrstvu vlastností a schémata AbcGeom má vlastní.

Soubory, které zapíše, čte Blender 4.5 (Alembic 1.8.3) a soubory Blenderu
čte prototype (§6).

## 1. Rychlý start

```bash
# demolice jako jeden archiv: město, 710 kusů v pohybu, drť, kamera; plyn jako VDB vedle
./build/prototype sim demolition - --frames 120 --export demolition.abc
# kulisa a kamera z Blenderu (examples/abc/shot.abc), v ní oheň
./build/prototype sim alembic_shot out/shot.png --every 24
```

V editoru:
- **File › Export Alembic…** zapíše záběr ze snímků v cache.
- **Shift+A › Geometry › Alembic Import:** geometrie ze souboru.
- **Shift+A › Render › Alembic Camera:** kamera ze souboru do vstupu Camera
  uzlu Output.

Z Pythonu:

```python
import pg
sim = pg.Network.example("demolition").simulate()
sim.export_alembic("demolition.abc", frames=120)   # nebo pg.AbcExport po snímcích
```

![Příklad alembic_shot ve snímcích 24 a 48: kulisa (dvě zdi, sloup, dvě bedny) a kamera, která najíždí, jsou z Alembicu, který zapsal Blender; oheň mezi bednami simuluje prototype, jeho kouř stoupá podél zadní zdi (Cycles)](img/alembic-shot.jpg)

## 2. Export záběru

`--export záběr.abc` (nebo File › Export Alembic…) zapíše jeden archiv:

| Objekt | Co v něm je |
|---|---|
| `/<uzel>` | zobrazená geometrie pod Xformem: polygony jako PolyMesh (`N`, `uv`, `Cd`, `v`), otevřené čáry jako Curves, volné body jako Points (`id`, `v`, šířky z `pscale`, `Cd`) |
| `/pieces` | každé těleso RBD Solveru jako Xform nad PolyMeshem svého tvaru kolem svého středu. Tvar se zapíše jednou, Xform ho každý snímek posune a otočí. Těleso rozdrcené na prach, nebo úlomek, který se ještě neodlomil, je v tu chvíli neviditelné. Plochy, které vyřízl lom, jsou FaceSet `inside`, aby mohly mít vlastní materiál. |
| `/grit` | drť: Points s šířkou podle velikosti, očíslované, v pohybu |
| `/grains` | zrna Grain Solveru: Points v jejich barvách |
| `/rebar` | výztuž tam, kam ji kusy odnesly: Curves |
| `/cloth` | látka tam, kde jsou její body: PolyMesh |
| `/water` | povrch vody: PolyMesh pro každý snímek s normálami, rychlostí a pěnou |
| `/rain` | kapky a kapičky: Points |
| `/camera` | Xform nad Camera: objektiv 24 mm vysoký jako v prototypu (film 2,4 cm na výšku × poměr stran obrazu) |
| plyn | soubory OpenVDB vedle archivu, jeden na snímek (`záběr_gas/záběr_gas.0001.vdb`), jako u exportu do USD |

Čas: snímek f je v čase f / fps sekund, jak snímek f píše Houdini, Maya
i Blender. Y je nahoře, jednotka je metr.

Co je velké a v každém snímku jiné (zobrazená geometrie, voda, body), se
zapisuje hned, jak snímek přijde, takže se záběr libovolné délky nemusí
vejít do paměti. Co je malé a musí existovat od prvního snímku (polohy
těles, kamera, viditelnost), se drží a zapíše na konci. Vzorek, který je
stejný jako předchozí, se nezapisuje znovu: tvar kusu, který stojí,
a kamera, která se nehýbe, mají jeden vzorek.

Demolice ze 120 snímků s 710 tělesy dá archiv o 14 MB a 120 souborů VDB
s plynem; Blender ho načte za 0,3 s.

## 3. Alembic Import

Geometrie archivu v daném snímku, ve světě, kam ji dají transformace:

| Z Alembicu | Do geometrie |
|---|---|
| **PolyMesh**, **SubD** | polygony (SubD jako řídicí síť, bez vyhlazení a bez hran a rohů zostření); pořadí rohů se otočí (Alembic je má po směru hodinových ručiček), a zpátky, když transformace zrcadlí |
| `N` | `N`: podle scope na rozích (face-varying) nebo bodech, transformované |
| `uv` | `uv` (vektor, z = 0) |
| `.velocities` | `v`, transformované |
| `.arbGeomParams` | atribut stejného jména; scope `con` na geometrii, `uni` na primitivech, `vtx`/`var` na bodech, `fvr` na rozích; `Cs` jako `Cd` |
| **FaceSet** | skupina primitiv pojmenovaná po FaceSetu (`inside`) |
| **Points** | volné body: `id`, `v`, `pscale` z šířky (polovina) |
| **Curves** | otevřené lomené čáry (kubické po řídicích bodech), `pscale` z šířky |
| cesta objektu | textový atribut primitiv `path` (`/pieces/body_0012`) |

Mezi dvěma vzorky se polohy prolnou, pokud mají oba stejně bodů; jinak platí
dřívější vzorek. Když se geometrie v souboru hýbe, uzel se vaří v každém
snímku znovu, jinak jen jednou.

Parametry:
- **File:** `.abc`. Relativní cesta se čte ze složky sítě. Soubor, který se
  změní, se načte znovu.
- **Objects:** které objekty číst, s tím, co je pod nimi. Cesty oddělené
  mezerou (`/pieces /city`). Prázdné pole znamená celý archiv.
- **Frame Offset:** posune čtení o tolik snímků. Snímek f se čte v čase
  (f + posun) / fps; Output s jiným fps přehrává soubor svou rychlostí.
- **Hidden:** číst i objekty, které v daném snímku nejsou vidět.
- **Face Sets as Groups**, **Path Attribute:** skupiny z FaceSetů a atribut
  `path`.

## 4. Alembic Camera

Kamera ze souboru. Zapojená do vstupu Camera uzlu Output je to kamera, přes
kterou se renderuje.
- **Poloha a otočení:** ze světové matice kamery v každém snímku. Úhly se
  volí nejblíž předchozímu snímku, takže otočení nepřeskočí ze 180° na −180°.
- **Objektiv:** zorný úhel zleva doprava odpovídá horizontální cloně filmu
  a ohnisku (`horizontalAperture`, `focalLength`), jako u USD Camera
  ([usd-import.md](usd-import.md)).
- **Object:** cesta ke kameře; prázdné pole znamená první kameru v archivu.
- **Width**, **Height**, **Plate**, **Plate Frame:** jako u USD Camera
  ([plate.md](plate.md)).

Posun filmu (`horizontalFilmOffset`) prototype nekreslí; uzel to nahlásí
jako varování.

## 5. Příklad

`alembic_shot` je kulisa z Blenderu (`examples/abc/shot.abc`, zapsaná
skriptem `examples/abc/make_shot.py`): roh zříceniny — dvě zdi, sloup, dvě
bedny — a kamera, která na ni dvě sekundy najíždí, 24 snímků za sekundu.
Alembic Import načte kulisu, Object z ní udělá překážku pro plyn a Alembic
Camera je kamera záběru. Mezi bednami hoří oheň a jeho kouř stoupá podél
zadní zdi.

## 6. Ověření

Zápis i čtení se porovnávaly s Blenderem 4.5.3, který má knihovnu Alembic
1.8.3 (`pip install bpy`). Prototype ho nepotřebuje.

- **Zápis → Blender:**
  - Blender načte archivy příkladů demolition, shatter_grit, flag, tarp,
    sand_pour a rain_pond bez chyby, s objekty, počty bodů a ploch, jaké
    prototype zapsal;
  - plochy míří ven (pořadí rohů), `Cd` po rozích sedí, body s proměnným
    počtem (drť, zrna, kapky) i čáry výztuže přečte;
  - kusy demolice stojí v Blenderu tam, kde v prototypu: těžiště všech
    710 kusů ve snímcích 1 a 90 se liší nejvýš o 6e-5 m, což je přesnost
    floatu u souřadnic do 25 m; čtečka prototypu dává polohy těles ze
    simulace na 1e-4 m (test). Rozdrcené kusy jsou skryté od snímku, kdy se
    rozpadly;
  - kamera stojí, dívá se a má objektiv jako v prototypu.
- **Blender → čtení:** soubory `tests/data/abc` zapsal Blender
  (`make_blender_abc.py`): krabice, kostka, která se otočí a posune,
  kamera s ohniskem 35 mm v pohybu, Bézierova křivka, vlněná mřížka (body se
  hýbou), mřížka, která roste (plochy přibývají), částice. Plochy míří ven,
  pohyb, kamera, ohnisko i časový rozsah sedí s tím, co Blender zapsal.
- **Poškozené soubory:** 300 náhodně poškozených archivů je odmítnuto bez
  pádu.

Testy — `tests/test_alembic.cpp` (11):
- Ogawa skupiny a data, vzorky vlastností, časové vzorkování;
- čtení souborů, které zapsala knihovna (Blender), a odmítnutí poškozených;
- geometrie zapsaná a přečtená zpátky ve světě;
- export demolice: kusy se posouvají a otáčejí jako v simulaci, rozdrcený
  kus zmizí;
- uzly Alembic Import a Alembic Camera.

## 7. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/io/Ogawa.h` | Kontejner Ogawa: skupiny a bloky dat, zápis i čtení |
| `src/pg/abc/Archive.h` | Archiv: objekty, vlastnosti (skalární, pole, složené), vzorky a jejich klíče (MurmurHash3), časové vzorkování, metadata |
| `src/pg/abc/Geom.h` | Schémata AbcGeom: Xform, PolyMesh, SubD, Points, Curves, Camera, FaceSet; zápis a čtení do geometrie (`importGeometry`), kamera (`cameraAt`) |
| `src/pg/sim/AbcExport.h` | Záběr do archivu, snímek po snímku |
| `src/pg/nodes/Abc.cpp` | Uzel Alembic Import (`abcimport`) |
| `src/pg/sim/Camera.cpp` | `cameraFromAlembic`: kamera z Alembicu jako kamera záběru |
| `src/pg/sim/Network.cpp` | Uzly `alembic_import` a `alembic_camera` |
| `tools/prototype/Commands.cpp` | `--export záběr.abc` |
| `examples/abc/make_shot.py` | Jak vznikl ukázkový záběr (Blender) |

## 8. Omezení

- **HDF5:** čtou se jen archivy Ogawa (Alembic 1.5 a novější, výchozí všude).
  Staré archivy HDF5 se odmítnou.
- **NuPatch** (NURBS) se nečte; uzel ho vypíše jako přeskočený. Světla
  a jiná schémata se přeskočí bez hlášení.
- **Materiály** se nezapisují ani nečtou; barva je `Cd`.
- **Plyn** jde do souborů OpenVDB vedle archivu: Alembic objemy nemá.
- **Kolize z geometrie:** Object s tvarem z Alembic Import bere tvar ze
  snímku 1, ani když se geometrie v souboru hýbe. Kamera se hýbe po snímcích.
- **Kamera:** posun filmu se nekreslí.
