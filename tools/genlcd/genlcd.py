#!/usr/bin/env python3
"""
genlcd - a MAME handheld romset into the file apps/nupogodi reads.

    python tools/genlcd/genlcd.py nupogodi.zip -o nupogodi.lcd

A MAME romset for one of these machines is two files: the mask ROM dump, and
an SVG of the case with every LCD segment in it as a separate shape. This
turns that pair into one flat container - a background bitmap, a coverage
bitmap per segment, and the ROM - because the app has no XML parser, no
decompressor and no floating point, and the machine with Inkscape on it is a
better place to do all three.

Shadows are baked here too, for the same reason. See "Shadows" below.

Neither input is in this repo and neither can be. See README.md.

Requires Pillow, and Inkscape 1.x on PATH (or INKSCAPE_PATH) unless --gw.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)

# SM5A: 9 O pins x 4 segments per pin x 2 commons. MAME labels each shape in
# the SVG with a <title> of "O.segment.common", and this is how that becomes
# the index the emulator asks for. The SM510 family uses a different formula
# (64*x + 4*y + z); only SM5A machines are handled here, which is every
# Elektronika Game & Watch clone.
SEGMENTS = 72
TITLE_RE = re.compile(r"^\s*(\d+)\.(\d+)\.(\d+)\s*$")

# What the app leaves for the artwork on a 1280x720 panel: the whole width,
# and the height less the system bar (64), the strip of mode switches
# (NPG_STRIP_H, 88) and the recess the case draws round the display on both
# sides (BEZEL, 14). The artwork is wider than it is tall, so it runs out of
# height first and the spare width down each side becomes the thumb pads -
# which is why the box is the full 1280 rather than something with margins
# already taken out of it.
#
# The bezel has to come out of the height here, because the app cannot get it
# back later: the artwork is drawn at the size it arrives at and nothing on
# the tablet rescales it, so an asset built to fill the box leaves the recess
# nowhere to go and it comes out narrower than it should be. Erring large is
# free - the case simply has more of itself on show.
# See NPG_STRIP_H and BEZEL in apps/nupogodi/.
DEFAULT_FIT = (1280, 540)

HDR = 80
SEGENT = 12
MAGIC = b"NEOSLCD2"

# ---------------------------------------------------------------------------
# Where each ROM keeps its clock
#
# These machines are watches, and the ROM holds the time as six BCD nibbles in
# its own RAM. Nothing in the romset says where; somebody had to find them.
# Keyed by the ROM's own SHA-1, because MAME ships the same dump under more
# than one romset name and the program is what the addresses belong to.
#
# hour tens, hour units, minute tens, minute units, second tens, second units,
# and then the bit inside the hour-tens nibble that means afternoon. Values
# from LCD-Game-Shrinker's custom/nupogodi.py.
# ---------------------------------------------------------------------------
CLOCKS = {
    # im-02.bin - Nu, pogodi!, and the other Elektronika Mickey Mouse clones
    # that share this program.
    "7e94fc255f32db725d5aa9e196088e490c1a1443": (36, 37, 38, 39, 40, 41, 2),
}

# ---------------------------------------------------------------------------
# Shadows
#
# Two layers throw them, and they are not the same distance away.
#
# The colour is screen printed on the inside of the front glass. The segments
# are in the liquid crystal below it. Light coming in from above therefore
# throws the printed artwork's shadow further down the reflector than it
# throws a segment's - the print is further from the back of the sandwich -
# and that difference is most of what makes one of these look like an object
# rather than a picture. COLOUR_MULT is that difference.
#
# Mixes are how dark a shadow gets at its darkest, out of 255. The clock app's
# LCD face tops out at 52 and that is the look being matched; a little more
# here because these shadows are thrown further and read as fainter for it.
# ---------------------------------------------------------------------------
COLOUR_MULT = 3
SEG_MIX = 88
COLOUR_MIX = 72

# ---------------------------------------------------------------------------
# The ghosts
#
# Nothing on one of these displays is ever absent. The electrodes are etched
# into the glass when it is made, so every egg, every wolf, every digit the
# machine will ever show is already there - "off" means the crystal in that
# cell is passing light, not that the shape has gone. Hold a real one up to a
# window and you can read the whole layout at once.
#
# So every segment is printed faintly into the background, and the lit ones
# are drawn over the top. It costs nothing at run time: an unlit segment is
# not drawn at all, it is simply already in the picture.
# ---------------------------------------------------------------------------
GHOST_MIX = 26

# Which way they fall: down and to the left, so the light is over the player's
# right shoulder. One unit each way per unit of reach, i.e. 45 degrees.
SHADOW_DX = -1
SHADOW_DY = 1

# How coloured a pixel has to be to count as print rather than as the LCD's
# own greenish ground. max(r,g,b) - min(r,g,b); the ground sits under 20.
COLOUR_SAT = 42


def log(*a):
    print(*a, file=sys.stderr)


# ---------------------------------------------------------------------------
# Inkscape
# ---------------------------------------------------------------------------

def inkscape_bin():
    exe = os.environ.get("INKSCAPE_PATH") or shutil.which("inkscape")
    if not exe:
        for guess in (r"C:\Program Files\Inkscape\bin\inkscape.exe",
                      r"C:\Program Files\Inkscape\inkscape.exe",
                      "/usr/bin/inkscape"):
            if Path(guess).exists():
                exe = guess
                break
    if not exe:
        sys.exit("no Inkscape. Install it, put it on PATH, or set INKSCAPE_PATH")
    return exe


def inkscape(exe, args):
    r = subprocess.run([exe] + args, capture_output=True, text=True)
    if r.returncode != 0:
        log(r.stdout, r.stderr)
        sys.exit(f"inkscape failed: {' '.join(args[:4])} ...")
    return r.stdout


# ---------------------------------------------------------------------------
# The romset
# ---------------------------------------------------------------------------

def unpack(zip_path, into):
    """Pull the ROM and the artwork out. A romset holds exactly one of each."""
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        svgs = [n for n in names if n.lower().endswith(".svg")]
        bins = [n for n in names if n.lower().endswith(".bin")]
        if len(svgs) != 1 or len(bins) != 1:
            sys.exit(f"{zip_path}: expected one .bin and one .svg, found "
                     f"{len(bins)} and {len(svgs)}: {names}")
        z.extract(svgs[0], into)
        z.extract(bins[0], into)
        return Path(into, bins[0]), Path(into, svgs[0])


def find_segments(svg_path):
    """
    id -> segment index, for every shape MAME labelled as a segment.

    The label is a <title> child, which is also what makes the shape show a
    tooltip in a browser - so the artwork is self-describing and this does not
    have to know anything about the game.
    """
    tree = ET.parse(svg_path)
    root = tree.getroot()
    out = {}

    for el in root.iter():
        title = el.find(f"{{{SVG_NS}}}title")
        if title is None or title.text is None:
            continue
        m = TITLE_RE.match(title.text)
        if not m:
            continue
        x, y, z = (int(v) for v in m.groups())
        idx = 8 * x + 2 * y + z
        if idx >= SEGMENTS:
            log(f"  ignoring segment {x}.{y}.{z} -> {idx}, past the SM5A's 72")
            continue
        eid = el.get("id")
        if not eid:
            sys.exit(f"segment {x}.{y}.{z} has a title but no id - cannot export it")
        out[eid] = idx

    return tree, root, out


def viewbox(root):
    vb = root.get("viewBox")
    if vb:
        p = re.split(r"[ ,\t]+", vb.strip())
        return float(p[2]), float(p[3])
    return float(root.get("width", 0)), float(root.get("height", 0))


def fit(nat_w, nat_h, box):
    """The artwork's size inside `box`, keeping its aspect."""
    s = min(box[0] / nat_w, box[1] / nat_h)
    # Even dimensions: every blit is happier and half a pixel of artwork is
    # not worth defending.
    return (max(2, int(nat_w * s) // 2 * 2), max(2, int(nat_h * s) // 2 * 2))


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def render_background(exe, tree, seg_ids, w, h, out):
    """
    The case and the printing on it, with every segment shape taken out.

    Removing rather than hiding, because a hidden element can still contribute
    through a filter or a clip path, and the one thing this image must not
    have in it is a segment.
    """
    root = tree.getroot()
    parent = {c: p for p in root.iter() for c in p}
    n = 0
    for el in list(parent):
        if el.get("id") in seg_ids:
            parent[el].remove(el)
            n += 1
    log(f"  background: removed {n} segment shapes")

    tmp = out.with_suffix(".svg")
    tree.write(tmp, xml_declaration=True, encoding="utf-8")
    inkscape(exe, [str(tmp), "--export-type=png", "--export-overwrite",
                   f"--export-width={w}", f"--export-height={h}",
                   "--export-background=#FFFFFF", "--export-background-opacity=1",
                   f"--export-filename={out}"])
    return out


def render_segments(exe, svg_path, seg_ids, w, h, outdir):
    """
    Each segment alone, on white, at the page's own size.

    --export-id-only draws just that shape and --export-area-page keeps the
    viewport, so every file comes back the same size as the background and
    already aligned with it - there is no coordinate arithmetic anywhere in
    this tool, and no question about ancestor transforms, because Inkscape is
    rendering the real document either way.

    No --export-filename: given one, Inkscape writes a single file and quietly
    drops the rest of the ids. Left to itself it writes <stem>_<id>.png beside
    the document, which is why the document is copied somewhere of its own
    first.
    """
    work = outdir / "art.svg"
    shutil.copyfile(svg_path, work)

    inkscape(exe, [str(work), f"--export-id={';'.join(seg_ids)}",
                   "--export-id-only", "--export-area-page",
                   "--export-type=png", "--export-overwrite",
                   f"--export-width={w}", f"--export-height={h}",
                   "--export-background=#FFFFFF", "--export-background-opacity=1"])

    found = {e: outdir / f"art_{e}.png" for e in seg_ids}
    missing = [e for e, p in found.items() if not p.exists()]
    if missing:
        log("  files present:", sorted(p.name for p in outdir.iterdir()))
        sys.exit(f"inkscape did not export {len(missing)} segments, e.g. {missing[:3]}")
    return found


# ---------------------------------------------------------------------------
# Shadow
# ---------------------------------------------------------------------------

def throw(img, reach):
    """
    Move a mask along the shadow's direction.

    ImageChops.offset wraps, which for a shadow means the bottom of the
    artwork reappearing along the top of it. The two pastes are what stop
    that; they clear whichever edges the shift brought round.
    """
    from PIL import ImageChops

    dx, dy = SHADOW_DX * reach, SHADOW_DY * reach
    out = ImageChops.offset(img, dx, dy)
    w, h = out.size

    if dy > 0:
        out.paste(0, (0, 0, w, dy))
    elif dy < 0:
        out.paste(0, (0, h + dy, w, h))
    if dx > 0:
        out.paste(0, (0, 0, dx, h))
    elif dx < 0:
        out.paste(0, (w + dx, 0, w, h))

    return out


def segment_map(darkness, reach, blur, mix, page):
    """
    One segment's coverage: its own darkening, with its shadow under it.

    `darkness` is the full page, 0 where the segment is not and 255 where it
    is solid. What comes back is a multiplier - 255 meaning "leave the
    background alone" - cropped to the smallest rectangle that has anything in
    it, plus where that rectangle sits.

    Folding the shadow into the segment's own map is what makes it free at
    runtime. The app multiplies the background by this once; multiply is
    associative, so "background, then shadow, then segment" and "background,
    then the two of them combined" are the same pixels, and the second needs
    no second blend, no second table and no ordering rule.
    """
    from PIL import Image, ImageChops, ImageFilter

    box = darkness.getbbox()
    if box is None:
        return None, None

    # Room for the shadow to fall into, and for the blur to spread.
    pad = reach + 3 * blur + 2
    W, H = page
    x0 = max(0, box[0] - pad)
    y0 = max(0, box[1] - pad)
    x1 = min(W, box[2] + pad)
    y1 = min(H, box[3] + pad)

    d = darkness.crop((x0, y0, x1, y1))

    sh = throw(d, reach)
    if blur:
        sh = sh.filter(ImageFilter.GaussianBlur(blur))
    sh = sh.point(lambda v: v * mix // 255)

    # Multipliers, then one multiply: shadow first, segment over it.
    m = ImageChops.multiply(ImageChops.invert(d), ImageChops.invert(sh))

    # Trim to what actually darkens anything.
    inner = ImageChops.invert(m).getbbox()
    if inner is None:
        return None, None
    m = m.crop(inner)
    return (x0 + inner[0], y0 + inner[1]), m


def background_shadow(bg, reach, blur, mix, sat):
    """
    Throw the printed artwork's shadow onto the LCD ground beneath it.

    The print is on the front glass and the segments are below it, so this
    travels COLOUR_MULT times as far as a segment's - which is the whole
    point, and is what stops the two layers reading as one flat picture.

    "Printed" is decided by saturation. The greens, browns and oranges are ink;
    the ground is a desaturated grey-green and must not cast anything, or the
    entire screen would sit in its own shadow.
    """
    from PIL import Image, ImageChops, ImageFilter

    r, g, b = bg.split()
    hi = ImageChops.lighter(ImageChops.lighter(r, g), b)
    lo = ImageChops.darker(ImageChops.darker(r, g), b)
    mask = ImageChops.subtract(hi, lo).point(lambda v: 255 if v >= sat else 0)

    if not mask.getbbox():
        log("  background: nothing coloured enough to cast a shadow")
        return bg, 0

    sh = throw(mask, reach)
    if blur:
        sh = sh.filter(ImageFilter.GaussianBlur(blur))

    # A caster does not stand in its own shadow: the ink keeps its colour.
    sh = ImageChops.multiply(sh, ImageChops.invert(mask))
    sh = sh.point(lambda v: v * mix // 255)

    m = ImageChops.invert(sh)
    out = Image.merge("RGB", tuple(ImageChops.multiply(c, m) for c in (r, g, b)))

    lit = sum(1 for v in mask.tobytes() if v)
    return out, lit


# ---------------------------------------------------------------------------
# Packing
# ---------------------------------------------------------------------------

def rgb565(img):
    px = img.convert("RGB").tobytes()
    out = bytearray(len(px) // 3 * 2)
    for i in range(len(px) // 3):
        r, g, b = px[3 * i], px[3 * i + 1], px[3 * i + 2]
        struct.pack_into("<H", out, 2 * i,
                         ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return bytes(out)


def pack(w, h, rom, bg, segs, title, clock):
    """
    segs is SEGMENTS entries of (x, y, w, h, nibbles) in index order.
    clock is (hh, hl, mh, ml, sh, sl, pm) or None.
    """
    nib = []
    table = []
    for (x, y, sw, sh, vals) in segs:
        if not sw or not sh:
            table.append((0, 0, 0, 0, 0))
            continue
        table.append((x, y, sw, sh, len(nib)))
        nib.extend(vals)

    # Low nibble first - the order apps/nupogodi/main/npg_asset.c unpacks.
    cov = bytearray((len(nib) + 1) // 2)
    for i, v in enumerate(nib):
        if i & 1:
            cov[i >> 1] |= v << 4
        else:
            cov[i >> 1] |= v

    segb = b"".join(struct.pack("<HHHHI", *t) for t in table)

    rom_off = HDR
    bg_off = rom_off + len(rom)
    seg_off = bg_off + len(bg)
    cov_off = seg_off + len(segb)

    c = clock or (0, 0, 0, 0, 0, 0, 0)
    hdr = (MAGIC +
           struct.pack("<HHHH", w, h, SEGMENTS, 0) +
           struct.pack("<IIIIII", rom_off, len(rom), bg_off, seg_off,
                       cov_off, len(cov)) +
           struct.pack("<7BB", *c, 0) +
           title.encode("ascii", "replace")[:31].ljust(32, b"\0"))
    assert len(hdr) == HDR, len(hdr)
    return hdr + rom + bg + segb + bytes(cov), len(nib)


# ---------------------------------------------------------------------------
# Assembling, from either kind of input
# ---------------------------------------------------------------------------

def add_ghosts(bg, darkness, mix):
    """
    Print every segment faintly into the background, lit or not.

    The union of them, once, rather than one composite per segment: where two
    electrodes overlap on the glass there is still only one sheet of crystal,
    so the overlap should not come out twice as dark.
    """
    from PIL import Image, ImageChops

    union = Image.new("L", bg.size, 0)
    for d in darkness.values():
        union = ImageChops.lighter(union, d)

    m = ImageChops.invert(union.point(lambda v: v * mix // 255))
    r, g, b = bg.split()
    return Image.merge("RGB", tuple(ImageChops.multiply(c, m) for c in (r, g, b)))


def build(w, h, rom, bg_img, darkness, clock, title, reach, args):
    """
    darkness: {index: full-page L image, 0..255} for the segments that exist.
    bg_img:   PIL RGB, w x h.
    """
    from PIL import Image

    if reach:
        bg_img, lit = background_shadow(bg_img, reach * COLOUR_MULT,
                                        max(1, reach * COLOUR_MULT // 3),
                                        COLOUR_MIX, args.sat)
        log(f"  background: {lit} printed pixels casting at {reach * COLOUR_MULT} px")

    if args.ghost:
        bg_img = add_ghosts(bg_img, darkness, args.ghost)
        log(f"  ghosts: every segment printed at {args.ghost}/255")

    segs = [(0, 0, 0, 0, [])] * SEGMENTS
    used = 0
    for idx, d in darkness.items():
        if reach:
            at, m = segment_map(d, reach, max(1, reach * 3 // 5), SEG_MIX, (w, h))
        else:
            box = d.getbbox()
            at, m = (box[:2], Image.eval(d.crop(box), lambda v: 255 - v)) if box else (None, None)
        if at is None:
            continue
        segs[idx] = (at[0], at[1], m.width, m.height, [v >> 4 for v in m.tobytes()])
        used += 1

    log(f"  {used} segments have pixels")
    return pack(w, h, rom, rgb565(bg_img), segs, title, clock)


# ---------------------------------------------------------------------------
# .gw, the shortcut
# ---------------------------------------------------------------------------

def from_gw(path, title, args, box):
    from PIL import Image

    src = Path(path).read_bytes()
    if src[:5] != b"SM5A_":
        sys.exit(f"{path}: not an SM5A .gw (header says {src[:8]!r})")

    f = dict(zip("bg bg_sz segpx segpx_sz segoff segoff_sz segx segx_sz segy "
                 "segy_sz segh segh_sz segw segw_sz mel mel_sz prog prog_sz "
                 "kbd kbd_sz".split(), struct.unpack_from("<20I", src, 28)))

    # A .gw carries no dimensions; the emulator it was made for hardcodes them.
    W, H = 320, 240
    if f["bg_sz"] != W * H * 2:
        sys.exit(f"{path}: background is {f['bg_sz']} bytes, not {W}x{H}")

    # It does carry the clock addresses, in the same order this tool uses.
    clock = tuple(src[16:22]) + (src[22],)
    if not clock[6]:
        clock = None

    px = []
    for i in range(W * H):
        c = struct.unpack_from("<H", src, f["bg"] + 2 * i)[0]
        r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
        px.append((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    bg = Image.new("RGB", (W, H))
    bg.putdata(px)

    u16 = lambda k, i: struct.unpack_from("<H", src, f[k] + 2 * i)[0]
    u32 = lambda k, i: struct.unpack_from("<I", src, f[k] + 4 * i)[0]

    def nib(n):
        """.gw packs the high nibble first; we pack the low one first."""
        b = src[f["segpx"] + (n >> 1)]
        return (b >> 4) if (n & 1) == 0 else (b & 0xF)

    darkness = {}
    for i in range(SEGMENTS):
        x, y = u16("segx", i), u16("segy", i)
        sw, sh = u16("segw", i), u16("segh", i)
        base = u32("segoff", i)
        if not sw or not sh:
            continue
        # .gw stores a multiplier; this wants darkness, on the whole page.
        cell = Image.frombytes("L", (sw, sh),
                               bytes(255 - (nib(base + p) * 17)
                                     for p in range(sw * sh)))
        page = Image.new("L", (W, H), 0)
        page.paste(cell, (x, y))
        darkness[i] = page

    rom = src[f["prog"]: f["prog"] + f["prog_sz"]]

    # A .gw was rendered for a Game & Watch's own 320x240 screen. Blow it up
    # here rather than on the tablet: the app draws its artwork at whatever
    # size it arrives, and Lanczos on a desktop beats pixel replication on the
    # panel - and doing it now means the shadows below are generated at the
    # final resolution instead of being magnified along with everything else.
    # It is still a bitmap being enlarged, and it still looks like one. The
    # romset is what makes this sharp.
    w, h = fit(W, H, box)
    if (w, h) != (W, H):
        log(f"  artwork: {W}x{H} bitmap -> {w}x{h} (soft; a romset would be sharp)")
        bg = bg.resize((w, h), Image.LANCZOS)
        darkness = {i: d.resize((w, h), Image.LANCZOS) for i, d in darkness.items()}

    reach = args.shadow if args.shadow >= 0 else max(3, h // 96)
    log(f"  shadows: segments {reach} px, print {reach * COLOUR_MULT} px")

    blob, nnib = build(w, h, rom, bg, darkness, clock, title, reach, args)
    return blob, nnib, w, h


# ---------------------------------------------------------------------------

def from_romset(args, box, title):
    from PIL import Image, ImageChops

    exe = inkscape_bin()
    log(f"inkscape: {exe}")

    tmp = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="genlcd"))
    tmp.mkdir(parents=True, exist_ok=True)

    rom_path, svg_path = unpack(args.input, tmp)
    rom = rom_path.read_bytes()
    sha = hashlib.sha1(rom).hexdigest()
    log(f"  rom: {rom_path.name}, {len(rom)} bytes, sha1 {sha[:12]}")

    clock = CLOCKS.get(sha)
    if clock:
        log(f"  clock: RAM {clock[:6]}, afternoon bit {clock[6]}")
    else:
        log("  clock: unknown for this ROM - TIME will run from whatever the "
            "game powers up with")

    tree, root, seg_ids = find_segments(svg_path)
    if not seg_ids:
        sys.exit(f"{svg_path.name}: no shapes titled 'O.segment.common'. Is this "
                 "an SM5A machine, and is it hap's artwork?")
    log(f"  segments: {len(seg_ids)} shapes labelled in the artwork")

    nat_w, nat_h = viewbox(root)
    w, h = fit(nat_w, nat_h, box)
    log(f"  artwork: {nat_w:g}x{nat_h:g} natural -> {w}x{h} to fit {box[0]}x{box[1]}")

    segdir = tmp / "seg"
    segdir.mkdir(exist_ok=True)
    files = render_segments(exe, svg_path, list(seg_ids), w, h, segdir)

    # After render_segments, because it removes the shapes from the tree.
    bg_png = render_background(exe, tree, set(seg_ids), w, h, tmp / "bg.png")
    bg = Image.open(bg_png).convert("RGB")

    # The shapes were drawn dark on white, so inverted grey is how dark they
    # are - which is the anti-aliased edge included, for free.
    darkness = {idx: ImageChops.invert(Image.open(files[eid]).convert("L"))
                for eid, idx in seg_ids.items()}

    reach = args.shadow if args.shadow >= 0 else max(3, h // 96)
    log(f"  shadows: segments {reach} px, print {reach * COLOUR_MULT} px")

    blob, nnib = build(w, h, rom, bg, darkness, clock, title, reach, args)
    if args.keep:
        log(f"  intermediates kept in {tmp}")
    return blob, nnib, w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="a MAME romset .zip, or a .gw with --gw")
    ap.add_argument("-o", "--out", default="nupogodi.lcd")
    ap.add_argument("--gw", action="store_true",
                    help="the input is a bzhxx .gw, already rendered at 320x240")
    ap.add_argument("--fit", default="x".join(str(v) for v in DEFAULT_FIT),
                    help=f"the box the artwork must fit in (default {DEFAULT_FIT[0]}x{DEFAULT_FIT[1]})")
    ap.add_argument("--shadow", type=int, default=-1, metavar="PX",
                    help="how far a segment's shadow falls; the printed artwork's "
                         f"falls {COLOUR_MULT}x that. 0 for none, default height/128")
    ap.add_argument("--ghost", type=int, default=GHOST_MIX, metavar="N",
                    help="how strongly every segment, lit or not, is printed into "
                         f"the background, 0-255. 0 for none, default {GHOST_MIX}")
    ap.add_argument("--sat", type=int, default=COLOUR_SAT, metavar="N",
                    help="how coloured a background pixel must be to be treated "
                         f"as print rather than as LCD ground (default {COLOUR_SAT})")
    ap.add_argument("--title", default="", help="what the status line says on launch")
    ap.add_argument("--keep", metavar="DIR",
                    help="keep the intermediate PNGs here, to look at when it goes wrong")
    args = ap.parse_args()

    title = args.title or Path(args.input).stem

    box = tuple(int(v) for v in args.fit.lower().split("x"))

    if args.gw:
        blob, nnib, w, h = from_gw(args.input, title, args, box)
    else:
        blob, nnib, w, h = from_romset(args, box, title)

    Path(args.out).write_bytes(blob)
    log(f"{args.out}: {w}x{h}, {nnib} coverage pixels, {len(blob)} bytes")
    log("put it on the card as apps/nupogodi/nupogodi.lcd:")
    log(f"  ./do upload --file {args.out} --as apps/nupogodi/nupogodi.lcd")


if __name__ == "__main__":
    main()
