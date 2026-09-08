/*
 * MAC prefix to manufacturer.
 *
 * Two sources, and nothing else. A short built-in table of the prefixes that
 * change how you read a row - a hypervisor, a Raspberry Pi - and the IEEE
 * registry, if it is on the card.
 *
 * The registry is 34,000 organisations and about two megabytes as the CSV
 * that IEEE publishes. It is not read into memory. tools/genoui turns it into
 * a flat file of fixed 32-byte records sorted by prefix, and this binary
 * searches it in place with neos_file_read_at() - sixteen reads of thirty-two
 * bytes to answer one question, against a megabyte held resident to answer
 * the same question faster than anybody could notice. That call exists for
 * exactly this: reading one member of a pack without holding the pack.
 *
 * Nothing is guessed. A prefix that is in neither source comes back empty and
 * stays empty on screen, because a vendor column that is sometimes a fact and
 * sometimes a plausible-looking invention is worse than one with gaps in it.
 */

#include "lanscan.h"

#define OUI_PATH  "apps/lanscan/oui.bin"

/*
 * The file: an 8-byte header, then `count` records of 32 bytes, sorted.
 *
 *   magic[4] = "NOUI", u32 count
 *   record   = u8 prefix[3], u8 pad, char name[28]   (name NUL-terminated)
 *
 * Fixed-width records are what make the search possible without an index, and
 * 32 bytes is one flash read either way, so the padding costs nothing real.
 */
#define OUI_HEADER   8
#define OUI_RECORD  32
#define OUI_NAME    28

static bool s_checked;
static int  s_entries;

/* The prefixes worth knowing on sight. Every one of these tells you something
   the IEEE file alone would not: that the "device" is not a device. */
typedef struct {
    uint8_t     prefix[3];
    const char *name;
} builtin_t;

static const builtin_t s_builtin[] = {
    { { 0x00, 0x50, 0x56 }, "VMware"            },
    { { 0x00, 0x0C, 0x29 }, "VMware"            },
    { { 0x00, 0x05, 0x69 }, "VMware"            },
    { { 0x00, 0x1C, 0x14 }, "VMware"            },
    { { 0x00, 0x15, 0x5D }, "Microsoft Hyper-V" },
    { { 0x08, 0x00, 0x27 }, "VirtualBox"        },
    { { 0x0A, 0x00, 0x27 }, "VirtualBox"        },
    { { 0x52, 0x54, 0x00 }, "QEMU/KVM"          },
    { { 0x00, 0x16, 0x3E }, "Xen"               },
    { { 0xB8, 0x27, 0xEB }, "Raspberry Pi"      },
    { { 0xDC, 0xA6, 0x32 }, "Raspberry Pi"      },
    { { 0xE4, 0x5F, 0x01 }, "Raspberry Pi"      },
    { { 0x28, 0xCD, 0xC1 }, "Raspberry Pi"      },
    { { 0x2C, 0xCF, 0x67 }, "Raspberry Pi"      },
    { { 0xD8, 0x3A, 0xDD }, "Raspberry Pi"      },
};
#define BUILTIN_COUNT  ((int)(sizeof(s_builtin) / sizeof(s_builtin[0])))

static void check_db(void)
{
    if (s_checked) {
        return;
    }
    s_checked = true;
    s_entries = 0;

    uint8_t head[OUI_HEADER];
    if (neos_file_read_at(OUI_PATH, head, sizeof(head), 0) != (int)sizeof(head)) {
        return;
    }
    if (memcmp(head, "NOUI", 4) != 0) {
        return;
    }
    const uint32_t count = ((uint32_t)head[4] << 24) | ((uint32_t)head[5] << 16) |
                           ((uint32_t)head[6] << 8) | head[7];
    /* A count that would put the last record past any plausible file is a
       truncated or corrupt download, and a binary search over it would read
       garbage rather than fail. */
    const int size = neos_file_size(OUI_PATH);
    if (count == 0 || count > 200000 ||
        size < (int)(OUI_HEADER + count * OUI_RECORD)) {
        return;
    }
    s_entries = (int)count;
}

bool lan_oui_have_db(void)
{
    check_db();
    return s_entries > 0;
}

int lan_oui_db_entries(void)
{
    check_db();
    return s_entries;
}

static bool search_db(const uint8_t mac[6], char *out, size_t size)
{
    check_db();
    if (s_entries <= 0) {
        return false;
    }

    int lo = 0;
    int hi = s_entries - 1;

    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;

        uint8_t rec[OUI_RECORD];
        const uint32_t off = (uint32_t)(OUI_HEADER + mid * OUI_RECORD);
        if (neos_file_read_at(OUI_PATH, rec, sizeof(rec), off) != (int)sizeof(rec)) {
            return false;       /* the card went away mid-search */
        }

        const int cmp = memcmp(rec, mac, 3);
        if (cmp == 0) {
            char name[OUI_NAME + 1];
            memcpy(name, rec + 4, OUI_NAME);
            name[OUI_NAME] = 0;
            lan_clean(out, size, name);
            return out[0] != 0;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

bool lan_oui_lookup(const uint8_t mac[6], char *out, size_t size)
{
    if (!mac || !out || size == 0) {
        return false;
    }
    out[0] = 0;

    /*
     * The locally-administered bit, which on a LAN in practice means a phone
     * rotating its address for privacy. There is no manufacturer to find, so
     * this is answered rather than searched for - looking it up would be
     * searching for something that is not there by design.
     */
    if (mac[0] & 0x02) {
        lan_copy(out, size, "(random MAC)");
        return true;
    }

    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (memcmp(s_builtin[i].prefix, mac, 3) == 0) {
            lan_copy(out, size, s_builtin[i].name);
            return true;
        }
    }
    return search_db(mac, out, size);
}
