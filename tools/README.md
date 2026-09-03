# tools

Build-time generators. Nothing here runs on the device; each produces C source
that is checked in, so a normal `idf.py build` never needs Python.

| Tool | Produces | Rerun when |
|---|---|---|
| [genfont](genfont/) | `neos/components/ngl/src/ngl_font_data.c` | the UI font or its sizes change |
| [genicons](genicons/) | `neos/components/ngl/src/ngl_icon_data.{c,h}` | icons are added, or sizes change |

Both need Pillow, which is deliberately *not* an ESP-IDF dependency:

```bash
python -m venv .venv && .venv/Scripts/pip install Pillow
```

Generated files carry a `GENERATED - do not edit` banner. Edit the tool or its
spec, never the output.
