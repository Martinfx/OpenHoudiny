# Obrázky a video

Záběr jde ven jako obrázek PNG, jako očíslovaná sekvence PNG, nebo rovnou
jako **video**: `.avi` (Motion JPEG) zapíše program sám, bez čehokoli
dalšího; `.mp4`, `.mov`, `.mkv` (H.264), `.webm` (VP9) a `.gif` zapíše přes
**ffmpeg**, když je nainstalovaný. Totéž umí editor i příkazová řádka,
pro simulace i pro náhled shaderu.

![Editor: render videa běží -- okno s průběhem, odhadem času a tlačítkem Stop](img/editor-render.png)

## 1. Rychlý start

```bash
./build/pgshader sim campfire fire.mp4             # celý záběr do videa (H.264 přes ffmpeg)
./build/pgshader sim campfire fire.avi             # totéž bez ffmpeg: Motion JPEG
./build/pgshader sim lakeside shot.mp4 --every 2   # každý druhý snímek, 15 fps
./build/pgshader sim campfire fire.png             # poslední snímek jako PNG
./build/pgshader render examples/shaders/fire.pgsg fire.mp4 --frames 90   # animovaný shader
```

V editoru:

| akce | kde |
|---|---|
| snímek na obrazovce jako PNG | **File › Render Image…**, nebo ikona fotoaparátu v záhlaví viewportu |
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
  Totéž stojí ve stavovém řádku a v terminálu (`pgshader: Rendered 150
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

## 4. Příkazová řádka

```
pgshader sim    NETWORK|EXAMPLE OUT.png|OUT.mp4|- [--frames N] [--every K] ...
pgshader render GRAPH.pgsg OUT.png|OUT.mp4 [--frames N] [--time S] [--size N] ...
```

- `sim` do videa dá každý snímek záběru (s `--every K` každý K-tý), do PNG
  jen poslední (s `--every K` očíslovanou sekvenci). Video se otevře dřív,
  než se začne simulovat: chybějící ffmpeg se ozve hned, ne po minutách.
- `render` do videa: `--frames` snímků náhledu (výchozí 90) po 1/30 s od
  `--time`, třeba smyčka ohně nebo kouře.
- Výpis řekne, kolik snímků a jakým kodekem se zapsalo a čím se kreslilo:

```
$ pgshader sim campfire fire.mp4 --frames 60
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

## 5. Když se obrázek „neuloží“

- Podívejte se na oznámení ve viewportu nebo na terminál: úspěch vypíše
  celou cestu (`pgshader: rendered /home/…/campfire.png (875 x 828)`),
  neúspěch důvod — nejde zapsat, a proč (např. `Permission denied`). Když
  ovladač při kreslení nahlásí chybu OpenGL, obrázek se uloží stejně a
  zpráva chybu připíše (`OpenGL reported error 0x…`).
- **Show** v oznámení otevře složku, kam soubor šel.
- Chybějící složky v cestě se vytvoří; do dialogu jde napsat i `~/…`.
- Render Frames do existující složky: otevřít ji (dvojklik) a **Choose**
  bez jména, nebo na ni jednou kliknout a Choose.

## 6. V kódu

| soubor | co dělá |
|---|---|
| `src/pg/io/Jpeg.h` | `encodeJpeg`, `writeJpeg`: baseline JPEG, 4:2:0, tabulky normy |
| `src/pg/io/Video.h` | `openVideo` → `VideoWriter` (`add`, `finish`): AVI sám, ostatní rourou do ffmpeg; `videoExtensions`, `ffmpegAvailable`, `frameRate` |
| `tools/pgshader/Offscreen.h` | kontext bez okna pro `render` a `sim`: EGL, nebo skryté okno GLFW |
| `tools/pgshader/RenderJob.h` | render po snímcích na pozadí editoru, okno s průběhem a Stop |
| `tests/test_video.cpp` | 5 testů: segmenty JPEG, struktura AVI a index, zlomky frekvence, chyby, dekódování přes ffmpeg |

## 7. Omezení

- Motion JPEG je velký (každý snímek celý): zhruba desetkrát víc než H.264.
- Zvuk žádný.
- Průhlednost (alfa) se do videa nezapisuje.
