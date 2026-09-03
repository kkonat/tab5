#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "neos_cfg.h"

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

bool neos_cfg_get(const char *path, const char *key, char *out, size_t out_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[160];
    bool found = false;

    while (!found && fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;

        const char *k = trim(line);
        const char *v = trim(eq + 1);
        if (strcasecmp(k, key) == 0 && *v) {
            strlcpy(out, v, out_sz);
            found = true;
        }
    }

    fclose(f);
    return found;
}
