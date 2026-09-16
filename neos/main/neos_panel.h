/*
 * The panels themselves.
 *
 * Each runs to completion on the ui task with the screen and the finger
 * already taken - see neos_ui.c - and returns when the user closes it. A panel
 * is therefore written as a small blocking program with its own event loop,
 * which is the same shape an app is written in, and deliberately so: there is
 * one way to write a screen on this machine.
 */
#pragma once

/** Pick a network, type its password, remember it. */
void neos_panel_wifi(void);

/** The date, the time, the month, and which zone all of that is in. */
void neos_panel_clock(void);

/** The main-rail reading behind the bar's battery icon, in full. */
void neos_panel_battery(void);
