# Path tracer: pořádný render

Viewport kreslí scénu rychle přes OpenGL. Stín je tam jedna mapa, okolní
světlo je odhad a tráva nepropouští světlo. **Path tracer** počítá světlo
tak, jak se opravdu šíří. Z kamery sleduje paprsky, které se odrážejí od
povrchů, lámou se ve skle a ve vodě, prochází stébly a listy a končí na
slunci nebo na obloze. Počítá se na procesoru, takže nepotřebuje grafickou
kartu. Stejný render dá **záložka Render** v editoru vedle Viewportu
i příkazová řádka (`prototype sim … --renderer path`), třeba na farmě.

![Louka: vlevo viewport (OpenGL), vpravo path tracer, 64 vzorků na pixel](img/pathtracer-meadow.jpg)

![Tráva zblízka: vlevo viewport, vpravo path tracer. Stébla prosvítají, stín pod stromy je prosvětlený odraženým světlem](img/pathtracer-grass.jpg)

## 1. Rychlý start

V editoru:

1. Nad viewportem klikni na záložku **Render**.
2. Render začne hned a s každým průchodem je méně zrnitý. Změna parametru,
   snímku nebo pohledu ho spustí znovu.
3. Pohled se otáčí myší stejně jako ve viewportu: levé tlačítko otáčí,
   prostřední (nebo Shift + levé) posouvá, pravé a kolečko přibližují.
   Přes kameru (klávesa **0** ve viewportu) se renderuje záběr kamery
   v jejím poměru stran.
4. Nastavení je v uzlu **Output** v sekci **Render**. Ikona Output
   v nástrojové liště záložky uzel vybere.
5. Ikona fotoaparátu uloží render jako **PNG**, nebo jako **EXR**
   s lineárním světlem, hloubkou, albedem a normálami.

Z příkazové řádky:

```bash
./build/prototype sim meadow louka.png --renderer path                 # poslední snímek, nastavení z Output
./build/prototype sim meadow louka.png --renderer path --samples 256   # víc vzorků, méně šumu
./build/prototype sim meadow louka.exr --renderer path                 # EXR: R G B A, Z, albedo.*, N.*
./build/prototype sim meadow louka.mp4 --renderer path --samples 32    # každý snímek do videa
./build/prototype sim meadow louka.png --renderer path --size 1920x1080 --yaw 40 --pitch 12
```

Z Pythonu:

```python
net.render("louka.png", renderer="path", samples=128)
```

Path tracer nepotřebuje OpenGL, takže běží i v buildu bez EGL a bez
editoru.

## 2. Záložka Render

| prvek | co dělá |
|---|---|
| ▶ / ⏸ | pozastaví nebo spustí render (po návratu z Viewportu pokračuje sám) |
| ↻ | začne znovu od nuly |
| 📷 | uloží render do PNG nebo EXR |
| 25 / 50 / 100 % | velikost renderu: z panelu, nebo přes kameru z rozlišení kamery |
| ikona Output | vybere uzel Output s nastavením |
| `26 / 128 samples · 8.8 s · 0.46 M paths/s` | kolik vzorků je hotovo, jak dlouho to trvá a jak rychle to jde |

Render běží ve vlastním vlákně na všech jádrech a okno zůstává plynulé.
Při odchodu na záložku Viewport se zastaví, takže viewport dostane
procesor. Při změně scény se rozpracovaný průchod přeruší hned
a začne nový. Obraz se po každém průchodu odšumí. Velký obraz (nad
milion pixelů) se odšumí po prvních průchodech, potom po každém osmém
a na konci.

Na čtyřjádrovém stroji bez grafické karty je první obraz louky
(122 577 trsů trávy, 84 stromů) v 50 % panelu hotový asi za sekundu.
Použitelný je po 16–32 vzorcích, čistý po 128.

## 3. Nastavení (uzel Output › Render)

| parametr | výchozí | co dělá |
|---|---|---|
| Samples | 128 | vzorků na pixel, pak se render zastaví; víc je čistší, ale pomalejší |
| Bounces | 4 | kolikrát se světlo nejvýš odrazí; 0 je jen přímé slunce a obloha |
| Denoise | zapnuto | odšumění podle barvy, normály a hloubky |
| F-Stop | 0 | clona objektivu; 0 znamená vše ostré, 2,8 malou hloubku ostrosti |
| Focus | 0 m | vzdálenost ostrosti; 0 zaostří na to, co je uprostřed obrazu |
| Clamp | 20 | nejvíc, kolik jeden odraz přidá pixelu; bere světlé tečky (fireflies) |
| Sun Size | 0,53° | úhlový průměr slunce: větší slunce dává měkčí stíny |

Světlo, obloha, podlaha, expozice a barva vody jsou ze stejného **Looku**
jako ve viewportu, takže jas obou sedí. Test ověřuje, že podlaha na slunci
má v path traceru stejnou hodnotu jako ve viewportu (0,8035 proti 0,8005).

## 4. Materiály

Materiál se čte z atributů geometrie, jako v Houdini: z primitivu, jinak
z prvního bodu, jinak z detailu.

| atribut | výchozí | co dělá |
|---|---|---|
| `Cd` | šedá | barva |
| `roughness` | 0,5 | 0 zrcadlo, 1 matný povrch (odlesk GGX) |
| `metallic` | 0 | 1 kov: barva tónuje odraz |
| `translucency` | 0 | kolik rozptýleného světla projde na druhou stranu: list, stéblo, papír |
| `glass` | – | 1 tabule skla (lom a Fresnel, index 1,5), 2 prasklina (matná bílá) |

Povrch vody je voda s indexem lomu 1,33. S hloubkou nabírá barvu Water
Looku (`waterColor`, `waterClarity`).

**Vegetace** má průsvitnost nastavenou sama: stébla z uzlu Grass 0,35,
listy z uzlu Tree 0,4, kůra 0. Proti slunci proto tráva a listí svítí.
`roughness` se nezapisuje. Po Merge s jinou geometrií by chybějící
hodnota 0 udělala z terénu zrcadlo.

## 5. Jak to funguje

- **Scéna** (`src/pg/render/Scene.h`): trojúhelníky zobrazené geometrie
  mají stejné normály a barvy jako ve viewportu (`sim::shadedTriangles`).
  Ke scéně patří i kusy a látka ze solverů, povrch vody a objekty
  (koule, kvádry… počítané přesně). Každá síť má hierarchii obalových
  kvádrů (BVH, SAH se 12 přihrádkami). Nad sítěmi je horní hierarchie.
  **Instance** (`core/Instances.h`) mají síť prototypu jednou a jen se
  umístí, takže louka se 122 000 trsů stojí 8 sítí trsů plus umístění.
  Horní hierarchie se staví paralelně a deterministicky.
- **Světlo** (`src/pg/render/PathTracer.h`): slunce je kotouč, přímo se
  vzorkuje v každém odrazu a váží se s odrazy (multiple importance
  sampling). Obloha je vzorec z Looku včetně Sky Behind. Povrch je
  kombinace difúzního rozptylu, odlesku GGX a průchodu na druhou stranu
  (translucency). Sklo a voda odrážejí a lámou podle Fresnela a Snella.
  Od třetího odrazu rozhoduje ruská ruleta.
- **Kamera**: tenká čočka (F-Stop, Focus). Pixel je vzorkovaný po celé
  ploše, takže hrany jsou vyhlazené.
- **Determinismus**: náhodná čísla vzorku závisí jen na pixelu, čísle
  vzorku a seedu. Render je proto na libovolném počtu vláken stejný (test
  `render_is_the_same_however_it_is_run`).
- **Odšumění**: à-trous vlnkový filtr (Dammertz a kol.) nad světlem, které
  povrch dostal. Barva povrchu se vydělí a potom vrátí, takže textura
  zůstane ostrá. Filtr se zastaví na hranách barvy, normály a hloubky
  a tam, kde se pixely liší víc než o šum (rozptyl vzorků z okolí 5 × 5).
- **Výstup**: expozice, tónová křivka ACES (Narkowicz) a gama 2,2, stejně
  jako viewport. EXR ukládá lineární světlo bez křivky.

## 6. Výkon

Čtyři jádra (Xeon 2,8 GHz), RelWithDebInfo:

| scéna | rozlišení | vzorků | čas |
|---|---|---|---|
| louka zdálky (tráva, stromy, keře) | 480 × 270 | 1 průchod | 0,84 s |
| louka zdálky | 640 × 360 | 64 | 96 s |
| tráva zblízka (paprsky jdou hluboko do trávy) | 640 × 360 | 64 | 157 s |

Uvaření louky, stavba scény (horní hierarchie přes 122 000 instancí)
a odšumění trvají dohromady necelou sekundu. Zbytek je sledování paprsků
hustou trávou.

## 7. Co zatím chybí

- Plyn (kouř, oheň, prach), déšť, drť jako body, plate a holdouty.
  Tyhle prvky kreslí jen viewport.
- Rozmazání pohybem (motion blur) a průchody pohybu a masek do EXR.
- Textury a UV, normálové mapy, subsurface scattering.
- Světla kromě slunce a oblohy (bodová, plošná), HDRI obloha.
- Vektorové instrukce (SIMD) a širší BVH: dnes je to skalární kód, zhruba
  10× pomalejší než Embree.
- Adaptivní vzorkování: víc vzorků tam, kde je šum.
