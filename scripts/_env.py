"""
Machine-local settings for the scripts in this directory.

Paths and device names differ per machine - where the IDF is checked out, which
COM port the tablet enumerated as, which letter the card reader took. None of
that belongs in the repo, so it is read from .env.local at the repo root (see
.env.local.example) and, failing that, worked out at runtime.

Nothing here is required. A machine with an ordinary ESP-IDF install and one
board plugged in needs no .env.local at all: the port is found from the chip's
USB VID and the IDF comes out of the environment idf.py already exports.

Precedence, highest first: the command line, the process environment, then
.env.local, then the fallback (auto-detection, or None).
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV_FILE = os.path.join(REPO, ".env.local")

# The ESP32-P4 exposes a native USB-Serial-JTAG device rather than going through
# a bridge chip, so the port is Espressif's own VID and needs no board database.
ESPRESSIF_VID = 0x303A

_loaded = None


def _load():
    """Parse .env.local into a dict. Missing file is not an error."""
    global _loaded
    if _loaded is not None:
        return _loaded

    _loaded = {}
    try:
        with open(ENV_FILE, encoding="utf-8") as fh:
            lines = fh.readlines()
    except OSError:
        return _loaded

    for n, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # "export KEY=value" too, so the same file can be sourced by a shell.
        if line.startswith("export "):
            line = line[len("export "):].lstrip()
        key, sep, value = line.partition("=")
        if not sep:
            sys.stderr.write("%s:%d: ignoring line without '=': %s\n"
                             % (ENV_FILE, n, line))
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        _loaded[key.strip()] = value
    return _loaded


def get(name, default=None):
    """A setting from the environment, then .env.local, then the default.

    The environment wins so that a value exported by idf.py's export script, or
    set for one command, is never silently overridden by the file.
    """
    value = os.environ.get(name)
    if value:
        return value
    value = _load().get(name)
    if value:
        return value
    return default


def detect_port():
    """The Tab5's serial port, by USB VID. None if it is not plugged in."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    for info in list_ports.comports():
        if info.vid == ESPRESSIF_VID:
            return info.device
    return None


def port(explicit=None):
    """Resolve --port: the flag, then NEOS_PORT, then the board on the bus.

    Exits with an explanation rather than returning None, because every caller
    needs a port and "could not open None" is the wrong end to debug it from.
    """
    if explicit:
        return explicit
    configured = get("NEOS_PORT")
    if configured:
        return configured
    found = detect_port()
    if found:
        return found
    sys.exit(
        "no Tab5 found on the USB bus (looking for VID %04X).\n"
        "Plug it in with a data-capable USB-C cable, or name the port:\n"
        "  --port COM16                    once ('/dev/ttyACM0' on Linux)\n"
        "  NEOS_PORT=COM16 in .env.local   every time"
        % ESPRESSIF_VID)


def add_port_argument(ap):
    """The --port flag, worded the same way everywhere it appears."""
    ap.add_argument("--port", "-p", default=None,
                    help="serial port (default: NEOS_PORT, else auto-detected)")
