# tools

Build-time generators. Nothing here runs on the device; each produces C source
that is checked in, so a normal `idf.py build` never needs Python.

| Tool | Produces | Rerun when |
|---|---|---|
| [genfont](genfont/) | `neos/components/ngl/src/ngl_font_data.c` | the UI font or its sizes change |
| [genicons](genicons/) | `neos/components/ngl/src/ngl_icon_data.{c,h}` | icons are added, or sizes change |
| [genkanji](genkanji/) | `apps/clock/main/clock_kanji.c` | the rain's glyph set changes |
| [genlcd](genlcd/) | the emulator's artwork, on the card | a different romset |
| [genoui](genoui/) | `apps/lanscan/card/oui.bin`, on the card | IEEE publishes a fresher registry |

genfont and genicons need Pillow, which is deliberately *not* an ESP-IDF
dependency:

```bash
python -m venv .venv && .venv/Scripts/pip install Pillow
```

Generated files carry a `GENERATED - do not edit` banner. Edit the tool or its
spec, never the output.

The last two are the exceptions to the first line of this file. genlcd's output
is somebody's traced artwork and a mask ROM, and genoui's is IEEE's registry;
neither is ours to check in, and both are card files rather than C, so they are
made locally and the apps that read them say so when they are absent.
