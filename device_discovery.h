#ifndef DEVICE_DISCOVERY_H
#define DEVICE_DISCOVERY_H

#include <stddef.h>

#include "demux_monitor.h"

#define DVB_MAX_ADAPTERS 30
#define DVB_MAX_SUBADAPTERS 4
#define DVB_DEVICE_COUNT (DVB_MAX_ADAPTERS * DVB_MAX_SUBADAPTERS)

// Historical deployments used adapter10 and higher.
// #define DVB_DEFAULT_MIN_ADAPTER  10
#define DVB_DEFAULT_MIN_ADAPTER 0
#define DVB_DEFAULT_MAX_ADAPTER DVB_MAX_ADAPTERS

// Adapter/subadapter scan selection used during startup device discovery.
struct dvb_scan_config {
  int min_adapter;
  int max_adapter;
  int max_subadapter;
  bool adapter_filter_enabled;
  bool adapter_enabled[DVB_MAX_ADAPTERS];
};

// Prepare, discover, and release the DVB device table owned by main().
void init_dvb_devices(struct dvb_data_s *dvb_data, int device_count);
int discover_dvb_devices(struct dvb_data_s *dvb_data, int device_count, const struct dvb_scan_config *config);
void cleanup_dvb_devices(struct dvb_data_s *dvb_data, int device_count);

// Helpers for translating adapter/subadapter coordinates into paths/table slots.
int dvb_device_index(int adapter, int subadapter, int max_subadapter);
bool dvb_scan_adapter_enabled(const struct dvb_scan_config *config, int adapter);
void format_scan_adapter_selection(const struct dvb_scan_config *config, char *buffer, size_t buffer_size);
void format_frontend_path(char *buffer, size_t buffer_size, int adapter, int subadapter);
void format_demux_path(char *buffer, size_t buffer_size, int adapter, int subadapter);

#endif
