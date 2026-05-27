
#ifndef COLOR_H
#define COLOR_H

#include <ncurses.h>

// Initialize all color pairs used by presentation macros.
void set_my_color(void);

#define RED_ON attron(COLOR_PAIR(1))
#define RED_OFF attroff(COLOR_PAIR(1))

#define RED_BOLD_ON attron(A_BOLD | COLOR_PAIR(1))
#define RED_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(1))

#define GREEN_ON attron(COLOR_PAIR(2))
#define GREEN_OFF attroff(COLOR_PAIR(2))

#define GREEN_BOLD_ON attron(A_BOLD | COLOR_PAIR(2))
#define GREEN_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(2))

#define YELLOW_ON attron(COLOR_PAIR(3))
#define YELLOW_OFF attroff(COLOR_PAIR(3))

#define YELLOW_BOLD_ON attron(A_BOLD | COLOR_PAIR(3))
#define YELLOW_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(3))

#define BLUE_ON attron(COLOR_PAIR(4))
#define BLUE_OFF attroff(COLOR_PAIR(4))

#define BLUE_BOLD_ON attron(A_BOLD | COLOR_PAIR(4))
#define BLUE_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(4))

#define MAGENTA_ON attron(COLOR_PAIR(5))
#define MAGENTA_OFF attroff(COLOR_PAIR(5))

#define MAGENTA_BOLD_ON attron(A_BOLD | COLOR_PAIR(5))
#define MAGENTA_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(5))

#define CYAN_ON attron(COLOR_PAIR(6))
#define CYAN_OFF attroff(COLOR_PAIR(6))

#define CYAN_BOLD_ON attron(A_BOLD | COLOR_PAIR(6))
#define CYAN_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(6))

#define WHITE_ON attron(COLOR_PAIR(7))
#define WHITE_OFF attroff(COLOR_PAIR(7))

#define WHITE_BOLD_ON attron(A_BOLD | COLOR_PAIR(7))
#define WHITE_BOLD_OFF attroff(A_BOLD | COLOR_PAIR(7))

#define REVERSE_RED_ON attron(COLOR_PAIR(8))
#define REVERSE_RED_OFF attroff(COLOR_PAIR(8))

#endif
