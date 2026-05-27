#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include <stdbool.h>

#include "device_discovery.h"

// Clear from the current cursor position to the end of the row and refresh.
int full_line(void);

// Return the available width for a signal bar, reserving its closing bracket.
int remaining_bar_width(void);

// Print a label/value pair only when it fits on the current terminal row.
bool print_field_if_fits(const char *label, const char *value);

// Render the startup diagnostic shown when no frontend devices are found.
int no_devices_line(const struct dvb_scan_config *config);

#endif
