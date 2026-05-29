/*
 * File role: ncurses color-pair initialization.
 *
 * Centralizes the terminal palette used by rendering macros so the rest of
 * the UI can refer to semantic color helpers instead of raw pair numbers.
 */

#include <ncurses.h>

// Map the classic terminal colors plus reverse-video status pairs to ncurses ids.
void set_my_color(void) {
  init_pair(1, COLOR_RED, COLOR_BLACK);
  init_pair(2, COLOR_GREEN, COLOR_BLACK);
  init_pair(3, COLOR_YELLOW, COLOR_BLACK);
  init_pair(4, COLOR_BLUE, COLOR_BLACK);
  init_pair(5, COLOR_MAGENTA, COLOR_BLACK);
  init_pair(6, COLOR_CYAN, COLOR_BLACK);
  init_pair(7, COLOR_WHITE, COLOR_BLACK);
  // Used under A_REVERSE, so this displays as red text on the normal bar background.
  init_pair(8, COLOR_WHITE, COLOR_RED);
}
