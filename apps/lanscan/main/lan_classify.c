/*
 * Evidence to a one-word guess.
 *
 * Deliberately conservative, and in a specific way: every rule needs a
 * positive signal - an open port, an advertised service, a vendor string -
 * and anything unmatched stays blank rather than being filed under a default.
 * "unknown" and "" look the same on screen and mean different things, and the
 * one this can honestly say is the empty one.
 *
 * Order matters more than the rules do. A desktop advertising _airplay is a
 * PC running iTunes and not a speaker, so the Windows ports are tested before
 * the service hints; a distro name beats the family name, so Raspbian is
 * tested before Debian and both before Linux.
 */

#include "lanscan.h"

static bool has_port(const lan_device_t *d, uint16_t port)
{
    for (int i = 0; i < d->nports; i++) {
        if (d->ports[i].port == port) {
            return true;
        }
    }
    return false;
}

static bool has_service(const lan_device_t *d, const char *needle)
{
    for (int i = 0; i < d->nservices; i++) {
        if (lan_has(d->services[i], needle)) {
            return true;
        }
    }
    return false;
}

/*
 * Everything the device said about itself, in one buffer.
 *
 * Built once per classification rather than searched field by field, because
 * almost every rule below wants "does the word 'printer' appear anywhere",
 * and the alternative is the same rule written six times with a different
 * field in it.
 */
static void gather(const lan_device_t *d, char *out, size_t size)
{
    int at = snprintf(out, size, "%s %s %s %s",
                      d->name, d->detail, d->vendor, d->workgroup);
    if (at < 0) {
        at = 0;
    }
    for (int i = 0; i < d->nservices && (size_t)at < size; i++) {
        const int n = snprintf(out + at, size - (size_t)at, " %s", d->services[i]);
        if (n < 0) {
            break;
        }
        at += n;
    }
    for (int i = 0; i < d->nports && (size_t)at < size; i++) {
        const int n = snprintf(out + at, size - (size_t)at, " %s %s %s",
                               d->ports[i].title, d->ports[i].server,
                               d->ports[i].banner);
        if (n < 0) {
            break;
        }
        at += n;
    }
}

typedef struct {
    const char *needle;
    const char *label;
} hint_t;

static const hint_t s_vendor_hints[] = {
    { "raspberry",       "SBC"             },
    { "ubiquiti",        "network gear"    },
    { "mikrotik",        "network gear"    },
    { "tp-link",         "network gear"    },
    { "netgear",         "network gear"    },
    { "zyxel",           "network gear"    },
    { "huawei",          "network gear"    },
    { "cisco",           "network gear"    },
    { "aruba",           "network gear"    },
    { "brother",         "printer"         },
    { "canon",           "printer"         },
    { "epson",           "printer"         },
    { "kyocera",         "printer"         },
    { "synology",        "NAS"             },
    { "qnap",            "NAS"             },
    { "western digital", "NAS"             },
    { "sonos",           "speaker"         },
    { "roku",            "TV/stick"        },
    { "amazon",          "smart device"    },
    { "google",          "smart device"    },
    { "nest",            "smart device"    },
    { "espressif",       "IoT/ESP"         },
    { "tuya",            "IoT"             },
    { "shelly",          "IoT"             },
    { "sonoff",          "IoT"             },
    { "xiaomi",          "IoT/phone"       },
    { "apple",           "Apple device"    },
    { "samsung",         "Samsung device"  },
    { "vmware",          "virtual machine" },
    { "hyper-v",         "virtual machine" },
    { "virtualbox",      "virtual machine" },
    { "qemu",            "virtual machine" },
    { "intel",           "PC"              },
    { "realtek",         "PC"              },
};

static const hint_t s_service_hints[] = {
    { "_ipp",             "printer"        },
    { "_printer",         "printer"        },
    { "_pdl-datastream",  "printer"        },
    { "_scanner",         "scanner"        },
    { "_uscan",           "scanner"        },
    { "_googlecast",      "cast device"    },
    { "_androidtvremote", "Android TV"     },
    { "_airplay",         "AirPlay device" },
    { "_raop",            "AirPlay speaker"},
    { "_sonos",           "speaker"        },
    { "_spotify-connect", "speaker"        },
    { "_hap",             "HomeKit device" },
    { "_matter",          "Matter device"  },
    { "_esphomelib",      "ESPHome node"   },
    { "_home-assistant",  "Home Assistant" },
    { "_octoprint",       "3D printer"     },
    { "_nvstream",        "gaming PC"      },
    { "_plexmediasvr",    "media server"   },
    { "_smb",             "file server"    },
    { "_afpovertcp",      "file server"    },
    { "_companion-link",  "Apple device"   },
};

static const char *guess_kind(const lan_device_t *d, const char *all)
{
    if (d->seen & LAN_SEEN_GW) {
        return "router";
    }
    if (d->seen & LAN_SEEN_SELF) {
        return "this tablet";
    }

    if (lan_has(all, "internetgatewaydevice") || lan_has(all, "router")) {
        return "router";
    }
    if (has_port(d, 9100) || has_port(d, 515) || has_port(d, 631) ||
        lan_has(all, "printer")) {
        return "printer";
    }
    if (has_port(d, 554) || has_port(d, 8554) || has_port(d, 37777) ||
        lan_has(all, "camera") || lan_has(all, "onvif")) {
        return "camera";
    }

    /*
     * Before the service hints, on purpose. 139 and 445 are not in here
     * because Samba on a NAS or a Pi serves both; these four are only ever a
     * real Windows machine, and a Windows machine advertising _airplay is one
     * with iTunes installed.
     */
    if (has_port(d, 135) || has_port(d, 3389) || has_port(d, 5985) || has_port(d, 5986)) {
        return "Windows PC";
    }

    for (int i = 0; i < (int)(sizeof(s_service_hints) / sizeof(s_service_hints[0])); i++) {
        if (has_service(d, s_service_hints[i].needle)) {
            return s_service_hints[i].label;
        }
    }

    if (has_port(d, 32400) || lan_has(all, "plex") || lan_has(all, "dlna") ||
        lan_has(all, "mediaserver")) {
        return "media server";
    }
    if (has_port(d, 62078)) {
        return "iPhone/iPad";
    }
    if (has_port(d, 5555) && !has_port(d, 22)) {
        return "Android device";
    }
    if (has_port(d, 2375) || has_port(d, 2376) || has_port(d, 6443)) {
        return "container host";
    }
    if (has_port(d, 3306) || has_port(d, 5432) || has_port(d, 27017) || has_port(d, 6379)) {
        return "server";
    }
    if (has_port(d, 22) && has_port(d, 80)) {
        return "Linux host";
    }

    for (int i = 0; i < (int)(sizeof(s_vendor_hints) / sizeof(s_vendor_hints[0])); i++) {
        if (lan_has(d->vendor, s_vendor_hints[i].needle)) {
            return s_vendor_hints[i].label;
        }
    }

    if (has_port(d, 22)) {
        return "Linux host";
    }
    if (has_port(d, 80) || has_port(d, 443)) {
        return "web device";
    }
    return "";
}

/* The classic initial-TTL fingerprint. Coarse, and free. */
static const char *os_from_ttl(uint8_t ttl)
{
    if (!ttl) {
        return "";
    }
    if (ttl > 128) {
        return "net-gear";     /* started at 255: routers, printers, *BSD */
    }
    if (ttl > 64) {
        return "Windows";      /* started at 128 */
    }
    if (ttl > 32) {
        return "Linux";        /* started at 64 */
    }
    return "";
}

static const hint_t s_os_hints[] = {
    { "raspbian", "Raspbian" }, { "debian",  "Debian"  },
    { "ubuntu",   "Ubuntu"   }, { "openwrt", "OpenWrt" },
    { "dd-wrt",   "DD-WRT"   }, { "mikrotik","RouterOS"},
    { "synology", "DSM"      }, { "freebsd", "FreeBSD" },
    { "openbsd",  "OpenBSD"  }, { "android", "Android" },
    { "darwin",   "macOS"    }, { "mac os",  "macOS"   },
    { "linux",    "Linux"    }, { "unix",    "Unix"    },
};

void lan_classify(lan_device_t *d)
{
    char all[512];
    gather(d, all, sizeof(all));

    const char *kind = guess_kind(d, all);
    if (kind[0]) {
        lan_copy(d->kind, sizeof(d->kind), kind);
    }

    /* Most specific first: a distro name beats the family name, and both beat
       anything the TTL could have told us. */
    for (int i = 0; i < (int)(sizeof(s_os_hints) / sizeof(s_os_hints[0])); i++) {
        if (lan_has(all, s_os_hints[i].needle)) {
            lan_copy(d->os, sizeof(d->os), s_os_hints[i].label);
            return;
        }
    }

    /*
     * "Windows Media Connect compatible" is a DLNA profile string that Linux
     * media servers advertise, so a bare "windows" proves nothing. Only these
     * shapes actually mean the host runs it.
     */
    if (lan_has(all, "microsoft-iis") || lan_has(all, "microsoft-httpapi") ||
        lan_has(all, "microsoft windows") || lan_has(all, "windows server") ||
        lan_has(all, "windows nt")) {
        lan_copy(d->os, sizeof(d->os), "Windows");
        return;
    }

    if (d->os[0] == 0) {
        lan_copy(d->os, sizeof(d->os), os_from_ttl(d->ttl));
    }
}
