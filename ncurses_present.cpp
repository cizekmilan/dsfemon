#include <ncurses.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/dvb/frontend.h>
#include "color.h"
#include "ncurses_present.h"

// Draw a percentage bar whose total width is controlled by the caller.
void linebar(int proc, int len) {
  if (len < 7)
    len = 7;
  if (proc > 100)
    proc = 100;
  if (proc < 0)
    proc = 0;

  int inner_len = len - 3;

  printw(" [");
  YELLOW_BOLD_ON;

  for (int i = 1; i <= inner_len; i++) {
    if ((float)proc >= (((float)i / (float)inner_len) * 100))
      printw("=");
    else
      printw(" ");
  }
  YELLOW_BOLD_OFF;
  printw("]");
}

// Render static frontend information returned by FE_GET_INFO.
void print_info(const char *text, dvb_frontend_info fe_info) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_COLL_ON;
  printw("TYPE: ");
  DESC_COLL_OFF;
  DESC_VALL_ON;

  switch (fe_info.type) {
    case FE_QPSK:
      printw("QPSK ");
      break;
    case FE_QAM:
      printw("QAM  ");
      break;
    case FE_OFDM:
      printw("OFDM ");
      break;
    case FE_ATSC:
      printw("ATSC ");
      break;
  }
  DESC_VALL_OFF;
  DESC_COLL_ON;
  printw("NAME: ");
  DESC_COLL_OFF;
  DESC_VALL_ON;
  printw("%s ", fe_info.name);
  DESC_VALL_OFF;
}

// Render frontend lock/status flags with highlighted active flags.
void print_status(const char *text, fe_status_t status) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;

  if (status & FE_HAS_SIGNAL) {
    DESC_VAL_ON;
    printw("*SIGNAL* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" SIGNAL  ");
    DESC_VALL_OFF;
  }

  if (status & FE_HAS_CARRIER) {
    DESC_VAL_ON;
    printw("*CARRIER* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" CARRIER  ");
    DESC_VALL_OFF;
  }

  if (status & FE_HAS_VITERBI) {
    DESC_VAL_ON;
    printw("*VITERBI* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" VITERBI  ");
    DESC_VALL_OFF;
  }

  if (status & FE_HAS_SYNC) {
    DESC_VAL_ON;
    printw("*SYNC* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" SYNC  ");
    DESC_VALL_OFF;
  }

  if (status & FE_HAS_LOCK) {
    DESC_VAL_ON;
    printw("*LOCK* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" LOCK  ");
    DESC_VALL_OFF;
  }

  if (status & FE_TIMEDOUT) {
    DESC_VAL_ON;
    printw("*TIMEDOUT* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" TIMEDOUT  ");
    DESC_VALL_OFF;
  }

  if (status & FE_REINIT) {
    DESC_VAL_ON;
    printw("*REINIT* ");
    DESC_VAL_OFF;
  } else {
    DESC_VALL_ON;
    printw(" REINIT  ");
    DESC_VALL_OFF;
  }
}
