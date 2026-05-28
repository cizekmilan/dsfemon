
#ifndef NCURSES_PRESENT_H
#define NCURSES_PRESENT_H

#include <linux/dvb/frontend.h>

#include "color.h"

#define DESC_COL_ON RED_BOLD_ON
#define DESC_COL_OFF RED_BOLD_OFF

#define DESC_COLL_ON RED_ON
#define DESC_COLL_OFF RED_OFF

#define DESC_VAL_ON GREEN_BOLD_ON
#define DESC_VAL_OFF GREEN_BOLD_OFF

#define DESC_VALL_ON GREEN_ON
#define DESC_VALL_OFF GREEN_OFF

// Draw a bounded percentage bar with room for both brackets.
void linebar(int proc, int len);

// Frontend presentation helpers used by the ncurses view layer.
void print_info(const char *text, dvb_frontend_info fe_info);
void print_status(const char *text, fe_status_t status);

#endif
