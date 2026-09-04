#!/usr/bin/env python3
"""
Start an app on a running NeOS tablet, over the same USB console as upload.

This is the other half of not touching the tablet. `upload` puts a rebuilt app
on the card; this runs it - so the edit/build/look loop is one command from a
keyboard, and autorun.cfg goes back to naming the shell rather than whatever is
being worked on this afternoon.

    ./do launch clock                   # run apps/clock, whatever is running now
    ./do launch clock --upload          # push the built ELF first, then run it
    ./do launch --home                  # back to the card's autorun app
    ./do launch clock --shot            # ... and pull a PNG once it has settled
    ./do launch clock --watch 20        # ... and print the console for 20 s

From PowerShell that is "do.ps1 launch"; the wrapper is what finds the ESP-IDF
virtualenv, which is the python that has pyserial.

What actually happens on the tablet is a queued request, not a start: NeOS runs
one app at a time on one stack, so the app that is running has to return before
the next one can be loaded. It is asked to close and it decides when to go. So
"@NEOSRUN-OK" means "asked", and what this script prints afterwards - "started"
or a timeout - comes from watching the console for the line NeOS logs when an
app is actually on the screen. An app that ignores close requests never gets
there, and the timeout is the only way to find that out. See neos_boot.c.
"""
import argparse
import os
import sys
import time

import _env

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing - run this through the wrapper, which picks the "
             "interpreter: ./do launch ... (do.ps1 launch ... on PowerShell)")

import screencap
import upload

REPO = _env.REPO

REQ = b"@NEOSRUN"
OK = b"@NEOSRUN-OK"
ERR = b"@NEOSRUN-ERR"

# CDC, so the rate is ignored; see the note in upload.py.
BAUD = 921600
REPLY_TIMEOUT_S = 10

# How long to wait for the app to actually be on the screen. Generous, because
# it covers the running app noticing the close request, unwinding whatever it
# was doing, and then the loader reading an ELF off an SD card.
START_TIMEOUT_S = 20

# NeOS logs this when an app is on the screen. Watching for it is the only
# honest way to say "started" - the reply to the request cannot know.
RUNNING = '==== running "'

# Long enough for a first frame. An app paints once before it does anything
# else, but "once" is after the card has given it its assets.
SETTLE_S = 2.0


def open_port(port_name):
    """
    The console, without rebooting the tablet on the way in.

    DTR/RTS are the reset and boot straps on an ESP board: asserting them on
    open would restart the very thing this is trying to talk to.
    """
    port = serial.Serial()
    port.port = port_name
    port.baudrate = BAUD
    port.timeout = 0.05
    port.write_timeout = 5
    port.dtr = False
    port.rts = False
    try:
        port.open()
    except serial.SerialException as e:
        sys.exit("%s: %s" % (port_name, e))
    return port


def lines(port, deadline):
    """Yield decoded console lines until `deadline`."""
    buf = b""
    while time.time() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.strip(b"\r").decode("utf-8", "replace")


def request(port, app):
    """Send the request and return the tablet's own words for what it did."""
    port.reset_input_buffer()
    port.write(b"\n" + REQ + ((" %s" % app).encode() if app else b"") + b"\n")
    port.flush()

    for line in lines(port, time.time() + REPLY_TIMEOUT_S):
        raw = line.encode("utf-8", "replace")
        if raw.startswith(ERR):
            sys.exit("  %s" % line[len(ERR) + 1:])
        if raw.startswith(OK):
            return line[len(OK) + 1:]
    sys.exit("no reply from the tablet - is it running NeOS, and has it been "
             "flashed since launch was added?")


def wait_until_running(port, echo, timeout):
    """
    Watch the console until an app is on the screen. Its name, or None.

    Only the "running" line answers the question. Its twin, the "returned"
    line, is not a crash from here and is deliberately not read as one: every
    launch begins with the outgoing app returning, so that line arrives first
    on the way to a launch that worked perfectly. An app that starts and dies
    on its first frame shows up after this has already said "started", which
    is what --watch is for.
    """
    for line in lines(port, time.time() + timeout):
        if echo and line:
            sys.stderr.write("  | %s\n" % line)
        at = line.find(RUNNING)
        if at >= 0:
            return line[at + len(RUNNING):].split('"')[0]
    return None


def watch(port, secs):
    """Pass the tablet's console through for a while. The point of --watch."""
    for line in lines(port, time.time() + secs):
        if line:
            print("  | %s" % line)


def shoot(port_name, app, quiet):
    """A PNG of whatever is on the screen now, named after the app."""
    width, height, rotation, raw = screencap.capture(port_name, quiet)

    folder = os.path.join(REPO, "screencaps")
    os.makedirs(folder, exist_ok=True)
    out = os.path.join(folder, "%s-%s.png"
                       % (time.strftime("%Y%m%d-%H%M%S"), app or "home"))
    screencap.png(out, width, height, raw)

    if not quiet:
        print("  %-34s %dx%d rot %d  %.1f KB"
              % (os.path.relpath(out, REPO), width, height, rotation * 90,
                 os.path.getsize(out) / 1024))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", nargs="?",
                    help="app directory under apps/ (omit with --home)")
    _env.add_port_argument(ap)
    ap.add_argument("--home", action="store_true",
                    help="go back to the card's autorun app instead")
    ap.add_argument("--upload", action="store_true",
                    help="push the app's built ELF and manifest first")
    ap.add_argument("--shot", action="store_true",
                    help="pull a screenshot once it has settled")
    ap.add_argument("--settle", type=float, default=SETTLE_S, metavar="SECS",
                    help="how long to let it draw before --shot (default %.1f)" % SETTLE_S)
    ap.add_argument("--watch", type=float, default=0, metavar="SECS",
                    help="print the tablet's console for this long afterwards")
    ap.add_argument("--timeout", type=float, default=START_TIMEOUT_S, metavar="SECS",
                    help="how long to wait for it to start (default %d)" % START_TIMEOUT_S)
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    if args.home:
        if args.app:
            ap.error("--home is instead of an app name, not as well as one")
        if args.upload:
            ap.error("--home uploads nothing - there is no app named to upload")
    elif not args.app:
        ap.error("name an app to launch, or --home to go back to the autorun one")

    # After the argument check: finding the board is worth nothing if the
    # command line was not going to do anything anyway.
    port_name = _env.port(args.port)

    if args.upload:
        local, remote, manifest = upload.app_paths(args.app)
        if not os.path.exists(local):
            sys.exit("%s is not built - run idf.py build in apps/%s first"
                     % (os.path.relpath(local, REPO), args.app))
        upload.upload(port_name, local, remote, args.quiet)
        if os.path.exists(manifest):
            upload.upload(port_name, manifest,
                          "apps/%s/manifest.json" % args.app, args.quiet)

    started = time.time()
    port = open_port(port_name)
    try:
        note = request(port, args.app)
        if not args.quiet:
            print("  requested %s" % note)

        name = wait_until_running(port, not args.quiet and args.watch > 0,
                                  args.timeout)
        if name is None:
            # Not an error worth exiting on if the console is being watched -
            # the reason is about to scroll past, which is more use than this.
            sys.stderr.write(
                "  nothing started within %g s - the app that was running may "
                "not be polling neos_app_close_requested()\n" % args.timeout)
            if not args.watch:
                return 1
        elif not args.quiet:
            print('  started "%s" in %.1f s' % (name, time.time() - started))

        if args.watch:
            watch(port, args.watch)
    finally:
        port.close()

    # After the port is closed: screencap opens its own, and two handles on one
    # CDC device is a Windows error rather than a shared conversation.
    if args.shot:
        time.sleep(args.settle)
        shoot(port_name, args.app, args.quiet)

    return 0


if __name__ == "__main__":
    sys.exit(main())
