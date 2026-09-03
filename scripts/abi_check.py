#!/usr/bin/env python3
"""
Check a built app ELF against the NeOS ABI.

Two things, both of which otherwise only show up on the device:

  1. The ABI guard is really in the binary. Apps link with --gc-sections and
     the guard is by design referenced by nothing, so if the `retain` attribute
     in neos_abi.h ever stops working the pointer is silently collected - and a
     guard that vanished looks exactly like a guard that passed. This is the
     only way to tell the difference.

  2. Every other symbol the app leaves undefined can actually be resolved -
     by the syscall table or by the loader's own libc table. Otherwise the
     first news of a missing syscall is "Can't find symbol" from the loader,
     with the app already half-relocated.

Pure stdlib and a hand-rolled ELF32 reader, so it runs without the toolchain
on the path and without pyelftools.

    python scripts/abi_check.py apps/hello/build/hello.app.elf
    python scripts/abi_check.py apps/*/build/*.app.elf
"""

import argparse
import glob
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ABI_H = ROOT / "neos" / "components" / "neos_api" / "include" / "neos_abi.h"
SYSCALLS_C = ROOT / "neos" / "main" / "neos_syscalls.c"

# The loader brings its own table of libc and IDF symbols alongside ours. Its
# entries sit behind Kconfig, and we read them all regardless: over-counting
# there only softens this check, while under-counting would flag working apps.
LOADER_SYMS_C = (ROOT / "neos" / "managed_components" / "espressif__elf_loader"
                 / "src" / "esp_elf_symbol.c")

GUARD_RE = re.compile(r"^neos_abi_(\d+)_(\d+)$")

# Undefined symbols an app may legitimately carry that no table resolves:
# its own entry point, and the linker's placeholder for the GOT.
NOT_A_SYSCALL = {"app_main", "_GLOBAL_OFFSET_TABLE_", ""}


# --- the firmware side: what the ABI header and the syscall table say --------

def firmware_abi():
    """(major, minor) the firmware currently builds apps against."""
    text = ABI_H.read_text(encoding="utf-8")
    def one(name):
        m = re.search(r"^#define\s+%s\s+(\d+)" % name, text, re.M)
        if not m:
            sys.exit("abi_check: no %s in %s" % (name, ABI_H))
        return int(m.group(1))
    return one("NEOS_ABI_MAJOR"), one("NEOS_ABI_MINOR")


def firmware_guards():
    """Every (major, minor) row of NEOS_ABI_GUARDS."""
    text = ABI_H.read_text(encoding="utf-8")
    m = re.search(r"#define\s+NEOS_ABI_GUARDS\(X\)((?:.*?\\\n)*.*)", text)
    if not m:
        sys.exit("abi_check: no NEOS_ABI_GUARDS list in %s" % ABI_H)
    return {(int(a), int(b)) for a, b in re.findall(r"X\(\s*(\d+)\s*,\s*(\d+)\s*\)", m.group(1))}


def exported_names(path):
    return set(re.findall(r"ESP_ELFSYM_EXPORT\(\s*([A-Za-z_]\w*)\s*\)",
                          path.read_text(encoding="utf-8")))


def firmware_exports():
    """Every name an app can resolve: our syscalls, the guards, the loader's."""
    names = exported_names(SYSCALLS_C)
    names |= {"neos_abi_%d_%d" % g for g in firmware_guards()}
    if LOADER_SYMS_C.exists():
        names |= exported_names(LOADER_SYMS_C)
    return names


# --- the app side: undefined symbols in the ELF ------------------------------

def elf_undefined_symbols(path):
    """Names of every undefined symbol in an ELF32 little-endian file."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("abi_check: %s is not a 32-bit little-endian ELF" % path)

    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    if not e_shoff or not e_shnum:
        sys.exit("abi_check: %s has no section headers" % path)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        _name, sh_type = struct.unpack_from("<II", data, off)
        sh_offset, sh_size, sh_link = struct.unpack_from("<III", data, off + 0x10)
        sh_entsize, = struct.unpack_from("<I", data, off + 0x24)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    def cstr(base, offset):
        end = data.index(b"\0", base + offset)
        return data[base + offset:end].decode("utf-8", "replace")

    SHT_SYMTAB, SHT_DYNSYM, SHN_UNDEF = 2, 11, 0
    undefined = set()
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type not in (SHT_SYMTAB, SHT_DYNSYM) or not sh_entsize:
            continue
        strtab = sections[sh_link][1]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_name, = struct.unpack_from("<I", data, off)
            st_shndx, = struct.unpack_from("<H", data, off + 0x0E)
            if st_shndx == SHN_UNDEF and st_name:
                undefined.add(cstr(strtab, st_name))
    return undefined


# --- the check ---------------------------------------------------------------

def check(path, exports, fw_major, fw_minor):
    undefined = elf_undefined_symbols(path)
    guards = sorted(n for n in undefined if GUARD_RE.match(n))
    problems = []

    if not guards:
        problems.append(
            "no ABI guard. The app was either not compiled against neos_abi.h, "
            "or --gc-sections collected the guard - check that the toolchain "
            "honours __attribute__((retain))")
    elif len(guards) > 1:
        problems.append("more than one ABI guard: %s" % ", ".join(guards))
    else:
        major, minor = (int(x) for x in GUARD_RE.match(guards[0]).groups())
        if major != fw_major:
            problems.append(
                "needs ABI %d.%d; this firmware is %d.%d, and a major "
                "difference cannot be bridged - the app must be rebuilt"
                % (major, minor, fw_major, fw_minor))
        elif minor > fw_minor:
            problems.append(
                "needs ABI %d.%d; this firmware only offers %d.%d"
                % (major, minor, fw_major, fw_minor))

    missing = sorted(undefined - exports - NOT_A_SYSCALL
                     - {n for n in undefined if GUARD_RE.match(n)})
    if missing:
        problems.append("not in the syscall table: %s" % ", ".join(missing))

    if problems:
        print("FAIL %s" % path)
        for p in problems:
            print("     %s" % p)
        return False

    print("ok   %s  (ABI %s, %d syscalls)"
          % (path, guards[0][len("neos_abi_"):].replace("_", "."),
             len(undefined) - 1))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="+", help="built .app.elf files (globs are expanded)")
    args = ap.parse_args()

    paths = [Path(p) for pattern in args.elf for p in sorted(glob.glob(pattern))]
    if not paths:
        sys.exit("abi_check: no such ELF files: %s" % " ".join(args.elf))

    fw_major, fw_minor = firmware_abi()
    if (fw_major, fw_minor) not in firmware_guards():
        sys.exit("abi_check: NEOS_ABI_MINOR is %d.%d but there is no matching row "
                 "in NEOS_ABI_GUARDS" % (fw_major, fw_minor))

    exports = firmware_exports()
    print("firmware ABI %d.%d, %d resolvable symbols"
          % (fw_major, fw_minor, len(exports)))
    ok = all([check(p, exports, fw_major, fw_minor) for p in paths])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
