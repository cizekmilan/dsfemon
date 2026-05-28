#include "demux_internal.h"

#include <stdint.h>
#include <string.h>

#include "nit_table.h"
#include "pat_table.h"
#include "pmt_table.h"
#include "sdt_table.h"

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
  if (!demux_has_pid_data(dvb_data, pid, header_len))
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
  if (section_number_i < 0 || !demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
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

// Return the end offset of usable SDT service entries, excluding the PSI CRC.
static int sdt_services_end(struct dvb_data_s *dvb_data) {
  return psi_payload_end(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN);
}

// Locate one SDT service entry and optionally return its descriptor loop end.
static bool find_sdt_part(struct dvb_data_s *dvb_data, int section_number_i, int *part_pointer, int *descriptor_loop_end) {
  if (section_number_i < 0 || !demux_has_pid_data(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN))
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

// Count PAT program entries available in the cached PAT section.
int si_count_pat_programs(struct dvb_data_s *dvb_data) {
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
int si_pat_program_pid(struct dvb_data_s *dvb_data, int pat_section) {
  int pointer;

  if (!find_pat_part(dvb_data, pat_section, &pointer))
    return -1;

  return read_13_bit_pid(dvb_data->pid_data[PAT_PID].data, pointer + 2);
}

// Find the PMT PID for one PAT program_number/service_id.
int si_find_program_pid(struct dvb_data_s *dvb_data, int program_number) {

  for (int pat_section = 0; pat_section < si_count_pat_programs(dvb_data); pat_section++) {
    if (pat_program_number(dvb_data, pat_section) == program_number)
      return si_pat_program_pid(dvb_data, pat_section);
  }

  return -1;
}

// Count elementary stream entries in a cached PMT section.
int count_pmt_streams(struct dvb_data_s *dvb_data, int program_pid) {
  if (!demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
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
int si_find_nit_pid(struct dvb_data_s *dvb_data) {

  for (int pat_section = 0; pat_section < si_count_pat_programs(dvb_data); pat_section++) {
    int program_number = pat_program_number(dvb_data, pat_section);
    int program_pid = si_pat_program_pid(dvb_data, pat_section);
    if (program_number == NIT_PROGRAM_NUMBER)
      return program_pid;
  }

  return -1;
}

// Extract the NIT network_name_descriptor text from the network descriptor loop.
int si_read_nit_network_name(struct dvb_data_s *dvb_data, int program_pid, char *network_name) {
  if (!demux_has_pid_data(dvb_data, program_pid, NIT_SECT_HEADER_LEN))
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
int si_count_sdt_services(struct dvb_data_s *dvb_data) {
  if (!demux_has_pid_data(dvb_data, SDT_PID, SDT_SECT_HEADER_LEN))
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
int si_sdt_service_id(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;

  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return read_u16_be(dvb_data->pid_data[SDT_PID].data, pointer);
}

// Return the running_status flag for one SDT service entry.
int si_sdt_service_running_status(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;

  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return (dvb_data->pid_data[SDT_PID].data[pointer + 3] >> 5) & 0x07;
}

// Return the free_CA_mode flag for one SDT service entry.
int si_sdt_service_free_ca_mode(struct dvb_data_s *dvb_data, int service_index) {
  int pointer;

  if (!find_sdt_part(dvb_data, service_index, &pointer, NULL))
    return -1;

  return (dvb_data->pid_data[SDT_PID].data[pointer + 3] >> 4) & 0x01;
}

// Extract the service descriptor name for one SDT service entry.
int si_read_sdt_service_name(struct dvb_data_s *dvb_data, int service_index, char *service_name) {
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
