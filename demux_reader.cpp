/*
 * File role: background demux section reader.
 *
 * Reads PAT, PMT, NIT, and SDT sections from the Linux DVB demux device and
 * stores raw, CRC-checked section bytes in the per-device PID cache.
 */

#include "demux_internal.h"

#include <linux/dvb/dmx.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "pat_table.h"
#include "sdt_table.h"

#define DEMUX_READ_TIMEOUT_MS 1000
#define TS_BUFFER_SIZE (256 * 1024)

// Replace the cached bytes for one PID. Readers hold data_lock while copying
// data out to a UI snapshot, so the UI never walks a partially updated table.
static void store_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned char *data, int len) {
  if (!demux_has_pid_data(dvb_data, pid, 0) || len <= 0)
    return;
  if (len > (int)sizeof(dvb_data->pid_data[pid].data))
    len = sizeof(dvb_data->pid_data[pid].data);

  pthread_mutex_lock(&dvb_data->data_lock);
  memcpy(dvb_data->pid_data[pid].data, data, len);
  dvb_data->pid_data[pid].len = len;
  pthread_mutex_unlock(&dvb_data->data_lock);
}

// Read one filtered PSI/SI section for a PID from the demux device.
static int read_pid(struct dvb_data_s *dvb_data, unsigned char *data, int size_data, int pid) {
  if (demux_stop_requested(dvb_data) || dvb_data->defd < 0 || !demux_valid_pid(pid))
    return 0;

  long dmx_buffer_size = TS_BUFFER_SIZE;

  if (ioctl(dvb_data->defd, DMX_SET_BUFFER_SIZE, dmx_buffer_size) < 0) {
    return -1;
  }

  struct dmx_sct_filter_params filter_params;
  memset(&filter_params, 0, sizeof(struct dmx_sct_filter_params));
  filter_params.pid = pid;
  filter_params.timeout = DEMUX_READ_TIMEOUT_MS;
  filter_params.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;

  if (ioctl(dvb_data->defd, DMX_SET_FILTER, &filter_params) < 0) {
    return -1;
  }

  int len = read(dvb_data->defd, data, size_data);
  if (demux_stop_requested(dvb_data))
    return 0;

  return len;
}

// Demux reader loop: refresh PAT/PMT/NIT/SDT caches until shutdown is requested.
static void *read_dvb(void *par) {
  struct dvb_data_s *dvb_data = (struct dvb_data_s *)par;

  if (dvb_data == NULL || dvb_data->pid_data == NULL)
    return NULL;

  while (!demux_stop_requested(dvb_data)) {
    unsigned char data[TS_SIZE * 5];
    int len;

    // Read PAT first; it tells us which PMT/NIT PIDs to refresh next.
    len = read_pid(dvb_data, data, sizeof(data), PAT_PID);
    if (len <= 0)
      continue;
    store_pid_data(dvb_data, PAT_PID, data, len);

    // Program 0 points to NIT; other program entries point to PMT sections.
    for (int pat_section = 0; pat_section < si_count_pat_programs(dvb_data); pat_section++) {
      int program_pid = si_pat_program_pid(dvb_data, pat_section);
      if (!demux_valid_pid(program_pid))
        continue;
      if (demux_stop_requested(dvb_data))
        break;

      len = read_pid(dvb_data, data, sizeof(data), program_pid);
      if (len <= 0)
        continue;
      store_pid_data(dvb_data, program_pid, data, len);
    }

    // SDT carries service names and service-level flags.
    if (demux_stop_requested(dvb_data))
      break;
    len = read_pid(dvb_data, data, sizeof(data), SDT_PID);
    if (len <= 0)
      continue;
    store_pid_data(dvb_data, SDT_PID, data, len);
  }

  return NULL;
}

// Start the per-device demux reader thread after discovery opens a demux fd.
int start_dvb_reader(struct dvb_data_s *dvb_data) {
  if (dvb_data == NULL || dvb_data->defd < 0 || dvb_data->pid_data == NULL)
    return -1;
  dvb_data->stop_demux_thread = 0;
  if (pthread_create(&dvb_data->demux_thread, NULL, read_dvb, dvb_data) != 0)
    return -1;
  dvb_data->demux_thread_started = 1;

  return 0;
}

// Ask the reader thread to stop and interrupt any active demux filter.
void request_dvb_reader_stop(struct dvb_data_s *dvb_data) {
  if (dvb_data == NULL)
    return;
  dvb_data->stop_demux_thread = 1;
  if (dvb_data->defd >= 0)
    ioctl(dvb_data->defd, DMX_STOP);
}

// Wait for the reader thread before freeing its cached PID data.
void join_dvb_reader(struct dvb_data_s *dvb_data) {
  if (dvb_data == NULL || !dvb_data->demux_thread_started)
    return;
  pthread_join(dvb_data->demux_thread, NULL);
  dvb_data->demux_thread_started = 0;
}
