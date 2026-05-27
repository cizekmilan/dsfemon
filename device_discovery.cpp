#include "device_discovery.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#define FRONTEND_DEVICE "/dev/dvb/adapter%d/frontend%d"
#define DEMUX_DEVICE "/dev/dvb/adapter%d/demux%d"

// Map adapter/subadapter coordinates to the flat dvb_data array index.
int dvb_device_index(int adapter, int subadapter, int max_subadapter) {
  return adapter * max_subadapter + subadapter;
}

// Check whether an adapter is part of the configured scan selection.
bool dvb_scan_adapter_enabled(const struct dvb_scan_config *config, int adapter) {
  if (adapter < config->min_adapter || adapter >= config->max_adapter)
    return false;

  if (!config->adapter_filter_enabled)
    return true;

  return config->adapter_enabled[adapter];
}

// Format the adapter selection for diagnostics shown before/inside ncurses.
void format_scan_adapter_selection(const struct dvb_scan_config *config, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0)
    return;

  int used = 0;

  if (!config->adapter_filter_enabled) {
    snprintf(buffer, buffer_size, "%d-%d", config->min_adapter, config->max_adapter - 1);

    return;
  }

  buffer[0] = '\0';

  for (int adapter = config->min_adapter; adapter < config->max_adapter; adapter++) {
    if (!dvb_scan_adapter_enabled(config, adapter))
      continue;

    int written = snprintf(buffer + used, buffer_size - used, "%s%d", used > 0 ? "," : "", adapter);
    if (written < 0)
      return;

    if ((size_t)written >= buffer_size - used)
      return;

    used += written;
  }

  if (used == 0)
    snprintf(buffer, buffer_size, "none");
}

// Build the frontend device path used by discovery and display.
void format_frontend_path(char *buffer, size_t buffer_size, int adapter, int subadapter) {
  snprintf(buffer, buffer_size, FRONTEND_DEVICE, adapter, subadapter);
}

// Build the demux device path paired with the frontend path.
void format_demux_path(char *buffer, size_t buffer_size, int adapter, int subadapter) {
  snprintf(buffer, buffer_size, DEMUX_DEVICE, adapter, subadapter);
}

// Initialize all slots to a closed/no-device state before discovery starts.
void init_dvb_devices(struct dvb_data_s *dvb_data, int device_count) {

  for (int i = 0; i < device_count; i++) {
    dvb_data[i].adapter = -1;
    dvb_data[i].subadapter = -1;
    dvb_data[i].fefd = -1;
    dvb_data[i].defd = -1;
    dvb_data[i].pid_data = NULL;
    dvb_data[i].demux_thread_started = 0;
    dvb_data[i].stop_demux_thread = 0;
    pthread_mutex_init(&dvb_data[i].data_lock, NULL);
  }
}

// Scan configured DVB adapter/subadapter paths and start demux readers where possible.
int discover_dvb_devices(struct dvb_data_s *dvb_data, int device_count, const struct dvb_scan_config *config) {
  int discovered_frontends = 0;

  for (int adapter = config->min_adapter; adapter < config->max_adapter; adapter++) {
    if (!dvb_scan_adapter_enabled(config, adapter))
      continue;

    for (int subadapter = 0; subadapter < config->max_subadapter; subadapter++) {
      int index = dvb_device_index(adapter, subadapter, config->max_subadapter);
      if (index < 0 || index >= device_count)
        continue;

      struct dvb_data_s *device = &dvb_data[index];
      device->adapter = adapter;
      device->subadapter = subadapter;

      char fedev[128];
      format_frontend_path(fedev, sizeof(fedev), adapter, subadapter);
      device->fefd = open(fedev, O_RDONLY | O_NONBLOCK);
      if (device->fefd >= 0)
        discovered_frontends++;

      char dedev[128];
      format_demux_path(dedev, sizeof(dedev), adapter, subadapter);
      device->defd = open(dedev, O_RDWR | O_LARGEFILE);

      if (device->defd >= 0) {
        device->pid_data = (pid_data_s *)calloc(TS_PID_COUNT, sizeof(*device->pid_data));
        if (device->pid_data == NULL)
          continue;

        if (start_dvb_reader(device) != 0) {
          free(device->pid_data);
          device->pid_data = NULL;
        }
      }
    }
  }

  return discovered_frontends;
}

// Stop reader threads, close file descriptors, and release per-device caches.
void cleanup_dvb_devices(struct dvb_data_s *dvb_data, int device_count) {

  for (int i = 0; i < device_count; i++)
    request_dvb_reader_stop(&dvb_data[i]);

  for (int i = 0; i < device_count; i++)
    join_dvb_reader(&dvb_data[i]);

  for (int i = 0; i < device_count; i++) {
    if (dvb_data[i].fefd >= 0) {
      close(dvb_data[i].fefd);
      dvb_data[i].fefd = -1;
    }

    if (dvb_data[i].defd >= 0) {
      close(dvb_data[i].defd);
      dvb_data[i].defd = -1;
    }

    free(dvb_data[i].pid_data);
    dvb_data[i].pid_data = NULL;
    pthread_mutex_destroy(&dvb_data[i].data_lock);
  }
}
