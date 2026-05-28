#include "demux_view.h"

#include <curses.h>

#include "color.h"
#include "demux_monitor.h"

// Print one service name and its trailing separator when it fits on the row.
static bool print_channel_name_if_fits(const char *service_name, int len, bool *printed_service) {
  int y, x;
  int row, col;
  getyx(stdscr, y, x);
  getmaxyx(stdscr, row, col);
  (void)y;
  (void)row;

  int prefix_len = *printed_service ? 0 : 5;
  int needed_len = prefix_len + len + 1;
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

// Render the current network name and the visible slice of its service list.
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

  int start_section = 0;
  if (snapshot.service_count > 0 && !channel_list_fits_row(&snapshot))
    start_section = channel_offset_seed % snapshot.service_count;
  bool printed_service = false;
  WHITE_ON;

  for (int service_index = start_section; service_index < snapshot.service_count; service_index++) {
    const struct demux_service_snapshot *service = &snapshot.services[service_index];
    if (service->name_len <= 0)
      continue;
    if (!print_channel_name_if_fits(service->name, service->name_len, &printed_service) && printed_service)
      break;
  }
  WHITE_OFF;

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
