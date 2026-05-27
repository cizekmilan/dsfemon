#include <stdio.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>

#include <ncurses.h>

#include "color.h"
#include "demux_monitor.h"
#include "pat_table.h"
#include "nit_table.h"
#include "pmt_table.h"
#include "sdt_table.h"

#define DEMUX_READ_TIMEOUT_MS 1000
#define TS_BUFFER_SIZE (256 * 1024)

// Validate MPEG-TS PID array indexes before touching pid_data.
static int valid_pid(int pid) {
  return pid >= 0 && pid <= TS_MAX_PID;
}

// Shared stop predicate used by blocking demux reads and the reader loop.
static int demux_stop_requested(struct dvb_data_s *dvb_data) {
  return dvb_data == NULL || dvb_data->stop_demux_thread;
}

// Check that cached bytes for a PID exist and contain the requested minimum.
static int has_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned int min_len) {
  return dvb_data != NULL &&
         dvb_data->pid_data != NULL &&
         valid_pid(pid) &&
         dvb_data->pid_data[pid].len >= min_len;
}

// Tiny local helper kept here to avoid pulling in C++ utility headers.
static int min_int(int a, int b) {
  return a < b ? a : b;
}

// Read a big-endian 16-bit value from PSI/SI bytes.
static uint16_t read_u16_be(const unsigned char *data, int offset) {
  return ((uint16_t)data[offset] << 8) | data[offset + 1];
}

// Read the common 12-bit length field used by PSI sections/descriptors.
static int read_12_bit_length(const unsigned char *data, int offset) {
  return ((data[offset] & 0x0f) << 8) | data[offset + 1];
}

// Read a 13-bit PID field, masking out reserved high bits.
static int read_13_bit_pid(const unsigned char *data, int offset) {
  return ((data[offset] & 0x1f) << 8) | data[offset + 1];
}

// Read the PSI section_length field from the section header.
static int psi_section_length(const unsigned char *data) {
  return read_12_bit_length(data, 1);
}

// Return the usable PSI payload end offset, excluding the trailing CRC32.
static int psi_payload_end(struct dvb_data_s *dvb_data, int pid, unsigned int header_len) {
  if (!has_pid_data(dvb_data, pid, header_len))
    return 0;

  int section_length = psi_section_length(dvb_data->pid_data[pid].data);
  if (section_length < 4)
    return 0;

  int end = 3 + section_length - 4; // Exclude the trailing PSI CRC32.
  if (end < (int)header_len)
    return 0;

  return min_int(end, dvb_data->pid_data[pid].len);
}

// Locate one PAT program entry. Each entry maps a program number to PMT/NIT PID.
static bool find_pat_part(struct dvb_data_s *dvb_data, int pat_section, int *part_pointer) {
  if (pat_section < 0)
    return false;

  int pointer = PAT_SECT_HEADER_LEN + pat_section * PAT_PART_SECT_LEN;
  int section_end = psi_payload_end(dvb_data, PAT_PID, PAT_SECT_HEADER_LEN);
  if (pointer + PAT_PART_SECT_LEN > section_end)
    return false;

  if (part_pointer != NULL)
    *part_pointer = pointer;

  return true;
}

// Locate one elementary stream entry inside a PMT program.
static bool find_pmt_part(struct dvb_data_s *dvb_data, int program_pid, int section_number_i, int *part_pointer) {
  if (section_number_i < 0 || !has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return false;

  const unsigned char *data = dvb_data->pid_data[program_pid].data;
  int pointer = PMT_SECT_HEADER_LEN + read_12_bit_length(data, 10);
  int section_end = psi_payload_end(dvb_data, program_pid, PMT_SECT_HEADER_LEN);
  int section_number = 0;

  while (pointer + PMT_PART_SECT_LEN <= section_end) {
    int es_info_len = read_12_bit_length(data, pointer + 3);
    int next_pointer = pointer + PMT_PART_SECT_LEN + es_info_len;
    if (next_pointer > section_end)
      break;

    if (section_number == section_number_i) {
      if (part_pointer != NULL)
        *part_pointer = pointer;
      return true;
    }

    section_number++;
    pointer = next_pointer;
  }

  return false;
}

static int sdt_services_end(struct dvb_data_s *dvb_data) {
  return psi_payload_end(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN);
}

// Locate one SDT service entry and optionally return its descriptor loop end.
static bool find_sdt_part(struct dvb_data_s *dvb_data, int section_number_i, int *part_pointer, int *descriptor_loop_end) {
  if (section_number_i < 0 || !has_pid_data(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN))
    return false;

  const unsigned char *data = dvb_data->pid_data[SDT_PID].data;
  int section_number = 0;
  int pointer = SDT_SECT_HEADER_LEN;
  int limit = sdt_services_end(dvb_data);

  while (pointer + SDT_PART_SECT_LEN <= limit) {
    int current_loop_end = pointer + SDT_PART_SECT_LEN + read_12_bit_length(data, pointer + 3);
    if (current_loop_end > limit)
      break;

    if (section_number_i == section_number) {
      if (part_pointer != NULL)
        *part_pointer = pointer;
      if (descriptor_loop_end != NULL)
        *descriptor_loop_end = current_loop_end;
      return true;
    }

    section_number++;
    pointer = current_loop_end;
  }

  return false;
}

// Replace the cached bytes for one PID. Readers hold data_lock while copying
// data out to a UI snapshot, so the UI never walks a partially updated table.
static void store_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned char *data, int len) {
  if (!has_pid_data(dvb_data, pid, 0) || len <= 0)
    return;
  if (len > (int)sizeof(dvb_data->pid_data[pid].data))
    len = sizeof(dvb_data->pid_data[pid].data);

  pthread_mutex_lock(&dvb_data->data_lock);
  memcpy(dvb_data->pid_data[pid].data, data, len);
  dvb_data->pid_data[pid].len = len;
  pthread_mutex_unlock(&dvb_data->data_lock);
}

// Developer/debug helper for inspecting raw PSI/SI bytes when needed.
void dump_hex(__u8 *buf, int size) {
  int i;
  unsigned char ch;
  char sascii[17];

  sascii[16] = 0x0;

  for (i = 0; i < size; i++) {
    ch = buf[i];

    if (i % 16 == 0) {
      printf("%04x ", i);
    }

    printf("%02x ", ch);
    if (ch >= ' ' && ch <= '}')
      sascii[i % 16] = ch;
    else
      sascii[i % 16] = '.';

    if (i % 16 == 15)
      printf("   %s\n", sascii);
  }

  // i++ after loop
  if (i % 16 != 0) {

    for (; i % 16 != 0; i++) {
      printf("   ");
      sascii[i % 16] = ' ';
    }

    printf("   %s\n", sascii);
  }
}

// Read one filtered PSI/SI section for a PID from the demux device.
int read_pid(struct dvb_data_s *dvb_data, unsigned char *data, int size_data, int pid) {
  if (demux_stop_requested(dvb_data) || dvb_data->defd < 0 || !valid_pid(pid))
    return 0;

  long dmx_buffer_size = TS_BUFFER_SIZE;

  if (ioctl(dvb_data->defd, DMX_SET_BUFFER_SIZE, dmx_buffer_size) < 0) {
    return -1;
  }

  struct dmx_sct_filter_params sctFilterParams;
  memset(&sctFilterParams, 0, sizeof(struct dmx_sct_filter_params));
  sctFilterParams.pid = pid;
  //    sctFilterParams.filter.filter[0] = 0x0;
  //    sctFilterParams.filter.mask[0] = 0xff;
  sctFilterParams.timeout = DEMUX_READ_TIMEOUT_MS;
  sctFilterParams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;

  if (ioctl(dvb_data->defd, DMX_SET_FILTER, &sctFilterParams) < 0) {
    return -1;
  }

  int len = read(dvb_data->defd, data, size_data);
  if (demux_stop_requested(dvb_data))
    return 0;

  return len;
}

// Count PAT program entries available in the cached PAT section.
static int count_pat_programs(struct dvb_data_s *dvb_data) {
  int section_end = psi_payload_end(dvb_data, PAT_PID, PAT_SECT_HEADER_LEN);
  if (section_end <= PAT_SECT_HEADER_LEN)
    return 0;

  return (section_end - PAT_SECT_HEADER_LEN) / PAT_PART_SECT_LEN;
}

// Return the PAT program_number for one PAT entry.
static int pat_program_number(struct dvb_data_s *dvb_data, int pat_section) {
  int pointer;
  if (!find_pat_part(dvb_data, pat_section, &pointer))
    return -1;

  return read_u16_be(dvb_data->pid_data[PAT_PID].data, pointer);
}

// Return the PMT/NIT PID referenced by one PAT entry.
static int pat_program_pid(struct dvb_data_s *dvb_data, int pat_section) {
  int pointer;
  if (!find_pat_part(dvb_data, pat_section, &pointer))
    return -1;

  return read_13_bit_pid(dvb_data->pid_data[PAT_PID].data, pointer + 2);
}

// Count elementary stream entries in a cached PMT section.
int count_pmt_streams(struct dvb_data_s *dvb_data, int program_pid) {
  if (!has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return 0;
  const unsigned char *data = dvb_data->pid_data[program_pid].data;

  int section_number = 0;
  int pointer = PMT_SECT_HEADER_LEN + read_12_bit_length(data, 10);
  int section_end = psi_payload_end(dvb_data, program_pid, PMT_SECT_HEADER_LEN);

  while (pointer + PMT_PART_SECT_LEN <= section_end) {
    int es_info_len = read_12_bit_length(data, pointer + 3);
    int next_pointer = pointer + PMT_PART_SECT_LEN + es_info_len;
    if (next_pointer > section_end)
      break;
    section_number++;
    pointer = next_pointer;
  }

  return section_number;
}

// Return the elementary PID for one PMT stream entry.
int pmt_stream_pid(struct dvb_data_s *dvb_data, int program_pid, int stream_index) {
  int pointer;
  if (!find_pmt_part(dvb_data, program_pid, stream_index, &pointer))
    return 0;

  return read_13_bit_pid(dvb_data->pid_data[program_pid].data, pointer + 1);
}

// Find the NIT PID by locating PAT program number 0.
static int find_nit_pid(struct dvb_data_s *dvb_data) {

  for (int pat_section = 0; pat_section < count_pat_programs(dvb_data); pat_section++) {
    int program_number = pat_program_number(dvb_data, pat_section);
    int program_pid = pat_program_pid(dvb_data, pat_section);
    if (program_number == NIT_PROGRAM_NUMBER)
      return program_pid;
  }

  return -1;
}

// Extract the NIT network_name_descriptor text from the network descriptor loop.
static int read_nit_network_name(struct dvb_data_s *dvb_data, int program_pid, char *network_name) {
  if (!has_pid_data(dvb_data, program_pid, NIT_SECT_HEADER_LEN))
    return 0;
  const unsigned char *data = dvb_data->pid_data[program_pid].data;

  int pointer = NIT_SECT_HEADER_LEN;
  int descriptors_end = pointer + read_12_bit_length(data, 8);
  int section_end = psi_payload_end(dvb_data, program_pid, NIT_SECT_HEADER_LEN);
  if (descriptors_end > section_end)
    descriptors_end = section_end;

  while (pointer + 2 <= descriptors_end) {
    int descriptor_tag = data[pointer];
    int descriptor_length = data[pointer + 1];
    int descriptor_end = pointer + 2 + descriptor_length;
    if (descriptor_end > descriptors_end)
      break;

    if (descriptor_tag == NIT_NETWORK_NAME_DESCRIPTOR) {
      int len = descriptor_length;
      if (len > DEMUX_NETWORK_NAME_SIZE - 1)
        len = DEMUX_NETWORK_NAME_SIZE - 1;
      memcpy(network_name, &data[pointer + 2], len);
      network_name[len] = '\0';
      return len;
    }

    pointer = descriptor_end;
  }

  return 0;
}

// Count SDT service entries available in the cached SDT section.
static int count_sdt_services(struct dvb_data_s *dvb_data) {
  if (!has_pid_data(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN))
    return 0;
  const unsigned char *data = dvb_data->pid_data[SDT_PID].data;

  int section_number = 0;

  int pointer = SDT_SECT_HEADER_LEN;
  int limit = sdt_services_end(dvb_data);

  while (pointer + SDT_PART_SECT_LEN <= limit) {
    int descriptor_loop_end = pointer + SDT_PART_SECT_LEN + read_12_bit_length(data, pointer + 3);
    if (descriptor_loop_end > limit)
      break;
    section_number++;
    pointer = descriptor_loop_end;
  }

  return section_number;
}

// Return the service_id for one SDT service entry.
static int sdt_service_id(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;
  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return read_u16_be(dvb_data->pid_data[SDT_PID].data, pointer);
}

// Return the running_status flag for one SDT service entry.
static int sdt_service_running_status(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;
  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return (dvb_data->pid_data[SDT_PID].data[pointer + 3] >> 5) & 0x07;
}

// Return the free_CA_mode flag for one SDT service entry.
static int sdt_service_free_ca_mode(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;
  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return (dvb_data->pid_data[SDT_PID].data[pointer + 3] >> 4) & 0x01;
}

// Extract the service descriptor name for one SDT service entry.
static int read_sdt_service_name(struct dvb_data_s *dvb_data, int service_index, char *service_name) {
  int part_pointer;
  int descriptor_loop_end;
  if (!find_sdt_part(dvb_data, service_index, &part_pointer, &descriptor_loop_end))
    return 0;

  int descriptor_pointer = part_pointer + SDT_PART_SECT_LEN;

  while (descriptor_pointer + 2 <= descriptor_loop_end) {
    int descriptor_tag = dvb_data->pid_data[SDT_PID].data[descriptor_pointer];
    int descriptor_length = dvb_data->pid_data[SDT_PID].data[descriptor_pointer + 1];
    int descriptor_end = descriptor_pointer + 2 + descriptor_length;
    if (descriptor_end > descriptor_loop_end)
      return 0;

    if (descriptor_tag == SERVICE_DESCRIPTOR) {
      int provider_name_length_pos = descriptor_pointer + 3;
      if (provider_name_length_pos >= descriptor_end)
        return 0;

      int service_provider_name_length = dvb_data->pid_data[SDT_PID].data[provider_name_length_pos];
      int service_name_length_pos = provider_name_length_pos + 1 + service_provider_name_length;
      if (service_name_length_pos >= descriptor_end)
        return 0;

      int service_name_length = dvb_data->pid_data[SDT_PID].data[service_name_length_pos];
      if (service_name_length_pos + 1 + service_name_length > descriptor_end)
        return 0;

      if (service_name_length > DEMUX_SERVICE_NAME_SIZE - 1)
        service_name_length = DEMUX_SERVICE_NAME_SIZE - 1;
      memcpy(service_name, &dvb_data->pid_data[SDT_PID].data[service_name_length_pos + 1], service_name_length);

      return service_name_length;
    }

    descriptor_pointer = descriptor_end;
  }

  return 0;
}

// Trim non-printable/whitespace padding from DVB text used by the UI.
static int clean_si_text(char *data, int len) {
  if (data == NULL || len <= 0)
    return 0;

  int start = 0;

  while (start < len) {
    unsigned char ch = (unsigned char)data[start];
    if (isprint(ch) && !isspace(ch))
      break;
    start++;
  }

  int end = len;

  while (end > start) {
    unsigned char ch = (unsigned char)data[end - 1];
    if (isprint(ch) && !isspace(ch))
      break;
    end--;
  }

  int clean_len = end - start;
  if (clean_len > 0 && start > 0)
    memmove(data, data + start, clean_len);

  data[clean_len] = '\0';

  return clean_len;
}

int read_demux_snapshot(struct dvb_data_s *dvb_data, struct demux_snapshot *snapshot) {
  if (dvb_data == NULL || snapshot == NULL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));

  // Copy only display-ready data while holding the lock; rendering uses
  // the snapshot without touching the live demux buffers.
  pthread_mutex_lock(&dvb_data->data_lock);

  snapshot->network_name_len = read_nit_network_name(dvb_data, find_nit_pid(dvb_data), snapshot->network_name);
  snapshot->network_name_len = clean_si_text(snapshot->network_name, snapshot->network_name_len);

  int service_count = count_sdt_services(dvb_data);

  for (int service_index = 0; service_index < service_count && snapshot->service_count < DEMUX_MAX_SERVICES; service_index++) {
    struct demux_service_snapshot *service = &snapshot->services[snapshot->service_count];

    service->name_len = read_sdt_service_name(dvb_data, service_index, service->name);
    service->name_len = clean_si_text(service->name, service->name_len);
    if (service->name_len <= 0)
      continue;

    service->service_id = sdt_service_id(dvb_data, service_index);
    service->running_status = sdt_service_running_status(dvb_data, service_index);
    service->free_ca_mode = sdt_service_free_ca_mode(dvb_data, service_index);
    snapshot->service_count++;
  }

  pthread_mutex_unlock(&dvb_data->data_lock);

  return 0;
}

// Demux reader loop: refresh PAT/PMT/NIT/SDT caches until shutdown is requested.
static void *read_dvb(void *par) {
  struct dvb_data_s *dvb_data = (struct dvb_data_s *)par;
  if (dvb_data == NULL || dvb_data->pid_data == NULL)
    return NULL;

  while (!demux_stop_requested(dvb_data)) {
    unsigned char data[TS_SIZE * 5];
    int len;

    // read PAT table
    len = read_pid(dvb_data, data, sizeof(data), PAT_PID);
    if (len <= 0)
      continue;
    store_pid_data(dvb_data, PAT_PID, data, len);

    // check PAT section and check PMT table
    for (int pat_section = 0; pat_section < count_pat_programs(dvb_data); pat_section++) {
      int program_pid = pat_program_pid(dvb_data, pat_section);
      if (!valid_pid(program_pid))
        continue;
      if (demux_stop_requested(dvb_data))
        break;

      // read PMT table and NIT table
      len = read_pid(dvb_data, data, sizeof(data), program_pid);
      if (len <= 0)
        continue;
      store_pid_data(dvb_data, program_pid, data, len);
    }

    // read SDT table
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

// Placeholder for the future interactive demux detail view.
int print_daemon_monitor(int adapter, int demux) {
  (void)adapter;
  (void)demux;
  int x, y;
  getyx(stdscr, y, x);
  (void)x;
  (void)y;
  printw("Sorry demux monitor is not inplemented now");

  return 1;
}
