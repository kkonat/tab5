/*
 * The system keyboard, core side. The app-facing call is neos_input_text().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Run the keyboard to completion on the calling task.
 *
 * Takes the screen and the finger itself, so it can be called from an app - as
 * neos_input_text() does - or from inside a panel that already holds both, as
 * the Wi-Fi list does when it asks for a password. Overlays and touch capture
 * both nest, which is what makes the second case work: the list gets its own
 * screen back when this returns, not the app's.
 *
 * @return true if the user pressed Enter, false if they cancelled. On false
 *         @p buf is untouched.
 */
bool neos_kbd_run(const char *title, char *buf, size_t size, uint32_t flags);
