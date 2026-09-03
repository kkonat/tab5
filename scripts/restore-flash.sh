#!/usr/bin/env bash
# Restore the M5Stack Tab5 (ESP32-P4) stock flash image from
# original_flash/tab5-stock-backup.tar.gz.
#
# Usage:
#   ./restore-flash.sh                  # auto-detect port, prompt before writing
#   ./restore-flash.sh -p COM16         # explicit port
#   ./restore-flash.sh -p COM16 -y      # no confirmation prompt
#
# Override the esptool binary with ESPTOOL=/path/to/esptool if needed.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCHIVE="$ROOT/original_flash/tab5-stock-backup.tar.gz"
IMAGE_NAME="tab5-backup-full.bin"
SHA_NAME="tab5-backup-full.bin.sha256"

CHIP=esp32p4
BAUD=921600
PORT=""
ASSUME_YES=0

die() { echo "error: $*" >&2; exit 1; }

usage() {
    # Print the leading comment block (everything after the shebang, up to the
    # first non-comment line) without depending on fixed line numbers.
    awk 'NR==1 && /^#!/ { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' \
        "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) PORT="${2:-}"; shift 2 ;;
        -b|--baud) BAUD="${2:-}"; shift 2 ;;
        -y|--yes)  ASSUME_YES=1; shift ;;
        -h|--help) usage 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

# --- locate esptool -----------------------------------------------------------
find_esptool() {
    if [[ -n "${ESPTOOL:-}" ]]; then
        [[ -x "$ESPTOOL" ]] || command -v "$ESPTOOL" >/dev/null || die "ESPTOOL set but not executable: $ESPTOOL"
        echo "$ESPTOOL"; return
    fi
    local idf="/c/ESP-IDF/.espressif/python_env/idf5.4_py3.11_env/Scripts/esptool.exe"
    [[ -x "$idf" ]] && { echo "$idf"; return; }
    for c in esptool esptool.py; do
        command -v "$c" >/dev/null && { echo "$c"; return; }
    done
    die "esptool not found. Install it (pip install esptool) or set ESPTOOL=/path/to/esptool"
}
ESPTOOL_BIN="$(find_esptool)"

# --- locate port --------------------------------------------------------------
# The Tab5's ESP32-P4 exposes a native USB-Serial-JTAG device: VID 303A, PID 1001.
autodetect_port() {
    if command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -Command \
            "(Get-CimInstance Win32_PnPEntity | Where-Object { \$_.DeviceID -match 'VID_303A' -and \$_.Name -match 'COM(\d+)' } | ForEach-Object { if (\$_.Name -match 'COM(\d+)') { \$matches[0] } } | Select-Object -First 1)" \
            2>/dev/null | tr -d '\r\n '
        return
    fi
    # Linux / macOS fallback
    local d
    for d in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem*; do
        [[ -e "$d" ]] && { echo "$d"; return; }
    done
}

if [[ -z "$PORT" ]]; then
    PORT="$(autodetect_port)"
    [[ -n "$PORT" ]] || die "could not auto-detect the Tab5. Plug it in (USB-C, data cable) and/or pass --port"
    echo "auto-detected port: $PORT"
fi

# --- extract and verify -------------------------------------------------------
[[ -f "$ARCHIVE" ]] || die "backup archive not found: $ARCHIVE"

TMPDIR_RESTORE="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_RESTORE"' EXIT
IMAGE="$TMPDIR_RESTORE/$IMAGE_NAME"
SHA_FILE="$TMPDIR_RESTORE/$SHA_NAME"

echo "extracting $(basename "$ARCHIVE") ..."
tar -xzf "$ARCHIVE" -C "$TMPDIR_RESTORE" "$IMAGE_NAME" "$SHA_NAME" \
    || die "failed to extract $IMAGE_NAME / $SHA_NAME from $ARCHIVE"
[[ -f "$IMAGE" ]] || die "$IMAGE_NAME missing from $ARCHIVE"

if [[ -f "$SHA_FILE" ]]; then
    expected="$(awk '{print $1}' "$SHA_FILE")"
    actual="$(sha256sum "$IMAGE" | awk '{print $1}')"
    [[ "$expected" == "$actual" ]] || die "checksum mismatch!
  expected: $expected
  actual:   $actual
The backup is corrupt - refusing to flash it."
    echo "sha256 verified: $actual"
else
    echo "warning: $SHA_NAME missing from archive, skipping checksum verification" >&2
fi

size=$(wc -c < "$IMAGE")
echo "image size: $size bytes"

# --- confirm ------------------------------------------------------------------
if [[ "$ASSUME_YES" -ne 1 ]]; then
    cat <<EOF

This will OVERWRITE the entire 16MB flash on the device at $PORT.
Everything currently on it - app, NVS, SPIFFS data - will be lost.

Note: this image is a raw dump of one specific unit, so its NVS carries that
unit's calibration and any Wi-Fi credentials stored at capture time.

EOF
    read -r -p "Type 'yes' to continue: " reply
    [[ "$reply" == "yes" ]] || { echo "aborted."; exit 1; }
fi

# --- flash --------------------------------------------------------------------
echo "writing flash ..."
"$ESPTOOL_BIN" --chip "$CHIP" -p "$PORT" -b "$BAUD" write_flash 0x0 "$IMAGE"

echo
echo "restore complete."
