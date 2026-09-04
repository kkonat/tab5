# genlcd

Turns a MAME romset for an SM5A handheld into the one file `apps/nupogodi`
reads off the card.

```bash
python tools/genlcd/genlcd.py nupogodi.zip -o nupogodi.lcd
./do upload --file nupogodi.lcd --as apps/nupogodi/nupogodi.lcd
```

## What you have to supply

`nupogodi.zip`, the MAME romset. Two files:

| | |
|---|---|
| `im-02.bin` | 1856 bytes, `CRC32 cb820c32`, `SHA1 7e94fc25…`. The mask ROM out of a КБ1013ВК1-2. This *is* the game - the difficulty curve, the hare at 200 and 500 points, the tune. |
| `nupogodi.svg` | 154233 bytes. The case, photographed and traced, with every LCD segment in it as a separate shape. |

Neither is in this repo and neither is going to be. The ROM is Elektronika's
and the artwork is somebody's work in a MAME artwork pack; what is checked in
is the tool that turns a copy you already have into something the tablet can
run.

The app refuses to start without it and says so on the status line, which is
the only way you will meet this file.

## What it needs installed

- **Inkscape 1.x** on `PATH`, or named by `INKSCAPE_PATH`. Tested against
  1.0.1. It is what turns the SVG into pixels, and it is doing more work than
  that sounds: it resolves gradients, clip paths and the transforms of every
  group a segment happens to sit inside.
- **Pillow**, for cropping and for the greyscale. Only the romset path needs
  it; `--gw` does not.

## How it works

MAME's artwork labels every segment shape with a `<title>` of
`O.segment.common` - the same string that shows as a tooltip if you open the
SVG in a browser. So the artwork says which shape is which segment and the
tool has to know nothing about the game:

```
segment index = 8*O + 2*segment + common          (SM5A: 9 x 4 x 2 = 72)
```

Then, twice:

1. **The background.** Every segment shape is *removed* from the document -
   removed rather than hidden, because a hidden element can still reach the
   picture through a filter or a clip path - and what is left is rendered at
   the target size. That is the case, the printing on it, and the blank LCD.

2. **Each segment, alone.** `--export-id-only --export-area-page` renders the
   real document with everything but one shape hidden, at the page's own size.
   Every file therefore comes back already aligned with the background, which
   is why there is no coordinate arithmetic in this tool and no chance of a
   segment landing two pixels out. The shape is drawn dark on white, so the
   grey level *is* the multiplier the app wants - 255 where the segment is
   not, 0 where it is solid, the anti-aliased edge in between - and it is then
   cropped to where it is not white.

An LCD segment darkens the artwork printed behind it rather than covering it.
That is why coverage is a multiplier and not an alpha, and why the hens still
show through a lit egg.

## Size

The default fits the artwork into **996x580**, which is what the app has left
on a 1280x720 panel after the system bar, the two thumb columns and the strip
of mode switches - see `NPG_COL_*` and `NPG_STRIP_H` in
`apps/nupogodi/main/nupogodi.h`. `nupogodi.svg` is 1715x1080, so that comes
out at 920x580 and about 1.3 MB on the card.

`--fit WxH` changes it. The app reads the dimensions out of the header and
centres whatever it is given, so there is nothing to keep in step; an asset
smaller than the space is upscaled by a whole number at load time, which is
worth knowing mostly as the reason a 320x240 one looks soft.

## `--gw`

```bash
python tools/genlcd/genlcd.py --gw somegame.gw -o nupogodi.lcd
```

Reads the container bzhxx's
[LCD-Game-Emulator](https://github.com/bzhxx/LCD-Game-Emulator) uses, which
[LCD-Game-Shrinker](https://github.com/bzhxx/LCD-Game-Shrinker) produces from
the same romsets. Needs no Inkscape and no Pillow, because a `.gw` is already
rendered - but it is rendered at 320x240 for a Game & Watch's own screen, and
on this panel that upscales to something visibly soft. It is here because it
is a quick check that the app works, and because it is the only way to get
something running before you have a romset.

## The container

Little-endian throughout. One header, then four blobs.

```
 0   8   "NEOSLCD1"
 8   2   u16  width            the artwork, in pixels
10   2   u16  height
12   2   u16  segments         72 for SM5A
14   2   u16  reserved
16   4   u32  rom offset
20   4   u32  rom length
24   4   u32  background offset       width*height*2, RGB565
28   4   u32  segment table offset    segments * 12
32   4   u32  coverage offset
36   4   u32  coverage length
40  32   char title[32]         what the status line says on launch
72  --   payload
```

Segment table entry, 12 bytes:

```
 0   2   u16  x        on the artwork
 2   2   u16  y
 4   2   u16  w        zero for a segment this game does not use -
 6   2   u16  h        the entry stays, so that an index is an index
 8   4   u32  first nibble of this segment's coverage
```

Coverage is 4 bits per pixel, row major, **low nibble first** within a byte.
15 means "the segment is not here", 0 means black. The app expands it to a
byte each on load, which doubles a few hundred kilobytes on a tablet with
thirty-two megabytes and takes the nibble arithmetic out of the one loop that
runs per pixel.

## Other machines

The IM-02's chip is in a dozen other Elektronika toys - Хоккей, Охота,
Биатлон, Квака-задавака - and MAME emulates them all through the same driver
with the same 72 segments and the same key matrix. Their romsets go through
this tool unchanged, and the app reads whatever `nupogodi.lcd` turns out to
be: the title in the header is what it puts on the status line. The one thing
that would not survive is the button labels, which say GAME A and GAME B
because that is what is printed on this machine.

Anything from the SM510 family is a different segment formula (`64*x + 4*y +
z`) and a different CPU core, and this tool stops rather than guessing: it
looks for shapes titled `O.segment.common` and says so if it finds none.
