# Písmo editoru

**Inter** 4.1 od Rasmuse Anderssona (<https://github.com/rsms/inter>), licence
SIL Open Font License 1.1 ([OFL.txt](OFL.txt)). Editor ho má zakompilované
(CMake z těchto souborů vyrobí `generated/EditorFonts.cpp`), takže vypadá
na každém systému stejně a nic nehledá za běhu.

Soubory jsou podmnožina řezů `Inter-Regular.ttf` a `Inter-SemiBold.ttf`
z vydání 4.1 (`extras/ttf`). Obsahuje latinku včetně češtiny, interpunkci,
šipky, matematické a geometrické značky; bez hintingu a bez tabulek
OpenType, které Dear ImGui nepoužívá. Každý řez má 33 kB místo 410 kB:

```bash
pyftsubset Inter-Regular.ttf --unicodes="U+0020-007E,U+00A0-017F,U+0192,U+02C6-02DD,U+2000-206F,U+20AC,U+2122,U+2190-21FF,U+2200-22FF,U+2300-23FF,U+25A0-25FF,U+2713-2714,U+2717" \
    --layout-features='' --no-hinting --desubroutinize --output-file=Inter-Regular.ttf
```

Znaky mimo tuto množinu doplní systémové DejaVu Sans, je-li nainstalované
(`Theme.cpp`).
