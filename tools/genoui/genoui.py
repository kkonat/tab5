"""
Turn the IEEE OUI registry into something a tablet can search.

lanscan wants to answer "who made the device with this MAC" thirty times a
scan. The registry that answers it is a 2 MB CSV of about 34,000 rows, and the
obvious two approaches are both wrong on this machine: parsing the CSV on the
device means holding a megabyte of strings in PSRAM for the life of the app,
and shipping a C array means compiling that megabyte into every copy of the
app on every card.

So this writes a third thing - a flat file of fixed-width records, sorted by
prefix - and the app binary-searches it in place with neos_file_read_at().
Sixteen reads of thirty-two bytes answers one lookup, nothing is resident, and
the file is data on the card rather than code in the image, so it can be
updated without rebuilding anything.

    python tools/genoui/genoui.py --fetch apps/lanscan/card/oui.bin
    python tools/genoui/genoui.py oui.csv apps/lanscan/card/oui.bin

Unlike everything else under tools/, the output is *not* checked in: it is
IEEE's data rather than ours, it is a megabyte, and it is optional - without
it lanscan resolves the built-in prefixes and leaves the rest of the vendor
column blank, which is the honest thing for it to do.
"""
import argparse
import csv
import io
import struct
import sys
import urllib.request

IEEE_URL = "https://standards-oui.ieee.org/oui/oui.csv"

MAGIC = b"NOUI"
NAME_LEN = 28
RECORD = 32          # 3 prefix + 1 pad + 28 name

# Suffixes that are on almost every row and identify nobody. Dropping them is
# what makes 28 characters enough for the part a person reads: "Cisco Systems"
# fits, "Cisco Systems, Inc." does not, and the four characters that would be
# lost are the ones nobody was going to read.
NOISE = (
    ", Inc.", " Inc.", ", Inc", " Inc", ", LLC", " LLC", ", Ltd.", " Ltd.",
    ", Ltd", " Ltd", " Limited", " Corporation", " Corp.", " Corp",
    " Co., Ltd.", " Co.,Ltd.", " Co., Ltd", " Co.,Ltd", " Co., LTD",
    " CO., LTD.", " CO.,LTD.", " GmbH", " S.A.", " B.V.", " A/S", " AB",
    " Technologies", " Technology", " Electronics",
)


def tidy(name: str) -> str:
    """Squeeze one organisation name into the 28 bytes a record has."""
    name = " ".join(name.split())
    changed = True
    while changed:
        changed = False
        for suffix in NOISE:
            if name.lower().endswith(suffix.lower()) and len(name) > len(suffix):
                name = name[: -len(suffix)].rstrip(" ,")
                changed = True
    # Non-ASCII would need a font the device does not have, so it is
    # transliterated to nothing rather than drawn as a box.
    name = "".join(ch for ch in name if 0x20 <= ord(ch) < 0x7F)
    return name[:NAME_LEN].strip()


def read_csv(handle) -> dict:
    table = {}
    for row in csv.DictReader(handle):
        prefix = (row.get("Assignment") or "").strip().upper()
        name = tidy(row.get("Organization Name") or "")
        if len(prefix) != 6 or not name:
            continue
        try:
            raw = bytes.fromhex(prefix)
        except ValueError:
            continue
        # Later rows win: IEEE reassigns, and the file is in issue order.
        table[raw] = name
    return table


def write_pack(table: dict, path: str) -> int:
    with open(path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack(">I", len(table)))
        # Sorted, because the whole point is that the device can binary-search
        # it. A record it cannot find because the file is not ordered looks
        # exactly like a prefix IEEE has not assigned.
        for prefix in sorted(table):
            name = table[prefix].encode("ascii", "ignore")[:NAME_LEN]
            out.write(prefix + b"\x00" + name + b"\x00" * (NAME_LEN - len(name)))
    return len(table)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?",
                        help="a local copy of IEEE's oui.csv")
    parser.add_argument("output", help="where to write oui.bin")
    parser.add_argument("--fetch", action="store_true",
                        help=f"download it from {IEEE_URL} instead")
    args = parser.parse_args()

    if args.fetch:
        print(f"fetching {IEEE_URL} ...")
        # standards-oui.ieee.org answers urllib's default agent with a 418, so
        # the request says it is a browser. Nothing else about it differs -
        # the file is public and there is no account.
        request = urllib.request.Request(
            IEEE_URL,
            headers={"User-Agent": "Mozilla/5.0 (compatible; neos-genoui/1.0)"})
        with urllib.request.urlopen(request, timeout=120) as response:
            body = response.read()
        handle = io.StringIO(body.decode("utf-8", "replace"), newline="")
    elif args.source:
        handle = open(args.source, "r", encoding="utf-8", errors="replace",
                      newline="")
    else:
        parser.error("give a source file, or pass --fetch")

    with handle:
        table = read_csv(handle)

    if not table:
        print("no usable rows - is that really IEEE's oui.csv?", file=sys.stderr)
        return 1

    count = write_pack(table, args.output)
    size = 8 + count * RECORD
    print(f"{count} prefixes -> {args.output} ({size // 1024} KB, "
          f"{RECORD} bytes each)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
