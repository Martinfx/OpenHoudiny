# Textury materiálů

Fotografie povrchů, které renderery kladou na materiály (`s@material`,
viz [docs/materials.md](../../docs/materials.md)). Každá složka je jedna sada
a jmenuje se jako materiál:

| Složka | Co to je | Velikost dlaždice |
|---|---|---|
| `concrete` | beton se skvrnami a póry | 3 m |
| `broken_concrete` | lom: kamínky kameniva v cementu | 0,8 m |
| `plaster` | světlá omítka, skvrnitá | 3 m |
| `brick_wall` | cihlová zeď s maltou (vlastní barvy, `tint 0`) | 2,4 m |
| `mortar` | fotky ze `sand`, jemnější zrno | 0,4 m |
| `metal` | kartáčovaný plech se škrábanci | 1 m |
| `asphalt` | drobné kamínky v dehtu | 2 m |
| `wood` | dřevo s kresbou | 1,2 m |
| `roof` | fotky z `concrete`, šestimetrová dlaždice (ploché střechy) | 6 m |
| `roof_tiles` | břidlicové tašky v řadách, kladené podél střechy (`projection face`) | 1,8 m |
| `paving` | dlažba z kostek asi 18 × 14 cm | 1,3 m |
| `bark` | kůra lípy | 1 m |
| `soil` | vlhká hlína | 2,5 m |
| `lawn` | trávník | 1,5 m |
| `sand` | písek | 1,2 m |

V každé složce je:

- `color.jpg` — barva (sRGB), nejvýš 1024 × 1024,
- `height.jpg` — výška 0–1; Cycles z ní dělá reliéf,
- `texture.txt` — `size` (kolik metrů jedna dlaždice pokryje), `depth`
  (kolik metrů je mezi nejnižším a nejvyšším místem výšky), `mean`
  (průměrná barva, lineární), `tint` (1: barva `Cd` povrchu nahradí barvu
  fotky a fotka kolem ní jen světlá a tmavne; 0: fotka jak je),
  `projection face` (klade se podél šikmé plochy, aby řady zůstaly
  vodorovné; jinak ze tří stran), `pictures` (fotky jiné složky), `source`
  a `license`.

Sady vyrábí skript [tools/textures/prepare.py](../../tools/textures/prepare.py)
z fotografií v repozitářích [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes)
a [BabylonJS/Assets](https://github.com/BabylonJS/Assets). Zmenší je na
nejvýš 1024 px a výšku dopočítá z normálové mapy integrací ve Fourierově
prostoru. Kam míří zelená osa mapy, pozná podle toho, který směr dá
skutečný povrch. Kde normálová mapa chybí, vezme výšku ze světlosti fotky.
Nakonec zapíše `texture.txt`.

## Licence a autoři

Fotografie jsou upravené (zmenšené, výška dopočítaná z normál nebo ze
světlosti; barva kovu složená z rýh jeho reliéfu a skvrn jeho fotky) a šíří
se pod licencí **[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/)**:

- `concrete`, `plaster`, `brick_wall`, `wood`, `bark`, `soil`, `paving`,
  `roof_tiles`, `metal` (a `roof`): **Amazon Lumberyard Bistro**, © Amazon,
  CC-BY 4.0, <https://developer.nvidia.com/orca/amazon-lumberyard-bistro>;
  převzato z [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes)
  (adresář `bistro/textures`: `MASTER_Concrete_Plaster`, `Concrete2`,
  `MASTER_Brick_Small_Red`, `MASTER_Wood_Brown`,
  `Foliage_Linde_Tree_Large_Trunk`, `Pavement_Ground_Wet`,
  `Pavement_Cobblestone_Big_BLENDSHADER`, `MASTER_Roofing_Shingle_Grey`,
  `Banner_Metal`).
- `asphalt`, `broken_concrete`, `lawn`, `sand` (a `mortar`):
  **Babylon.js Assets**, © Babylon.js, CC-BY 4.0,
  <https://github.com/BabylonJS/Assets> (`meshes/PowerPlant/gravel_a.png`,
  `textures/rockyGround_basecolor.png` a `rockyGround_normal.png`,
  `textures/grass.png`, `textures/sand.jpg`).

Zbytek programu má svou vlastní licenci; tyto soubory pod ni nespadají.

## Vlastní textury

- **Jinou knihovnu** nastavíte na uzlu Output parametrem **Texture Folder**
  (nebo proměnnou prostředí `PG_TEXTURES`): složka se složkami pojmenovanými
  jako materiály.
- **Jednomu objektu** dáte texturu uzlem **Material** (parametr Texture):
  vyberte obrázek barvy ze sady z Poly Haven nebo ambientCG. Ostatní mapy se
  najdou vedle něj podle jmen (`_diff_`, `_rough_`, `_disp_`; `_Color`,
  `_Roughness`, `_Displacement`).
