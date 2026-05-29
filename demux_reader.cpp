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
#define SDT_READ_ATTEMPT_LIMIT 8

// Read the PSI section_length field directly from raw section bytes.
static int psi_section_length(const unsigned char *data) {
  return ((data[1] & 0x0f) << 8) | data[2];
}

// Return the full section byte length, including the 3-byte PSI header.
static int psi_section_total_length(const unsigned char *data) {
  return 3 + psi_section_length(data);
}

// Replace the cached bytes for one PID. Readers hold data_lock while copying
// data out to a UI snapshot, so the UI never walks a partially updated table.
static void store_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned char *data, int len) {
  if (!demux_has_pid_data(dvb_data, pid, 0) || len <= 0)
    return;
  if (len > DEMUX_SECTION_DATA_SIZE)
    len = DEMUX_SECTION_DATA_SIZE;

  pthread_mutex_lock(&dvb_data->data_lock);
  memcpy(dvb_data->pid_data[pid].data, data, len);
  dvb_data->pid_data[pid].len = len;
  pthread_mutex_unlock(&dvb_data->data_lock);
}

// Clear all previously collected SDT sections when the SDT version changes.
static void reset_sdt_cache(struct sdt_section_cache_s *cache, int table_id, int transport_stream_id, int version_number, int last_section_number) {
  memset(cache, 0, sizeof(*cache));
  cache->initialized = 1;
  cache->complete = 0;
  cache->table_id = table_id;
  cache->transport_stream_id = transport_stream_id;
  cache->version_number = version_number;
  cache->last_section_number = last_section_number;
}

// Store one actual-transport SDT section by section_number.
static void store_sdt_section(struct dvb_data_s *dvb_data, unsigned char *data, int len) {
  if (dvb_data == NULL || dvb_data->sdt_cache == NULL || len < SDT_SECT_HEADER_LEN)
    return;

  int total_len = psi_section_total_length(data);
  if (total_len > len || total_len > DEMUX_SECTION_DATA_SIZE)
    return;

  int table_id = data[0];
  int current_next = data[5] & 0x01;
  int section_number = data[6];
  int last_section_number = data[7];
  if (table_id != SDT_TABLE_ID_ACTUAL || !current_next)
    return;
  if (section_number > last_section_number || last_section_number >= DEMUX_MAX_SDT_SECTIONS)
    return;

  int transport_stream_id = (data[3] << 8) | data[4];
  int version_number = (data[5] >> 1) & 0x1f;

  pthread_mutex_lock(&dvb_data->data_lock);

  struct sdt_section_cache_s *cache = dvb_data->sdt_cache;
  if (!cache->initialized ||
      cache->table_id != table_id ||
      cache->transport_stream_id != transport_stream_id ||
      cache->version_number != version_number ||
      cache->last_section_number != last_section_number) {
    reset_sdt_cache(cache, table_id, transport_stream_id, version_number, last_section_number);
  }

  memcpy(cache->sections[section_number].data, data, total_len);
  cache->sections[section_number].len = total_len;
  cache->complete = 1;

  for (int current_section = 0; current_section <= cache->last_section_number; current_section++) {
    if (cache->sections[current_section].len == 0) {
      cache->complete = 0;
      break;
    }
  }

  pthread_mutex_unlock(&dvb_data->data_lock);
}

// Check whether all SDT sections for the current version have arrived.
static int sdt_cache_complete(struct dvb_data_s *dvb_data) {
  int complete = 0;

  pthread_mutex_lock(&dvb_data->data_lock);

  struct sdt_section_cache_s *cache = dvb_data->sdt_cache;
  if (cache != NULL && cache->initialized)
    complete = cache->complete;

  pthread_mutex_unlock(&dvb_data->data_lock);

  return complete;
}

// Return how many SDT sections the reader should try to collect in this pass.
static int sdt_expected_section_count(struct dvb_data_s *dvb_data) {
  int section_count = 1;

  pthread_mutex_lock(&dvb_data->data_lock);

  struct sdt_section_cache_s *cache = dvb_data->sdt_cache;
  if (cache != NULL && cache->initialized)
    section_count = cache->last_section_number + 1;

  pthread_mutex_unlock(&dvb_data->data_lock);

  if (section_count > SDT_READ_ATTEMPT_LIMIT)
    section_count = SDT_READ_ATTEMPT_LIMIT;

  return section_count;
}

// Read one filtered PSI/SI section for a PID from the demux device.
static int read_pid_filtered(struct dvb_data_s *dvb_data, unsigned char *data, int size_data, int pid, int table_id, int table_mask) {
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
  if (table_id >= 0) {
    filter_params.filter.filter[0] = table_id;
    filter_params.filter.mask[0] = table_mask;
  }

  if (ioctl(dvb_data->defd, DMX_SET_FILTER, &filter_params) < 0) {
    return -1;
  }

  int len = read(dvb_data->defd, data, size_data);
  if (demux_stop_requested(dvb_data))
    return 0;

  return len;
}

// Read one PSI/SI section without table_id filtering.
static int read_pid(struct dvb_data_s *dvb_data, unsigned char *data, int size_data, int pid) {
  return read_pid_filtered(dvb_data, data, size_data, pid, -1, 0);
}

// Read one or more SDT sections so service lists spanning sections become stable.
static void refresh_sdt_sections(struct dvb_data_s *dvb_data, unsigned char *data, int size_data) {
  int max_reads = 1;

  for (int read_attempt = 0; read_attempt < max_reads && !demux_stop_requested(dvb_data); read_attempt++) {
    int len = read_pid_filtered(dvb_data, data, size_data, SDT_PID, SDT_TABLE_ID_ACTUAL, 0xff);
    if (len <= 0)
      return;

    // Keep the last raw SDT section available for diagnostics/fallbacks, then
    // update the multi-section cache used by the detail view.
    store_pid_data(dvb_data, SDT_PID, data, len);
    store_sdt_section(dvb_data, data, len);
    if (sdt_cache_complete(dvb_data))
      return;

    max_reads = sdt_expected_section_count(dvb_data);
  }
}

// Demux reader loop: refresh PAT/PMT/NIT/SDT caches until shutdown is requested.
static void *read_dvb(void *par) {
  struct dvb_data_s *dvb_data = (struct dvb_data_s *)par;

  if (dvb_data == NULL || dvb_data->pid_data == NULL || dvb_data->sdt_cache == NULL)
    return NULL;

  while (!demux_stop_requested(dvb_data)) {
    unsigned char data[DEMUX_SECTION_DATA_SIZE];
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
    refresh_sdt_sections(dvb_data, data, sizeof(data));
  }

  return NULL;
}

// Start the per-device demux reader thread after discovery opens a demux fd.
int start_dvb_reader(struct dvb_data_s *dvb_data) {
  if (dvb_data == NULL || dvb_data->defd < 0 || dvb_data->pid_data == NULL || dvb_data->sdt_cache == NULL)
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
