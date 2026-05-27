#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <curses.h>

#include "command_line.h"
#include "color.h"
#include "demux_monitor.h"
#include "demux_view.h"
#include "device_discovery.h"
#include "frontend_view.h"
#include "ui_helpers.h"

#define DSFEMON_VERSION "v0.71.2026"
#define DSFEMON_TITLE "dsfemon - DVB Frontend monitor, originally developed by David Seidl"
// Number of monitor refreshes between automatic rotations of long service lists.
#define CHANNEL_ROTATION_REFRESHES 8
// Lines used by one complete frontend block: frontend rows, demux row, detail row, and separator.
#define FRONTEND_BLOCK_LINES 9
// The first row is reserved for program/version and page status.
#define HEADER_BAR_LINES 1
// Keep the first frontend visually separated from the header bar.
#define HEADER_GAP_LINES 1
// The last row is reserved for keyboard help.
#define FOOTER_BAR_LINES 1
// Main monitor refresh cadence.
#define REFRESH_INTERVAL_US 500000

static volatile sig_atomic_t g_stop_requested = 0;

// Async-signal-safe stop request consumed by the main loop.
static void request_stop(int signal_number) {
  (void)signal_number;
  g_stop_requested = 1;
}

// atexit fallback so the terminal returns to normal even on early exits.
static void close_curses(void) {
  endwin();
}

// Keep keyboard handling small and compatible with top-like behavior.
static bool quit_key(int key) {
  return key == 'q' || key == 'Q';
}

// Move to the previous page using common terminal navigation keys.
static bool previous_page_key(int key) {
  return key == KEY_PPAGE || key == KEY_UP;
}

// Move to the next page using common terminal navigation keys.
static bool next_page_key(int key) {
  return key == KEY_NPAGE || key == KEY_DOWN;
}

// Count opened DVB frontends that match the current scan selection.
static unsigned int count_frontends(struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config) {
  unsigned int frontend_count = 0;

  for (int adapter = scan_config->min_adapter; adapter < scan_config->max_adapter; adapter++) {
    if (!dvb_scan_adapter_enabled(scan_config, adapter))
      continue;

    for (int subadapter = 0; subadapter < scan_config->max_subadapter; subadapter++) {
      struct dvb_data_s *current_dvb_data = &dvb_data[dvb_device_index(adapter, subadapter, scan_config->max_subadapter)];
      if (current_dvb_data->fefd >= 0)
        frontend_count++;
    }
  }

  return frontend_count;
}

// Compute how many complete frontend blocks fit between header and footer.
static unsigned int frontends_per_page(int terminal_rows) {
  int available_rows = terminal_rows - HEADER_BAR_LINES - HEADER_GAP_LINES - FOOTER_BAR_LINES;

  if (available_rows <= 0)
    return 0;

  return available_rows / FRONTEND_BLOCK_LINES;
}

// Convert frontend count and page capacity into a user-visible page count.
static unsigned int count_pages(unsigned int frontend_count, unsigned int page_capacity) {
  if (frontend_count == 0 || page_capacity == 0)
    return 1;

  return (frontend_count + page_capacity - 1) / page_capacity;
}

// Keep page index valid after terminal resize or device-count changes.
static unsigned int clamp_page(unsigned int current_page, unsigned int page_count) {
  if (page_count == 0 || current_page < page_count)
    return current_page;

  return page_count - 1;
}

// Render one frontend block: frontend rows, demux summary, and detail placeholder.
static unsigned int render_frontend(struct dvb_data_s *dvb_data, int adapter, int subadapter, unsigned int card_count, unsigned int line, unsigned int refresh_cycle) {
  char fedev[128];

  format_frontend_path(fedev, sizeof(fedev), adapter, subadapter);

  line = render_frontend_status_lines(dvb_data->fefd, fedev, line);
  move(line, 0);
  line += demux_main_info(dvb_data, (refresh_cycle / CHANNEL_ROTATION_REFRESHES) + card_count);
  move(line, 0);
  line += detail_line(card_count, dvb_data);
  move(line, 0);
  line += full_line();

  return line;
}

// Render only complete frontend blocks that belong to the requested page.
static unsigned int render_frontend_page(struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, unsigned int page, unsigned int page_capacity, unsigned int first_line, unsigned int refresh_cycle) {
  unsigned int line = first_line;
  unsigned int frontend_index = 0;
  unsigned int first_frontend = page * page_capacity;
  unsigned int last_frontend = first_frontend + page_capacity;

  if (page_capacity == 0)
    return line;

  for (int adapter = scan_config->min_adapter; adapter < scan_config->max_adapter; adapter++) {
    if (!dvb_scan_adapter_enabled(scan_config, adapter))
      continue;

    for (int subadapter = 0; subadapter < scan_config->max_subadapter; subadapter++) {
      struct dvb_data_s *current_dvb_data = &dvb_data[dvb_device_index(adapter, subadapter, scan_config->max_subadapter)];
      if (current_dvb_data->fefd < 0)
        continue;

      if (frontend_index >= first_frontend && frontend_index < last_frontend)
        line = render_frontend(current_dvb_data, adapter, subadapter, frontend_index, line, refresh_cycle);

      frontend_index++;
    }
  }

  return line;
}

// Render the right-aligned header counters with highlighted numeric values.
static void render_header_status(int header_row, int col, const char *left_text, unsigned int current_page, unsigned int page_count, unsigned int frontend_count) {
  char status_text[128];

  snprintf(status_text, sizeof(status_text), " frontends: %u | page %u/%u ",
           frontend_count,
           current_page + 1,
           page_count);

  int status_len = strlen(status_text);
  int status_col = col > status_len ? col - status_len : 0;
  if (status_col <= (int)strlen(left_text))
    return;

  move(header_row, status_col);
  printw(" frontends: ");
  REVERSE_RED_ON;
  printw("%u", frontend_count);
  REVERSE_RED_OFF;
  printw(" | page ");
  REVERSE_RED_ON;
  printw("%u", current_page + 1);
  REVERSE_RED_OFF;
  printw("/");
  REVERSE_RED_ON;
  printw("%u", page_count);
  REVERSE_RED_OFF;
  printw(" ");
}

// Show program identity on the left and page/device status on the right.
static void render_header_bar(int header_row, int col, unsigned int current_page, unsigned int page_count, unsigned int frontend_count) {
  if (col <= 0)
    return;

  char left_text[128];

  snprintf(left_text, sizeof(left_text), " %s ", DSFEMON_TITLE);

  attron(A_REVERSE);
  mvhline(header_row, 0, ' ', col);
  mvaddnstr(header_row, 0, left_text, col);
  render_header_status(header_row, col, left_text, current_page, page_count, frontend_count);

  attroff(A_REVERSE);
}

// Keep the footer help text in one place so the spinner can avoid overlapping it.
static const char *footer_help_text(void) {
  return " PgUp/PgDn page | Up/Down page | q quit ";
}

// Return the right-aligned footer indicator column, or -1 when the row is tight.
static int footer_indicator_column(int col) {
  if (col <= 0)
    return -1;

  int indicator_len = strlen(" " DSFEMON_VERSION " [/] ");
  int indicator_col = col > indicator_len ? col - indicator_len : -1;

  if (indicator_col <= (int)strlen(footer_help_text()))
    return -1;

  return indicator_col;
}

// Show compact bottom keyboard help plus a refresh spinner on the right.
static void render_help_bar(int footer_row, int col, unsigned int refresh_cycle) {
  if (col <= 0)
    return;

  static const char spinner[] = "|/-\\";
  const char *help_text = footer_help_text();

  attron(A_REVERSE);
  mvhline(footer_row, 0, ' ', col);
  mvaddnstr(footer_row, 0, help_text, col);

  int indicator_col = footer_indicator_column(col);
  if (indicator_col >= 0) {
    move(footer_row, indicator_col);
    printw(" %s [", DSFEMON_VERSION);
    REVERSE_RED_ON;
    printw("%c", spinner[refresh_cycle % 4]);
    REVERSE_RED_OFF;
    printw("] ");
  }

  attroff(A_REVERSE);
  refresh();
}

// Explain why a page with discovered frontends cannot render any complete block.
static unsigned int render_terminal_too_small_line(unsigned int line) {
  move(line, 0);
  RED_BOLD_ON;
  printw("Terminal window is too small to display one complete frontend block.");
  RED_BOLD_OFF;
  full_line();

  return line + 1;
}

// Apply one keyboard action to the running/page state.
static void handle_key(int key, bool *running, unsigned int *current_page, unsigned int page_count) {
  if (quit_key(key)) {
    *running = false;

    return;
  }

  if (next_page_key(key) && *current_page + 1 < page_count) {
    (*current_page)++;

    return;
  }

  if (previous_page_key(key) && *current_page > 0)
    (*current_page)--;
}

// Program entry point: initialize ncurses, discover devices, render until quit.
int main(int argc, char **argv) {
  struct command_line_options options;
  char command_line_error[160];

  init_command_line_options(&options);

  if (parse_command_line(argc, argv, &options, command_line_error, sizeof(command_line_error)) != 0) {
    fprintf(stderr, "%s\n\n", command_line_error);
    print_usage(stderr, argv[0]);

    return 1;
  }

  if (options.action == COMMAND_LINE_HELP) {
    print_usage(stdout, argv[0]);

    return 0;
  }

  if (options.action == COMMAND_LINE_VERSION) {
    printf("dsfemon %s\n", DSFEMON_VERSION);

    return 0;
  }

  atexit(close_curses);
  signal(SIGINT, request_stop);
  signal(SIGTERM, request_stop);

  initscr();
  noecho();
  cbreak();
  keypad(stdscr, TRUE);
  curs_set(0);

  if (has_colors()) {
    start_color();
    set_my_color();
  }

  struct dvb_scan_config scan_config = options.scan_config;

  struct dvb_data_s *dvb_data = (dvb_data_s *)calloc(DVB_DEVICE_COUNT, sizeof(*dvb_data));

  if (dvb_data == NULL) {
    endwin();
    fprintf(stderr, "Cannot allocate DVB device state\n");
    return 1;
  }

  init_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
  discover_dvb_devices(dvb_data, DVB_DEVICE_COUNT, &scan_config);

  bool running = true;
  unsigned int refresh_cycle = 0;
  unsigned int current_page = 0;

  while (running) {
    if (g_stop_requested)
      running = false;
    int key;

    int row, col;
    getmaxyx(stdscr, row, col);

    unsigned int frontend_count = count_frontends(dvb_data, &scan_config);
    unsigned int page_capacity = frontends_per_page(row);
    unsigned int page_count = count_pages(frontend_count, page_capacity);
    current_page = clamp_page(current_page, page_count);
    unsigned int line = HEADER_BAR_LINES + HEADER_GAP_LINES;
    int footer_row = row > 0 ? row - 1 : 0;

    if (row > 0) {
      render_header_bar(0, col, current_page, page_count, frontend_count);

      if (row > HEADER_BAR_LINES) {
        move(HEADER_BAR_LINES, 0);
        full_line();
      }
    }

    if (line < (unsigned int)footer_row) {
      if (frontend_count == 0) {
        move(line, 0);
        line += no_devices_line(&scan_config);
      } else if (page_capacity == 0) {
        line = render_terminal_too_small_line(line);
      } else {
        line = render_frontend_page(dvb_data, &scan_config, current_page, page_capacity, line, refresh_cycle);
      }
    }

    for (int i = line; i < footer_row; i++) {
      move(i, 0);
      full_line();
    }

    if (row > HEADER_BAR_LINES)
      render_help_bar(footer_row, col, refresh_cycle);

    timeout(0);
    key = getch();
    handle_key(key, &running, &current_page, page_count);

    usleep(REFRESH_INTERVAL_US);
    refresh_cycle++;
    if (g_stop_requested)
      running = false;
  }

  cleanup_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
  free(dvb_data);
  curs_set(1);
  endwin();

  return 0;
}
