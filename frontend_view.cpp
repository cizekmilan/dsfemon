/*
 * File role: frontend status rendering.
 *
 * Draws each frontend block: device info, lock flags, signal/CNR bars,
 * BER/block counters, and compact DVBv5 tuning parameters.
 */

#include "frontend_view.h"

#include <curses.h>
#include <linux/dvb/frontend.h>
#include <stdint.h>
#include <stdio.h>

#include "color.h"
#include "ncurses_present.h"
#include "ui_helpers.h"

#define DVB_RELATIVE_SCALE_MAX 0xffff

// Render frontend path, type, and tuner name.
static int device_info_line(const struct frontend_status_snapshot *snapshot, const char *devname) {
  RED_BOLD_ON;
  printw("Found device: ");
  RED_BOLD_OFF;
  GREEN_ON;
  printw("%s ", devname);
  GREEN_OFF;

  if (snapshot->has_info)
    print_info("FRONTEND INFO:", snapshot->info);

  full_line();

  return 1;
}

// Render FE_HAS_* lock/status flags.
static int device_status_line(const struct frontend_status_snapshot *snapshot) {
  if (snapshot->has_status)
    print_status("FRONTEND STATUS: ", snapshot->status);

  full_line();

  return 1;
}

// Convert the DVB API's 16-bit relative scale to a rounded display percentage.
static int relative_value_percent(unsigned long long value) {
  if (value >= DVB_RELATIVE_SCALE_MAX)
    return 100;

  return (int)((value * 100 + DVB_RELATIVE_SCALE_MAX / 2) / DVB_RELATIVE_SCALE_MAX);
}

// Convert a DVBv5 statistic into the compact text used beside bars/counters.
static bool format_dtv_stat_value(const struct dtv_stats *stat, char *buffer, size_t buffer_size) {
  if (stat->scale == FE_SCALE_DECIBEL) {
    snprintf(buffer, buffer_size, "%5.1f dB", (float)stat->svalue / 1000);
    return true;
  }

  if (stat->scale == FE_SCALE_RELATIVE) {
    snprintf(buffer, buffer_size, "%3d%%", relative_value_percent(stat->uvalue));
    return true;
  }

  if (stat->scale == FE_SCALE_COUNTER) {
    snprintf(buffer, buffer_size, "%llu", (unsigned long long)stat->uvalue);
    return true;
  }

  return false;
}

// Return a DVBv5 counter value only when the statistic really is a counter.
static bool stat_counter_value(const struct dtv_stats *stat, unsigned long long *value) {
  if (stat->scale != FE_SCALE_COUNTER)
    return false;
  *value = (unsigned long long)stat->uvalue;

  return true;
}

// Render a pre/post BER field, preferring a rate when total count is available.
static void print_ber_field(const char *label, const struct dtv_stats *errors, const struct dtv_stats *total, bool has_total) {
  unsigned long long error_count;
  if (!stat_counter_value(errors, &error_count))
    return;

  DESC_COL_ON;
  printw("%s ", label);
  DESC_COL_OFF;
  DESC_VALL_ON;

  if (has_total) {
    unsigned long long total_count;

    if (stat_counter_value(total, &total_count) && total_count > 0) {
      printw("%.2e ", (double)error_count / (double)total_count);
      DESC_VALL_OFF;
      return;
    }
  }

  printw("%llu ", error_count);
  DESC_VALL_OFF;
}

// Render an error/total counter pair such as errored blocks.
static void print_counter_pair_field(const char *label, const struct dtv_stats *value, const struct dtv_stats *total, bool has_total) {
  unsigned long long value_count;
  if (!stat_counter_value(value, &value_count))
    return;

  DESC_COL_ON;
  printw("%s ", label);
  DESC_COL_OFF;
  DESC_VALL_ON;
  printw("%llu", value_count);

  if (has_total) {
    unsigned long long total_count;
    if (stat_counter_value(total, &total_count))
      printw("/%llu", total_count);
  }

  printw(" ");
  DESC_VALL_OFF;
}

// Render signal percentage, optional DVBv5 dB value, and the signal bar.
static int device_signal_line(const struct frontend_status_snapshot *snapshot) {
  char signal[32];
  bool has_v5_signal = snapshot->has_v5 &&
                       snapshot->v5.has_signal_strength &&
                       format_dtv_stat_value(&snapshot->v5.signal_strength, signal, sizeof(signal));

  if (has_v5_signal || snapshot->has_legacy_signal) {
    DESC_COL_ON;
    printw("SIGNAL:");
    DESC_COL_OFF;
    DESC_VALL_ON;
    if (snapshot->has_legacy_signal)
      printw("%3d%%", relative_value_percent(snapshot->legacy_signal));
    if (has_v5_signal)
      printw(" %s", signal);
    DESC_VALL_OFF;

    int len = remaining_bar_width();
    if (len > 0 && snapshot->has_legacy_signal)
      linebar(relative_value_percent(snapshot->legacy_signal), len);
    else if (len > 0 && snapshot->v5.signal_strength.scale == FE_SCALE_RELATIVE)
      linebar(relative_value_percent(snapshot->v5.signal_strength.uvalue), len);
  }

  full_line();

  return 1;
}

// Render SNR/CNR percentage, optional DVBv5 dB value, and the CNR/SNR bar.
static int device_snr_line(const struct frontend_status_snapshot *snapshot) {
  char cnr[32];
  bool has_v5_cnr = snapshot->has_v5 &&
                    snapshot->v5.has_cnr &&
                    format_dtv_stat_value(&snapshot->v5.cnr, cnr, sizeof(cnr));

  if (has_v5_cnr || snapshot->has_legacy_snr) {
    DESC_COL_ON;
    printw(has_v5_cnr ? "CNR:" : "SNR:");
    DESC_COL_OFF;
    DESC_VALL_ON;
    if (snapshot->has_legacy_snr)
      printw("   %3d%%", relative_value_percent(snapshot->legacy_snr));
    if (has_v5_cnr)
      printw(" %s", cnr);
    DESC_VALL_OFF;

    int len = remaining_bar_width();
    if (len > 0 && snapshot->has_legacy_snr)
      linebar(relative_value_percent(snapshot->legacy_snr), len);
    else if (len > 0 && snapshot->v5.cnr.scale == FE_SCALE_RELATIVE)
      linebar(relative_value_percent(snapshot->v5.cnr.uvalue), len);
  }

  full_line();

  return 1;
}

// Render DVBv5 BER/block statistics or fall back to legacy BER counters.
static int device_ber_block_line(const struct frontend_status_snapshot *snapshot) {
  if (snapshot->has_v5 &&
      (snapshot->v5.has_error_block_count || snapshot->v5.has_pre_error_bit_count || snapshot->v5.has_post_error_bit_count)) {
    if (snapshot->v5.has_error_block_count)
      print_counter_pair_field("ERR BLK:", &snapshot->v5.error_block_count, &snapshot->v5.total_block_count, snapshot->v5.has_total_block_count);
    if (snapshot->v5.has_pre_error_bit_count)
      print_ber_field("PRE BER:", &snapshot->v5.pre_error_bit_count, &snapshot->v5.pre_total_bit_count, snapshot->v5.has_pre_total_bit_count);
    if (snapshot->v5.has_post_error_bit_count)
      print_ber_field("POST BER:", &snapshot->v5.post_error_bit_count, &snapshot->v5.post_total_bit_count, snapshot->v5.has_post_total_bit_count);
    full_line();

    return 1;
  }

  if (snapshot->has_legacy_ber) {
    DESC_COL_ON;
    printw("BER: ");
    DESC_COL_OFF;
    DESC_VALL_ON;
    printw("%5.0f ", (float)snapshot->legacy_ber);
    DESC_VALL_OFF;
  }

  if (snapshot->has_legacy_uncorrected_blocks) {
    DESC_COL_ON;
    printw("UNCORRECT BLOCK: ");
    DESC_COL_OFF;
    DESC_VALL_ON;
    printw("%u ", snapshot->legacy_uncorrected_blocks);
    DESC_VALL_OFF;
  }

  full_line();

  return 1;
}

// Human-readable delivery-system label for the compact parameter row.
static const char *delivery_system_name(fe_delivery_system_t delivery_system) {

  switch (delivery_system) {
    case SYS_DVBC_ANNEX_A:
      return "DVB-C";
    case SYS_DVBC_ANNEX_B:
      return "DVB-C/B";
    case SYS_DVBT:
      return "DVB-T";
    case SYS_DSS:
      return "DSS";
    case SYS_DVBS:
      return "DVB-S";
    case SYS_DVBS2:
      return "DVB-S2";
    case SYS_DVBH:
      return "DVB-H";
    case SYS_ISDBT:
      return "ISDB-T";
    case SYS_ISDBS:
      return "ISDB-S";
    case SYS_ISDBC:
      return "ISDB-C";
    case SYS_ATSC:
      return "ATSC";
    case SYS_ATSCMH:
      return "ATSC-M/H";
    case SYS_DTMB:
      return "DTMB";
    case SYS_CMMB:
      return "CMMB";
    case SYS_DAB:
      return "DAB";
    case SYS_DVBT2:
      return "DVB-T2";
    case SYS_TURBO:
      return "TURBO";
    case SYS_DVBC_ANNEX_C:
      return "DVB-C/C";
    default:
      return "UNKNOWN";
  }
}

// Human-readable FEC label for the compact parameter row.
static const char *code_rate_name(fe_code_rate_t code_rate) {

  switch (code_rate) {
    case FEC_NONE:
      return "NONE";
    case FEC_1_2:
      return "1/2";
    case FEC_2_3:
      return "2/3";
    case FEC_3_4:
      return "3/4";
    case FEC_4_5:
      return "4/5";
    case FEC_5_6:
      return "5/6";
    case FEC_6_7:
      return "6/7";
    case FEC_7_8:
      return "7/8";
    case FEC_8_9:
      return "8/9";
    case FEC_AUTO:
      return "AUTO";
    default:
      return "UNKNOWN";
  }
}

// Human-readable modulation label for the compact parameter row.
static const char *modulation_name(fe_modulation_t modulation) {

  switch (modulation) {
    case QPSK:
      return "QPSK";
    case QAM_16:
      return "QAM16";
    case QAM_32:
      return "QAM32";
    case QAM_64:
      return "QAM64";
    case QAM_128:
      return "QAM128";
    case QAM_256:
      return "QAM256";
    case QAM_AUTO:
      return "AUTO";
    case VSB_8:
      return "VSB8";
    case VSB_16:
      return "VSB16";
    case PSK_8:
      return "8PSK";
    case APSK_16:
      return "16APSK";
    case APSK_32:
      return "32APSK";
    case DQPSK:
      return "DQPSK";
    case QAM_4_NR:
      return "QAM4-NR";
    default:
      return "UNKNOWN";
  }
}

// Identify delivery systems where the driver frequency is satellite IF.
static bool is_satellite_delivery(fe_delivery_system_t delivery_system) {
  return delivery_system == SYS_DVBS ||
         delivery_system == SYS_DVBS2 ||
         delivery_system == SYS_DSS ||
         delivery_system == SYS_TURBO;
}

// Identify delivery systems where symbol rate is a meaningful frontend value.
static bool uses_symbol_rate(fe_delivery_system_t delivery_system) {
  return is_satellite_delivery(delivery_system) ||
         delivery_system == SYS_DVBC_ANNEX_A ||
         delivery_system == SYS_DVBC_ANNEX_B ||
         delivery_system == SYS_DVBC_ANNEX_C ||
         delivery_system == SYS_ISDBC;
}

// Hide symbol-rate placeholders for terrestrial systems where SR is not used.
static bool should_show_symbol_rate(const struct frontend_v5_snapshot *snapshot) {
  if (!snapshot->has_symbol_rate || snapshot->symbol_rate == 0)
    return false;

  if (!snapshot->has_delivery_system || snapshot->delivery_system == SYS_UNDEFINED)
    return true;

  return uses_symbol_rate(snapshot->delivery_system);
}

// Convert DVB-S/S2 IF frequency back to the user-facing transponder frequency.
static float satellite_frequency_ghz(uint32_t frequency) {
  if (frequency + 10600000 >= 11700000)
    return (float)(frequency + 10600000) / 1000000;

  return (float)(frequency + 9750000) / 1000000;
}

// Render compact DVBv5 tuning parameters when the driver returns useful data.
static int device_fe_param_v5_line(const struct frontend_v5_snapshot *snapshot) {
  bool show_symbol_rate = should_show_symbol_rate(snapshot);
  bool has_useful_data = (snapshot->has_delivery_system && snapshot->delivery_system != SYS_UNDEFINED) ||
                         (snapshot->has_frequency && snapshot->frequency > 0) ||
                         show_symbol_rate ||
                         (snapshot->has_bandwidth_hz && snapshot->bandwidth_hz > 0);
  if (!has_useful_data)
    return 0;

  if (snapshot->has_delivery_system)
    print_field_if_fits("SYS:", delivery_system_name(snapshot->delivery_system));

  if (snapshot->has_frequency && snapshot->frequency > 0) {
    char frequency[32];
    if (snapshot->has_delivery_system && is_satellite_delivery(snapshot->delivery_system))
      snprintf(frequency, sizeof(frequency), "%5.3f GHz", satellite_frequency_ghz(snapshot->frequency));
    else if (snapshot->frequency > 10000000)
      snprintf(frequency, sizeof(frequency), "%5.3f MHz", (float)snapshot->frequency / 1000000);
    else
      snprintf(frequency, sizeof(frequency), "%5.3f MHz", (float)snapshot->frequency / 1000);
    print_field_if_fits("FREQ:", frequency);
  }

  if (show_symbol_rate) {
    char symbol_rate[32];
    snprintf(symbol_rate, sizeof(symbol_rate), "%5.3f Msym/s", (float)snapshot->symbol_rate / 1000000);
    print_field_if_fits("SR:", symbol_rate);
  }

  if (snapshot->has_bandwidth_hz && snapshot->bandwidth_hz > 0) {
    char bandwidth[32];
    snprintf(bandwidth, sizeof(bandwidth), "%5.3f MHz", (float)snapshot->bandwidth_hz / 1000000);
    print_field_if_fits("BW:", bandwidth);
  }

  if (snapshot->has_inner_fec)
    print_field_if_fits("FEC:", code_rate_name(snapshot->inner_fec));
  if (snapshot->has_modulation)
    print_field_if_fits("MOD:", modulation_name(snapshot->modulation));

  full_line();

  return 1;
}

// Render the parameter row, including an explicit message when DVBv5 data is absent.
static int device_fe_param_line(const struct frontend_status_snapshot *snapshot) {
  if (snapshot->has_v5 && device_fe_param_v5_line(&snapshot->v5) > 0)
    return 1;

  DESC_COL_ON;
  printw("PARAMS: ");
  DESC_COL_OFF;
  DESC_VALL_ON;
  printw("DVBv5 properties unavailable");
  DESC_VALL_OFF;
  full_line();

  return 1;
}

// Render all frontend-only rows and return the next free screen line.
unsigned int render_frontend_status_lines(const struct frontend_status_snapshot *frontend_status, const char *frontend_path, unsigned int line) {
  move(line, 0);
  line += device_info_line(frontend_status, frontend_path);
  move(line, 0);
  line += device_status_line(frontend_status);
  move(line, 0);
  line += device_signal_line(frontend_status);
  move(line, 0);
  line += device_snr_line(frontend_status);
  move(line, 0);
  line += device_ber_block_line(frontend_status);
  move(line, 0);
  line += device_fe_param_line(frontend_status);

  return line;
}
