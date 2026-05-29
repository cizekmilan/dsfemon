/*
 * File role: fullscreen demux detail view.
 *
 * Renders the selected frontend's service table and selected-service metadata,
 * and owns the scroll/selection state used while the detail screen is open.
 */

#include "demux_detail_view.h"

#include <stdio.h>
#include <string.h>

#include <curses.h>

#include "color.h"
#include "device_discovery.h"
#include "ui_helpers.h"

// Empty row between detail metadata and the service table header.
#define DETAIL_TABLE_GAP_LINES 1
// Selected-service metadata rows shown above the detail table, including a gap.
#define DETAIL_SELECTED_SUMMARY_LINES 5

// Small local helper for range calculations without pulling extra C++ headers.
static unsigned int min_uint(unsigned int a, unsigned int b) {
  return a < b ? a : b;
}

// Convert available terminal columns into a safe one-line detail value width.
static size_t detail_value_width(int col, const char *label) {
  size_t label_len = strlen(label);

  if (col <= (int)label_len + 1)
    return 0;

  return (size_t)col - label_len - 1;
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
static void clamp_detail_service_selection(struct demux_detail_state *state) {
  if (state->service_count == 0) {
    state->selected_service = 0;
    state->scroll_offset = 0;

    return;
  }

  if (state->selected_service >= state->service_count)
    state->selected_service = state->service_count - 1;

  if (state->page_capacity == 0) {
    state->scroll_offset = state->selected_service;

    return;
  }

  if (state->selected_service < state->scroll_offset)
    state->scroll_offset = state->selected_service;

  if (state->selected_service >= state->scroll_offset + state->page_capacity)
    state->scroll_offset = state->selected_service - state->page_capacity + 1;

  unsigned int max_scroll_offset = state->service_count > state->page_capacity ? state->service_count - state->page_capacity : 0;
  if (state->scroll_offset > max_scroll_offset)
    state->scroll_offset = max_scroll_offset;
}

// Reset service-table navigation when entering a new detail screen.
static void reset_detail_service_selection(struct demux_detail_state *state) {
  state->selected_service = 0;
  state->scroll_offset = 0;
  state->service_count = 0;
  state->page_capacity = 0;
}

// Build the initial detail state without relying on a long positional initializer.
void init_demux_detail_state(struct demux_detail_state *state) {
  memset(state, 0, sizeof(*state));
}

// Start a detail session with a fresh snapshot cache for the selected frontend.
void reset_demux_detail_state(struct demux_detail_state *state, unsigned int selected_frontend) {
  reset_detail_service_selection(state);
  state->snapshot_valid = false;
  state->snapshot_frontend = selected_frontend;
  memset(&state->snapshot, 0, sizeof(state->snapshot));
}

// Move selection one service down.
void demux_detail_select_next(struct demux_detail_state *state) {
  if (state->selected_service + 1 >= state->service_count)
    return;

  state->selected_service++;
  clamp_detail_service_selection(state);
}

// Move selection one service up.
void demux_detail_select_previous(struct demux_detail_state *state) {
  if (state->selected_service == 0)
    return;

  state->selected_service--;
  clamp_detail_service_selection(state);
}

// Move selection one visible page down.
void demux_detail_select_next_page(struct demux_detail_state *state) {
  if (state->page_capacity == 0 || state->selected_service + 1 >= state->service_count)
    return;

  state->selected_service += state->page_capacity;
  clamp_detail_service_selection(state);
}

// Move selection one visible page up.
void demux_detail_select_previous_page(struct demux_detail_state *state) {
  if (state->page_capacity == 0 || state->selected_service == 0)
    return;

  if (state->selected_service > state->page_capacity)
    state->selected_service -= state->page_capacity;
  else
    state->selected_service = 0;

  clamp_detail_service_selection(state);
}

// Jump to the first service in the detail table.
void demux_detail_select_first(struct demux_detail_state *state) {
  state->selected_service = 0;
  clamp_detail_service_selection(state);
}

// Jump to the last service in the detail table.
void demux_detail_select_last(struct demux_detail_state *state) {
  if (state->service_count == 0)
    return;

  state->selected_service = state->service_count - 1;
  clamp_detail_service_selection(state);
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
    case 0x07:
      snprintf(buffer, buffer_size, "FM Rad");
      break;
    case 0x0a:
      snprintf(buffer, buffer_size, "AAC Rad");
      break;
    case 0x0b:
      snprintf(buffer, buffer_size, "Mosaic");
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
static bool append_stream_summary_text(char *buffer, size_t buffer_size, size_t *used, const char *text) {
  if (buffer_size == 0 || used == NULL)
    return false;

  if (*used >= buffer_size - 1)
    return false;

  int written = snprintf(buffer + *used, buffer_size - *used, "%s", text);
  if (written < 0)
    return false;

  if ((size_t)written >= buffer_size - *used)
    *used = buffer_size - 1;
  else
    *used += written;

  return true;
}

// Return whether appending text would keep the visible summary inside its row.
static bool stream_summary_fits(size_t used, const char *text, const char *suffix, size_t max_text_width) {
  if (max_text_width == 0)
    return true;

  return used + strlen(text) + strlen(suffix) <= max_text_width;
}

// Append a compact marker for streams hidden by terminal width or snapshot limits.
static void append_stream_summary_more(char *buffer, size_t buffer_size, size_t *used, int omitted_count, size_t max_text_width) {
  char more_text[40];
  const char *separator = *used > 0 ? " | " : "";
  const char *stream_word = omitted_count == 1 ? "stream" : "streams";

  snprintf(more_text, sizeof(more_text), "%s... | +%d %s", separator, omitted_count, stream_word);

  if (max_text_width > 0 && *used + strlen(more_text) > max_text_width) {
    snprintf(more_text, sizeof(more_text), "%s...", separator);

    if (max_text_width > 0 && *used + strlen(more_text) > max_text_width)
      return;
  }

  append_stream_summary_text(buffer, buffer_size, used, more_text);
}

// Build a compact PMT stream list for the selected service.
static void format_stream_summary(const struct demux_service_snapshot *service, char *buffer, size_t buffer_size, size_t max_text_width) {
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
    char stream_piece[96];
    char more_suffix[40];
    int omitted_after_stream = service->stream_count - stream_index - 1;
    const char *separator = used > 0 ? " | " : "";

    format_stream_type(service->streams[stream_index].type, stream_type_text, sizeof(stream_type_text));
    snprintf(stream_text, sizeof(stream_text), "%d %s", service->streams[stream_index].pid, stream_type_text);
    snprintf(stream_piece, sizeof(stream_piece), "%s%s", separator, stream_text);
    snprintf(more_suffix, sizeof(more_suffix), " | ... | +%d streams", omitted_after_stream);

    if (!stream_summary_fits(used, stream_piece, omitted_after_stream > 0 ? more_suffix : "", max_text_width)) {
      append_stream_summary_more(buffer, buffer_size, &used, service->stream_count - stream_index, max_text_width);

      return;
    }

    append_stream_summary_text(buffer, buffer_size, &used, stream_piece);
  }

  if (service->stream_count > service->stored_stream_count) {
    append_stream_summary_more(buffer, buffer_size, &used, service->stream_count - service->stored_stream_count, max_text_width);
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

// Build a compact teletext/subtitle summary for the selected service.
static void format_extra_summary(const struct demux_service_snapshot *service, char *buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size, "TTX %s | Sub %s",
           service->teletext_len > 0 ? service->teletext : "-",
           service->subtitle_len > 0 ? service->subtitles : "-");
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

// Print a preformatted table segment without wrapping on narrow terminals.
static void add_table_text(int col, const char *text) {
  add_detail_clipped_text(col, text);
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

// Print one service table row, clipping each segment to the terminal width.
static void render_service_table_row(unsigned int line, int col, unsigned int service_index, const struct demux_service_snapshot *service, bool selected) {
  char service_id_text[16];
  char service_type_text[16];
  char program_pid_text[16];
  char stream_count_text[16];
  char text[96];
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

  snprintf(text, sizeof(text), "%3u   %-5s   ", service_index + 1, service_id_text);
  add_table_text(col, text);

  if (!selected)
    CYAN_ON;
  snprintf(text, sizeof(text), "%-7s", service_type_text);
  add_table_text(col, text);
  if (!selected)
    CYAN_OFF;

  snprintf(text, sizeof(text), "   %-7s   %-7s   ", program_pid_text, stream_count_text);
  add_table_text(col, text);

  if (!selected && service->languages_len > 0)
    CYAN_ON;
  snprintf(text, sizeof(text), "%-15s", languages_text);
  add_table_text(col, text);
  if (!selected && service->languages_len > 0)
    CYAN_OFF;

  add_table_text(col, "   ");

  if (!selected && service->free_ca_mode)
    YELLOW_BOLD_ON;
  else if (!selected)
    GREEN_ON;
  snprintf(text, sizeof(text), "%-3s", service->free_ca_mode ? "yes" : "no");
  add_table_text(col, text);
  if (!selected && service->free_ca_mode)
    YELLOW_BOLD_OFF;
  else if (!selected)
    GREEN_OFF;

  add_table_text(col, "   ");
  if (!selected)
    detail_status_color_on(service->running_status);
  snprintf(text, sizeof(text), "%-8s", running_status_name(service->running_status));
  add_table_text(col, text);
  if (!selected)
    detail_status_color_off(service->running_status);

  add_table_text(col, "   ");
  add_table_text(col, service->name);

  if (selected)
    attroff(A_REVERSE);
}

// Show extra data for the currently selected service without crowding each row.
static unsigned int render_selected_service_summary(const struct demux_detail_state *state, const struct demux_snapshot *snapshot, unsigned int line, int col) {
  if (snapshot->service_count <= 0 || state->selected_service >= (unsigned int)snapshot->service_count)
    return line;

  const struct demux_service_snapshot *service = &snapshot->services[state->selected_service];
  char service_type_text[16];
  char pcr_pid_text[16];
  char stream_summary[512];
  char extra_summary[128];
  char ca_summary[256];
  size_t stream_summary_width = detail_value_width(col, "Streams: ");

  format_service_type(service->service_type, service_type_text, sizeof(service_type_text));

  if (service->pcr_pid >= 0)
    snprintf(pcr_pid_text, sizeof(pcr_pid_text), "%d", service->pcr_pid);
  else
    snprintf(pcr_pid_text, sizeof(pcr_pid_text), "-");

  format_stream_summary(service, stream_summary, sizeof(stream_summary), stream_summary_width);
  format_extra_summary(service, extra_summary, sizeof(extra_summary));
  format_ca_summary(service, ca_summary, sizeof(ca_summary));

  move(line++, 0);
  full_line();

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
  full_line();

  move(line++, 0);
  add_detail_label(col, "Audio: ");
  if (service->languages_len > 0)
    add_detail_good_value(col, service->languages);
  else
    add_detail_clipped_text(col, "-");
  add_detail_clipped_text(col, " | ");
  add_detail_label(col, "Extra: ");
  if (service->teletext_len > 0 || service->subtitle_len > 0)
    add_detail_good_value(col, extra_summary);
  else
    add_detail_clipped_text(col, extra_summary);
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
static unsigned int render_service_table(struct demux_detail_state *state, const struct demux_snapshot *snapshot, unsigned int line, int footer_row) {
  int row, col;
  char table_header[160];
  getmaxyx(stdscr, row, col);
  (void)row;

  if (line >= (unsigned int)footer_row)
    return line;

  int selected_summary_lines = snapshot->service_count > 0 ? DETAIL_SELECTED_SUMMARY_LINES : 0;
  int table_rows = footer_row - (int)line - 2 - DETAIL_TABLE_GAP_LINES - selected_summary_lines;
  state->service_count = snapshot->service_count;
  state->page_capacity = table_rows > 0 ? table_rows : 0;
  clamp_detail_service_selection(state);

  move(line++, 0);
  printw("Services: %d", snapshot->service_count);
  if (snapshot->service_count > 0 && state->page_capacity > 0) {
    unsigned int first_visible = state->scroll_offset + 1;
    unsigned int last_visible = min_uint(state->scroll_offset + state->page_capacity, state->service_count);
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
           "Audio",
           "CA",
           "Status",
           "Service");
  attron(A_REVERSE);
  mvhline(line, 0, ' ', col);
  mvaddnstr(line, 0, table_header, col);
  attroff(A_REVERSE);
  line++;

  if (state->page_capacity == 0)
    return line;

  unsigned int first_service = state->scroll_offset;
  unsigned int last_service = min_uint(first_service + state->page_capacity, state->service_count);

  for (unsigned int service_index = first_service; service_index < last_service && line < (unsigned int)footer_row; service_index++) {
    const struct demux_service_snapshot *service = &snapshot->services[service_index];
    render_service_table_row(line, col, service_index, service, service_index == state->selected_service);
    line++;
  }

  return line;
}

// Keep the last useful detail snapshot visible across transient demux gaps.
static void update_detail_snapshot_cache(struct demux_detail_state *state, struct demux_snapshot *snapshot, unsigned int selected_frontend) {
  if (snapshot->service_count > 0) {
    if (snapshot->network_name_len == 0 &&
        state->snapshot_valid &&
        state->snapshot_frontend == selected_frontend &&
        state->snapshot.network_name_len > 0) {
      snapshot->network_name_len = state->snapshot.network_name_len;
      snprintf(snapshot->network_name, sizeof(snapshot->network_name), "%s", state->snapshot.network_name);
    }

    state->snapshot = *snapshot;
    state->snapshot_frontend = selected_frontend;
    state->snapshot_valid = true;

    return;
  }

  if (state->snapshot_valid && state->snapshot_frontend == selected_frontend)
    *snapshot = state->snapshot;
}

// Render the fullscreen demux detail view for the selected frontend.
unsigned int render_demux_detail(struct demux_detail_state *state,
                                 struct dvb_data_s *selected_dvb_data,
                                 unsigned int selected_frontend,
                                 unsigned int frontend_count,
                                 unsigned int line,
                                 int footer_row) {
  char fedev[128];
  char dedev[128];
  struct demux_snapshot snapshot;

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
  update_detail_snapshot_cache(state, &snapshot, selected_frontend);

  move(line++, 0);
  CYAN_BOLD_ON;
  printw("Demux detail");
  CYAN_BOLD_OFF;
  printw(" for frontend %u/%u", selected_frontend + 1, frontend_count);
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
