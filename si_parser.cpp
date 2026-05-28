#include "demux_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
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

// Locate the PMT program descriptor loop before elementary stream entries.
static bool find_pmt_program_descriptors(struct dvb_data_s *dvb_data, int program_pid, int *descriptors_start, int *descriptors_end) {
  if (!demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return false;

  const unsigned char *data = dvb_data->pid_data[program_pid].data;
  int current_descriptors_start = PMT_SECT_HEADER_LEN;
  int current_descriptors_end = current_descriptors_start + read_12_bit_length(data, 10);
  int section_end = psi_payload_end(dvb_data, program_pid, PMT_SECT_HEADER_LEN);
  if (current_descriptors_end > section_end)
    return false;

  if (descriptors_start != NULL)
    *descriptors_start = current_descriptors_start;

  if (descriptors_end != NULL)
    *descriptors_end = current_descriptors_end;

  return true;
}

// Locate one elementary stream descriptor loop inside a PMT program.
static bool find_pmt_stream_descriptors(struct dvb_data_s *dvb_data, int program_pid, int stream_index, int *descriptors_start, int *descriptors_end) {
  int part_pointer;

  if (!find_pmt_part(dvb_data, program_pid, stream_index, &part_pointer))
    return false;

  int current_descriptors_start = part_pointer + PMT_PART_SECT_LEN;
  int current_descriptors_end = current_descriptors_start + read_12_bit_length(dvb_data->pid_data[program_pid].data, part_pointer + 3);

  if (descriptors_start != NULL)
    *descriptors_start = current_descriptors_start;

  if (descriptors_end != NULL)
    *descriptors_end = current_descriptors_end;

  return true;
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

// Locate the standard SDT service descriptor for one service entry.
static bool find_sdt_service_descriptor(struct dvb_data_s *dvb_data, int service_index, int *descriptor_pointer, int *descriptor_end) {
  int part_pointer;
  int descriptor_loop_end;

  if (!find_sdt_part(dvb_data, service_index, &part_pointer, &descriptor_loop_end))
    return false;

  int pointer = part_pointer + SDT_PART_SECT_LEN;

  while (pointer + 2 <= descriptor_loop_end) {
    int current_descriptor_tag = dvb_data->pid_data[SDT_PID].data[pointer];
    int current_descriptor_length = dvb_data->pid_data[SDT_PID].data[pointer + 1];
    int current_descriptor_end = pointer + 2 + current_descriptor_length;
    if (current_descriptor_end > descriptor_loop_end)
      return false;

    if (current_descriptor_tag == SERVICE_DESCRIPTOR) {
      if (descriptor_pointer != NULL)
        *descriptor_pointer = pointer;

      if (descriptor_end != NULL)
        *descriptor_end = current_descriptor_end;

      return true;
    }

    pointer = current_descriptor_end;
  }

  return false;
}

// Keep language columns compact by translating common ISO 639-2 codes to two letters.
static void format_iso639_language_code(const unsigned char *raw_code, char *language, size_t language_size) {
  char iso_code[4];

  if (language_size == 0)
    return;

  for (int i = 0; i < 3; i++)
    iso_code[i] = (char)tolower(raw_code[i]);
  iso_code[3] = '\0';

  if (strcmp(iso_code, "ces") == 0 || strcmp(iso_code, "cze") == 0)
    snprintf(language, language_size, "cs");
  else if (strcmp(iso_code, "eng") == 0)
    snprintf(language, language_size, "en");
  else if (strcmp(iso_code, "deu") == 0 || strcmp(iso_code, "ger") == 0)
    snprintf(language, language_size, "de");
  else if (strcmp(iso_code, "slk") == 0 || strcmp(iso_code, "slo") == 0)
    snprintf(language, language_size, "sk");
  else if (strcmp(iso_code, "pol") == 0)
    snprintf(language, language_size, "pl");
  else if (strcmp(iso_code, "hun") == 0)
    snprintf(language, language_size, "hu");
  else if (strcmp(iso_code, "fra") == 0 || strcmp(iso_code, "fre") == 0)
    snprintf(language, language_size, "fr");
  else if (strcmp(iso_code, "ita") == 0)
    snprintf(language, language_size, "it");
  else if (strcmp(iso_code, "spa") == 0)
    snprintf(language, language_size, "es");
  else if (strcmp(iso_code, "por") == 0)
    snprintf(language, language_size, "pt");
  else if (strcmp(iso_code, "rus") == 0)
    snprintf(language, language_size, "ru");
  else if (strcmp(iso_code, "ukr") == 0)
    snprintf(language, language_size, "uk");
  else if (strcmp(iso_code, "ron") == 0 || strcmp(iso_code, "rum") == 0)
    snprintf(language, language_size, "ro");
  else if (strcmp(iso_code, "nld") == 0 || strcmp(iso_code, "dut") == 0)
    snprintf(language, language_size, "nl");
  else
    snprintf(language, language_size, "%s", iso_code);
}

// Check whether a comma-separated compact list already contains one token.
static bool list_contains_token(const char *list, const char *token) {
  size_t token_len = strlen(token);
  const char *pointer = list;

  while (*pointer != '\0') {
    const char *token_end = strchr(pointer, ',');
    size_t current_len = token_end != NULL ? (size_t)(token_end - pointer) : strlen(pointer);
    if (current_len == token_len && strncmp(pointer, token, token_len) == 0)
      return true;

    if (token_end == NULL)
      break;

    pointer = token_end + 1;
  }

  return false;
}

// Append a unique comma-separated token if it fits completely.
static int append_unique_token(char *buffer, size_t buffer_size, const char *token) {
  if (buffer_size == 0 || token == NULL || token[0] == '\0')
    return 0;

  if (list_contains_token(buffer, token))
    return strlen(buffer);

  size_t current_len = strlen(buffer);
  size_t token_len = strlen(token);
  size_t separator_len = current_len > 0 ? 1 : 0;
  if (current_len + separator_len + token_len >= buffer_size)
    return current_len;

  if (separator_len > 0)
    strcat(buffer, ",");
  strcat(buffer, token);

  return strlen(buffer);
}

// Store one unique language token in a temporary ordered list.
static void add_unique_language(char languages[][4], int *language_count, int max_language_count, const char *language) {
  if (language == NULL || language[0] == '\0')
    return;

  for (int i = 0; i < *language_count; i++) {
    if (strcmp(languages[i], language) == 0)
      return;
  }

  if (*language_count >= max_language_count)
    return;

  snprintf(languages[*language_count], 4, "%s", language);
  (*language_count)++;
}

// Move preferred local languages to the front while keeping other languages stable.
static int ordered_language_count(char ordered_languages[][4], int max_language_count, char languages[][4], int language_count) {
  static const char *preferred_languages[] = {"cs", "sk", "de", "en"};
  int ordered_count = 0;

  for (unsigned int preferred_index = 0; preferred_index < sizeof(preferred_languages) / sizeof(preferred_languages[0]); preferred_index++) {
    for (int language_index = 0; language_index < language_count; language_index++) {
      if (strcmp(languages[language_index], preferred_languages[preferred_index]) == 0)
        add_unique_language(ordered_languages, &ordered_count, max_language_count, languages[language_index]);
    }
  }

  for (int language_index = 0; language_index < language_count; language_index++)
    add_unique_language(ordered_languages, &ordered_count, max_language_count, languages[language_index]);

  return ordered_count;
}

// Format at most three language tokens and append a compact +N overflow marker.
static int format_language_summary(char languages[][4], int language_count, char *buffer, size_t buffer_size) {
  char ordered_languages[16][4];
  int ordered_count = ordered_language_count(ordered_languages, 16, languages, language_count);
  int visible_count = ordered_count > 3 ? 3 : ordered_count;

  if (buffer_size == 0)
    return 0;

  buffer[0] = '\0';

  for (int i = 0; i < visible_count; i++)
    append_unique_token(buffer, buffer_size, ordered_languages[i]);

  if (ordered_count > visible_count) {
    char more_text[8];

    snprintf(more_text, sizeof(more_text), "+%d", ordered_count - visible_count);
    append_unique_token(buffer, buffer_size, more_text);
  }

  return strlen(buffer);
}

// Append a unique CA system/PID pair in compact form.
static void append_ca_descriptor(char *buffer, size_t buffer_size, const unsigned char *data, int descriptor_pointer) {
  char token[32];
  int ca_system_id = read_u16_be(data, descriptor_pointer + 2);
  int ca_pid = read_13_bit_pid(data, descriptor_pointer + 4);

  snprintf(token, sizeof(token), "0x%04x/%d", ca_system_id, ca_pid);
  append_unique_token(buffer, buffer_size, token);
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

// Return the MPEG-TS stream_type for one PMT stream entry.
int pmt_stream_type(struct dvb_data_s *dvb_data, int program_pid, int stream_index) {
  int pointer;

  if (!find_pmt_part(dvb_data, program_pid, stream_index, &pointer))
    return -1;

  return dvb_data->pid_data[program_pid].data[pointer];
}

// Return the PCR PID from the cached PMT section header.
int pmt_pcr_pid(struct dvb_data_s *dvb_data, int program_pid) {
  if (!demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return -1;

  return read_13_bit_pid(dvb_data->pid_data[program_pid].data, 8);
}

// Collect unique ISO 639 language codes from PMT stream descriptors.
int pmt_read_audio_languages(struct dvb_data_s *dvb_data, int program_pid, char *languages, size_t languages_size) {
  char collected_languages[16][4];
  int language_count = 0;

  if (languages == NULL || languages_size == 0)
    return 0;

  languages[0] = '\0';

  if (!demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return 0;

  const unsigned char *data = dvb_data->pid_data[program_pid].data;
  int stream_count = count_pmt_streams(dvb_data, program_pid);

  for (int stream_index = 0; stream_index < stream_count; stream_index++) {
    int descriptors_start;
    int descriptors_end;
    if (!find_pmt_stream_descriptors(dvb_data, program_pid, stream_index, &descriptors_start, &descriptors_end))
      continue;

    int descriptor_pointer = descriptors_start;

    while (descriptor_pointer + 2 <= descriptors_end) {
      int descriptor_tag = data[descriptor_pointer];
      int descriptor_length = data[descriptor_pointer + 1];
      int descriptor_end = descriptor_pointer + 2 + descriptor_length;
      if (descriptor_end > descriptors_end)
        break;

      if (descriptor_tag == PMT_ISO_639_LANGUAGE_DESCRIPTOR) {
        for (int language_pointer = descriptor_pointer + 2; language_pointer + 4 <= descriptor_end; language_pointer += 4) {
          char language[4];

          format_iso639_language_code(&data[language_pointer], language, sizeof(language));
          add_unique_language(collected_languages, &language_count, 16, language);
        }
      }

      descriptor_pointer = descriptor_end;
    }
  }

  return format_language_summary(collected_languages, language_count, languages, languages_size);
}

// Collect CA descriptors from PMT program and stream descriptor loops.
int pmt_read_ca_details(struct dvb_data_s *dvb_data, int program_pid, char *ca_details, size_t ca_details_size) {
  if (ca_details == NULL || ca_details_size == 0)
    return 0;

  ca_details[0] = '\0';

  if (!demux_has_pid_data(dvb_data, program_pid, PMT_SECT_HEADER_LEN))
    return 0;

  const unsigned char *data = dvb_data->pid_data[program_pid].data;
  int descriptors_start;
  int descriptors_end;

  if (find_pmt_program_descriptors(dvb_data, program_pid, &descriptors_start, &descriptors_end)) {
    int descriptor_pointer = descriptors_start;

    while (descriptor_pointer + 2 <= descriptors_end) {
      int descriptor_tag = data[descriptor_pointer];
      int descriptor_length = data[descriptor_pointer + 1];
      int descriptor_end = descriptor_pointer + 2 + descriptor_length;
      if (descriptor_end > descriptors_end)
        break;

      if (descriptor_tag == PMT_CA_DESCRIPTOR && descriptor_length >= 4)
        append_ca_descriptor(ca_details, ca_details_size, data, descriptor_pointer);

      descriptor_pointer = descriptor_end;
    }
  }

  int stream_count = count_pmt_streams(dvb_data, program_pid);

  for (int stream_index = 0; stream_index < stream_count; stream_index++) {
    if (!find_pmt_stream_descriptors(dvb_data, program_pid, stream_index, &descriptors_start, &descriptors_end))
      continue;

    int descriptor_pointer = descriptors_start;

    while (descriptor_pointer + 2 <= descriptors_end) {
      int descriptor_tag = data[descriptor_pointer];
      int descriptor_length = data[descriptor_pointer + 1];
      int descriptor_end = descriptor_pointer + 2 + descriptor_length;
      if (descriptor_end > descriptors_end)
        break;

      if (descriptor_tag == PMT_CA_DESCRIPTOR && descriptor_length >= 4)
        append_ca_descriptor(ca_details, ca_details_size, data, descriptor_pointer);

      descriptor_pointer = descriptor_end;
    }
  }

  return strlen(ca_details);
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

// Return the service_type from the SDT service descriptor.
int si_sdt_service_type(struct dvb_data_s *dvb_data, int service_index) {
  int descriptor_pointer;
  int descriptor_end;

  if (!find_sdt_service_descriptor(dvb_data, service_index, &descriptor_pointer, &descriptor_end))
    return -1;

  if (descriptor_pointer + 2 >= descriptor_end)
    return -1;

  return dvb_data->pid_data[SDT_PID].data[descriptor_pointer + 2];
}

// Extract the service provider name from one SDT service descriptor.
int si_read_sdt_service_provider_name(struct dvb_data_s *dvb_data, int service_index, char *provider_name) {
  int descriptor_pointer;
  int descriptor_end;

  if (!find_sdt_service_descriptor(dvb_data, service_index, &descriptor_pointer, &descriptor_end))
    return 0;

  int provider_name_length_pos = descriptor_pointer + 3;
  if (provider_name_length_pos >= descriptor_end)
    return 0;

  int provider_name_length = dvb_data->pid_data[SDT_PID].data[provider_name_length_pos];
  if (provider_name_length_pos + 1 + provider_name_length > descriptor_end)
    return 0;

  if (provider_name_length > DEMUX_PROVIDER_NAME_SIZE - 1)
    provider_name_length = DEMUX_PROVIDER_NAME_SIZE - 1;
  memcpy(provider_name, &dvb_data->pid_data[SDT_PID].data[provider_name_length_pos + 1], provider_name_length);

  return provider_name_length;
}

// Extract the service descriptor name for one SDT service entry.
int si_read_sdt_service_name(struct dvb_data_s *dvb_data, int service_index, char *service_name) {
  int descriptor_pointer;
  int descriptor_end;

  if (!find_sdt_service_descriptor(dvb_data, service_index, &descriptor_pointer, &descriptor_end))
    return 0;

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
