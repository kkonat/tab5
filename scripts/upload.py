#!/usr/bin/env python3
"""
Push a file to a running NeOS tablet over the console.

The tablet writes it straight to the card and rescans, so an app appears in
the launcher without the card ever leaving the slot. Uploading is the fast
path; scripts/deploy-card.ps1 is still the way to seed a card from scratch or
to fix one whose firmware will not boot.

    # an app: goes to /apps/<name>/, named by its manifest's "entry"
    python scripts/upload.py --app hello

    # any single file, path relative to the root of the card
    python scripts/upload.py --file autorun.cfg
    python scripts/upload.py --file some/local/icon.png --as apps/hello/icon.png

Needs pyserial. The ESP-IDF virtualenv has it:

    C:/ESP-IDF/.espressif/python_env/idf5.4_py3.11_env/Scripts/python.exe \
        scripts/upload.py --app hello
"""
import argparse
import json
import os
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing - run this with the ESP-IDF python (see the docstring)")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"@NEOSPUT "
MAGIC_DEL = b"@NEOSDEL "
RDY = b"@NEOSPUT-RDY"
OK = b"@NEOSPUT-OK"
ERR = b"@NEOSPUT-ERR"

# The tablet's USB port is the chip's own USB Serial/JTAG, so this is a CDC
# link and the rate is ignored - the wire runs at USB speed. It is set only
# because pyserial insists on a number.
BAUD = 921600
REPLY_TIMEOUT_S = 10


def app_paths(name):
    """Where an app's ELF is built, and what the card should call it."""
    manifest = os.path.join(REPO, "apps", name, "manifest.json")
    entry = "app.elf"
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as fh:
            entry = json.load(fh).get("entry") or entry
    local = os.path.join(REPO, "apps", name, "build", "%s.app.elf" % name)
    return local, "apps/%s/%s" % (name, entry), manifest


def wait_for(port, prefixes, deadline):
    """
    Read lines until one starts with a prefix we care about.

    The port is also carrying ESP_LOG output, so almost everything arriving
    here is not for us. Anything unrecognised is passed through to stderr -
    when an upload fails, the reason is usually in those lines.
    """
    buf = b""
    while time.time() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip(b"\r")
            for p in prefixes:
                if line.startswith(p):
                    return line
            if line:
                sys.stderr.write("  | %s\n" % line.decode("utf-8", "replace"))
    return None


def upload(port_name, local, remote, quiet=False):
    with open(local, "rb") as fh:
        data = fh.read()
    if not data:
        sys.exit("%s is empty" % local)

    crc = zlib.crc32(data) & 0xFFFFFFFF

    # DTR/RTS are the reset and boot straps on an ESP board: asserting them on
    # open would reboot the tablet, which is the one thing an upload must not do.
    # A write timeout matters here - if the device is not draining the endpoint
    # the write blocks forever rather than failing.
    port = serial.Serial()
    port.port = port_name
    port.baudrate = BAUD
    port.timeout = 0.05
    port.write_timeout = 10
    port.dtr = False
    port.rts = False
    port.open()

    try:
        port.reset_input_buffer()
        hdr = b"\n" + MAGIC + ("%s %d %08x\n" % (remote, len(data), crc)).encode()
        port.write(hdr)
        port.flush()

        sent = 0
        started = time.time()
        while sent < len(data):
            reply = wait_for(port, (RDY, ERR), time.time() + REPLY_TIMEOUT_S)
            if reply is None:
                sys.exit("timed out waiting for the tablet - is it running NeOS?")
            if reply.startswith(ERR):
                sys.exit(reply.decode("utf-8", "replace"))

            want = int(reply.split()[1])
            port.write(data[sent:sent + want])
            port.flush()
            sent += want

            if not quiet:
                pct = sent * 100 // len(data)
                sys.stdout.write("\r  %s  %3d%%  %d/%d bytes" % (remote, pct, sent, len(data)))
                sys.stdout.flush()

        final = wait_for(port, (OK, ERR), time.time() + REPLY_TIMEOUT_S)
        if not quiet:
            sys.stdout.write("\r" + " " * 60 + "\r")
        if final is None:
            sys.exit("no confirmation from the tablet - the file may be incomplete")
        if final.startswith(ERR):
            sys.exit(final.decode("utf-8", "replace"))

        secs = max(time.time() - started, 1e-3)
        print("  %-28s %6d bytes  %5.1f KB/s" % (remote, len(data), len(data) / secs / 1024))
    finally:
        port.close()


def delete(port_name, remote):
    """Remove a file, or an app directory and everything in it."""
    port = serial.Serial()
    port.port = port_name
    port.baudrate = BAUD
    port.timeout = 0.05
    port.write_timeout = 5
    port.dtr = False
    port.rts = False
    port.open()
    try:
        port.reset_input_buffer()
        port.write(b"\n" + MAGIC_DEL + remote.encode() + b"\n")
        port.flush()
        reply = wait_for(port, (OK, ERR), time.time() + REPLY_TIMEOUT_S)
        if reply is None:
            sys.exit("no reply from the tablet")
        if reply.startswith(ERR):
            sys.exit(reply.decode("utf-8", "replace"))
        print("  deleted %s" % remote)
    finally:
        port.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM16", help="serial port (default COM16)")
    ap.add_argument("--app", help="app directory under apps/ - uploads its built ELF and manifest")
    ap.add_argument("--file", help="a local file to upload")
    ap.add_argument("--as", dest="remote", help="path on the card (default: the file's own name)")
    ap.add_argument("--rm", help="delete a path on the card (a directory goes with its contents)")
    ap.add_argument("--rm-app", help="delete an app directory from the card")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    if not any((args.app, args.file, args.rm, args.rm_app)):
        ap.error("give --app, --file, --rm or --rm-app")

    if args.rm_app:
        delete(args.port, "apps/%s" % args.rm_app)
    if args.rm:
        delete(args.port, args.rm)

    if args.app:
        local, remote, manifest = app_paths(args.app)
        if not os.path.exists(local):
            sys.exit("%s is not built - run idf.py elf in apps/%s first" % (local, args.app))
        upload(args.port, local, remote, args.quiet)
        if os.path.exists(manifest):
            upload(args.port, manifest, "apps/%s/manifest.json" % args.app, args.quiet)

    if args.file:
        local = args.file if os.path.exists(args.file) else os.path.join(REPO, args.file)
        if not os.path.exists(local):
            sys.exit("no such file: %s" % args.file)
        remote = args.remote or os.path.basename(local)
        upload(args.port, local, remote, args.quiet)


if __name__ == "__main__":
    main()
