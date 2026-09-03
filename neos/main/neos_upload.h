/*
 * Receiving files over the console. See neos_upload.c for the wire format.
 */
#pragma once

#include "esp_err.h"

/** Claim the console's receive half and start listening. Call once at boot. */
esp_err_t neos_upload_init(void);
