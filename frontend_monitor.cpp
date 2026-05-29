/*
 * File role: DVBv5 frontend property reader.
 *
 * Reads FE_GET_PROPERTY tuning parameters and statistics one property at a
 * time so older drivers can return partial DVBv5 data without hiding all fields.
 */

#include "frontend_monitor.h"

#include <linux/dvb/frontend.h>
#include <string.h>
#include <sys/ioctl.h>

// Thin wrapper around FE_GET_PROPERTY for one batch of DVBv5 properties.
static int get_frontend_properties(int fd, struct dtv_property *properties, unsigned int property_count) {
  struct dtv_properties props;

  memset(&props, 0, sizeof(props));
  props.num = property_count;
  props.props = properties;

  return ioctl(fd, FE_GET_PROPERTY, &props);
}

// A statistic is useful only when the driver returned at least one scaled value.
static bool property_has_stat(const struct dtv_property *property) {
  return property->u.st.len > 0 &&
         property->u.st.stat[0].scale != FE_SCALE_NOT_AVAILABLE;
}

// The monitor currently displays the first stat entry returned by the driver.
static struct dtv_stats property_first_stat(const struct dtv_property *property) {
  return property->u.st.stat[0];
}

// Read one DVBv5 scalar property without letting unsupported commands poison
// the rest of the frontend snapshot on older drivers.
static bool read_frontend_property_data(int fd, unsigned int command, uint32_t *value) {
  struct dtv_property property;

  memset(&property, 0, sizeof(property));
  property.cmd = command;

  if (get_frontend_properties(fd, &property, 1) < 0)
    return false;

  *value = property.u.data;

  return true;
}

// Read one DVBv5 statistic property and keep only scaled values.
static bool read_frontend_property_stat(int fd, unsigned int command, struct dtv_stats *stat) {
  struct dtv_property property;

  memset(&property, 0, sizeof(property));
  property.cmd = command;

  if (get_frontend_properties(fd, &property, 1) < 0)
    return false;

  if (!property_has_stat(&property))
    return false;

  *stat = property_first_stat(&property);

  return true;
}

// Read basic tuning parameters independently so one unsupported property does
// not hide all other PARAMS fields.
static bool read_frontend_v5_params(int fd, struct frontend_v5_snapshot *snapshot) {
  bool has_data = false;
  uint32_t value;

  if (read_frontend_property_data(fd, DTV_DELIVERY_SYSTEM, &value)) {
    snapshot->has_delivery_system = true;
    snapshot->delivery_system = (fe_delivery_system_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_FREQUENCY, &value)) {
    snapshot->has_frequency = true;
    snapshot->frequency = value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_MODULATION, &value)) {
    snapshot->has_modulation = true;
    snapshot->modulation = (fe_modulation_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_BANDWIDTH_HZ, &value)) {
    snapshot->has_bandwidth_hz = true;
    snapshot->bandwidth_hz = value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_SYMBOL_RATE, &value)) {
    snapshot->has_symbol_rate = true;
    snapshot->symbol_rate = value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_INNER_FEC, &value)) {
    snapshot->has_inner_fec = true;
    snapshot->inner_fec = (fe_code_rate_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_CODE_RATE_HP, &value)) {
    snapshot->has_code_rate_hp = true;
    snapshot->code_rate_hp = (fe_code_rate_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_CODE_RATE_LP, &value)) {
    snapshot->has_code_rate_lp = true;
    snapshot->code_rate_lp = (fe_code_rate_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_GUARD_INTERVAL, &value)) {
    snapshot->has_guard_interval = true;
    snapshot->guard_interval = (fe_guard_interval_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_TRANSMISSION_MODE, &value)) {
    snapshot->has_transmission_mode = true;
    snapshot->transmission_mode = (fe_transmit_mode_t)value;
    has_data = true;
  }

  if (read_frontend_property_data(fd, DTV_HIERARCHY, &value)) {
    snapshot->has_hierarchy = true;
    snapshot->hierarchy = (fe_hierarchy_t)value;
    has_data = true;
  }

  return has_data;
}

// Read optional DVBv5 statistics independently; failure leaves PARAMS intact.
static bool read_frontend_v5_stats(int fd, struct frontend_v5_snapshot *snapshot) {
  bool has_data = false;

  if (read_frontend_property_stat(fd, DTV_STAT_SIGNAL_STRENGTH, &snapshot->signal_strength)) {
    snapshot->has_signal_strength = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_CNR, &snapshot->cnr)) {
    snapshot->has_cnr = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_PRE_ERROR_BIT_COUNT, &snapshot->pre_error_bit_count)) {
    snapshot->has_pre_error_bit_count = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_PRE_TOTAL_BIT_COUNT, &snapshot->pre_total_bit_count)) {
    snapshot->has_pre_total_bit_count = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_POST_ERROR_BIT_COUNT, &snapshot->post_error_bit_count)) {
    snapshot->has_post_error_bit_count = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_POST_TOTAL_BIT_COUNT, &snapshot->post_total_bit_count)) {
    snapshot->has_post_total_bit_count = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_ERROR_BLOCK_COUNT, &snapshot->error_block_count)) {
    snapshot->has_error_block_count = true;
    has_data = true;
  }

  if (read_frontend_property_stat(fd, DTV_STAT_TOTAL_BLOCK_COUNT, &snapshot->total_block_count)) {
    snapshot->has_total_block_count = true;
    has_data = true;
  }

  return has_data;
}

// Read DVBv5 frontend data. Each property is isolated because older drivers may
// reject newer commands while supporting other DVBv5 values.
int read_frontend_v5_snapshot(int fd, struct frontend_v5_snapshot *snapshot) {
  if (fd < 0 || snapshot == NULL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));

  bool has_params = read_frontend_v5_params(fd, snapshot);
  bool has_stats = read_frontend_v5_stats(fd, snapshot);

  if (!has_params && !has_stats)
    return -1;

  return 0;
}
