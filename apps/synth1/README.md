# Synth1

One oscillator, an LFO bending it, and a scope. It is the first step towards a
Minimoog on this tablet, and it exists to answer one question before any of the
rest is worth building:

**can this machine feed a codec every five milliseconds and still let a knob
move under a finger?**

Everything on the panel is arranged around that. The sound is deliberately the
simplest thing worth calling a synthesiser, because the interesting number is
not what the voice costs - it is what is *left*.

## The answer, and where to read it

Two lines along the bottom. The audio line has the veto.

```
audio  load 3.4%  blk 168us max 291us  lead 24ms  under 0
ui  58 fps  frame 4120us  paint 2260us  60Hz cap - tap to free  canvas psram
```

| | |
|---|---|
| `load` | what generating the sound costs, as a fraction of real time. This is the number the full instrument has to fit inside: two oscillators, noise, a four-pole ladder and two envelopes is something like five times this voice, so a load of 4% here says the Minimoog lands near 20% and a load of 20% says it does not fit at all. |
| `blk` | how long one 240-frame block took, and the worst since the app opened. The worst is the one that matters - a mean of 170 µs with a spike of 5 ms is a click. |
| `lead` | how much sound the speaker had left when the last block was handed over. The DMA holds 30 ms, so this sitting near 24-30 is the loop comfortably ahead. |
| `under` | **blocks that arrived to find the speaker already dry.** Not a percentage: any number here at all means the sound broke, and nothing on the other line excuses it. It is drawn in red. |
| `fps` / `frame` | the drawing loop. Capped at 60 by default; tap the two lines to take the cap off and see what the frame can actually do. |
| `paint` | how much of `frame` was spent handing rectangles to the panel rather than filling them. The two have different cures, so they are counted apart: a large `paint` means too much area is being presented, and a large `frame - paint` means too much work is being done to fill it. |

The LFO bar in the header is the one reading that is evidence rather than a
readout. It is fed from the value the audio thread last left behind, so a bar
that is moving means blocks are being generated. A frame counter would go on
counting perfectly happily over a dead codec.

**None of these numbers has been measured on the tablet yet** - the app is the
instrument, not the result. What has been measured is everything in
`test/`, which runs on a PC.

## The three decisions

Each is written up at length where it is made. In short:

**The voice runs on a thread of its own.** An app on NeOS runs on the boot task
at priority 1 and a pthread comes up at 5, so the scheduler takes the core away
from the drawing loop the moment the codec has room. Being late is therefore
not something the UI can cause, which is the difference between a synth and a
player: `apps/radio` pumps its codec from the same loop that draws, because its
sound arrives from a socket on somebody else's schedule and the loop has to be
there to receive it. This app generates its own sound, so the only thing that
could make it late is the frame it happens to be in the middle of - and a full
repaint is longer than the codec's cushion. See [`syn_audio.h`](main/syn_audio.h).

**Nothing is ever rotated.** The panel is 720x1280 portrait underneath, and the
ordinary path - draw into `ngl_screen()`, out through `ngl_flush()` - writes the
picture twice with a 90 degree rotate as the second write. `lab/defender`
measured that on this machine: the PPA scales in order at 85 Mpixels/s and
rotates at 19, because a rotate writes a column where it read a row and PSRAM
never gets to burst. So the canvas here is already the panel's shape and the
turn is in the coordinates - a rectangle maps to a rectangle, a circle to a
circle, and underneath every primitive is still ngl writing panel rows in
order. See [`turn.h`](../common/turn/turn.h), which `apps/moog` shares.

**A frame costs what changed.** A control repaints its cell when its own value
changed, the footer repaints its two rows when the instruments are due, and the
scope repaints a *column* when that column moved - erasing only the part of the
column the trace has left rather than clearing the box. The scope is the widget
that changes every single frame, so it is the one where this is the difference
between a few thousand pixels and a third of a megapixel. See the head of
[`syn_ui.c`](main/syn_ui.c).

## The controls

Landscape, and it may be turned over: only the accelerometer's x axis has a
say and only past half a g, so a tablet lying flat or held portrait keeps the
orientation it had. The display is locked rather than left to the OS watcher,
because a control surface that reshuffled itself mid-note would be unplayable.

| | |
|---|---|
| **PITCH** | 27.5 Hz to 3520 Hz, logarithmic. Seven octaves, which is the Minimoog's own range near enough |
| **LEVEL** | output level |
| **LFO RATE** | 0.05-20 Hz in the LO range, 20-2000 Hz in the HI one |
| **LFO AMT** | full deflection is an octave either way |
| **SHAPE** | saw, triangle, square, sample-and-hold. Drag to step, or tap to advance one |
| **RANGE** | tap HI or LO. LO is vibrato; HI takes the LFO into the audio band and the modulation becomes FM rather than a moving pitch - which is also the setting where every sample of the block genuinely differs from the last, so it is the one to leave running while reading the load |

Knobs are dragged vertically from anywhere and pick up from where they were
rather than jumping to the finger: 300 pixels of travel is the full range, so a
70-pixel knob is a handle and the screen is the track. Reaching for a rotary
gesture would put the finger over the thing it is turning.

The scope's timebase follows the pitch - about three periods of whatever is
playing - and triggers on a rising zero crossing, so the waveform stands still
instead of sliding at the difference between the pitch and the frame rate. The
capture is 43 ms, so the bottom octave shows a slice rather than a cycle.

The app takes the whole panel, so there is no system bar: the cross in the
corner is the way out, and NeOS's four-finger hold still works.

## Building and running

```
.\do.ps1 build-all -Apps synth1 -NoFirmware
.\do.ps1 deploy-card
```

`FAILED: synth1.elf` at the end of a build is normal and is not a regression:
apps link `-nostdlib -shared` against the syscall table, so the ordinary
firmware ELF target can never link. The target that matters, `synth1.app.elf`,
is finished before it, and `scripts/abi_check.py` is what says whether the
build is good.

## The tests

```
cd apps/synth1/test && ./run.sh
```

287 checks, none of which need a tablet. Two things are worth knowing about
what they cover.

**The turn is checked against ngl, not against itself.** `ngl_draw.c`,
`ngl_text.c` and `ngl_font_data.c` are compiled on the host unmodified, the
same drawing is done twice - once turned through `turn.c` into a 720x1280
canvas and once flat through ngl into an ordinary 1280x720 surface - and every
logical pixel is compared through a copy of `ngl_screen.c`'s `phys_index()`.
That function *is* this machine's definition of which way up it is. Rectangles,
lines and text all agree pixel for pixel at both landscape rotations. This
matters because every way of getting a rotation wrong - one axis flipped, both
flipped, text mirrored, correct at one rotation and not the other - still fills
the screen and still responds to touch.

**The voice is measured rather than listened to.** A pitch is a zero-crossing
count; an octave of modulation is a pitch that doubles, which a square LFO at
full depth says unambiguously; a glide is the absence of a step. The one that
really needed it is `exp2_fast()`, which replaced `powf()` in the per-sample
path: the first version was a fifth-order Taylor series and was 0.15 cents out
at the top of its interval, which is inaudible but was arbitrary rather than
chosen. It is now 0.0013 cents. A hand-rolled exponential that is slightly
wrong is an instrument that is slightly out of tune, and that is not the first
thing anyone would suspect.

What the tests say nothing about is how it looks, or whether the two loops
really do share the machine. Both want a panel and a speaker.

## What comes next

The eventual instrument is two oscillators, a noise source, a mixer, a
four-pole Moog ladder, a pitch LFO, a filter LFO, two ASR envelopes and a
master; three pages behind `<` `>` buttons at the screen edges, over a
1.5-octave touch keyboard.

All of that is `syn_dsp.c` and `syn_ui.c`. Neither the audio thread nor the
canvas has anything to learn from it - which is the point of them being
separate, and the reason this step was worth taking on its own.

The ladder is the part to be wary of: four one-pole sections with a
nonlinearity in each is where a soft-synth's budget usually goes, and its cost
is per sample rather than per note. The `load` figure with the LFO in its HI
range is the honest baseline to extrapolate from.
