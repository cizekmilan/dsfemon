#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include <curses.h>

#include "color.h"
#include "demux_monitor.h"
#include "demux_view.h"
#include "device_discovery.h"
#include "frontend_view.h"
#include "ui_helpers.h"

#define DSFEMON_VERSION "0.10-modern"
#define CHANNEL_ROTATION_REFRESHES 8

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

// Program entry point: initialize ncurses, discover devices, render until quit.
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
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

  struct dvb_scan_config scan_config = {
      DVB_DEFAULT_MIN_ADAPTER,
      DVB_DEFAULT_MAX_ADAPTER,
      DVB_MAX_SUBADAPTERS};

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

  while (running) {
    if (g_stop_requested)
      running = false;
    unsigned int line = 0;
    unsigned int card_count = 0;
    int key;

    int row, col;
    getmaxyx(stdscr, row, col);
    (void)col;

    for (int adapter = scan_config.min_adapter; running && adapter < scan_config.max_adapter; adapter++)
      for (int subadapter = 0; running && subadapter < scan_config.max_subadapter; subadapter++) {
        struct dvb_data_s *current_dvb_data = &dvb_data[dvb_device_index(adapter, subadapter, scan_config.max_subadapter)];
        if (current_dvb_data->fefd < 0)
          continue;

        line = render_frontend(current_dvb_data, adapter, subadapter, card_count, line, refresh_cycle);

        timeout(0);
        key = getch();

        if (quit_key(key)) {
          running = false;
          break;
        }

        card_count++;
      }
    timeout(0);
    key = getch();
    if (quit_key(key))
      running = false;

    if (card_count == 0) {
      move(line, 0);
      line += no_devices_line(&scan_config);
    }

    move(line, 0);
    printw("Femon - DVB Frontend monitor developed by David Seidl, version %s", DSFEMON_VERSION);
    line += full_line();

    for (int i = line; i < row; i++)
      full_line();
    usleep(500000);
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
