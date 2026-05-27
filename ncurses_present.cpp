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

// Render legacy bandwidth enum values.
void print_bandwidth(const char *text, fe_bandwidth_t bandwidth) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (bandwidth) {
    case BANDWIDTH_8_MHZ:
      printw("8MHz ");
      break;
    case BANDWIDTH_7_MHZ:
      printw("7MHz ");
      break;
    case BANDWIDTH_6_MHZ:
      printw("6MHz ");
      break;
    case BANDWIDTH_AUTO:
      printw("AUTO ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render legacy code-rate enum values.
void print_code_rate(const char *text, fe_code_rate_t code_rate) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (code_rate) {
    case FEC_NONE:
      printw("NONE ");
      break;
    case FEC_1_2:
      printw("1/2 ");
      break;
    case FEC_2_3:
      printw("2/3 ");
      break;
    case FEC_3_4:
      printw("3/4 ");
      break;
    case FEC_4_5:
      printw("4/5 ");
      break;
    case FEC_5_6:
      printw("5/6 ");
      break;
    case FEC_6_7:
      printw("6/7 ");
      break;
    case FEC_7_8:
      printw("7/8 ");
      break;
    case FEC_8_9:
      printw("8/9 ");
      break;
    case FEC_AUTO:
      printw("AUTO ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render legacy modulation enum values.
void print_modulation(const char *text, fe_modulation_t fe_modulation) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (fe_modulation) {
    case QPSK:
      printw("QPSK ");
      break;
    case QAM_16:
      printw("QAM 16 ");
      break;
    case QAM_32:
      printw("QAM 32 ");
      break;
    case QAM_64:
      printw("QAM 64 ");
      break;
    case QAM_128:
      printw("QAM 128 ");
      break;
    case QAM_256:
      printw("QAM 256 ");
      break;
    case QAM_AUTO:
      printw("QAM AUTO ");
      break;
    case VSB_8:
      printw("VSB 8 ");
      break;
    case VSB_16:
      printw("VSB 16 ");
      break;
    case PSK_8:
      printw("8PSK ");
      break;
    case APSK_16:
      printw("16APSK ");
      break;
    case APSK_32:
      printw("32APSK ");
      break;
    case DQPSK:
      printw("DQPSK ");
      break;
    case QAM_4_NR:
      printw("QAM 4-NR ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render legacy OFDM transmission-mode enum values.
void print_transmit_mode(const char *text, fe_transmit_mode_t transmit_mode) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (transmit_mode) {
    case TRANSMISSION_MODE_2K:
      printw("2K ");
      break;
    case TRANSMISSION_MODE_8K:
      printw("8K ");
      break;
    case TRANSMISSION_MODE_AUTO:
      printw("AUTO ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render legacy OFDM guard-interval enum values.
void print_guard_interval(const char *text, fe_guard_interval_t fe_guard_interval) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (fe_guard_interval) {
    case GUARD_INTERVAL_1_32:
      printw("1/32 ");
      break;
    case GUARD_INTERVAL_1_16:
      printw("1/16 ");
      break;
    case GUARD_INTERVAL_1_8:
      printw("1/8 ");
      break;
    case GUARD_INTERVAL_1_4:
      printw("1/4 ");
      break;
    case GUARD_INTERVAL_AUTO:
      printw("AUTO ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render legacy OFDM hierarchy enum values.
void print_hierarchy(const char *text, fe_hierarchy_t fe_hierarchy) {
  DESC_COL_ON;
  printw("%s ", text);
  DESC_COL_OFF;
  DESC_VALL_ON;

  switch (fe_hierarchy) {
    case HIERARCHY_NONE:
      printw("NONE ");
      break;
    case HIERARCHY_1:
      printw("1 ");
      break;
    case HIERARCHY_2:
      printw("2 ");
      break;
    case HIERARCHY_4:
      printw("4 ");
      break;
    case HIERARCHY_AUTO:
      printw("AUTO ");
      break;
    default:
      printw("UNKNOWN ");
      break;
  }
  DESC_VALL_OFF;
}

// Render the historical satellite frequency display format.
void print_frequency(const char *unit, __u32 frequency) {
  DESC_COL_ON;
  printw("FREQUENCY: ");
  DESC_COL_OFF;
  DESC_VALL_ON;

  if (frequency + 10600000 >= 11700000) {
    printw("%5.3f%s ", (float)(frequency + 10600000) / 1000000, unit);
  } else {
    printw("%5.3f%s ", (float)frequency + 9750000, unit);
  }

  DESC_VALL_OFF;
}
