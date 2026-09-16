#!/bin/sh
#
# Build and run moog's host tests.
#
#   ./run.sh            build and run
#   ./run.sh shot       build, then write one painted frame per orientation
#   ./run.sh clean      throw the build away
#
# Two things are under test and neither needs a tablet.
#
# The turn: turn.c draws a landscape picture into a portrait canvas so that
# nothing is rotated on the way to the glass, and every way of getting that
# wrong still fills the screen. So ngl's own sources are compiled here - the
# same ngl_draw.c, ngl_text.c and ngl_font_data.c that go on the device - and
# the same drawing is done twice, once turned and once flat, and compared pixel
# for pixel through a copy of ngl_screen.c's phys_index(). That is a real
# check against the machine's own definition of which way up it is, rather
# than a check that the app agrees with itself.
#
# The voice: a pitch is a zero-crossing count and an octave of modulation is a
# pitch that doubles, both of which are measurements rather than opinions. The
# one that matters most is exp2_fast(), which replaced powf() in the per-sample
# path - an exponential that is slightly wrong is an instrument that is
# slightly out of tune, and that is not what anybody would suspect first.
#
# What is NOT covered is how any of it looks, or whether the two loops share
# the machine, which is the whole question the app exists to answer. Those want
# a panel and a speaker.

set -e
cd "$(dirname "$0")"

if [ "$1" = "clean" ]; then
    rm -f test_moog test_moog.exe shot-90.ppm shot-270.ppm
    echo "cleaned"
    exit 0
fi

APP=..
NEOS=../../../neos/components
CC=${CC:-gcc}

#
# Three flags need explaining, and the first two are the same ones
# apps/radio/test/run.sh gives.
#
# -D'_Static_assert(c,m)=' turns off the ABI layout assertions in ngl.h. They
# say a struct that crosses into an app has the offsets it had when the ABI was
# frozen, which is a statement about a 32-bit target and is false on a 64-bit
# desktop. They are checked where they mean something - every target build of
# every app - and abi_check.py backs them up from the other side.
#
# -Wno-attributes is for neos_abi.h's `retain`, which this compiler does not
# have and does not need: nothing here is linked --gc-sections.
#
# -DNGL_INTERNAL is what lets ngl's own sources see the layout of
# ngl_surface_t, exactly as the component's CMakeLists defines it. It is passed
# to everything here rather than to ngl alone, which is a deliberate difference
# from the device build: the tests read pixels back out of a surface through
# ngl_surface_row(), and the app's own files still only use the public API.
#
# The test include path puts . first so that ngl_draw.c picks up the stub
# esp_heap_caps.h in this directory - the one thing in ngl that is ESP-IDF and
# not arithmetic.
#
$CC -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Wno-unused-function -Wno-attributes \
    -DNGL_INTERNAL=1 \
    -D'_Static_assert(c,m)=' \
    -I. -I"$APP/main" -I"$APP/../common/turn" -I"$APP/../common/widget" -I"$APP/../common/dsp" -I"$NEOS/neos_api/include" \
    -I"$NEOS/ngl/include" -I"$NEOS/ngl/src" \
    test_moog.c stubs.c \
    "$APP/../common/turn/turn.c" \
    "$APP/../common/widget/wg.c" \
    "$APP/main/mg_ui.c" \
    "$APP/main/mg_panel.c" \
    "$NEOS/ngl/src/ngl_draw.c" \
    "$NEOS/ngl/src/ngl_text.c" \
    "$NEOS/ngl/src/ngl_font_data.c" \
    -lm -o test_moog

# Whichever name the toolchain gave it. On Windows gcc appends .exe, and a
# stale extensionless file from another build sits in front of it otherwise.
#
# `shot` paints one frame and writes it out as the panel would show it - read
# back through turn_point(), so a wrong turn is visibly wrong in the file. The
# tests prove the arithmetic; this is for the half that is a matter of looking,
# and it is a great deal cheaper than a build, a card and a walk to the tablet.
#
if [ -x test_moog.exe ]; then
    ./test_moog.exe "$@"
else
    ./test_moog "$@"
fi
