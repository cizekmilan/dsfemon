#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
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
#define DSFEMON_TITLE "dsfemon - DVB frontend monitor, originally developed by David Seidl"
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
// Keyboard polling cadence. Lower than DVB refresh so navigation feels immediate.
#define INPUT_POLL_INTERVAL_US 25000
// Keep standalone Esc responsive while still allowing arrow-key escape sequences.
#define ESC_KEY_DELAY_MS 50

static volatile sig_atomic_t g_stop_requested = 0;

// Mutable UI state shared by rendering and keyboard handling in the main thread.
struct screen_state {
  bool detail_open;
  bool redraw_requested;
  unsigned int refresh_cycle;
  unsigned int current_page;
  unsigned int selected_frontend;
  unsigned int frontend_count;
  unsigned int page_capacity;
  unsigned int page_count;
  unsigned int detail_selected_service;
  unsigned int detail_scroll_offset;
  unsigned int detail_service_count;
  unsigned int detail_page_capacity;
};

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
  return key == KEY_PPAGE;
}

// Move to the next page using common terminal navigation keys.
static bool next_page_key(int key) {
  return key == KEY_NPAGE;
}

// Move the selected frontend row inside/across monitor pages.
static bool previous_frontend_key(int key) {
  return key == KEY_UP;
}

// Move the selected frontend row inside/across monitor pages.
static bool next_frontend_key(int key) {
  return key == KEY_DOWN;
}

// Open the selected frontend's demux detail view.
static bool enter_key(int key) {
  return key == '\n' || key == '\r' || key == KEY_ENTER;
}

// Leave a temporary detail screen without quitting the whole program.
static bool detail_back_key(int key) {
  return key == 27 || key == KEY_BACKSPACE;
}

// Monotonic microsecond clock used to decouple input polling from DVB redraws.
static unsigned long long monotonic_time_us(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return (unsigned long long)now.tv_sec * 1000000 + (unsigned long long)now.tv_nsec / 1000;
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

// Keep the selected frontend index valid when devices or filters change.
static unsigned int clamp_selected_frontend(unsigned int selected_frontend, unsigned int frontend_count) {
  if (frontend_count == 0)
    return 0;

  if (selected_frontend < frontend_count)
    return selected_frontend;

  return frontend_count - 1;
}

// Small local helper for range calculations without pulling extra C++ headers.
static unsigned int min_uint(unsigned int a, unsigned int b) {
  return a < b ? a : b;
}

// Align the current page so the selected frontend's detail row is visible.
static unsigned int page_for_selected_frontend(unsigned int selected_frontend, unsigned int page_capacity) {
  if (page_capacity == 0)
    return 0;

  return selected_frontend / page_capacity;
}

// Translate DVB SDT running_status values into compact table labels.
static const char *running_status_name(int running_status) {
  switch (running_status) {
    case 1:
      return "stopped";
    case 2:
      return "starts";
    case 3:
      return "pausing";
    case 4:
      return "running";
    case 5:
      return "off-air";
    default:
      return "unknown";
  }
}

// Keep the selected service and scroll offset valid for the current detail table.
static void clamp_detail_service_selection(struct screen_state *state) {
  if (state->detail_service_count == 0) {
    state->detail_selected_service = 0;
    state->detail_scroll_offset = 0;

    return;
  }

  if (state->detail_selected_service >= state->detail_service_count)
    state->detail_selected_service = state->detail_service_count - 1;

  if (state->detail_page_capacity == 0) {
    state->detail_scroll_offset = state->detail_selected_service;

    return;
  }

  if (state->detail_selected_service < state->detail_scroll_offset)
    state->detail_scroll_offset = state->detail_selected_service;

  if (state->detail_selected_service >= state->detail_scroll_offset + state->detail_page_capacity)
    state->detail_scroll_offset = state->detail_selected_service - state->detail_page_capacity + 1;

  unsigned int max_scroll_offset = state->detail_service_count > state->detail_page_capacity ? state->detail_service_count - state->detail_page_capacity : 0;
  if (state->detail_scroll_offset > max_scroll_offset)
    state->detail_scroll_offset = max_scroll_offset;
}

// Reset service-table navigation when entering a new detail screen.
static void reset_detail_service_selection(struct screen_state *state) {
  state->detail_selected_service = 0;
  state->detail_scroll_offset = 0;
  state->detail_service_count = 0;
  state->detail_page_capacity = 0;
}

// Build the initial monitor state without relying on a long positional initializer.
static struct screen_state init_screen_state(void) {
  struct screen_state state;

  memset(&state, 0, sizeof(state));
  state.redraw_requested = true;
  state.page_count = 1;

  return state;
}

// Render one frontend block: frontend rows, demux summary, and detail placeholder.
static unsigned int render_frontend(struct dvb_data_s *dvb_data, int adapter, int subadapter, unsigned int card_count, unsigned int selected_frontend, unsigned int line, unsigned int refresh_cycle) {
  char fedev[128];

  format_frontend_path(fedev, sizeof(fedev), adapter, subadapter);

  line = render_frontend_status_lines(dvb_data->fefd, fedev, line);
  move(line, 0);
  line += demux_main_info(dvb_data, (refresh_cycle / CHANNEL_ROTATION_REFRESHES) + card_count);
  move(line, 0);
  line += detail_line(card_count, card_count == selected_frontend);
  move(line, 0);
  line += full_line();

  return line;
}

// Render only complete frontend blocks that belong to the requested page.
static unsigned int render_frontend_page(struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, unsigned int page, unsigned int page_capacity, unsigned int selected_frontend, unsigned int first_line, unsigned int refresh_cycle) {
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
        line = render_frontend(current_dvb_data, adapter, subadapter, frontend_index, selected_frontend, line, refresh_cycle);

      frontend_index++;
    }
  }

  return line;
}

// Find the Nth discovered frontend in the same order used by the monitor view.
static struct dvb_data_s *frontend_by_index(struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, unsigned int selected_frontend) {
  unsigned int frontend_index = 0;

  for (int adapter = scan_config->min_adapter; adapter < scan_config->max_adapter; adapter++) {
    if (!dvb_scan_adapter_enabled(scan_config, adapter))
      continue;

    for (int subadapter = 0; subadapter < scan_config->max_subadapter; subadapter++) {
      struct dvb_data_s *current_dvb_data = &dvb_data[dvb_device_index(adapter, subadapter, scan_config->max_subadapter)];
      if (current_dvb_data->fefd < 0)
        continue;

      if (frontend_index == selected_frontend)
        return current_dvb_data;

      frontend_index++;
    }
  }

  return NULL;
}

// Print one service table row, clipping the service name to the terminal width.
static void render_service_table_row(unsigned int line, int col, unsigned int service_index, const struct demux_service_snapshot *service, bool selected) {
  char service_id_text[16];
  char program_pid_text[16];
  char row_text[DEMUX_SERVICE_NAME_SIZE + 80];

  if (service->service_id >= 0)
    snprintf(service_id_text, sizeof(service_id_text), "%d", service->service_id);
  else
    snprintf(service_id_text, sizeof(service_id_text), "-");

  if (service->program_pid >= 0)
    snprintf(program_pid_text, sizeof(program_pid_text), "%d", service->program_pid);
  else
    snprintf(program_pid_text, sizeof(program_pid_text), "-");

  snprintf(row_text, sizeof(row_text), "%3u  %-5s  %-7s  %-3s %-8s  %s",
           service_index + 1,
           service_id_text,
           program_pid_text,
           service->free_ca_mode ? "yes" : "no",
           running_status_name(service->running_status),
           service->name);

  if (selected)
    attron(A_REVERSE);

  mvhline(line, 0, ' ', col);
  if (col > 0)
    mvaddnstr(line, 0, row_text, col);

  if (selected)
    attroff(A_REVERSE);
}

// Render the current service table viewport for the selected frontend.
static unsigned int render_service_table(struct screen_state *state, const struct demux_snapshot *snapshot, unsigned int line, int footer_row) {
  int row, col;
  getmaxyx(stdscr, row, col);
  (void)row;

  if (line >= (unsigned int)footer_row)
    return line;

  int table_rows = footer_row - (int)line - 2;
  state->detail_service_count = snapshot->service_count;
  state->detail_page_capacity = table_rows > 0 ? table_rows : 0;
  clamp_detail_service_selection(state);

  move(line++, 0);
  printw("Services: %d", snapshot->service_count);
  if (snapshot->service_count > 0 && state->detail_page_capacity > 0) {
    unsigned int first_visible = state->detail_scroll_offset + 1;
    unsigned int last_visible = min_uint(state->detail_scroll_offset + state->detail_page_capacity, state->detail_service_count);
    printw(" | showing %u-%u", first_visible, last_visible);
  }
  full_line();

  if (snapshot->service_count == 0) {
    move(line++, 0);
    WHITE_BOLD_ON;
    printw("Waiting for named SDT services.");
    WHITE_BOLD_OFF;
    full_line();

    return line;
  }

  if (line >= (unsigned int)footer_row)
    return line;

  attron(A_REVERSE);
  mvhline(line, 0, ' ', col);
  mvaddnstr(line, 0, " No   SID    PMT PID  CA  Status    Service", col);
  attroff(A_REVERSE);
  line++;

  if (state->detail_page_capacity == 0)
    return line;

  unsigned int first_service = state->detail_scroll_offset;
  unsigned int last_service = min_uint(first_service + state->detail_page_capacity, state->detail_service_count);

  for (unsigned int service_index = first_service; service_index < last_service && line < (unsigned int)footer_row; service_index++) {
    const struct demux_service_snapshot *service = &snapshot->services[service_index];
    render_service_table_row(line, col, service_index, service, service_index == state->detail_selected_service);
    line++;
  }

  return line;
}

// Render the fullscreen demux detail view for the selected frontend.
static unsigned int render_demux_detail(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, unsigned int line, int footer_row) {
  char fedev[128];
  char dedev[128];
  struct demux_snapshot snapshot;
  struct dvb_data_s *selected_dvb_data = frontend_by_index(dvb_data, scan_config, state->selected_frontend);

  if (selected_dvb_data == NULL) {
    move(line, 0);
    RED_BOLD_ON;
    printw("Selected frontend is not available.");
    RED_BOLD_OFF;
    full_line();

    return line + 1;
  }

  format_frontend_path(fedev, sizeof(fedev), selected_dvb_data->adapter, selected_dvb_data->subadapter);
  format_demux_path(dedev, sizeof(dedev), selected_dvb_data->adapter, selected_dvb_data->subadapter);
  read_demux_snapshot(selected_dvb_data, &snapshot);

  move(line++, 0);
  CYAN_BOLD_ON;
  printw("Demux detail");
  CYAN_BOLD_OFF;
  printw(" for frontend %u/%u", state->selected_frontend + 1, state->frontend_count);
  full_line();

  move(line++, 0);
  printw("Frontend: %s", fedev);
  full_line();

  move(line++, 0);
  printw("Demux:    %s", dedev);
  full_line();

  move(line++, 0);
  printw("Network:  %s", snapshot.network_name_len > 0 ? snapshot.network_name : "waiting for demux info");
  full_line();

  return render_service_table(state, &snapshot, line, footer_row);
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

// Keep monitor keyboard help in one place so the spinner can avoid overlapping it.
static const char *monitor_footer_help_text(void) {
  return " Up/Down select | Enter detail | PgUp/PgDn page | q quit ";
}

// Keep detail keyboard help separate from the monitor navigation help.
static const char *detail_footer_help_text(void) {
  return " Up/Down service | PgUp/PgDn page | Esc back | q quit ";
}

// Return the right-aligned footer indicator column, or -1 when the row is tight.
static int footer_indicator_column(int col, const char *help_text) {
  if (col <= 0)
    return -1;

  int indicator_len = strlen(" " DSFEMON_VERSION " [/] ");
  int indicator_col = col > indicator_len ? col - indicator_len : -1;

  if (indicator_col <= (int)strlen(help_text))
    return -1;

  return indicator_col;
}

// Show compact bottom keyboard help plus a refresh spinner on the right.
static void render_help_bar(int footer_row, int col, const char *help_text, unsigned int refresh_cycle) {
  if (col <= 0)
    return;

  static const char spinner[] = "|/-\\";

  attron(A_REVERSE);
  mvhline(footer_row, 0, ' ', col);
  mvaddnstr(footer_row, 0, help_text, col);

  int indicator_col = footer_indicator_column(col, help_text);
  if (indicator_col >= 0) {
    move(footer_row, indicator_col);
    printw(" %s [", DSFEMON_VERSION);
    REVERSE_RED_ON;
    printw("%c", spinner[refresh_cycle % 4]);
    REVERSE_RED_OFF;
    printw("] ");
  }

  attroff(A_REVERSE);
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

// Recompute counts and page placement before drawing or handling navigation.
static void update_screen_state(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, int row) {
  state->frontend_count = count_frontends(dvb_data, scan_config);
  state->page_capacity = frontends_per_page(row);
  state->page_count = count_pages(state->frontend_count, state->page_capacity);
  state->selected_frontend = clamp_selected_frontend(state->selected_frontend, state->frontend_count);

  if (state->frontend_count == 0)
    state->detail_open = false;

  if (state->frontend_count > 0 && state->page_capacity > 0 && !state->detail_open)
    state->current_page = page_for_selected_frontend(state->selected_frontend, state->page_capacity);

  state->current_page = clamp_page(state->current_page, state->page_count);
}

// Render the content area between the fixed header and footer bars.
static unsigned int render_screen_body(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, unsigned int line, int footer_row) {
  if (state->detail_open && line < (unsigned int)footer_row)
    return render_demux_detail(state, dvb_data, scan_config, line, footer_row);

  if (line >= (unsigned int)footer_row)
    return line;

  if (state->frontend_count == 0) {
    move(line, 0);

    return line + no_devices_line(scan_config);
  }

  if (state->page_capacity == 0)
    return render_terminal_too_small_line(line);

  return render_frontend_page(dvb_data, scan_config, state->current_page, state->page_capacity, state->selected_frontend, line, state->refresh_cycle);
}

// Draw one complete screen frame and flush it once after all rows are updated.
static void render_screen(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config) {
  int row, col;
  getmaxyx(stdscr, row, col);

  update_screen_state(state, dvb_data, scan_config, row);

  unsigned int line = HEADER_BAR_LINES + HEADER_GAP_LINES;
  int footer_row = row > 0 ? row - 1 : 0;

  if (row > 0) {
    render_header_bar(0, col, state->current_page, state->page_count, state->frontend_count);

    if (row > HEADER_BAR_LINES) {
      move(HEADER_BAR_LINES, 0);
      full_line();
    }
  }

  line = render_screen_body(state, dvb_data, scan_config, line, footer_row);

  for (int i = line; i < footer_row; i++) {
    move(i, 0);
    full_line();
  }

  if (row > HEADER_BAR_LINES)
    render_help_bar(footer_row, col, state->detail_open ? detail_footer_help_text() : monitor_footer_help_text(), state->refresh_cycle);

  refresh();
}

// Apply one keyboard action to the monitor selection/page state.
static void handle_monitor_key(int key, bool *running, struct screen_state *state) {
  if (quit_key(key)) {
    *running = false;

    return;
  }

  if (enter_key(key) && state->frontend_count > 0) {
    state->detail_open = true;
    reset_detail_service_selection(state);

    return;
  }

  if (next_frontend_key(key) && state->selected_frontend + 1 < state->frontend_count) {
    state->selected_frontend++;

    if (state->page_capacity > 0)
      state->current_page = page_for_selected_frontend(state->selected_frontend, state->page_capacity);

    return;
  }

  if (previous_frontend_key(key) && state->selected_frontend > 0) {
    state->selected_frontend--;

    if (state->page_capacity > 0)
      state->current_page = page_for_selected_frontend(state->selected_frontend, state->page_capacity);

    return;
  }

  unsigned int page_count = count_pages(state->frontend_count, state->page_capacity);

  if (next_page_key(key) && state->current_page + 1 < page_count) {
    state->current_page++;
    state->selected_frontend = clamp_selected_frontend(state->current_page * state->page_capacity, state->frontend_count);

    return;
  }

  if (previous_page_key(key) && state->current_page > 0) {
    state->current_page--;
    state->selected_frontend = clamp_selected_frontend(state->current_page * state->page_capacity, state->frontend_count);
  }
}

// Apply one keyboard action while the demux detail screen is open.
static void handle_detail_key(int key, bool *running, struct screen_state *state) {
  if (quit_key(key)) {
    *running = false;

    return;
  }

  if (detail_back_key(key)) {
    state->detail_open = false;

    return;
  }

  if (next_frontend_key(key) && state->detail_selected_service + 1 < state->detail_service_count) {
    state->detail_selected_service++;
    clamp_detail_service_selection(state);

    return;
  }

  if (previous_frontend_key(key) && state->detail_selected_service > 0) {
    state->detail_selected_service--;
    clamp_detail_service_selection(state);

    return;
  }

  if (next_page_key(key) && state->detail_page_capacity > 0 && state->detail_selected_service + 1 < state->detail_service_count) {
    state->detail_selected_service += state->detail_page_capacity;
    clamp_detail_service_selection(state);

    return;
  }

  if (previous_page_key(key) && state->detail_page_capacity > 0 && state->detail_selected_service > 0) {
    if (state->detail_selected_service > state->detail_page_capacity)
      state->detail_selected_service -= state->detail_page_capacity;
    else
      state->detail_selected_service = 0;

    clamp_detail_service_selection(state);
  }
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
  set_escdelay(ESC_KEY_DELAY_MS);
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
  struct screen_state screen = init_screen_state();
  unsigned long long next_refresh_time_us = 0;

  timeout(INPUT_POLL_INTERVAL_US / 1000);

  while (running) {
    if (g_stop_requested)
      running = false;

    unsigned long long now_us = monotonic_time_us();
    bool refresh_due = now_us >= next_refresh_time_us;

    if (screen.redraw_requested || refresh_due) {
      render_screen(&screen, dvb_data, &scan_config);
      screen.redraw_requested = false;

      if (refresh_due) {
        screen.refresh_cycle++;
        next_refresh_time_us = monotonic_time_us() + REFRESH_INTERVAL_US;
      }
    }

    int key = getch();
    if (key != ERR) {
      if (screen.detail_open)
        handle_detail_key(key, &running, &screen);
      else
        handle_monitor_key(key, &running, &screen);

      screen.redraw_requested = true;
    }

    if (g_stop_requested)
      running = false;
  }

  cleanup_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
  free(dvb_data);
  curs_set(1);
  endwin();

  return 0;
}
