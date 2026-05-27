
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

// Legacy frontend enum presentation helpers used by the ncurses view layer.
void print_info(const char *text, dvb_frontend_info fe_info);
void print_status(const char *text, fe_status_t status);
void print_bandwidth(const char *text, fe_bandwidth_t bandwidth);
void print_code_rate(const char *text, fe_code_rate_t code_rate);
void print_modulation(const char *text, fe_modulation_t fe_modulation);
void print_transmit_mode(const char *text, fe_transmit_mode_t transmit_mode);
void print_guard_interval(const char *text, fe_guard_interval_t fe_guard_interval);
void print_hierarchy(const char *text, fe_hierarchy_t fe_hierarchy);
void print_frequency(const char *unit, __u32 frequency);

#endif
