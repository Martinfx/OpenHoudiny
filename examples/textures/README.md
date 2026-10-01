# Textury materiálů

Fotografie povrchů, které renderery kladou na materiály (`s@material`,
viz [docs/materials.md](../../docs/materials.md)). Každá složka je jedna sada
a jmenuje se jako materiál:

| Složka | Co to je | Velikost dlaždice |
|---|---|---|
| `concrete` | beton se skvrnami a póry | 3 m |
| `plaster` | světlá omítka, skvrnitá | 3 m |
| `brick_wall` | cihlová zeď s maltou (vlastní barvy, `tint 0`) | 2,4 m |
| `wood` | dřevo s kresbou | 1,2 m |
| `bark` | kůra lípy | 1 m |
| `soil` | vlhká hlína | 2,5 m |
| `roof` | fotky z `concrete`, šestimetrová dlaždice (ploché střechy) | 6 m |

V každé složce je:

- `color.jpg` — barva (sRGB), 1024 × 1024,
- `height.jpg` — výška 0–1; Cycles z ní dělá reliéf,
- `texture.txt` — `size` (kolik metrů jedna dlaždice pokryje), `depth`
  (kolik metrů je mezi nejnižším a nejvyšším místem výšky), `mean`
  (průměrná barva, lineární), `tint` (1: barva `Cd` povrchu nahradí barvu
  fotky a fotka kolem ní jen světlá a tmavne; 0: fotka jak je),
  `pictures` (fotky jiné složky), `source` a `license`.

Sady vyrábí skript [tools/textures/prepare.py](../../tools/textures/prepare.py)
z fotografií v repozitáři [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes):
zmenší je na 1024 px, výšku dopočítá z normálové mapy (integrací ve
Fourierově prostoru) a zapíše `texture.txt`.

## Licence a autoři

Fotografie jsou upravené (zmenšené, výška dopočítaná z normál) a šíří se pod
licencí **[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/)**:

- `concrete`, `plaster`, `brick_wall`, `wood`, `bark`, `soil` (a `roof`):
  **Amazon Lumberyard Bistro**, © Amazon, CC-BY 4.0,
  <https://developer.nvidia.com/orca/amazon-lumberyard-bistro>; převzato
  z [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes) (adresář
  `bistro/textures`: `MASTER_Concrete_Plaster`, `Concrete2`,
  `MASTER_Brick_Small_Red`, `MASTER_Wood_Brown`,
  `Foliage_Linde_Tree_Large_Trunk`, `Pavement_Ground_Wet`).

Zbytek programu má svou vlastní licenci; tyto soubory pod ni nespadají.

## Vlastní textury

- **Jinou knihovnu** nastavíte na uzlu Output parametrem **Texture Folder**
  (nebo proměnnou prostředí `PG_TEXTURES`): složka se složkami pojmenovanými
  jako materiály.
- **Jednomu objektu** dáte texturu uzlem **Material** (parametr Texture):
  vyberte obrázek barvy ze sady z Poly Haven nebo ambientCG. Ostatní mapy se
  najdou vedle něj podle jmen (`_diff_`, `_rough_`, `_disp_`; `_Color`,
  `_Roughness`, `_Displacement`).
