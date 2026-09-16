# Moog

A Minimoog Model D: three oscillators, noise, a mixer, a four-pole ladder and
two contours, over an octave and a half of touch keyboard.

Thirty-six controls will not fit across 1280 pixels next to each other, so the
panel is cut where the instrument is:

| page | sections | what it is |
|---|---|---|
| 1 | CONTROLLERS, OSCILLATOR BANK | what the sound is made of |
| 2 | MIXER, MODIFIERS | how much of each, and what happens to it |

`<` and `>` at the screen edges change pages. The master fader is on the right
of both of them, because it belongs to neither: it is the one control you reach
for without looking, and a volume you have to change pages to find is a volume
you cannot turn down in a hurry.

## Playing it

Low-note priority and single trigger, which is the Model D's and is half of why
Minimoog lines sound the way they do. The lowest key held is the one that
sounds, so a left hand can hold a bass note while the right plays over it; and
the contours restart only when the gate *rises*, so a key pressed while another
is still down slides to the new pitch under a note that is already sounding
rather than striking it again. GLIDE decides how fast that slide is.

More than one finger at a time, and a knob at the same time as a chord — NeOS
reports every contact, so which of them is a key and which is a gesture is
sorted out in `mg_app.c`. Turning two knobs at once is the one thing it does
not offer.

| | |
|---|---|
| a knob | drag up and down from anywhere; it picks up from where it was rather than jumping to the finger, so a 60-pixel knob still has 300 pixels of travel |
| a selector | drag to step through its positions, or tap to advance one |
| a switch | tap the half you want — both positions are labelled and the lit one is in force |
| the header | tap to come off the 60 Hz pacing, which is the only way to see what the frame can actually do |
| the cross | the way out. The app takes the whole panel, so there is no system bar and no close button but that one |

Four fingers held anywhere is NeOS's escape gesture, so playing a four-note
chord will occasionally ask the app to close. The touch task sees that before
the app does and an app that could swallow it would be an app with no way out
at all, so it stays.

## What is not the Minimoog's

Three deliberate departures, each because the tablet is not the instrument:

**No wheels.** The Model D's pitch and modulation wheels are to the left of the
keyboard and are not in the panel photograph this was built from. What that
costs is the modulation *depth*: MODULATION MIX still decides what the source
is made of — oscillator 3 at one end, noise at the other — but with no wheel to
set how much of it arrives, the two routing switches carry a fixed depth chosen
to be musical. A semitone and a half of pitch, two octaves of cutoff. A wheel
is the obvious next control, and it is a row in `mg_defs[]` and two lines
elsewhere.

**FEEDBACK / OFF, not FEEDBACK / SIDECHAIN.** The external input is real and
does what the famous patch-cable trick does — the output returned to the mixer,
which is what makes a Minimoog scream. The other position would be an external
signal, and `neos_sys.h` has no audio *input*: apps can take the speaker and
nothing else. A switch position that fed nothing would be worse than one that
says OFF.

**The oscillators are digital and say so.** Sawtooth and the three rectangles
carry a PolyBLEP correction at each discontinuity, which takes the worst of the
aliasing away; the triangle is generated naively, because its harmonics fall
off as the square of their number and what folds back is 40 dB down where a
sawtooth's is 6. The ladder is Stilson and Smith's four-pole with a saturating
nonlinearity in the feedback, not a component model.

## The two loops

Both of the decisions `apps/synth1` was written to test are taken as settled
here and are not re-argued:

**The voice runs on a thread above the drawing loop.** An app on NeOS runs on
the boot task at priority 1 and a pthread comes up at 5, so the scheduler takes
the core away from whatever is repainting the instant the codec has room.
Nothing in the panel has to be quick for the sound to be continuous. See
[`mg_audio.h`](main/mg_audio.h) — the patch crosses into the thread under a
seqlock, which a six-knob synth did not need and a thirty-six-field one does.

**Nothing is ever rotated.** The canvas is the panel's own 720x1280 and the
landscape turn is in the coordinates, so no frame goes through the PPA's
rotate — the expensive half of what `lab/defender` measured. That layer is
[`apps/common/turn`](../common/turn), shared with synth1, and the widgets are
[`apps/common/widget`](../common/widget), also shared: two synthesisers on one
machine drawn in two hands would look like two programs by two people.

**A frame costs what changed.** The unit of repainting is the control: dragging
a knob repaints one cell of about 20,000 pixels out of 921,600, and pressing a
key repaints that key and the black keys that overhang it. The header reports
`fps`, the audio load as a percentage of real time, and `u` — the underrun
count, which is the only number with a veto. Any value there at all means the
sound broke, and it is drawn in red.

**None of those numbers has been measured on the tablet yet.** The header is
the instrument; what has been measured is everything in `test/`.

## Building and running

```
.\do.ps1 build-all -Apps moog -NoFirmware
.\do.ps1 upload --app moog          # straight to the card on a running tablet
```

`FAILED: moog.elf` at the end of a build is normal and is not a regression:
apps link `-nostdlib -shared` against the syscall table, so the ordinary
firmware ELF target can never link. The target that matters, `moog.app.elf`, is
finished before it, and `scripts/abi_check.py` is what says whether the build is
good.

`do.ps1 screencap` will **not** show this app. `neos_screencap.c` captures the
logical back buffer — what apps draw into — and this one never draws there. Use
`test/run.sh shot`, which paints a real frame on the host and reads it back
through the same map the panel uses.

## The tests

```
cd apps/moog/test && ./run.sh          # 1349 checks
cd apps/moog/test && ./run.sh shot     # one frame per page, as PPM
```

The turn is not retested here — it is compared pixel for pixel against ngl's
own primitives in `apps/synth1/test`, at both rotations, and it is the same
code. What these cover is what is new:

**Thirty-six rectangles** that must be on screen, must not overlap, and must
each answer to a touch in their middle — on the page they are on and not on the
other. A cell overlapping its neighbour is a finger that turns the wrong knob,
and since the hit test returns the first match it is always the *same* wrong
knob, which reads as one control being dead rather than two being confused.

**The keyboard**, whose picture and hit test are two separate pieces of
arithmetic over the same semitone pattern. Where they disagree, a key lights
when you press the one beside it.

**That painting one control at a time arrives at the picture a full repaint
would have drawn** — the whole screen compared, after a sequence of knob moves
and key presses. The way per-cell repainting goes wrong is not a crash; it is a
neighbour's ticks or a black key's overhang rubbed out and not put back, which
on a moving panel reads as something smearing and gets shrugged at.

**And the instrument**, driven at every extreme the panel can reach. Three
things came out of writing those:

- the whole instrument was tuned five and three quarter octaves flat, because
  `base` subtracted A440's note number from what was an *offset* from a note
  rather than a note. It sounded exactly like a broken oscillator and was
  nothing of the sort.
- the textbook ladder's `y - y³/6` saturator is not a saturator. Its derivative
  goes negative past 1.41 and it runs away past 2.45 — fine for one oscillator
  at moderate level, and this panel can ask for three at full plus noise plus
  the output fed back with EMPHASIS at the stop. That is a self-oscillating
  filter turning into full-scale noise. It is now a bounded soft clip.
- and the release tail was being tested against silence rather than against
  what it was decaying *from*, which was the test being wrong rather than the
  code.

The stability test now drives every waveform at every range at three pitches
with everything on and everything routed, and checks the filter state — because
an int16 cannot hold a NaN, so a ladder that has come apart writes zeroes and
reads as a perfectly quiet pass.
