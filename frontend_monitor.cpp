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

// Keep DVBv5 property collection in one ioctl batch so the UI sees a coherent
// frontend snapshot for the current refresh.
int read_frontend_v5_snapshot(int fd, struct frontend_v5_snapshot *snapshot) {
  if (fd < 0 || snapshot == NULL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));

  struct dtv_property properties[19];
  memset(properties, 0, sizeof(properties));
  properties[0].cmd = DTV_DELIVERY_SYSTEM;
  properties[1].cmd = DTV_FREQUENCY;
  properties[2].cmd = DTV_MODULATION;
  properties[3].cmd = DTV_BANDWIDTH_HZ;
  properties[4].cmd = DTV_SYMBOL_RATE;
  properties[5].cmd = DTV_INNER_FEC;
  properties[6].cmd = DTV_CODE_RATE_HP;
  properties[7].cmd = DTV_CODE_RATE_LP;
  properties[8].cmd = DTV_GUARD_INTERVAL;
  properties[9].cmd = DTV_TRANSMISSION_MODE;
  properties[10].cmd = DTV_HIERARCHY;
  properties[11].cmd = DTV_STAT_SIGNAL_STRENGTH;
  properties[12].cmd = DTV_STAT_CNR;
  properties[13].cmd = DTV_STAT_PRE_ERROR_BIT_COUNT;
  properties[14].cmd = DTV_STAT_PRE_TOTAL_BIT_COUNT;
  properties[15].cmd = DTV_STAT_POST_ERROR_BIT_COUNT;
  properties[16].cmd = DTV_STAT_POST_TOTAL_BIT_COUNT;
  properties[17].cmd = DTV_STAT_ERROR_BLOCK_COUNT;
  properties[18].cmd = DTV_STAT_TOTAL_BLOCK_COUNT;

  if (get_frontend_properties(fd, properties, sizeof(properties) / sizeof(properties[0])) < 0)
    return -1;

  snapshot->has_delivery_system = true;
  snapshot->delivery_system = (fe_delivery_system_t)properties[0].u.data;
  snapshot->has_frequency = true;
  snapshot->frequency = properties[1].u.data;
  snapshot->has_modulation = true;
  snapshot->modulation = (fe_modulation_t)properties[2].u.data;
  snapshot->has_bandwidth_hz = true;
  snapshot->bandwidth_hz = properties[3].u.data;
  snapshot->has_symbol_rate = true;
  snapshot->symbol_rate = properties[4].u.data;
  snapshot->has_inner_fec = true;
  snapshot->inner_fec = (fe_code_rate_t)properties[5].u.data;
  snapshot->has_code_rate_hp = true;
  snapshot->code_rate_hp = (fe_code_rate_t)properties[6].u.data;
  snapshot->has_code_rate_lp = true;
  snapshot->code_rate_lp = (fe_code_rate_t)properties[7].u.data;
  snapshot->has_guard_interval = true;
  snapshot->guard_interval = (fe_guard_interval_t)properties[8].u.data;
  snapshot->has_transmission_mode = true;
  snapshot->transmission_mode = (fe_transmit_mode_t)properties[9].u.data;
  snapshot->has_hierarchy = true;
  snapshot->hierarchy = (fe_hierarchy_t)properties[10].u.data;

  if (property_has_stat(&properties[11])) {
    snapshot->has_signal_strength = true;
    snapshot->signal_strength = property_first_stat(&properties[11]);
  }

  if (property_has_stat(&properties[12])) {
    snapshot->has_cnr = true;
    snapshot->cnr = property_first_stat(&properties[12]);
  }

  if (property_has_stat(&properties[13])) {
    snapshot->has_pre_error_bit_count = true;
    snapshot->pre_error_bit_count = property_first_stat(&properties[13]);
  }

  if (property_has_stat(&properties[14])) {
    snapshot->has_pre_total_bit_count = true;
    snapshot->pre_total_bit_count = property_first_stat(&properties[14]);
  }

  if (property_has_stat(&properties[15])) {
    snapshot->has_post_error_bit_count = true;
    snapshot->post_error_bit_count = property_first_stat(&properties[15]);
  }

  if (property_has_stat(&properties[16])) {
    snapshot->has_post_total_bit_count = true;
    snapshot->post_total_bit_count = property_first_stat(&properties[16]);
  }

  if (property_has_stat(&properties[17])) {
    snapshot->has_error_block_count = true;
    snapshot->error_block_count = property_first_stat(&properties[17]);
  }

  if (property_has_stat(&properties[18])) {
    snapshot->has_total_block_count = true;
    snapshot->total_block_count = property_first_stat(&properties[18]);
  }

  return 0;
}
