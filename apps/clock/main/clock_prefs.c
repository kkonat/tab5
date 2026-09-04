/*
 * Remembering what was chosen.
 *
 * Two things now - which face is showing and which city the place band was
 * pointed at - in one file on the card, in the same `key = value` shape as
 * autorun.cfg and for the same reason: it is on a card that will end up in a
 * machine with nothing but Notepad, and a missing brace should not be the
 * difference between a clock that starts and one that does not. Anything this
 * cannot parse is treated as absent, so the worst a corrupted file can do is
 * hand back the defaults.
 *
 * Both values are keys rather than indices. The tables in clock_app.c and
 * clock_city.c will be reordered eventually - a face added in the middle, two
 * towns swapped - and a stored 3 would then quietly become a different clock
 * in a different city; a stored "lcd" and "krakow" would not.
 *
 * The whole file is read once and rewritten whole, which is what keeps the two
 * settings from having to know about each other: neither save has to preserve
 * the other's line, because both lines are always written from the same pair
 * of strings.
 *
 * Nothing is checked hard, because this is a clock. A card that is not there,
 * or is write-protected, or was pulled between the tap and the save, costs the
 * user a preference they can set again in two taps.
 */
#include <stdio.h>
#include <string.h>

#include "clock.h"

#define PREFS_PATH "apps/clock/clock.cfg"

/* Comfortably past anything this writes; a file bigger than it is refused by
   neos_file_read() rather than truncated, which is what we want. */
#define PREFS_MAX  256

#define KEY_FACE "mode"
#define KEY_CITY "city"

static char s_face[24];
static char s_city[24];
static bool s_loaded;

/** Trim spaces, tabs and a stray carriage return off both ends, in place. */
static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        *--e = 0;
    }
    return p;
}

static void load(void)
{
    char buf[PREFS_MAX + 1];

    if (s_loaded) {
        return;
    }
    s_loaded = true;

    const int n = neos_file_read(PREFS_PATH, buf, PREFS_MAX);
    if (n <= 0) {
        /* -3 is "no such file", which is every first run. */
        return;
    }
    buf[n] = 0;

    /*
     * A line at a time rather than searching the whole buffer for a key, so
     * that a key inside a comment does not count.
     */
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = 0;
        }

        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *k = trim(line);
            const char *v = trim(eq + 1);

            if (*v) {
                if (strcmp(k, KEY_FACE) == 0) {
                    snprintf(s_face, sizeof(s_face), "%s", v);
                } else if (strcmp(k, KEY_CITY) == 0) {
                    snprintf(s_city, sizeof(s_city), "%s", v);
                }
            }
        }

        line = nl ? nl + 1 : 0;
    }
}

const char *clk_prefs_load(void)
{
    load();
    return s_face[0] ? s_face : 0;
}

const char *clk_prefs_city(void)
{
    load();
    return s_city[0] ? s_city : 0;
}

static void store(void)
{
    char out[PREFS_MAX];
    const int n = snprintf(out, sizeof(out),
                           "# NeOS clock. Written by the app; safe to edit.\n"
                           "# One of: neos, led-green, led-red, lcd, vfd, epaper\n"
                           "%s = %s\n"
                           "# Where the place band says it is. Delete the line\n"
                           "# to go back to wherever the network thinks we are.\n"
                           "%s = %s\n",
                           KEY_FACE, s_face, KEY_CITY, s_city);
    if (n <= 0 || n >= (int)sizeof(out)) {
        return;
    }

    const int err = neos_file_write(PREFS_PATH, out, (size_t)n);
    if (err != 0) {
        printf("[clock] could not save settings (%d) - carrying on\n", err);
    }
}

void clk_prefs_save_face(const char *key)
{
    if (!key || !*key) {
        return;
    }
    load();
    snprintf(s_face, sizeof(s_face), "%s", key);
    store();
}

void clk_prefs_save_city(const char *key)
{
    load();
    snprintf(s_city, sizeof(s_city), "%s", key ? key : "");
    store();
}
