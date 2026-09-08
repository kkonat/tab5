/*
 * Which ports to try, and what a number means.
 *
 * Straight from the desktop version, because the lists are the interesting
 * part and they do not become different lists on a different machine. What is
 * different is how many of them get used: see lan_engine.c for why the knock
 * runs against confirmed hosts rather than the whole subnet.
 *
 * The tables are const, so they live in the app's read-only image in PSRAM
 * and cost nothing but their own size.
 */

#include "lanscan.h"

/* The quick pass: services almost everything on a LAN answers on. */
const uint16_t lan_top_ports[] = {
    80, 443, 22, 445, 139, 135, 8080, 53, 3389, 5000, 8443, 21, 23, 631,
    9100, 1883, 5353, 62078, 32400, 8006, 554,
};
const int lan_top_ports_count = (int)(sizeof(lan_top_ports) / sizeof(lan_top_ports[0]));

/*
 * The thorough pass, roughly nmap's top 200 plus the usual home and IoT
 * suspects. Ordered by how likely a home network is to have one open rather
 * than numerically, because LAN_DEPTH_NORMAL takes the first 48 of it and a
 * prefix of a numerically sorted list would be almost all of it below 1024.
 */
const uint16_t lan_deep_ports[] = {
    /* the ones a home LAN actually has */
    8081, 8123, 8096, 3000, 5001, 9000, 8888, 8200, 1900, 5900, 5555,
    2049, 548, 111, 8009, 8010, 7000, 3306, 5432, 6379, 27017, 9091,
    8291, 8728, 161, 389, 636, 873, 902, 990, 993, 995, 25, 110, 143,
    587, 465, 8443, 4443, 2375, 2376, 6443, 9200, 5601, 8500, 11211,
    /* and the rest of the list */
    1, 7, 9, 13, 19, 26, 37, 42, 49, 70, 79, 81, 82, 83, 88, 106, 113,
    119, 144, 179, 199, 427, 444, 512, 513, 514, 515, 543, 544, 593,
    646, 1025, 1026, 1027, 1080, 1110, 1194, 1234, 1352, 1400, 1433,
    1521, 1723, 1755, 1801, 2000, 2001, 2121, 2181, 2222, 2483, 3001,
    3128, 3260, 3333, 3478, 3689, 3690, 4000, 4040, 4444, 4567, 4664,
    4747, 5009, 5040, 5060, 5100, 5222, 5631, 5666, 5672, 5800, 5901,
    5985, 5986, 6000, 6001, 6667, 7001, 7070, 7100, 7777, 8000, 8008,
    8086, 8088, 8089, 8181, 8333, 8388, 8686, 8765, 8834, 8889, 9001,
    9009, 9090, 9295, 9443, 9999, 10000, 10001, 12345, 20000, 32469,
    49152, 49153, 49154, 50000, 51413, 55000,
};
const int lan_deep_ports_count = (int)(sizeof(lan_deep_ports) / sizeof(lan_deep_ports[0]));

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t    port;
    const char *name;
} port_name_t;

/*
 * A sorted table and a binary search, rather than the switch a compiler would
 * turn into a jump table with 65,000 holes in it. It is looked up once per
 * open port found, so the search is not the point - the point is that adding
 * a service is adding a line.
 */
static const port_name_t s_names[] = {
    {    1, "tcpmux"      }, {    7, "echo"        }, {    9, "discard"     },
    {   13, "daytime"     }, {   19, "chargen"     }, {   21, "ftp"         },
    {   22, "ssh"         }, {   23, "telnet"      }, {   25, "smtp"        },
    {   37, "time"        }, {   42, "wins"        }, {   49, "tacacs"      },
    {   53, "domain"      }, {   70, "gopher"      }, {   79, "finger"      },
    {   80, "http"        }, {   81, "http-alt"    }, {   82, "http-alt"    },
    {   88, "kerberos"    }, {  106, "pop3pw"      }, {  110, "pop3"        },
    {  111, "rpcbind"     }, {  113, "ident"       }, {  119, "nntp"        },
    {  135, "msrpc"       }, {  139, "netbios"     }, {  143, "imap"        },
    {  161, "snmp"        }, {  179, "bgp"         }, {  389, "ldap"        },
    {  427, "svrloc"      }, {  443, "https"       }, {  445, "smb"         },
    {  465, "smtps"       }, {  500, "isakmp"      }, {  512, "exec"        },
    {  513, "login"       }, {  514, "shell"       }, {  515, "printer"     },
    {  543, "klogin"      }, {  548, "afp"         }, {  554, "rtsp"        },
    {  587, "submission"  }, {  593, "rpc-http"    }, {  631, "ipp"         },
    {  636, "ldaps"       }, {  873, "rsync"       }, {  902, "vmware"      },
    {  990, "ftps"        }, {  993, "imaps"       }, {  995, "pop3s"       },
    { 1080, "socks"       }, { 1194, "openvpn"     }, { 1433, "mssql"       },
    { 1521, "oracle"      }, { 1723, "pptp"        }, { 1883, "mqtt"        },
    { 1900, "upnp"        }, { 2000, "sccp"        }, { 2049, "nfs"         },
    { 2121, "ftp-alt"     }, { 2181, "zookeeper"   }, { 2222, "ssh-alt"     },
    { 2375, "docker"      }, { 2376, "docker-tls"  }, { 3000, "http-dev"    },
    { 3128, "squid"       }, { 3260, "iscsi"       }, { 3306, "mysql"       },
    { 3389, "rdp"         }, { 3478, "stun"        }, { 3689, "daap"        },
    { 3690, "svn"         }, { 4040, "http-alt"    }, { 5000, "upnp/http"   },
    { 5001, "http-alt"    }, { 5009, "airport"     }, { 5060, "sip"         },
    { 5222, "xmpp"        }, { 5353, "mdns"        }, { 5432, "postgres"    },
    { 5555, "adb"         }, { 5601, "kibana"      }, { 5672, "amqp"        },
    { 5800, "vnc-http"    }, { 5900, "vnc"         }, { 5901, "vnc"         },
    { 5985, "winrm"       }, { 5986, "winrm-tls"   }, { 6000, "x11"         },
    { 6379, "redis"       }, { 6443, "kubernetes"  }, { 6667, "irc"         },
    { 7000, "afs/http"    }, { 7070, "rtsp-alt"    }, { 8000, "http-alt"    },
    { 8006, "proxmox"     }, { 8008, "http-alt"    }, { 8009, "ajp/cast"    },
    { 8080, "http-proxy"  }, { 8081, "http-alt"    }, { 8086, "influxdb"    },
    { 8088, "http-alt"    }, { 8096, "jellyfin"    }, { 8123, "home-assist" },
    { 8181, "http-alt"    }, { 8200, "vault/gopro" }, { 8291, "winbox"      },
    { 8443, "https-alt"   }, { 8500, "consul"      }, { 8728, "mikrotik"    },
    { 8834, "nessus"      }, { 8888, "http-alt"    }, { 9000, "http-alt"    },
    { 9090, "http-alt"    }, { 9091, "transmission"}, { 9100, "jetdirect"   },
    { 9200, "elastic"     }, { 9295, "playstation" }, { 9443, "https-alt"   },
    {10000, "webmin"      }, {11211, "memcached"   }, {20000, "dnp3"        },
    {27017, "mongodb"     }, {32400, "plex"        }, {32469, "plex-dlna"   },
    {49152, "upnp-dyn"    }, {51413, "bittorrent"  }, {62078, "iphone-sync" },
};

const char *lan_service_name(uint16_t port)
{
    int lo = 0;
    int hi = (int)(sizeof(s_names) / sizeof(s_names[0])) - 1;

    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_names[mid].port == port) {
            return s_names[mid].name;
        }
        if (s_names[mid].port < port) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return "";
}

/*
 * Ports worth speaking HTTP to.
 *
 * Not the same question as "is this a web server": it is "would a GET here
 * tell us more than listening would". The TLS ports are deliberately absent -
 * an app has no TLS, so 443 gets neither a title nor a certificate name, and
 * pretending to probe it would mean sending a plaintext GET at a TLS listener
 * and reporting whatever alert came back as a banner.
 */
bool lan_is_http_port(uint16_t port)
{
    switch (port) {
    case 80: case 81: case 82: case 83: case 280: case 591:
    case 3000: case 3001: case 4040: case 4567: case 5000: case 5001:
    case 5601: case 7000: case 8000: case 8008: case 8010: case 8080:
    case 8081: case 8086: case 8088: case 8089: case 8096: case 8123:
    case 8181: case 8200: case 8765: case 8888: case 8889: case 9000:
    case 9090: case 9091: case 10000: case 32400:
        return true;
    default:
        return false;
    }
}

/* Services that announce themselves the moment you connect. Anything else
   gets a newline to see whether that shakes something loose. */
bool lan_talks_first(uint16_t port)
{
    switch (port) {
    case 21: case 22: case 23: case 25: case 79: case 110: case 119:
    case 143: case 465: case 587: case 993: case 995: case 3306:
    case 5432: case 6379: case 11211: case 1883: case 9100:
        return true;
    default:
        return false;
    }
}
