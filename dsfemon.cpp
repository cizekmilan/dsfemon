/*
 * dsfemon - DVB frontend monitor for Linux
 *
 * File role: top-level ncurses application loop, page/detail navigation,
 * header/footer bars, keyboard handling, and frontend page rendering.
 *
 * 2012: Originally developed as Femon by David Seidl.
 * 2026: Modernized, refactored, and extended as dsfemon by Milan Cizek.
 *
 * Continued with permission from the original author under the condition
 * that open-source principles are preserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <curses.h>

#include "command_line.h"
#include "color.h"
#include "demux_detail_view.h"
#include "demux_monitor.h"
#include "demux_view.h"
#include "device_discovery.h"
#include "frontend_status_cache.h"
#include "frontend_view.h"
#include "ui_helpers.h"

#define DSFEMON_VERSION "v0.78"
#define DSFEMON_TITLE "dsfemon - DVB frontend monitor"
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
// Keyboard polling cadence. Lower than DVB refresh so navigation feels immediate.
#define INPUT_POLL_INTERVAL_US 25000
// Extra wait used only to distinguish standalone Esc from terminal key sequences.
#define ESC_SEQUENCE_TIMEOUT_MS 80
// Keep standalone Esc responsive while still allowing terminal key sequences.
#define ESC_KEY_DELAY_MS 50
// Internal marker for Esc-prefixed terminal sequences we intentionally ignore.
#define IGNORED_ESCAPE_SEQUENCE_KEY -2

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
  struct demux_detail_state detail;
};

// One colored or plain segment in the footer keyboard help.
struct footer_help_segment {
  const char *text;
  bool highlighted;
};

static const struct footer_help_segment MONITOR_FOOTER_HELP[] = {
    {" ", false},
    {"Up", true},
    {"/", false},
    {"Down", true},
    {" select | ", false},
    {"Enter", true},
    {" detail | ", false},
    {"PgUp", true},
    {"/", false},
    {"PgDn", true},
    {" page | ", false},
    {"Home", true},
    {"/", false},
    {"End", true},
    {" jump | ", false},
    {"Q", true},
    {"uit ", false},
};

static const struct footer_help_segment DETAIL_FOOTER_HELP[] = {
    {" ", false},
    {"Up", true},
    {"/", false},
    {"Down", true},
    {" service | ", false},
    {"PgUp", true},
    {"/", false},
    {"PgDn", true},
    {" page | ", false},
    {"Home", true},
    {"/", false},
    {"End", true},
    {" jump | ", false},
    {"ESC", true},
    {" back | ", false},
    {"Q", true},
    {"uit ", false},
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

// Jump to the first selectable item in the current view.
static bool first_item_key(int key) {
  if (key == KEY_HOME)
    return true;

#ifdef KEY_BEG
  if (key == KEY_BEG)
    return true;
#endif

#ifdef KEY_FIND
  if (key == KEY_FIND)
    return true;
#endif

#ifdef KEY_A1
  if (key == KEY_A1)
    return true;
#endif

  return false;
}

// Jump to the last selectable item in the current view.
static bool last_item_key(int key) {
  if (key == KEY_END)
    return true;

#ifdef KEY_LL
  if (key == KEY_LL)
    return true;
#endif

#ifdef KEY_SELECT
  if (key == KEY_SELECT)
    return true;
#endif

#ifdef KEY_C1
  if (key == KEY_C1)
    return true;
#endif

  return false;
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

// Restore the normal input polling timeout after short local timeout changes.
static void restore_input_timeout(void) {
  timeout(INPUT_POLL_INTERVAL_US / 1000);
}

// Decide whether a raw Esc-prefixed sequence has received its final byte.
static bool escape_sequence_complete(const int *sequence, int sequence_len) {
  if (sequence_len < 2)
    return false;

  int first = sequence[0];
  int last = sequence[sequence_len - 1];
  if (first == 'O')
    return true;

  if (first != '[')
    return true;

  return last == '~' || (last >= '@' && last <= '~' && last != '[');
}

// Parse common terminal Home/End escape sequences not translated by terminfo.
static int key_from_escape_sequence(const int *sequence, int sequence_len) {
  if (sequence_len < 2)
    return IGNORED_ESCAPE_SEQUENCE_KEY;

  if (sequence[0] == 'O') {
    if (sequence[1] == 'H')
      return KEY_HOME;

    if (sequence[1] == 'F')
      return KEY_END;

    return IGNORED_ESCAPE_SEQUENCE_KEY;
  }

  if (sequence[0] != '[')
    return IGNORED_ESCAPE_SEQUENCE_KEY;

  int last = sequence[sequence_len - 1];
  if (last == 'H')
    return KEY_HOME;

  if (last == 'F')
    return KEY_END;

  if (last != '~')
    return IGNORED_ESCAPE_SEQUENCE_KEY;

  int value = 0;
  bool has_value = false;
  for (int i = 1; i < sequence_len && sequence[i] >= '0' && sequence[i] <= '9'; i++) {
    value = value * 10 + sequence[i] - '0';
    has_value = true;
  }

  if (!has_value)
    return IGNORED_ESCAPE_SEQUENCE_KEY;

  if (value == 1 || value == 7)
    return KEY_HOME;

  if (value == 4 || value == 8)
    return KEY_END;

  return IGNORED_ESCAPE_SEQUENCE_KEY;
}

// Normalize raw Esc-prefixed Home/End sequences before view-specific handlers run.
static int normalize_input_key(int key) {
  if (key != 27)
    return key;

  int sequence[8];
  int sequence_len = 0;

  timeout(ESC_SEQUENCE_TIMEOUT_MS);

  while (sequence_len < (int)(sizeof(sequence) / sizeof(sequence[0]))) {
    int next_key = getch();
    if (next_key == ERR)
      break;

    sequence[sequence_len++] = next_key;
    if (escape_sequence_complete(sequence, sequence_len))
      break;
  }

  restore_input_timeout();

  if (sequence_len == 0)
    return 27;

  return key_from_escape_sequence(sequence, sequence_len);
}

// Leave the detail screen on a standalone Esc key.
static bool detail_back_key(int key) {
  if (key == KEY_BACKSPACE)
    return true;

  return key == 27;
}

// Monotonic microsecond clock used to schedule UI redraws independently of input polling.
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

// Align the current page so the selected frontend's detail row is visible.
static unsigned int page_for_selected_frontend(unsigned int selected_frontend, unsigned int page_capacity) {
  if (page_capacity == 0)
    return 0;

  return selected_frontend / page_capacity;
}

// Build the initial monitor state without relying on a long positional initializer.
static struct screen_state init_screen_state(void) {
  struct screen_state state;

  memset(&state, 0, sizeof(state));
  state.redraw_requested = true;
  state.page_count = 1;
  init_demux_detail_state(&state.detail);

  return state;
}

// Render one frontend block: frontend rows, demux summary, and detail entry.
static unsigned int render_frontend(struct dvb_data_s *dvb_data, const struct frontend_status_snapshot *frontend_status, int adapter, int subadapter, unsigned int card_count, unsigned int selected_frontend, unsigned int line, unsigned int refresh_cycle) {
  char fedev[128];

  format_frontend_path(fedev, sizeof(fedev), adapter, subadapter);

  line = render_frontend_status_lines(frontend_status, fedev, line);
  move(line, 0);
  line += demux_main_info(dvb_data, (refresh_cycle / CHANNEL_ROTATION_REFRESHES) + card_count);
  move(line, 0);
  line += detail_line(card_count, card_count == selected_frontend);
  move(line, 0);
  line += full_line();

  return line;
}

// Render only complete frontend blocks that belong to the requested page.
static unsigned int render_frontend_page(struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, struct frontend_status_cache *status_cache, unsigned int page, unsigned int page_capacity, unsigned int selected_frontend, unsigned int first_line, unsigned int refresh_cycle) {
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
      int device_index = dvb_device_index(adapter, subadapter, scan_config->max_subadapter);
      struct dvb_data_s *current_dvb_data = &dvb_data[device_index];
      if (current_dvb_data->fefd < 0)
        continue;

      if (frontend_index >= first_frontend && frontend_index < last_frontend) {
        struct frontend_status_snapshot frontend_status;
        copy_frontend_status_cache_snapshot(status_cache, device_index, &frontend_status);
        line = render_frontend(current_dvb_data, &frontend_status, adapter, subadapter, frontend_index, selected_frontend, line, refresh_cycle);
      }

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

// Return the footer help segment array for the active screen.
static const struct footer_help_segment *footer_help_segments(bool detail_open, size_t *segment_count) {
  if (detail_open) {
    *segment_count = sizeof(DETAIL_FOOTER_HELP) / sizeof(DETAIL_FOOTER_HELP[0]);

    return DETAIL_FOOTER_HELP;
  }

  *segment_count = sizeof(MONITOR_FOOTER_HELP) / sizeof(MONITOR_FOOTER_HELP[0]);

  return MONITOR_FOOTER_HELP;
}

// Print one footer segment while respecting the terminal width.
static void add_footer_text(int col, const char *text) {
  int y, x;
  getyx(stdscr, y, x);
  (void)y;

  int remaining_cols = col - x;
  if (remaining_cols > 0)
    addnstr(text, remaining_cols);
}

// Highlight a keyboard token inside the reverse-video footer bar.
static void add_footer_key(int col, const char *text) {
  REVERSE_RED_ON;
  add_footer_text(col, text);
  REVERSE_RED_OFF;
}

// Count footer help columns from the same segments used for colored rendering.
static int footer_help_text_len(bool detail_open) {
  size_t segment_count;
  const struct footer_help_segment *segments = footer_help_segments(detail_open, &segment_count);
  int text_len = 0;

  for (size_t i = 0; i < segment_count; i++)
    text_len += strlen(segments[i].text);

  return text_len;
}

// Render colored keyboard help without changing the plain text layout length.
static void render_footer_help_text(int col, bool detail_open) {
  size_t segment_count;
  const struct footer_help_segment *segments = footer_help_segments(detail_open, &segment_count);

  for (size_t i = 0; i < segment_count; i++) {
    if (segments[i].highlighted)
      add_footer_key(col, segments[i].text);
    else
      add_footer_text(col, segments[i].text);
  }
}

// Return the right-aligned footer indicator column, or -1 when the row is tight.
static int footer_indicator_column(int col, bool detail_open) {
  if (col <= 0)
    return -1;

  int indicator_len = strlen(" " DSFEMON_VERSION " [/] ");
  int indicator_col = col > indicator_len ? col - indicator_len : -1;

  if (indicator_col <= footer_help_text_len(detail_open))
    return -1;

  return indicator_col;
}

// Show compact bottom keyboard help plus a refresh spinner on the right.
static void render_help_bar(int footer_row, int col, bool detail_open, unsigned int refresh_cycle) {
  if (col <= 0)
    return;

  static const char spinner[] = "|/-\\";

  attron(A_REVERSE);
  mvhline(footer_row, 0, ' ', col);
  move(footer_row, 0);
  render_footer_help_text(col, detail_open);

  int indicator_col = footer_indicator_column(col, detail_open);
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
static unsigned int render_screen_body(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, struct frontend_status_cache *status_cache, unsigned int line, int footer_row) {
  if (state->detail_open && line < (unsigned int)footer_row) {
    struct dvb_data_s *selected_dvb_data = frontend_by_index(dvb_data, scan_config, state->selected_frontend);

    return render_demux_detail(&state->detail, selected_dvb_data, state->selected_frontend, state->frontend_count, line, footer_row);
  }

  if (line >= (unsigned int)footer_row)
    return line;

  if (state->frontend_count == 0) {
    move(line, 0);

    return line + no_devices_line(scan_config);
  }

  if (state->page_capacity == 0)
    return render_terminal_too_small_line(line);

  return render_frontend_page(dvb_data, scan_config, status_cache, state->current_page, state->page_capacity, state->selected_frontend, line, state->refresh_cycle);
}

// Draw one complete screen frame and flush it once after all rows are updated.
static void render_screen(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, struct frontend_status_cache *status_cache) {
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

  line = render_screen_body(state, dvb_data, scan_config, status_cache, line, footer_row);

  for (int i = line; i < footer_row; i++) {
    move(i, 0);
    full_line();
  }

  if (row > HEADER_BAR_LINES)
    render_help_bar(footer_row, col, state->detail_open, state->refresh_cycle);

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
    reset_demux_detail_state(&state->detail, state->selected_frontend);

    return;
  }

  if (first_item_key(key) && state->frontend_count > 0) {
    state->selected_frontend = 0;

    if (state->page_capacity > 0)
      state->current_page = page_for_selected_frontend(state->selected_frontend, state->page_capacity);

    return;
  }

  if (last_item_key(key) && state->frontend_count > 0) {
    state->selected_frontend = state->frontend_count - 1;

    if (state->page_capacity > 0)
      state->current_page = page_for_selected_frontend(state->selected_frontend, state->page_capacity);

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

  if (first_item_key(key)) {
    demux_detail_select_first(&state->detail);

    return;
  }

  if (last_item_key(key)) {
    demux_detail_select_last(&state->detail);

    return;
  }

  if (next_frontend_key(key)) {
    demux_detail_select_next(&state->detail);

    return;
  }

  if (previous_frontend_key(key)) {
    demux_detail_select_previous(&state->detail);

    return;
  }

  if (next_page_key(key)) {
    demux_detail_select_next_page(&state->detail);

    return;
  }

  if (previous_page_key(key)) {
    demux_detail_select_previous_page(&state->detail);
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

  unsigned long long refresh_interval_us = (unsigned long long)options.refresh_interval_ms * 1000;
  struct frontend_status_cache *status_cache = create_frontend_status_cache(dvb_data, DVB_DEVICE_COUNT, refresh_interval_us);

  if (status_cache == NULL) {
    endwin();
    fprintf(stderr, "Cannot allocate frontend status cache\n");
    cleanup_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
    free(dvb_data);
    return 1;
  }

  if (start_frontend_status_cache(status_cache) != 0) {
    endwin();
    fprintf(stderr, "Cannot start frontend status worker\n");
    destroy_frontend_status_cache(status_cache);
    cleanup_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
    free(dvb_data);
    return 1;
  }

  bool running = true;
  struct screen_state screen = init_screen_state();
  unsigned long long next_refresh_time_us = 0;

  timeout(INPUT_POLL_INTERVAL_US / 1000);

  while (running) {
    if (g_stop_requested)
      running = false;

    int key = getch();
    if (key != ERR) {
      key = normalize_input_key(key);

      if (key != IGNORED_ESCAPE_SEQUENCE_KEY) {
        if (screen.detail_open)
          handle_detail_key(key, &running, &screen);
        else
          handle_monitor_key(key, &running, &screen);

        screen.redraw_requested = true;
      }
    }

    unsigned long long now_us = monotonic_time_us();
    bool refresh_due = now_us >= next_refresh_time_us;

    if (screen.redraw_requested || refresh_due) {
      render_screen(&screen, dvb_data, &scan_config, status_cache);
      screen.redraw_requested = false;

      if (refresh_due) {
        screen.refresh_cycle++;
        next_refresh_time_us = monotonic_time_us() + refresh_interval_us;
      }
    }

    if (g_stop_requested)
      running = false;
  }

  destroy_frontend_status_cache(status_cache);
  cleanup_dvb_devices(dvb_data, DVB_DEVICE_COUNT);
  free(dvb_data);
  curs_set(1);
  endwin();

  return 0;
}
