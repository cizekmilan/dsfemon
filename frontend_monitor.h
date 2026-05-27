#ifndef FRONTEND_MONITOR_H
#define FRONTEND_MONITOR_H

#include <linux/dvb/frontend.h>
#include <stdbool.h>
#include <stdint.h>

// Raw DVBv5 frontend properties and statistics returned by FE_GET_PROPERTY.
// Each value has a matching has_* flag because drivers may omit fields.
struct frontend_v5_snapshot {
  bool has_delivery_system;
  bool has_frequency;
  bool has_modulation;
  bool has_bandwidth_hz;
  bool has_symbol_rate;
  bool has_inner_fec;
  bool has_code_rate_hp;
  bool has_code_rate_lp;
  bool has_guard_interval;
  bool has_transmission_mode;
  bool has_hierarchy;
  bool has_signal_strength;
  bool has_cnr;
  bool has_pre_error_bit_count;
  bool has_pre_total_bit_count;
  bool has_post_error_bit_count;
  bool has_post_total_bit_count;
  bool has_error_block_count;
  bool has_total_block_count;

  fe_delivery_system_t delivery_system;
  uint32_t frequency;
  fe_modulation_t modulation;
  uint32_t bandwidth_hz;
  uint32_t symbol_rate;
  fe_code_rate_t inner_fec;
  fe_code_rate_t code_rate_hp;
  fe_code_rate_t code_rate_lp;
  fe_guard_interval_t guard_interval;
  fe_transmit_mode_t transmission_mode;
  fe_hierarchy_t hierarchy;
  struct dtv_stats signal_strength;
  struct dtv_stats cnr;
  struct dtv_stats pre_error_bit_count;
  struct dtv_stats pre_total_bit_count;
  struct dtv_stats post_error_bit_count;
  struct dtv_stats post_total_bit_count;
  struct dtv_stats error_block_count;
  struct dtv_stats total_block_count;
};

// Read one DVBv5 property batch from an open frontend file descriptor.
int read_frontend_v5_snapshot(int fd, struct frontend_v5_snapshot *snapshot);

#endif
