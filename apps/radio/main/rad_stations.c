/*
 * stations.conf, and the settings beside it.
 *
 * Both live next to the app on the card and both are meant to be edited by
 * hand there, which is the whole reason they are text and the reason a line
 * that will not parse is skipped rather than fatal: one bad line should not
 * take the radio out.
 *
 * The station file's format is the pi-Q one unchanged - `name | url | flags`
 * - so a file moves between the two machines without conversion. The settings
 * file is not: the Pi wrote JSON, and this app has no JSON parser and no
 * business growing one for sixteen integers and a volume. It is `key = value`
 * per line, with the same keys the synth used, so the two are still readable
 * against each other by eye.
 *
 * Neither is held open. neos_file_read() and neos_file_write() take whole
 * files for the reason their own comments give - the card can be pulled at
 * any moment - and these are both well under a kilobyte.
 */

#include "radio.h"

#include <string.h>

#define STATIONS_PATH  "apps/radio/stations.conf"
#define SETTINGS_PATH  "apps/radio/settings.conf"

#define FILE_MAX  4096

static const char STATIONS_HEAD[] =
    "# NeOS radio stations.\n"
    "#\n"
    "#   name | url | flags\n"
    "#\n"
    "# flags is comma-separated. The only one that means anything is\n"
    "# 'auto': that station starts playing when the app opens. The app\n"
    "# rewrites this file when you tap a station's star, so keep any\n"
    "# comments of your own above this line.\n"
    "#\n"
    "# http and https both play. What decides whether a station works is\n"
    "# what it sends: this app decodes MP3, so an AAC stream is refused\n"
    "# with a message saying so rather than played badly.\n\n";

/* ------------------------------------------------------------------ */
/* URLs                                                               */
/* ------------------------------------------------------------------ */

bool rad_url_split(const char *url, char *host, size_t host_sz,
                   uint16_t *port, char *path, size_t path_sz, bool *secure)
{
    if (!url) {
        return false;
    }

    bool tls = false;
    if (rad_starts(url, "http://")) {
        url += 7;
    } else if (rad_starts(url, "https://")) {
        url += 8;
        tls = true;
    } else {
        return false;       /* not a scheme this app dials */
    }
    if (secure) {
        *secure = tls;
    }

    /* Any userinfo is skipped. Nothing here authenticates, but a URL that
       carries a user@ would otherwise be read as a hostname with an @ in it,
       which resolves to nothing and reports a confusing failure. */
    const char *at = NULL;
    for (const char *p = url; *p && *p != '/'; p++) {
        if (*p == '@') {
            at = p;
        }
    }
    if (at) {
        url = at + 1;
    }

    size_t n = 0;
    while (url[n] && url[n] != ':' && url[n] != '/') {
        n++;
    }
    if (n == 0 || n + 1 > host_sz) {
        return false;
    }
    memcpy(host, url, n);
    host[n] = 0;
    url += n;

    uint16_t p = tls ? 443 : 80;
    if (*url == ':') {
        url++;
        unsigned value = 0;
        while (*url >= '0' && *url <= '9') {
            value = value * 10 + (unsigned)(*url++ - '0');
            if (value > 65535u) {
                return false;
            }
        }
        if (value == 0) {
            return false;
        }
        p = (uint16_t)value;
    }
    if (port) {
        *port = p;
    }

    if (*url != '/') {
        rad_copy(path, path_sz, "/");
    } else {
        rad_copy(path, path_sz, url);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* In place: trim, and cut at the first '#'. */
static char *clean(char *line)
{
    char *hash = strchr(line, '#');
    if (hash) {
        *hash = 0;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r')) {
        line[--n] = 0;
    }
    return line;
}

int rad_stations_load(rad_station_t *out, int max)
{
    static char text[FILE_MAX];

    const int len = neos_file_read(STATIONS_PATH, text, sizeof(text) - 1);
    if (len <= 0) {
        return 0;           /* -3, no such file, is the ordinary first run */
    }
    text[len] = 0;

    int   count = 0;
    char *save  = text;

    while (*save && count < max) {
        char *line = save;
        char *nl   = strchr(save, '\n');
        if (nl) {
            *nl  = 0;
            save = nl + 1;
        } else {
            save = line + strlen(line);
        }

        line = clean(line);
        if (!*line) {
            continue;
        }

        /* name | url | flags. A line with no url is not a station, and the
           name may be empty - in which case the url stands in for it, which
           is what an ICY name later replaces. */
        char *bar1 = strchr(line, '|');
        if (!bar1) {
            continue;
        }
        *bar1 = 0;
        char *url = clean(bar1 + 1);
        char *flags = strchr(url, '|');
        if (flags) {
            *flags = 0;
            url    = clean(url);
            flags  = clean(flags + 1);
        }
        char *name = clean(line);
        if (!*url) {
            continue;
        }

        rad_station_t *st = &out[count];
        rad_copy(st->name, sizeof(st->name), *name ? name : url);
        rad_copy(st->url, sizeof(st->url), url);
        st->autoplay = flags && rad_find(flags, "auto") != NULL;
        st->secure   = rad_starts(url, "https://");
        count++;
    }
    return count;
}

bool rad_stations_save(const rad_station_t *list, int count)
{
    static char text[FILE_MAX];

    int at = (int)sizeof(STATIONS_HEAD) - 1;
    memcpy(text, STATIONS_HEAD, (size_t)at);

    for (int i = 0; i < count; i++) {
        const int room = (int)sizeof(text) - at;
        if (room < 8) {
            break;
        }
        const int n = snprintf(text + at, (size_t)room, "%s | %s%s\n",
                               list[i].name, list[i].url,
                               list[i].autoplay ? " | auto" : "");
        if (n <= 0 || n >= room) {
            break;
        }
        at += n;
    }
    return neos_file_write(STATIONS_PATH, text, (size_t)at) == 0;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

void rad_settings_load(rad_app_t *app)
{
    for (int p = 0; p < P_COUNT; p++) {
        app->params[p] = rad_ranges[p].def;
    }
    app->volume = 25;

    static char text[FILE_MAX];
    const int len = neos_file_read(SETTINGS_PATH, text, sizeof(text) - 1);
    if (len <= 0) {
        return;
    }
    text[len] = 0;

    char *save = text;
    while (*save) {
        char *line = save;
        char *nl   = strchr(save, '\n');
        if (nl) {
            *nl  = 0;
            save = nl + 1;
        } else {
            save = line + strlen(line);
        }

        line = clean(line);
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char *key   = clean(line);
        char *value = clean(eq + 1);

        bool neg = false;
        if (*value == '-') {
            neg = true;
            value++;
        }
        int number = 0;
        for (; *value >= '0' && *value <= '9'; value++) {
            number = number * 10 + (*value - '0');
        }
        if (neg) {
            number = -number;
        }

        if (rad_streq(key, "volume")) {
            app->volume = number < 0 ? 0 : (number > 100 ? 100 : number);
            continue;
        }
        if (rad_streq(key, "tab")) {
            /* Which page was up. The clock app remembers which face survives a
               reboot for the same reason: coming back to where you were is
               what "the app was already open" ought to mean. */
            app->tab = (number > 0 && number < TAB_COUNT) ? (rad_tab_t)number
                                                          : TAB_NOW;
            continue;
        }
        for (int p = 0; p < P_COUNT; p++) {
            if (rad_streq(key, rad_ranges[p].key)) {
                app->params[p] = rad_clamp_param((rad_param_t)p, number);
                break;
            }
        }
    }
}

void rad_settings_save(rad_app_t *app)
{
    static char text[FILE_MAX];

    int at = snprintf(text, sizeof(text),
                      "# NeOS radio settings. Written by the app; safe to edit.\n"
                      "# Gains are tenths of a decibel, Q and slope hundredths,\n"
                      "# frequencies hertz.\n\n"
                      "volume = %d\ntab = %d\n",
                      app->volume, (int)app->tab);
    if (at < 0) {
        return;
    }

    for (int p = 0; p < P_COUNT; p++) {
        const int room = (int)sizeof(text) - at;
        if (room < 8) {
            break;
        }
        const int n = snprintf(text + at, (size_t)room, "%s = %d\n",
                               rad_ranges[p].key, app->params[p]);
        if (n <= 0 || n >= room) {
            break;
        }
        at += n;
    }
    (void)neos_file_write(SETTINGS_PATH, text, (size_t)at);
}
