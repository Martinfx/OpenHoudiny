# Barvy: AgX a ACES

Renderery počítají světlo, ne obraz: lineární hodnoty v primárních barvách
Rec. 709 (sRGB), kde 1 je bílá a slunce i plamen jdou daleko nad ni. Na
obrazovku je převádí **pohled** (View) uzlu Output. Kromě AgX z Blenderu
umí prototype oba výstupní převody **ACES** tak, jak je ukazuje OpenColorIO
v konfiguracích ACES, a EXR zapisuje i čte v barevných prostorech ACES.
Vše bez knihovny OpenColorIO, ověřeno proti ní hodnotu po hodnotě (§4).

![Syté barvy a bílá, každý řádek doprava jasnější, od 5 clon pod bílou po 7 nad ní, ve čtyřech pohledech. AgX jde do bílé plynule, ACES 1.0 stáčí modrou do fialové a oranžovou do žluté, ACES 2.0 odstín drží, Standard ořízne vše nad bílou](img/color-ramps.jpg)

![Stejný snímek táboráku (Cycles, světlo z jednoho EXR) v pohledech AgX Punchy, ACES 1.0, ACES 2.0 a Standard](img/color-views.jpg)

## 1. Rychlý start

```bash
./build/prototype sim campfire out/fire.png --renderer cycles --set output.render_view=aces2
./build/prototype sim campfire out/fire.exr --renderer cycles --set output.render_exr_space=acescg
```

V editoru: uzel **Output**, sekce **Render**, **View** a **EXR Color Space**.

## 2. Pohledy

| View | Co dělá |
|---|---|
| `agx_punchy` (výchozí) | AgX jako v Blenderu s lookem Punchy: víc kontrastu a barev |
| `agx` | AgX jako v Blenderu: jasné barvy přecházejí do bílé jako na filmu |
| `aces` (ACES Fit) | křivka viewportu (Narkowiczova aproximace ACES), každý kanál zvlášť |
| `aces1` (ACES 1.0) | „ACES 1.0 - SDR Video“ z konfigurací OpenColorIO pro ACES 1.3 |
| `aces2` (ACES 2.0) | „ACES 2.0 - SDR 100 nits (Rec.709)“ z konfigurací pro ACES 2.0 a 2.1 |
| `standard` | světlo, jak je, v sRGB; co je nad bílou, se ořízne |

Všechny jdou na obrazovku sRGB a platí pro oba renderery: path tracer
i Cycles, záložku Render, obrázky, sekvence i videa. Expozice (Output ›
Image › Exposure) se použije před pohledem.

- **ACES 1.0** je Reference Rendering Transform a výstup pro video, jak je
  má CTL Akademie: v RRT záře ve stínech sytých barev, modifikátor
  červené (jasně červená nezrůžoví), filmová křivka v AP1. Pak výstup:
  křivka pro kino (48 nitů) roztažená na 100 nitů, gama pro šero
  obývacího pokoje a o něco méně sytosti. Jasné syté barvy se stáčejí
  (oranžový plamen do žluté), to je známá vlastnost ACES 1.
- **ACES 2.0** tónuje světlost modelu vnímání barev (Hellwig 2022, jak ho
  ACES ladí), stlačí s ní sytost a barvy mimo Rec. 709 přivede dovnitř po
  přímkách k ohnisku. Odstín drží: plamen zůstane oranžový, jen jasnější
  a bělejší.

**Plate** (záznam kamery pod CG) se do světla vrací týmž pohledem
pozpátku ([plate.md](plate.md)). U ACES jsou zpětné převody ty, které má
OpenColorIO. ACES 1.0 navíc dopočítá světlo Newtonovou metodou: zpětný
modifikátor červené je jen přibližný, a sytá červená by se jinak vrátila
o dva stupně z 255 jinak (i v OpenColorIO).

## 3. EXR v prostorech ACES

**EXR Color Space** (Output › Render) říká, v jakém prostoru je světlo
v EXR, ať je z Cycles, z path traceru nebo z viewportu:

| `render_exr_space` | Prostor | Kdy |
|---|---|---|
| `rec709` (výchozí) | lineární Rec. 709 (sRGB), D65 | jak renderery počítají, Nuke a Blender ve výchozím nastavení |
| `acescg` | ACEScg: primárky AP1, bílá ACES (asi D60) | kompozice v pipeline ACES |
| `aces2065_1` | ACES2065-1: primárky AP0, obsáhnou každou barvu | předávání materiálu v ACES |

Převádí se Bradfordovou adaptací bílé, stejnými maticemi jako
v OpenColorIO. Do jiného prostoru jde světlo (`R`, `G`, `B`) a barva
povrchů (`albedo.*`). Hloubka, normály, vektory pohybu a masky zůstávají,
jak jsou, a `catcher.*` je dál násobitel plate po kanálech. Soubor má
vždy atribut **chromaticities** (primárky a bílá), takže ho Nuke, Resolve
nebo OpenImageIO poznají.

**Čtení.** EXR s chromaticities jiného prostoru než Rec. 709 (třeba
ACEScg z jiného programu) prototype převede do Rec. 709: plate ve viewportu
i v obou rendererech, obraz načtený z Pythonu (`pg.read_picture`)
a oblohu z obrázku v Cycles (tu načítá Cycles sám, převod je v jejím
shaderu). Soubor bez atributu je Rec. 709, jak to má OpenEXR.

## 4. Ověření

Referenci dalo **OpenColorIO 2.6** (`pip install opencolorio`) s vlastními
vestavěnými konfiguracemi `cg-config-v2.2.0_aces-v1.3_ocio-v2.4` (ACES 1.0)
a `cg-config-v5.0.0_aces-v2.1_ocio-v2.6` (ACES 2.0), převod z „Linear
Rec.709 (sRGB)“ na „sRGB - Display“. Skript `tests/data/aces/make_aces.py`
zapsal `aces.txt`: šedé od hlubokého stínu po tisícinásobek bílé, primárky
a barvy mezi nimi, pleť, obloha, listí, oheň, každé v 20 jasech, a 400
náhodných barev. Navíc 1531 barev obrazu zpátky a převody do ACEScg
a ACES2065-1.

| | odchylka od OpenColorIO |
|---|---|
| ACES 1.0, světlo → obraz (780 hodnot) | nejvýš 2,1 · 10⁻⁵ na škále 0–1 (setina stupně z 255) |
| ACES 2.0, světlo → obraz (780 hodnot) | nejvýš 8,8 · 10⁻⁶ |
| ACES 2.0, obraz → světlo | nejvýš 5 · 10⁻⁴ poměrně |
| ACEScg, ACES2065-1 | nejvýš 2 · 10⁻⁶ poměrně |
| obraz → světlo → obraz, oba ACES | nejvýš 1,8 · 10⁻⁴ (OpenColorIO u ACES 1.0: 8,6 · 10⁻³) |

Jeden pixel stojí asi 0,4 µs (ACES 1.0) a 0,5 µs (ACES 2.0). Snímek
1280 × 720 je na čtyřech jádrech hotový za 0,13 s. Tabulky ACES 2.0
(hranice gamutu po stupních odstínu) se spočítají při prvním použití
za 7 ms.

Testy — `tests/test_aces.cpp` (5):
- oba pohledy a převody prostorů proti hodnotám OpenColorIO;
- obraz zpátky na světlo, které ho ukáže, do čtvrt stupně z 255;
- pohledy Outputu jsou tytéž převody, expozice se použije předem;
- EXR v ACEScg má chromaticities AP1 a přečte se zpátky v Rec. 709.

Obloha z obrázku v ACEScg osvětlí podlahu v Cycles stejně jako táž obloha
v Rec. 709, do 2 % v každém kanálu
(`render_cycles_lights_the_scene_with_a_sky_picture`).

Test `render_unshown_gives_back_the_light_a_picture_shows` vrací všech
256 šedí a 4000 barev fotografie na svůj stupeň v každém ze šesti pohledů.

## 5. V kódu

| Soubor | Co dělá |
|---|---|
| `src/pg/render/Aces.h` | ACES 1.0 a 2.0 tam i zpět, kódování sRGB |
| `src/pg/core/ColorSpace.h` | ACEScg, ACES2065-1, chromaticities, Bradford |
| `src/pg/render/PathTracer.cpp` | `shown`, `unshown`: pohled pro oba renderery |
| `src/pg/render/Save.cpp`, `src/pg/gl/Volume.cpp` | EXR v prostoru z Outputu |
| `src/pg/io/Exr.cpp`, `ExrRead.cpp`, `Picture.cpp` | atribut chromaticities, převod při čtení |

## 6. Omezení

- **Konfigurace OpenColorIO** (`config.ocio`, proměnná `OCIO`) se nečtou:
  pohledy a prostory jsou pevně dané, jak je popisuje tahle stránka.
- **Obrazovka** je vždy sRGB. Rec. 1886 (gama 2,4), Display P3 ani HDR
  (PQ) nejsou.
- **Pracovní prostor** je lineární Rec. 709. Renderery nepočítají v ACEScg,
  barvy a textury se do něj nepřevádějí.
- **Viewport** ukazuje křivku ACES Fit, ne pohled zvolený na Outputu.
  Pohled Outputu je vidět v záložce Render a v obrázcích.
- **Looky** (LMT, třeba Reference Gamut Compression z ACES 1.3) a LUT
  (`.cube`) se nepoužívají.
