/*
 * File role: compact demux rendering for the main monitor page.
 *
 * Draws the network/service summary row and the selectable "See demux details"
 * entry, including safe fitting/rotation of long service lists.
 */

#include "demux_view.h"

#include <curses.h>

#include "color.h"
#include "demux_monitor.h"

// Compute the printed width of one service item, including the first-list marker.
static int channel_name_width(int len, bool printed_service) {
  int prefix_len = printed_service ? 0 : 5;

  return prefix_len + len + 1;
}

// Print one service name and its trailing separator when it fits on the row.
static bool print_channel_name_if_fits(const char *service_name, int len, bool *printed_service) {
  int y, x;
  int row, col;
  getyx(stdscr, y, x);
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  int needed_len = channel_name_width(len, *printed_service);
  if (x + needed_len > col)
    return false;

  if (!*printed_service) {
    BLUE_BOLD_ON;
    printw(" -> |");
    BLUE_BOLD_OFF;
    *printed_service = true;
  }

  printw("%s", service_name);
  BLUE_BOLD_ON;
  printw("|");
  BLUE_BOLD_OFF;

  return true;
}

// Check whether the full service list can be rendered without rotation.
static bool channel_list_fits_row(const struct demux_snapshot *snapshot) {
  int y, x;
  int row, col;
  getyx(stdscr, y, x);
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  int required_len = 0;

  for (int service_index = 0; service_index < snapshot->service_count; service_index++) {
    int len = snapshot->services[service_index].name_len;
    if (len <= 0)
      continue;
    if (required_len == 0)
      required_len += 5;
    required_len += len + 1;
  }

  return required_len == 0 || x + required_len <= col;
}

// Estimate how many services fit into one row-filling slice from a start index.
static int channel_list_slice_capacity(const struct demux_snapshot *snapshot, int start_service) {
  int y, x;
  int row, col;
  getyx(stdscr, y, x);
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  bool counted_service = false;
  int current_x = x;
  int visible_count = 0;

  for (int offset = 0; offset < snapshot->service_count; offset++) {
    int service_index = (start_service + offset) % snapshot->service_count;
    int len = snapshot->services[service_index].name_len;
    if (len <= 0)
      continue;

    int needed_len = channel_name_width(len, counted_service);
    if (current_x + needed_len > col) {
      if (counted_service)
        break;

      continue;
    }

    current_x += needed_len;
    counted_service = true;
    visible_count++;
  }

  return visible_count > 0 ? visible_count : 1;
}

// Render one row-filling circular slice of the service list.
static void render_channel_list_slice(const struct demux_snapshot *snapshot, int start_service) {
  bool printed_service = false;

  WHITE_ON;
  for (int offset = 0; offset < snapshot->service_count; offset++) {
    int service_index = (start_service + offset) % snapshot->service_count;
    const struct demux_service_snapshot *service = &snapshot->services[service_index];
    if (service->name_len <= 0)
      continue;

    if (!print_channel_name_if_fits(service->name, service->name_len, &printed_service) && printed_service)
      break;
  }
  WHITE_OFF;
}

// Render the current network name and the visible circular slice of its service list.
int demux_main_info(struct dvb_data_s *dvb_data, unsigned int channel_offset_seed) {
  int row, col;
  getmaxyx(stdscr, row, col);
  (void)row;

  struct demux_snapshot snapshot;
  read_demux_snapshot(dvb_data, &snapshot);

  BLUE_BOLD_ON;
  if (snapshot.network_name_len == 0)
    printw("Wait for demux info ");
  else
    printw("%s", snapshot.network_name);
  BLUE_BOLD_OFF;

  int start_service = 0;
  if (snapshot.service_count > 0 && !channel_list_fits_row(&snapshot)) {
    int slice_capacity = channel_list_slice_capacity(&snapshot, 0);
    start_service = (channel_offset_seed * slice_capacity) % snapshot.service_count;
  }

  if (snapshot.service_count > 0)
    render_channel_list_slice(&snapshot, start_service);

  hline(' ', col);

  return 1;
}

// Reserve the row that will become the demux/service detail entry point.
int detail_line(unsigned int frontend_index, bool selected) {
  int row, col;
  getmaxyx(stdscr, row, col);
  (void)row;

  attron(A_UNDERLINE);
  if (selected) {
    attron(A_REVERSE);
    attron(A_BOLD);
    printw(">");
    attroff(A_BOLD);
  } else {
    attron(A_REVERSE);
    printw("%u", frontend_index);
    attroff(A_REVERSE);
  }

  WHITE_BOLD_ON;
  printw(" See demux details ...");
  hline(' ', col);
  WHITE_BOLD_OFF;
  attroff(A_UNDERLINE);

  if (selected)
    attroff(A_REVERSE);

  return 1;
}
