# Editor font

**Inter** 4.1 by Rasmus Andersson (<https://github.com/rsms/inter>), licensed
under the SIL Open Font License 1.1 ([OFL.txt](OFL.txt)). The font is compiled
into the editor (CMake generates `generated/EditorFonts.cpp` from these files),
so the editor looks the same on every system and looks nothing up at run time.

The files are a subset of the `Inter-Regular.ttf` and `Inter-SemiBold.ttf`
styles from the 4.1 release (`extras/ttf`). They contain Latin script
including Czech, punctuation, arrows, and mathematical and geometric symbols;
there is no hinting and none of the OpenType tables that Dear ImGui does not
use. Each style takes 33 kB instead of 410 kB:

```bash
pyftsubset Inter-Regular.ttf --unicodes="U+0020-007E,U+00A0-017F,U+0192,U+02C6-02DD,U+2000-206F,U+20AC,U+2122,U+2190-21FF,U+2200-22FF,U+2300-23FF,U+25A0-25FF,U+2713-2714,U+2717" \
    --layout-features='' --no-hinting --desubroutinize --output-file=Inter-Regular.ttf
```

Characters outside this set fall back to the system DejaVu Sans, if it is
installed (`Theme.cpp`).
