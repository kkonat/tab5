/*
 * wifiscan - the 2.4 GHz band, drawn.
 *
 * A list of networks sorted by signal is the wrong picture for the question
 * people actually have, which is "where is there room". Channels overlap: a
 * 802.11 carrier is 22 MHz wide and the channels are 5 MHz apart, so a network
 * on 6 is sitting on top of a network on 8 and next to nothing on 1. That is a
 * fact about a spectrum and it only reads as a spectrum - bells on a frequency
 * axis, overlapping where the radios overlap.
 *
 * What this tablet can see is the limit on all of it. The P4 has no radio; the
 * ESP32-C6 over SDIO does, and it is 2.4 GHz only, so there is no 5 GHz band
 * here to select between - the band selector zooms instead, which is what the
 * width is actually for. And NeOS collapses a scan to one entry per name, so
 * this draws one bell per *network* and not one per radio: a mesh with three
 * access points is one bell, on whichever channel its strongest radio is on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"
#include "neos_api.h"
#include "neos_net.h"
#include "neos_sys.h"

/** The most networks NeOS will report. One per name, not one per radio. */
#define WS_MAX NEOS_NET_SCAN_MAX

/*
 * How much of the band is on screen.
 *
 * Not a band selector in the 2.4/5 sense - see the file header - but the same
 * control in the place where it earns something. The whole band on a 1280 px
 * landscape screen is about 60 px per channel, which is room for one vertical
 * name per channel; half the band is room for three. So the zoom is what makes
 * the names readable in a crowded flat, and ALL is what makes the shape of the
 * whole band readable at once.
 */
typedef enum {
    WS_SPAN_LOW = 0,   /**< channels 1-7  */
    WS_SPAN_HIGH,      /**< channels 7-13 */
    WS_SPAN_ALL,       /**< 1-13, or 1-14 if anything is up there */
    WS_SPAN_COUNT,
} ws_span_t;

typedef struct {
    char        ssid[33];
    int8_t      rssi;      /**< dBm, negative */
    uint8_t     chan;
    bool        secure;
    ngl_color_t colour;    /**< assigned once and kept - see ws_model.c */
} ws_ap_t;

typedef struct {
    ws_ap_t  ap[WS_MAX];
    int      n;          /**< how many we hold */
    int      found;      /**< how many the scan found, which may exceed n */
    bool     scanning;   /**< a scan is in the air right now */
    bool     ever;       /**< at least one scan has come back */
    uint32_t rev;        /**< bumped whenever the contents change */
    uint32_t next_ms;    /**< when to ask for the next scan */
} ws_model_t;

/** Milliseconds since boot, narrowed - nothing here measures days. */
static inline uint32_t ws_now(void) { return (uint32_t)neos_uptime_ms(); }

/**
 * The centre of channel @p ch, in MHz.
 *
 * 2407 + 5n for 1-13. Channel 14 is the exception in the standard as well as
 * here: it is 12 MHz above 13 rather than 5, it is Japan only, and it is DSSS
 * only - so a bell on it is wider than the one this draws. It is here at all
 * because a scan that reports it and a plot that cannot place it would be a
 * plot that quietly lies.
 */
static inline int ws_chan_mhz(int ch) { return ch == 14 ? 2484 : 2407 + 5 * ch; }

/* ws_model.c */
void ws_model_init(ws_model_t *m);
void ws_model_pump(ws_model_t *m);

/* ws_plot.c */
void ws_ui_init(void);
void ws_ui_repaint(void);
void ws_ui_tick(const ws_model_t *m);
