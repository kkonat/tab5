#!/usr/bin/env python3
"""
Pull a screenshot off a running NeOS tablet, over the same USB console as
scripts/upload.py.

What lands in screencaps/ is the logical screen - system bar, any panel that
happens to be up, already the right way round for the rotation the tablet is
being held at. Nothing on the device is paused for it, so a screenshot taken
mid-animation looks like the animation was mid-frame, because it was.

    ./do screencap                      # screencaps/<timestamp>.png
    ./do screencap --name mandel        # ...-mandel.png
    ./do screencap --out C:/tmp/shot.png
    ./do screencap --port COM16     # only if auto-detection picks wrong

From PowerShell that is "do.ps1 screencap"; the wrapper is reached by path or
by extension because "do" on its own is a keyword in both shells.

It is there to pick the interpreter: this needs pyserial, which the ESP-IDF
virtualenv has and the system python does not. Nothing else - the PNG comes out
of zlib, because that virtualenv has no Pillow and a screenshot tool is not
worth an install.

The link is shared with everything else on the tablet that prints, so a row now
and then arrives with a log line spliced into the middle of it. Rows are
numbered and checksummed for exactly that reason: a damaged one is dropped and
asked for again, and only a row that will not come back cleanly at all is worth
telling anybody about. See neos_screencap.c for the device's half.
"""
import argparse
import base64
import binascii
import os
import struct
import sys

import _env
import time
import zlib
from array import array

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing - run this through the wrapper, which picks the "
             "interpreter: ./do screencap ... (do.ps1 screencap ... on PowerShell)")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REQ = b"@NEOSCAP"
HDR = b"@NEOSCAP-HDR"
DAT = b"@NEOSCAP-D "
END = b"@NEOSCAP-END"
ERR = b"@NEOSCAP-ERR"

# CDC again, so the rate is ignored; see the note in upload.py.
BAUD = 921600
START_TIMEOUT_S = 10
ROW_TIMEOUT_S = 20

# How many times to go back for rows that did not survive. Each round costs one
# request per gap and re-sends only the rows in it, so this is cheap; it is
# capped because a device that has stopped answering should say so rather than
# be asked forever.
MAX_ROUNDS = 6


class Pass(object):
    """What one request brought back."""

    def __init__(self):
        self.width = None
        self.height = None
        self.rotation = None
        self.damaged = 0        # rows that arrived and did not survive the trip
        self.noise = []         # console lines that were not part of the image


def read_lines(port, deadline):
    """
    Yield console lines until nothing has arrived for a while.

    `deadline` is a one-element list so the caller can push it forward as rows
    come in: a whole screen takes longer than any single gap in it should.
    """
    buf = b""
    while time.time() < deadline[0]:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.strip(b"\r")


def rle_decode(tokens, width):
    """
    One row of the device's byte-tagged RLE back to raw little-endian RGB565.

    Tag < 0x80 is a literal of (tag + 1) pixels; tag >= 0x80 is one pixel
    repeated (tag - 0x80 + 2) times. See neos_screencap.c.
    """
    out = bytearray()
    i = 0
    n = len(tokens)
    while i < n:
        tag = tokens[i]
        i += 1
        if tag & 0x80:
            out += bytes(tokens[i:i + 2]) * ((tag & 0x7F) + 2)
            i += 2
        else:
            count = tag + 1
            out += tokens[i:i + 2 * count]
            i += 2 * count
    if len(out) != width * 2:
        raise ValueError("row is %d pixels, expected %d" % (len(out) // 2, width))
    return bytes(out)


def one_pass(port, rows, first, count, quiet):
    """
    Ask for rows and collect the ones that arrive intact into `rows`.

    A row is kept only if it decodes and its CRC matches, so a line that had a
    log message spliced into it is simply not there afterwards - which is the
    whole mechanism: the caller sees a gap and asks again.
    """
    got = Pass()

    request = REQ if first is None else b"%s %d %d" % (REQ, first, count)
    port.reset_input_buffer()
    port.write(b"\n" + request + b"\n")
    port.flush()

    deadline = [time.time() + START_TIMEOUT_S]

    for line in read_lines(port, deadline):
        if line.startswith(ERR):
            sys.exit(line.decode("utf-8", "replace"))

        if line.startswith(HDR):
            got.width, got.height, got.rotation = (int(f) for f in line.split()[1:4])
            deadline[0] = time.time() + ROW_TIMEOUT_S
            continue

        if line.startswith(END):
            return got

        if line.startswith(DAT):
            deadline[0] = time.time() + ROW_TIMEOUT_S
            if got.width is None:
                continue                                # a row before its header
            try:
                index, crc, payload = line[len(DAT):].split(b" ", 2)
                row = rle_decode(base64.b64decode(payload), got.width)
                if zlib.crc32(row) & 0xFFFFFFFF != int(crc, 16):
                    raise ValueError("crc mismatch")
                rows[int(index)] = row
            except (ValueError, binascii.Error):
                got.damaged += 1
            else:
                if not quiet and len(rows) % 64 == 0:
                    sys.stdout.write("\r  %d rows " % len(rows))
                    sys.stdout.flush()
            continue

        # Everything else on the wire is somebody else's output. The tail of a
        # split row lands here too, and is worth counting rather than printing.
        if line and len(line) < 160:
            got.noise.append(line.decode("utf-8", "replace"))

    if got.width is None:
        sys.exit("no reply from the tablet - is it running NeOS, and has it been "
                 "flashed since screencap was added?")
    sys.exit("the tablet stopped sending partway through")


def gaps(rows, height):
    """The missing rows, as (first, count) runs - one request each."""
    runs = []
    y = 0
    while y < height:
        if y in rows:
            y += 1
            continue
        start = y
        while y < height and y not in rows:
            y += 1
        runs.append((start, y - start))
    return runs


def png(path, width, height, rgb565):
    """
    Write RGB565 out as a PNG, with zlib doing the only hard part.

    A 5- or 6-bit channel is widened by repeating its top bits into the bottom
    ones, which is what keeps white at 0xFF rather than landing it at 0xF8 and
    tinting every flat area of the image.
    """
    table = [bytes((((v >> 11) & 0x1F) << 3 | ((v >> 11) & 0x1F) >> 2,
                    ((v >> 5) & 0x3F) << 2 | ((v >> 5) & 0x3F) >> 4,
                    (v & 0x1F) << 3 | (v & 0x1F) >> 2))
             for v in range(65536)]

    px = array("H")
    px.frombytes(rgb565)
    if sys.byteorder != "little":
        px.byteswap()

    raw = bytearray()
    for y in range(height):
        raw.append(0)                                   # filter: none
        raw += b"".join(map(table.__getitem__, px[y * width:(y + 1) * width]))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", binascii.crc32(kind + data) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        fh.write(chunk(b"IEND", b""))


def capture(port_name, quiet=False):
    """Ask for the screen and return (width, height, rotation, raw RGB565)."""
    # DTR/RTS are the reset and boot straps on an ESP board: asserting them on
    # open would reboot the tablet, and a picture of the boot screen is not the
    # one anybody asked for.
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

    try:
        rows = {}
        first = one_pass(port, rows, None, None, quiet)
        width, height = first.width, first.height
        damaged = first.damaged
        noise = list(first.noise)
        rounds = 0

        while len(rows) < height and rounds < MAX_ROUNDS:
            rounds += 1
            before = len(rows)
            for start, count in gaps(rows, height):
                again = one_pass(port, rows, start, count, quiet)
                damaged += again.damaged
                noise += again.noise
                if (again.width, again.height) != (width, height):
                    sys.exit("the screen changed size mid-capture (%dx%d, now %dx%d) - "
                             "it rotated; try again"
                             % (width, height, again.width, again.height))
            if len(rows) == before:
                break                                   # asking again is not helping

        if not quiet:
            sys.stdout.write("\r" + " " * 24 + "\r")

        for line in noise:
            sys.stderr.write("  | %s\n" % line)

        if len(rows) < height:
            sys.exit("%d of %d rows never arrived intact, after %d rounds of asking - "
                     "something is writing to the console faster than this can work "
                     "around it" % (height - len(rows), height, rounds))

        if damaged and not quiet:
            print("  %d row%s re-sent (a log line landed in the middle)"
                  % (damaged, "" if damaged == 1 else "s"))

        return width, height, first.rotation, b"".join(rows[y] for y in range(height))
    finally:
        port.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    _env.add_port_argument(ap)
    ap.add_argument("--out", help="where to write the PNG (default screencaps/<timestamp>.png)")
    ap.add_argument("--name", help="a label appended to the default filename")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    started = time.time()
    width, height, rotation, raw = capture(_env.port(args.port), args.quiet)

    out = args.out
    if not out:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        name = "%s-%s.png" % (stamp, args.name) if args.name else "%s.png" % stamp
        folder = os.path.join(REPO, "screencaps")
        os.makedirs(folder, exist_ok=True)
        out = os.path.join(folder, name)

    # The file is a PNG whatever it is called, and a .jpg that is really a PNG
    # opens fine in every viewer and then confuses whatever it is handed to next.
    if os.path.splitext(out)[1].lower() != ".png":
        sys.stderr.write("  note: writing a PNG to %s\n" % os.path.basename(out))

    png(out, width, height, raw)

    if not args.quiet:
        shown = os.path.relpath(out, REPO) if os.path.abspath(out).startswith(REPO) else out
        print("  %-34s %dx%d rot %d  %.1f KB  %.1f s"
              % (shown, width, height, rotation * 90,
                 os.path.getsize(out) / 1024, time.time() - started))


if __name__ == "__main__":
    main()
