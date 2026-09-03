# genfont

Rasterises a TTF into fixed-cell 1-bit bitmap fonts for the `ngl` graphics
library, and writes `neos/components/ngl/src/ngl_font_data.c`.

```bash
python tools/genfont/genfont.py C:/Windows/Fonts/CascadiaCode.ttf \
       neos/components/ngl/src/ngl_font_data.c
```

## What it emits

Two fonts, named semantically rather than by size, because the sizes get
retuned and these symbols are part of the OS ABI that apps link against:

| Symbol | Cell | Bytes |
|---|---|---|
| `ngl_font_small` | 16x32 | 6,080 |
| `ngl_font_large` | 24x48 | 13,680 |

ASCII 32..126 only. Glyphs are packed MSB-first, `bytes_per_row` per scanline,
cells stored consecutively from `first`.

## Changing the sizes

Edit `CELLS` at the top of the script and rerun. For each cell the script
searches point sizes and takes the largest whose advance width fits the cell
width *and* whose ascent+descent fit the cell height, so glyphs are never
clipped - they may just sit narrower than the cell.

The layout constants in the launcher and apps are hand-tuned to the current
sizes and do **not** follow automatically. Changing font size means revisiting
those.

## Font choice

Currently Cascadia Code, which ships with Windows under the SIL Open Font
License - so the generated bitmaps can be shared. Consolas would work as well
but is not redistributable. Any monospace TTF works; pass its path.

1-bit, no anti-aliasing. Fine at these cell sizes; if it ever looks coarse the
fix is a 2-bit alpha format rather than a bigger cell.
