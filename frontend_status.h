#ifndef FRONTEND_STATUS_H
#define FRONTEND_STATUS_H

#include <linux/dvb/frontend.h>
#include <stdbool.h>
#include <stdint.h>

#include "frontend_monitor.h"

// UI-facing frontend snapshot collected once per refresh. It keeps legacy
// relative metrics for bars and DVBv5 values for modern labels/statistics.
struct frontend_status_snapshot {
  bool has_info;
  bool has_status;
  bool has_legacy_signal;
  bool has_legacy_snr;
  bool has_legacy_ber;
  bool has_legacy_uncorrected_blocks;
  bool has_v5;

  struct dvb_frontend_info info;
  fe_status_t status;
  uint16_t legacy_signal;
  uint16_t legacy_snr;
  uint32_t legacy_ber;
  uint32_t legacy_uncorrected_blocks;
  struct frontend_v5_snapshot v5;
};

// Collect legacy frontend ioctls and DVBv5 properties from one frontend fd.
int read_frontend_status_snapshot(int fd, struct frontend_status_snapshot *snapshot);

#endif
