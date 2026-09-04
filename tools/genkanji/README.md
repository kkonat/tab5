# genkanji

Rasterises the clock app's rain glyphs.

```bash
python tools/genkanji/genkanji.py C:/Windows/Fonts/msgothic.ttc \
       apps/clock/main/clock_kanji.c
```

Output is `apps/clock/main/clock_kanji.c`, which is checked in - a normal
`idf.py build` never runs this.

## Why this is not genfont

Same 1-bit format, same 16x32 cell, so the rain can put a kanji and a latin
character in the same grid position without either knowing about the other.
Two differences, both because kanji are square:

- the glyph is drawn into a 16x16 box and centred in the 32-tall cell rather
  than filling it. Stretching a kanji to twice its width does not produce a
  taller kanji, it produces a wrong one.
- codepoints are not contiguous, so glyphs are indexed from 128. The clock
  stores a rain cell as `128..255` for a kanji and an ordinary character code
  for latin, and 128 is the line between them.

## The set

49 characters, chosen for legibility at 16 pixels rather than for meaning:
weekday elements, weather, the simplest shapes in the language, the numerals,
and a clock's own vocabulary. Everything in it is under about nine strokes,
because anything denser is a grey smudge at this size. Edit `KANJI` in the
script and rerun to change it; the check on `last > 255` is what stops the set
growing past what a 1-bit font can index.

## The face

Any TTF or TTC with CJK coverage. `msgothic.ttc` is what this was generated
with because it is on the machine and it is a gothic face, which is the right
weight for glyphs this small.

Note the licensing point from [genicons](../genicons/README.md) applies here
too and is the reason the font itself is not vendored: pass the path to a face
you have, or to an OFL one such as Noto Sans JP, and rerun.
