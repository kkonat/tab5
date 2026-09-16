#!/bin/sh
#
# Build and run the radio app's host tests.
#
#   ./run.sh            build and run
#   ./run.sh clean      throw the build away
#
# Everything under test here is ordinary C with no NeOS in it, so it compiles
# and runs on the machine you are editing on - which is the point. A resampler
# that drops a sample, a biquad whose coefficients were transcribed wrong and
# an ICY demux that loses a byte when a metadata block straddles a read are
# all bugs you would otherwise meet as "it crackles sometimes", on a tablet,
# through a serial log.
#
# What is NOT covered is anything that draws. ngl is stubbed out to a set of
# calls that count and return, so the tests check the layout and the hit
# testing agree - which is the part that can be wrong arithmetically - and say
# nothing about what the page looks like. That still wants eyes on a panel.

set -e
cd "$(dirname "$0")"

if [ "$1" = "clean" ]; then
    rm -f test_radio test_radio.exe tone.mp3 tone48.mp3
    echo "cleaned"
    exit 0
fi

APP=..
NEOS=../../../neos/components
CC=${CC:-gcc}

#
# The fixtures for the end-to-end test: real MP3 files, made here rather than
# checked in. A repository of source should not carry two audio files to prove
# a decoder decodes, and ffmpeg is on most machines that have a toolchain.
# Without it those three cases say SKIP and the rest of the suite still runs.
#
if [ ! -f tone.mp3 ] && command -v ffmpeg >/dev/null 2>&1; then
    echo "generating fixtures with ffmpeg"
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=1000:sample_rate=44100:duration=2" \
        -af "volume=20dB" -ac 2 -c:a libmp3lame -b:a 128k -y tone.mp3
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "sine=frequency=440:sample_rate=48000:duration=1" \
        -af "volume=20dB" -ac 1 -c:a libmp3lame -b:a 96k -y tone48.mp3
fi

#
# Two flags need explaining.
#
# -D'_Static_assert(c,m)=' turns off the ABI layout assertions in ngl.h and
# neos_sock.h. Those say a struct that crosses into an app has the offsets it
# had when the ABI was frozen, which is a statement about a 32-bit target and
# is false on a 64-bit desktop. The assertions are checked where they mean
# something - every target build of every app - and abi_check.py backs them up
# from the other side.
#
# -Wno-attributes is for neos_abi.h's `retain`, which this compiler does not
# have and does not need: nothing here is linked --gc-sections.
#
$CC -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Wno-unused-function -Wno-attributes \
    -D'_Static_assert(c,m)=' \
    -I"$APP/main" -I"$NEOS/neos_api/include" -I"$NEOS/ngl/include" \
    test_radio.c stubs.c \
    "$APP/main/rad_util.c" \
    "$APP/main/rad_stations.c" \
    "$APP/main/rad_eq.c" \
    "$APP/main/rad_mp3.c" \
    "$APP/main/rad_ui.c" \
    "$APP/main/rad_eqpage.c" \
    -lm -o test_radio

# Whichever name the toolchain gave it. On Windows gcc appends .exe, and a
# stale extensionless file from another build sits in front of it otherwise.
if [ -x test_radio.exe ]; then
    ./test_radio.exe
else
    ./test_radio
fi
