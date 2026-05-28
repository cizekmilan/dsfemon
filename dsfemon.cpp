#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <curses.h>

#include "command_line.h"
#include "color.h"
#include "demux_monitor.h"
#include "demux_view.h"
#include "device_discovery.h"
#include "frontend_status_cache.h"
#include "frontend_view.h"
#include "ui_helpers.h"

#define DSFEMON_VERSION "v0.75"
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
// Empty row between detail metadata and the service table header.
#define DETAIL_TABLE_GAP_LINES 1
// Selected-service metadata rows shown above the detail table.
#define DETAIL_SELECTED_SUMMARY_LINES 3
// Main UI refresh cadence.
#define REFRESH_INTERVAL_US 500000
// Keyboard polling cadence. Lower than DVB refresh so navigation feels immediate.
#define INPUT_POLL_INTERVAL_US 25000
// Extra wait used only to distinguish standalone Esc from terminal key sequences.
#define ESC_SEQUENCE_TIMEOUT_MS 80
// Keep standalone Esc responsive while still allowing terminal key sequences.
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
  bool detail_snapshot_valid;
  unsigned int detail_snapshot_frontend;
  struct demux_snapshot detail_snapshot;
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

// Restore the normal input polling timeout after short local timeout changes.
static void restore_input_timeout(void) {
  timeout(INPUT_POLL_INTERVAL_US / 1000);
}

// Detect keys such as Home/End that arrive as Esc-prefixed terminal sequences.
static bool escape_sequence_follows(void) {
  timeout(ESC_SEQUENCE_TIMEOUT_MS);
  int next_key = getch();
  if (next_key == ERR) {
    restore_input_timeout();

    return false;
  }

  timeout(0);
  while (getch() != ERR) {
  }
  restore_input_timeout();

  return true;
}

// Leave the detail screen without treating Home/End escape sequences as Esc.
static bool detail_back_key(int key) {
  if (key == KEY_BACKSPACE)
    return true;

  if (key != 27)
    return false;

  return !escape_sequence_follows();
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

// Keep running-state coloring readable inside the detail table.
static void detail_status_color_on(int running_status) {
  switch (running_status) {
    case 4:
      GREEN_BOLD_ON;
      break;
    case 1:
    case 5:
      RED_ON;
      break;
    case 2:
    case 3:
      YELLOW_ON;
      break;
    default:
      WHITE_ON;
      break;
  }
}

// Turn off the color enabled for one running-state value.
static void detail_status_color_off(int running_status) {
  switch (running_status) {
    case 4:
      GREEN_BOLD_OFF;
      break;
    case 1:
    case 5:
      RED_OFF;
      break;
    case 2:
    case 3:
      YELLOW_OFF;
      break;
    default:
      WHITE_OFF;
      break;
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

// Start a detail session with a fresh snapshot cache for the selected frontend.
static void reset_detail_screen(struct screen_state *state) {
  reset_detail_service_selection(state);
  state->detail_snapshot_valid = false;
  state->detail_snapshot_frontend = state->selected_frontend;
  memset(&state->detail_snapshot, 0, sizeof(state->detail_snapshot));
}

// Build the initial monitor state without relying on a long positional initializer.
static struct screen_state init_screen_state(void) {
  struct screen_state state;

  memset(&state, 0, sizeof(state));
  state.redraw_requested = true;
  state.page_count = 1;

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

// Convert DVB service_type values into short labels suitable for a compact table.
static void format_service_type(int service_type, char *buffer, size_t buffer_size) {
  switch (service_type) {
    case 0x01:
      snprintf(buffer, buffer_size, "TV");
      break;
    case 0x02:
      snprintf(buffer, buffer_size, "Radio");
      break;
    case 0x03:
      snprintf(buffer, buffer_size, "Text");
      break;
    case 0x0c:
      snprintf(buffer, buffer_size, "Data");
      break;
    case 0x11:
      snprintf(buffer, buffer_size, "MPEG2HD");
      break;
    case 0x16:
      snprintf(buffer, buffer_size, "H264 SD");
      break;
    case 0x19:
      snprintf(buffer, buffer_size, "H264 HD");
      break;
    case 0x1f:
      snprintf(buffer, buffer_size, "HEVC");
      break;
    case 0x20:
      snprintf(buffer, buffer_size, "HEVC 4K");
      break;
    default:
      if (service_type >= 0)
        snprintf(buffer, buffer_size, "0x%02x", service_type);
      else
        snprintf(buffer, buffer_size, "-");
      break;
  }
}

// Convert PMT stream_type values into compact names for the selected-service summary.
static void format_stream_type(int stream_type, char *buffer, size_t buffer_size) {
  switch (stream_type) {
    case 0x01:
      snprintf(buffer, buffer_size, "MPEG1 video");
      break;
    case 0x02:
      snprintf(buffer, buffer_size, "MPEG2 video");
      break;
    case 0x03:
    case 0x04:
      snprintf(buffer, buffer_size, "MPEG audio");
      break;
    case 0x06:
      snprintf(buffer, buffer_size, "private");
      break;
    case 0x0f:
      snprintf(buffer, buffer_size, "AAC audio");
      break;
    case 0x11:
      snprintf(buffer, buffer_size, "LATM AAC");
      break;
    case 0x1b:
      snprintf(buffer, buffer_size, "H264 video");
      break;
    case 0x24:
      snprintf(buffer, buffer_size, "HEVC video");
      break;
    default:
      if (stream_type >= 0)
        snprintf(buffer, buffer_size, "0x%02x", stream_type);
      else
        snprintf(buffer, buffer_size, "-");
      break;
  }
}

// Append formatted stream text while keeping the summary safely terminated.
static void append_stream_summary_text(char *buffer, size_t buffer_size, size_t *used, const char *text) {
  if (buffer_size == 0 || used == NULL)
    return;

  if (*used >= buffer_size - 1)
    return;

  int written = snprintf(buffer + *used, buffer_size - *used, "%s", text);
  if (written < 0)
    return;

  if ((size_t)written >= buffer_size - *used)
    *used = buffer_size - 1;
  else
    *used += written;
}

// Build a compact PMT stream list for the selected service.
static void format_stream_summary(const struct demux_service_snapshot *service, char *buffer, size_t buffer_size) {
  size_t used = 0;

  if (buffer_size == 0)
    return;

  buffer[0] = '\0';

  if (service->stream_count <= 0) {
    append_stream_summary_text(buffer, buffer_size, &used, "waiting for PMT data");

    return;
  }

  for (int stream_index = 0; stream_index < service->stored_stream_count; stream_index++) {
    char stream_type_text[24];
    char stream_text[80];

    format_stream_type(service->streams[stream_index].type, stream_type_text, sizeof(stream_type_text));
    if (stream_index > 0)
      append_stream_summary_text(buffer, buffer_size, &used, " |");

    snprintf(stream_text, sizeof(stream_text), "%d %s", service->streams[stream_index].pid, stream_type_text);
    append_stream_summary_text(buffer, buffer_size, &used, stream_text);
  }

  if (service->stream_count > service->stored_stream_count) {
    char more_text[32];

    snprintf(more_text, sizeof(more_text), " | +%d more", service->stream_count - service->stored_stream_count);
    append_stream_summary_text(buffer, buffer_size, &used, more_text);
  }
}

// Build a compact conditional-access summary for the selected service.
static void format_ca_summary(const struct demux_service_snapshot *service, char *buffer, size_t buffer_size) {
  if (service->ca_detail_len > 0)
    snprintf(buffer, buffer_size, "%s", service->ca_detail);
  else if (service->free_ca_mode)
    snprintf(buffer, buffer_size, "yes, details unavailable");
  else
    snprintf(buffer, buffer_size, "free");
}

// Print text without letting colored summary fields overflow the terminal row.
static void add_detail_clipped_text(int col, const char *text) {
  int y, x;
  getyx(stdscr, y, x);
  (void)y;

  int remaining_cols = col - x;
  if (remaining_cols > 0)
    addnstr(text, remaining_cols);
}

// Highlight a compact detail summary label.
static void add_detail_label(int col, const char *label) {
  CYAN_BOLD_ON;
  add_detail_clipped_text(col, label);
  CYAN_BOLD_OFF;
}

// Highlight a positive detail summary value.
static void add_detail_good_value(int col, const char *value) {
  GREEN_ON;
  add_detail_clipped_text(col, value);
  GREEN_OFF;
}

// Highlight a technical/detail value.
static void add_detail_info_value(int col, const char *value) {
  WHITE_BOLD_ON;
  add_detail_clipped_text(col, value);
  WHITE_BOLD_OFF;
}

// Highlight a warning-style detail summary value.
static void add_detail_warning_value(int col, const char *value) {
  YELLOW_BOLD_ON;
  add_detail_clipped_text(col, value);
  YELLOW_BOLD_OFF;
}

// Print one service table row, clipping the service name to the terminal width.
static void render_service_table_row(unsigned int line, int col, unsigned int service_index, const struct demux_service_snapshot *service, bool selected) {
  char service_id_text[16];
  char service_type_text[16];
  char program_pid_text[16];
  char stream_count_text[16];
  const char *languages_text = service->languages_len > 0 ? service->languages : "-";

  if (service->service_id >= 0)
    snprintf(service_id_text, sizeof(service_id_text), "%d", service->service_id);
  else
    snprintf(service_id_text, sizeof(service_id_text), "-");

  format_service_type(service->service_type, service_type_text, sizeof(service_type_text));

  if (service->program_pid >= 0)
    snprintf(program_pid_text, sizeof(program_pid_text), "%d", service->program_pid);
  else
    snprintf(program_pid_text, sizeof(program_pid_text), "-");

  if (service->stream_count > 0)
    snprintf(stream_count_text, sizeof(stream_count_text), "%d", service->stream_count);
  else
    snprintf(stream_count_text, sizeof(stream_count_text), "-");

  if (selected)
    attron(A_REVERSE);

  mvhline(line, 0, ' ', col);
  move(line, 0);
  printw("%3u   %-5s   ", service_index + 1, service_id_text);

  if (!selected)
    CYAN_ON;
  printw("%-7s", service_type_text);
  if (!selected)
    CYAN_OFF;

  printw("   %-7s   %-7s   ", program_pid_text, stream_count_text);

  if (!selected && service->languages_len > 0)
    CYAN_ON;
  printw("%-15s", languages_text);
  if (!selected && service->languages_len > 0)
    CYAN_OFF;

  printw("   ");

  if (!selected && service->free_ca_mode)
    YELLOW_BOLD_ON;
  else if (!selected)
    GREEN_ON;
  printw("%-3s", service->free_ca_mode ? "yes" : "no");
  if (!selected && service->free_ca_mode)
    YELLOW_BOLD_OFF;
  else if (!selected)
    GREEN_OFF;

  printw("   ");
  if (!selected)
    detail_status_color_on(service->running_status);
  printw("%-8s", running_status_name(service->running_status));
  if (!selected)
    detail_status_color_off(service->running_status);
  printw("   ");

  int y, x;
  getyx(stdscr, y, x);
  (void)y;

  int remaining_cols = col - x;
  if (remaining_cols > 0)
    addnstr(service->name, remaining_cols);

  if (selected)
    attroff(A_REVERSE);
}

// Show extra data for the currently selected service without crowding each row.
static unsigned int render_selected_service_summary(const struct screen_state *state, const struct demux_snapshot *snapshot, unsigned int line, int col) {
  if (snapshot->service_count <= 0 || state->detail_selected_service >= (unsigned int)snapshot->service_count)
    return line;

  const struct demux_service_snapshot *service = &snapshot->services[state->detail_selected_service];
  char service_type_text[16];
  char pcr_pid_text[16];
  char stream_summary[512];
  char ca_summary[256];

  format_service_type(service->service_type, service_type_text, sizeof(service_type_text));

  if (service->pcr_pid >= 0)
    snprintf(pcr_pid_text, sizeof(pcr_pid_text), "%d", service->pcr_pid);
  else
    snprintf(pcr_pid_text, sizeof(pcr_pid_text), "-");

  format_stream_summary(service, stream_summary, sizeof(stream_summary));
  format_ca_summary(service, ca_summary, sizeof(ca_summary));

  move(line++, 0);
  add_detail_label(col, "Selected: ");
  add_detail_info_value(col, service->name);
  add_detail_clipped_text(col, " | ");
  add_detail_label(col, "Type: ");
  add_detail_good_value(col, service_type_text);
  add_detail_clipped_text(col, " | ");
  add_detail_label(col, "Provider: ");
  add_detail_good_value(col, service->provider_name_len > 0 ? service->provider_name : "-");
  add_detail_clipped_text(col, " | ");
  add_detail_label(col, "PCR PID: ");
  add_detail_warning_value(col, pcr_pid_text);
  full_line();

  move(line++, 0);
  add_detail_label(col, "Streams: ");
  add_detail_info_value(col, stream_summary);
  add_detail_clipped_text(col, " | ");
  add_detail_label(col, "Lang: ");
  if (service->languages_len > 0)
    add_detail_good_value(col, service->languages);
  else
    add_detail_clipped_text(col, "-");
  full_line();

  move(line++, 0);
  add_detail_label(col, "CA: ");
  if (service->ca_detail_len > 0 || service->free_ca_mode)
    add_detail_warning_value(col, ca_summary);
  else
    add_detail_good_value(col, ca_summary);
  full_line();

  return line;
}

// Render the current service table viewport for the selected frontend.
static unsigned int render_service_table(struct screen_state *state, const struct demux_snapshot *snapshot, unsigned int line, int footer_row) {
  int row, col;
  char table_header[160];
  getmaxyx(stdscr, row, col);
  (void)row;

  if (line >= (unsigned int)footer_row)
    return line;

  int selected_summary_lines = snapshot->service_count > 0 ? DETAIL_SELECTED_SUMMARY_LINES : 0;
  int table_rows = footer_row - (int)line - 2 - DETAIL_TABLE_GAP_LINES - selected_summary_lines;
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

  line = render_selected_service_summary(state, snapshot, line, col);

  if (line >= (unsigned int)footer_row)
    return line;

  for (int i = 0; i < DETAIL_TABLE_GAP_LINES && line < (unsigned int)footer_row; i++) {
    move(line++, 0);
    full_line();
  }

  if (line >= (unsigned int)footer_row)
    return line;

  snprintf(table_header, sizeof(table_header), "%3s   %-5s   %-7s   %-7s   %-7s   %-15s   %-3s   %-8s   %s",
           "No",
           "SID",
           "Type",
           "PMT PID",
           "Streams",
           "Lang",
           "CA",
           "Status",
           "Service");
  attron(A_REVERSE);
  mvhline(line, 0, ' ', col);
  mvaddnstr(line, 0, table_header, col);
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

// Keep the last useful detail snapshot visible across transient demux gaps.
static void update_detail_snapshot_cache(struct screen_state *state, struct demux_snapshot *snapshot) {
  if (snapshot->service_count > 0) {
    if (snapshot->network_name_len == 0 &&
        state->detail_snapshot_valid &&
        state->detail_snapshot_frontend == state->selected_frontend &&
        state->detail_snapshot.network_name_len > 0) {
      snapshot->network_name_len = state->detail_snapshot.network_name_len;
      snprintf(snapshot->network_name, sizeof(snapshot->network_name), "%s", state->detail_snapshot.network_name);
    }

    state->detail_snapshot = *snapshot;
    state->detail_snapshot_frontend = state->selected_frontend;
    state->detail_snapshot_valid = true;

    return;
  }

  if (state->detail_snapshot_valid && state->detail_snapshot_frontend == state->selected_frontend)
    *snapshot = state->detail_snapshot;
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
  update_detail_snapshot_cache(state, &snapshot);

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
static unsigned int render_screen_body(struct screen_state *state, struct dvb_data_s *dvb_data, const struct dvb_scan_config *scan_config, struct frontend_status_cache *status_cache, unsigned int line, int footer_row) {
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
    reset_detail_screen(state);

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

  struct frontend_status_cache *status_cache = create_frontend_status_cache(dvb_data, DVB_DEVICE_COUNT);

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
      if (screen.detail_open)
        handle_detail_key(key, &running, &screen);
      else
        handle_monitor_key(key, &running, &screen);

      screen.redraw_requested = true;
    }

    unsigned long long now_us = monotonic_time_us();
    bool refresh_due = now_us >= next_refresh_time_us;

    if (screen.redraw_requested || refresh_due) {
      render_screen(&screen, dvb_data, &scan_config, status_cache);
      screen.redraw_requested = false;

      if (refresh_due) {
        screen.refresh_cycle++;
        next_refresh_time_us = monotonic_time_us() + REFRESH_INTERVAL_US;
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
