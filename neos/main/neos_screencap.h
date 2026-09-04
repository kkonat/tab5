/*
 * Sending the screen to the host. See neos_screencap.c for the wire format.
 */
#pragma once

/**
 * Encode the screen and write it to the host.
 *
 * @p arg is what followed the verb on the console line: empty for the whole
 * screen, or "<first> <count>" when the host is asking again for rows that
 * did not survive sharing the link with something else's output.
 *
 * Called from the console receive task, which it blocks for the transfer.
 */
void neos_screencap_send(const char *arg);
