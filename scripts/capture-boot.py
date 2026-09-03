#!/usr/bin/env python3
"""
Reset the tablet and capture its console to stdout.

`idf.py monitor` is the right tool when there is somebody at the keyboard. This
one is for when there is not: it resets the board, reads for a fixed number of
seconds and exits, so a boot can be captured into a file, diffed, grepped, or
pasted into an issue without anything having to be interrupted by hand.

The reset is the same one esptool does - RTS drives EN through the USB bridge -
with the boot strap left released so the chip comes up running the app rather
than in the ROM loader.

    python scripts/capture-boot.py                       # COM16, 20 s
    python scripts/capture-boot.py --port COM16 --secs 40
    python scripts/capture-boot.py > boot.log

Needs pyserial, which the ESP-IDF virtualenv already has because esptool
depends on it - so run this with the IDF python, not the system one:

    C:\\ESP-IDF\\.espressif\\python_env\\idf5.4_py3.11_env\\Scripts\\python.exe \\
        scripts/capture-boot.py
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("no pyserial - run this with the ESP-IDF virtualenv's python, "
             "see the note at the top of this file")

# sdkconfig.defaults sets the console to 921600; idf.py monitor reads that from
# the build, and this has nowhere to read it from, so it is repeated here.
DEFAULT_BAUD = 921600


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", "-p", default="COM16")
    ap.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--secs", "-s", type=float, default=20.0,
                    help="how long to read for after the reset")
    ap.add_argument("--no-reset", action="store_true",
                    help="just listen; for catching a fault on a board that is "
                         "already running")
    args = ap.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        sys.exit("%s: %s" % (args.port, e))

    if not args.no_reset:
        # DTR low first: on this bridge it is the boot strap, and pulling EN
        # while it is asserted is how you end up in the download loader with a
        # console that says nothing.
        port.setDTR(False)
        port.setRTS(True)
        time.sleep(0.15)
        port.setRTS(False)

    end = time.time() + args.secs
    out = sys.stdout.buffer
    while time.time() < end:
        chunk = port.read(4096)
        if chunk:
            out.write(chunk)
            out.flush()
    port.close()


if __name__ == "__main__":
    main()
