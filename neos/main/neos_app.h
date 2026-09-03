/*
 * Loading and running an app off the card.
 *
 * The manifest format and the ELF execution path are OS concerns, not the
 * frontend's: anything that launches an app goes through here, so the crash
 * breadcrumb is written in exactly one place.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Read <dir>/manifest.json. Fills @p name and @p entry, and @p desc if the
 * manifest has one. False if it is missing, unparseable, or incomplete.
 */
bool neos_app_manifest(const char *dir, char *name, size_t name_sz,
                       char *entry, size_t entry_sz,
                       char *desc, size_t desc_sz);

/**
 * Load and run one app, leaving a crash breadcrumb around the call.
 * @p slot is passed to the app as argv[1]. True if it returned cleanly.
 */
bool neos_app_run(const char *appdir, const char *dirname,
                  const char *name, const char *entry, int slot);
