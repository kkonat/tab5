# genoui

Turns the IEEE OUI registry into the file lanscan searches for MAC vendors.

```bash
python tools/genoui/genoui.py --fetch apps/lanscan/card/oui.bin
python tools/genoui/genoui.py oui.csv apps/lanscan/card/oui.bin   # from a local copy
```

`deploy-card` copies anything in `apps/<app>/card/` to `/apps/<app>/` on the
card, so the file lands where `lan_oui.c` looks for it:
`apps/lanscan/oui.bin`.

## Why this is not a C array

Every other generator under `tools/` writes C that is checked in. This one
writes data that is not, and the difference is the size and the ownership.

34,000 organisations is about a megabyte. As a C array it would be compiled
into the app image, so every copy of `lanscan` on every card would carry it
and `neos_file_read_at()` would have nothing to do. As a CSV parsed on the
device it would be a megabyte of strings resident in PSRAM for the life of the
app, to answer a question that comes up thirty times a scan.

As a sorted flat file it is neither: the app binary-searches it in place, which
is sixteen reads of thirty-two bytes per lookup and nothing resident. It is
also data on a card rather than code in an image, so a fresher registry is a
file copy and not a rebuild.

And it is IEEE's data, not this project's, which is the other reason it is not
in the repo.

## The format

```
offset  bytes  what
0       4      "NOUI"
4       4      record count, big-endian
8       32     record 0
40      32     record 1
...
```

One record:

```
0       3      the OUI, as bytes
3       1      pad
4       28     the organisation, NUL-padded ASCII
```

Records are sorted by prefix, which is the whole point - the device cannot
build an index, so the file has to be one. Fixed 32-byte records make the
address of record *n* arithmetic rather than a lookup, and the four bytes of
padding cost nothing real: a 32-byte read and a 31-byte read are the same read.

## The names

Squeezed to fit 28 characters, by dropping the suffixes that are on almost
every row and identify nobody - `Inc.`, `Ltd.`, `GmbH`, `Corporation`,
`Technologies`. "Cisco Systems" fits and "Cisco Systems, Inc." does not, and
the characters that would be lost are the ones nobody was going to read.
Non-ASCII is dropped rather than drawn, because the device's font is 1-bit
ASCII and the alternative is a row of boxes.

## Without it

lanscan works. `lan_oui.c` has a built-in table of the prefixes that change how
you read a row - the hypervisors and the Raspberry Pi ranges - and anything
else stays blank. Nothing is guessed: a vendor column that is sometimes a fact
and sometimes a plausible invention is worse than one with gaps in it, and the
footer says the file is missing rather than leaving the gaps unexplained.
