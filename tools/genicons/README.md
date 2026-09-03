# genicons

Rasterises named glyphs from an icon font into 8-bit alpha bitmaps, and writes
`neos/components/ngl/src/ngl_icon_data.c` plus a matching header.

```bash
python tools/genicons/genicons.py tools/genicons/icons.json \
       neos/components/ngl/src/ngl_icon_data.c
```

## Why alpha, not 1-bit like the text fonts

Icons are big enough that jagged edges show, and an alpha bitmap can be tinted
to any theme colour at draw time rather than baked. A 32x32 icon costs 1 KB,
which is nothing against 32 MB of PSRAM. Text fonts stay 1-bit only because
there are 95 glyphs per size.

Draw one with:

```c
ngl_icon(sc, x, y, &ngl_icon_close_32, TH_CLOSE_X);
```

## The spec file

`icons.json` names the font, the sizes to emit, and the glyphs:

```json
{
  "font": "C:/Windows/Fonts/segmdl2.ttf",
  "sizes": [24, 32],
  "icons": { "close": "0xE711", "wifi": "0xE701" }
}
```

Each entry produces `ngl_icon_<name>_<size>` for every size. Codepoints are
whatever the chosen font uses - they are **not** portable between icon fonts.

## Choosing an icon font

The default is Segoe MDL2 Assets purely because it ships with Windows and needs
no download. It is a Microsoft font: fine on your own machine, not
redistributable.

For anything you intend to share, switch to one of these and update the
codepoints:

| Font | Licence | Codepoints |
|---|---|---|
| Google Material Symbols | Apache-2.0 | fonts.google.com/icons |
| Font Awesome Free | CC BY 4.0 | fontawesome.com/search?o=r&m=free |

Point `font` at the downloaded TTF and replace the values in `icons`. Nothing
else changes.

## Sizing

For each glyph and cell the script finds the largest point size that fits the
whole glyph bounding box inside the cell, then centres it using that box - so
icons are optically centred rather than centred on the font's own origin, which
for icon fonts is often nowhere near the visible mark.
