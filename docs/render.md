# Obrázky a video

Záběr jde ven jako obrázek PNG, jako očíslovaná sekvence PNG, nebo rovnou
jako **video**: `.avi` (Motion JPEG) zapíše program sám, bez čehokoli
dalšího; `.mp4`, `.mov`, `.mkv` (H.264), `.webm` (VP9) a `.gif` zapíše přes
**ffmpeg**, když je nainstalovaný. Totéž umí editor i příkazová řádka,
pro simulace i pro náhled shaderu. Pro compositing jde snímek do **EXR**:
v lineárním světle a s průchody — hloubkou, vektory pohybu a maskami
([§4](#4-exr-pro-compositing)). Pořádný render se sledováním světla
(path tracer) je v záložce **Render** a v `--renderer path`: viz
[pathtracer.md](pathtracer.md).

![Editor: render videa běží -- okno s průběhem, odhadem času a tlačítkem Stop](img/editor-render.png)

## 1. Rychlý start

```bash
./build/prototype sim campfire fire.mp4             # celý záběr do videa (H.264 přes ffmpeg)
./build/prototype sim campfire fire.avi             # totéž bez ffmpeg: Motion JPEG
./build/prototype sim lakeside shot.mp4 --every 2   # každý druhý snímek, 15 fps
./build/prototype sim campfire fire.png             # poslední snímek jako PNG
./build/prototype sim wall_collapse zed.exr --every 1   # každý snímek jako EXR s průchody
./build/prototype render examples/shaders/fire.pgsg fire.mp4 --frames 90   # animovaný shader
```

V editoru:

| akce | kde |
|---|---|
| snímek na obrazovce jako PNG | **File › Render Image…**, nebo ikona fotoaparátu v záhlaví viewportu |
| snímek na obrazovce jako EXR s průchody | **File › Render Image…**, přípona `.exr` |
| všechny snímky jako PNG | **File › Render Frames…** (složka) |
| celý záběr jako video | **File › Render Video…**, nebo ikona filmu v záhlaví viewportu |
| animovaný náhled shaderu | v síti Shaders **File › Save Preview Video…** (5 s, 720 × 720) |

Render jde kamerou, pokud ji síť má připojenou do Outputu, v rozlišení jejího
obrazu; jinak pohledem viewportu v jeho velikosti. Obrázek je vždy
vyhlazený: kreslí se ve dvojnásobném rozlišení a zmenší se.

## 2. Render sekvence a videa v editoru

Render Frames a Render Video kreslí snímek po snímku **na pozadí okna**:
editor nezamrzne, okno ukazuje, kolik je hotovo, kolik trvá snímek a kolik
zbývá, a **Stop** (nebo Esc) render ukončí. Co se stihlo, zůstane: video se
dopíše a jde přehrát, snímky PNG zůstanou ve složce.

- Záběr se renderuje celý, od snímku 1 do posledního podle Outputu.
  Snímky, které simulace ještě nespočítala, render počká („Waiting for the
  simulation to reach frame 41…“) — video jde spustit hned po otevření
  scény.
- Každý snímek se vykreslí tak, jak by ho ukázal viewport na tom snímku:
  objekty a vzhled animované klíči, zobrazená geometrie (třeba body
  z Liquid Points) toho snímku, pohled kamery toho snímku. Bez vodítek,
  gizma a zvýraznění výběru.
- Když se simulace zastaví dřív (plná cache, snímky načtené z disku
  končí), render skončí tam a řekne proč.
- Po skončení ukáže viewport **oznámení**: co se zapsalo a kam, s tlačítky
  **Open** (otevře soubor v programu, který na to systém má) a **Show**
  (otevře složku). U dlouhého renderu oznámení zůstane, dokud se nezavře.
  Totéž stojí ve stavovém řádku a v terminálu (`prototype: Rendered 150
  frames into fire.mp4 …`).
- Dialog nabízí složku, kam šel poslední render; jinak složku sítě, jinak
  aktuální složku — a když do ní nejde zapisovat (program spuštěný
  z nabídky prostředí bývá v `/`), domovskou.

## 3. Formáty videa

| přípona | kodek | co je potřeba |
|---|---|---|
| `.avi` | Motion JPEG, kvalita 90, každý snímek klíčový | nic — kodér JPEG i kontejner AVI jsou v programu |
| `.mp4` `.mov` `.mkv` | H.264 (libx264, CRF 18, yuv420p); bez libx264 OpenH264 nebo MPEG-4 part 2 | ffmpeg |
| `.webm` | VP9 (CRF 28), bez něj VP8 | ffmpeg s libvpx |
| `.gif` | paleta spočítaná ze záběru, rozptyl sierra2_4a, opakuje se | ffmpeg |

- ffmpeg se hledá na `PATH`; proměnná **`PG_FFMPEG`** může ukázat jinam
  (`PG_FFMPEG=/opt/ffmpeg/bin/ffmpeg`). Bez ffmpeg nabízí dialog jen `.avi`
  a příkazová řádka u `.mp4` řekne, že ho potřebuje.
- H.264 a VP9 chtějí sudé rozměry: lichý řádek či sloupec se doplní
  opakováním posledního. AVI si rozměr nechá.
- Snímková frekvence je ta z Outputu (30 fps); `--every K` ji dělí K, aby
  video běželo ve skutečném čase. Necelé frekvence se zapíší jako zlomek
  (29,97 → 2997/100).
- AVI má limit 2 GB (32bitová RIFF); pro delší videa je `.mp4`.

Ověřeno: AVI i MP4/WebM dekóduje ffmpeg 6.1, snímky se vrací na svá místa
a blízko originálu (test `videos_decode_to_what_went_in_where_ffmpeg_is`);
JPEG má na skutečném renderu PSNR 44,6 dB a stejné číslo jako kodér JPEG
v ffmpeg na umělém obrázku s ostrými hranami (26,0 dB).

## 4. EXR pro compositing

`prototype sim záběr OUT.exr` zapíše snímky do OpenEXR (s `--every K` každý
K-tý jako `OUT_0001.exr`…, s `--start` a `--end` jen díl záběru). Zapisovač
je vlastní, bez knihovny; soubory čte knihovna OpenEXR 3.5 kanál po kanálu
stejně, 32bitové bit po bitu.

| kanál | co v něm je |
|---|---|
| `R`, `G`, `B`, `A` | obraz v **lineárním světle** (half float): expozice ano, tónová křivka a gama ne, takže světlé nebe a prach proti slunci jdou nad 1. `A` je 1 — obraz je celý, s pozadím |
| `Z` | hloubka nejbližšího povrchu podél osy pohledu, v metrech (float); kde povrch není (nebe), nekonečno |
| `forward.u`, `forward.v` | **vektory pohybu**: o kolik pixelů se bod posune do dalšího snímku, doprava a nahoru (jako v Nuke). Kusy a zobrazená geometrie podle rychlosti svých bodů `v`, všechno podle pohybu kamery |
| `mask.floor`, `mask.geometry`, `mask.pieces`, `mask.objects`, `mask.water` | kolik z pixelu je podlaha, zobrazená geometrie, kusy RBD, objekty, voda: pokrytí z vyhlazení 2 × 2 |
| `mask.smoke` | kolik z toho, co je za kouřem, kouř zakrývá: jeho neprůhlednost |

Když má kamera **plate** (obraz záběru), je v `R`, `G`, `B` jen CG a `A`
říká, kolik z pixelu zakrývá. Kanály `catcher.R/G/B` pak říkají, čím
plate vynásobit tam, kde na něj CG vrhá stín nebo svítí oheň. Záběr je
`plate × catcher × (1 − A) + RGB` ([plate.md](plate.md#4-exr-pro-compositing-nad-plate)).

Jak se to počítá: renderer kreslí v režimu průchodů do 16bitových floatů
a vedle obrazu do dvou dalších cílů (MRT). Povrchy se nejdřív rasterizují
do G-bufferu a každý roh trojúhelníku dostane polohu teď a v příštím
snímku (bod posunutý o `v` × délka snímku, promítnutý kamerou příštího
snímku). Rozdíl po pixelech je vektor pohybu. Hlavní průchod pak k obrazu
zapíše hloubku, neprůhlednost kouře a to, co je v pixelu za povrch.
Podlahu, objekty, vodu a nebe posouvá jen kamera. Drť a déšť jsou
v obraze, ale ne v hloubce, maskách a pohybu.

Ověřeno: kamera jedoucí doprava posune nehybnou scénu doleva
(`forward.u` −0,64 px), kamera jedoucí nahoru dolů (`forward.v` −0,59 px, blízká
podlaha −3 px); obraz bez průchodů je pixel po pixelu stejný jako předtím.

## 5. Příkazová řádka

```
prototype sim    NETWORK|EXAMPLE OUT.png|OUT.mp4|- [--frames N] [--every K] ...
prototype render GRAPH.pgsg OUT.png|OUT.mp4 [--frames N] [--time S] [--size N] ...
```

- `sim` do videa dá každý snímek záběru (s `--every K` každý K-tý), do PNG
  jen poslední (s `--every K` očíslovanou sekvenci). Video se otevře dřív,
  než se začne simulovat: chybějící ffmpeg se ozve hned, ne po minutách.
- `render` do videa: `--frames` snímků náhledu (výchozí 90) po 1/30 s od
  `--time`, třeba smyčka ohně nebo kouře.
- Síť bez simulace, jen se zobrazenou geometrií, se kreslí kamerou
  Outputu po všech jeho snímcích, když ji Output má: layout, previz nebo
  plate natočený z kulisy ([plate.md](plate.md#6-render-jen-z-geometrie)).
  Bez kamery je to jeden obrázek z pohledu na geometrii.
- Výpis řekne, kolik snímků a jakým kodekem se zapsalo a čím se kreslilo:

```
$ prototype sim campfire fire.mp4 --frames 60
wrote fire.mp4 (60 frames at 30 fps, H.264 (ffmpeg)): campfire, gas 64 x 96 x 64 cells, 60 frames (2.0 s); simulation 67.1 ms/frame, rendering 395 ms/image through EGL
```

### Čím se kreslí bez okna

Příkazy nepotřebují okno. OpenGL kontext hledají v tomto pořadí:

1. **EGL bez displeje** — Mesa surfaceless (i bez GPU, llvmpipe), pak
   každé GPU zařízení zvlášť (cesta ovladače NVIDIA bez X), pak výchozí
   displej;
2. **skryté okno GLFW** — v buildu s editorem, když EGL nedá nic a displej
   je k dispozici.

Když nevyjde nic, chyba vypíše, co který způsob řekl, například:

```
sim: no OpenGL context to draw with -- EGL: no EGL context without a window
(surfaceless: does not start (EGL error 0x3001); device 0: ...); a hidden window: X11: Failed to open display
```

`-` místo jména obrázku kreslení vynechá úplně (jen cache a export,
viz [cache.md](cache.md)).

## 6. Když se obrázek „neuloží“

- Podívejte se na oznámení ve viewportu nebo na terminál: úspěch vypíše
  celou cestu (`prototype: rendered /home/…/campfire.png (875 x 828)`),
  neúspěch důvod — nejde zapsat, a proč (např. `Permission denied`). Když
  ovladač při kreslení nahlásí chybu OpenGL, obrázek se uloží stejně a
  zpráva chybu připíše (`OpenGL reported error 0x…`).
- **Show** v oznámení otevře složku, kam soubor šel.
- Chybějící složky v cestě se vytvoří; do dialogu jde napsat i `~/…`.
- Render Frames do existující složky: otevřít ji (dvojklik) a **Choose**
  bez jména, nebo na ni jednou kliknout a Choose.

## 7. V kódu

| soubor | co dělá |
|---|---|
| `src/pg/io/Jpeg.h` | `encodeJpeg`, `writeJpeg`: baseline JPEG, 4:2:0, tabulky normy |
| `src/pg/io/Video.h` | `openVideo` → `VideoWriter` (`add`, `finish`): AVI sám, ostatní rourou do ffmpeg; `videoExtensions`, `ffmpegAvailable`, `frameRate` |
| `tools/prototype/Offscreen.h` | kontext bez okna pro `render` a `sim`: EGL, nebo skryté okno GLFW |
| `tools/prototype/RenderJob.h` | render po snímcích na pozadí editoru, okno s průběhem a Stop |
| `tests/test_video.cpp` | 5 testů: segmenty JPEG, struktura AVI a index, zlomky frekvence, chyby, dekódování přes ffmpeg |
| `src/pg/io/Exr.h` | `formatExr`, `writeExr`: OpenEXR 2, řádky, half i float, RLE jako OpenEXR, textové a maticové atributy |
| `src/pg/gl/Volume.h` | `VolumeRenderer::passes`, `readPasses`, `writePassesExr`: průchody a jejich zápis |
| `tests/test_exr.cpp` | 3 testy: hlavička a řádky podle rozvržení OpenEXR a hodnoty zpět vlastním čtením RLE, běhy se zmenší a šum zůstane, chyby |

## 8. Omezení

- EXR je komprimované jen RLE: masky a nebe se zmenší hodně, obraz,
  hloubka a pohyb málo — 1280 × 720 má asi 15 MB. ZIP (deflate) zatím ne.
- Sekvence do EXR jen z příkazové řádky; editor zapíše do EXR jeden snímek
  (Render Image).
- Kouř nemá vektory pohybu ani hloubku (rychlost plynu snímek nedrží).
- Kryptomatte ne: masky jsou po druzích povrchu, ne po objektech.

- Motion JPEG je velký (každý snímek celý): zhruba desetkrát víc než H.264.
- Zvuk žádný.
- Průhlednost (alfa) se do videa nezapisuje.
