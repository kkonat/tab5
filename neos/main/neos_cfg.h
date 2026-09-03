/*
 * autorun.cfg - what the card wants NeOS to run.
 *
 * Deliberately not JSON. This file is edited by hand, on a card, often on a
 * machine that has nothing but Notepad, and a missing brace should not be the
 * difference between a tablet that boots and one that does not.
 *
 *     # NeOS autorun
 *     app = launcher
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Read one `key = value` line out of @p path.
 *
 * Blank lines and everything after a '#' are ignored, keys are matched
 * case-insensitively, and surrounding whitespace is stripped from both sides.
 * False if the file is unreadable or the key is absent or empty.
 */
bool neos_cfg_get(const char *path, const char *key, char *out, size_t out_sz);
