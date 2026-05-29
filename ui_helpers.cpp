/*
 * File role: shared ncurses UI helpers.
 *
 * Contains common row-clearing, bounded field-printing, bar-width, and
 * no-device diagnostic helpers used across the monitor views.
 */

#include "ui_helpers.h"

#include <curses.h>
#include <string.h>

#include "color.h"
#include "ncurses_present.h"

// Clear the rest of the current row so shorter redraws do not leave stale text.
int full_line(void) {
  int x, y;
  getyx(stdscr, y, x);

  int row, col;
  getmaxyx(stdscr, row, col);
  (void)row;

  for (int i = x; i < col; i++)
    mvaddch(y, i, ' ');

  return 1;
}

// Compute remaining bar width from the current cursor position.
int remaining_bar_width(void) {
  int x, y;
  getyx(stdscr, y, x);

  int row, col;
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  // Leave one trailing cell for full_line(), otherwise it can erase the closing bracket.
  int len = col - x - 1;

  return len >= 7 ? len : 0;
}

// Keep compact DVBv5 fields from overflowing into the terminal edge.
bool print_field_if_fits(const char *label, const char *value) {
  int y, x;
  int row, col;
  getyx(stdscr, y, x);
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  int needed_len = strlen(label) + 1 + strlen(value) + 1;
  if (x + needed_len > col)
    return false;

  DESC_COL_ON;
  printw("%s ", label);
  DESC_COL_OFF;
  DESC_VALL_ON;
  printw("%s ", value);
  DESC_VALL_OFF;

  return true;
}

// Show the configured adapter selection when discovery finds no frontends.
int no_devices_line(const struct dvb_scan_config *config) {
  int row, col;
  char adapter_selection[128];
  const char *selection_label = config->adapter_filter_enabled ? "selected adapters " : "configured adapter range ";

  getmaxyx(stdscr, row, col);
  (void)row;
  format_scan_adapter_selection(config, adapter_selection, sizeof(adapter_selection));

  RED_BOLD_ON;
  printw("No DVB frontend devices found in ");
  printw("%s", selection_label);
  RED_BOLD_OFF;
  DESC_VALL_ON;
  printw("%s", adapter_selection);
  DESC_VALL_OFF;
  printw(". Check /dev/dvb, permissions, or adapter scan settings.");
  hline(' ', col);

  return 1;
}
