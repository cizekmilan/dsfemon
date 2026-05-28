#include "demux_internal.h"

#include <ctype.h>
#include <string.h>

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

// Build a display-ready demux snapshot without exposing live PID buffers to UI code.
int read_demux_snapshot(struct dvb_data_s *dvb_data, struct demux_snapshot *snapshot) {
  if (dvb_data == NULL || snapshot == NULL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));

  // Copy only display-ready data while holding the lock; rendering uses
  // the snapshot without touching the live demux buffers.
  pthread_mutex_lock(&dvb_data->data_lock);

  snapshot->network_name_len = si_read_nit_network_name(dvb_data, si_find_nit_pid(dvb_data), snapshot->network_name);
  snapshot->network_name_len = clean_si_text(snapshot->network_name, snapshot->network_name_len);

  int service_count = si_count_sdt_services(dvb_data);

  for (int service_index = 0; service_index < service_count && snapshot->service_count < DEMUX_MAX_SERVICES; service_index++) {
    struct demux_service_snapshot *service = &snapshot->services[snapshot->service_count];

    service->name_len = si_read_sdt_service_name(dvb_data, service_index, service->name);
    service->name_len = clean_si_text(service->name, service->name_len);
    if (service->name_len <= 0)
      continue;

    service->service_id = si_sdt_service_id(dvb_data, service_index);
    service->service_type = si_sdt_service_type(dvb_data, service_index);
    service->program_pid = si_find_program_pid(dvb_data, service->service_id);
    service->pcr_pid = pmt_pcr_pid(dvb_data, service->program_pid);
    service->stream_count = count_pmt_streams(dvb_data, service->program_pid);
    service->stored_stream_count = service->stream_count;
    if (service->stored_stream_count > DEMUX_MAX_SERVICE_STREAMS)
      service->stored_stream_count = DEMUX_MAX_SERVICE_STREAMS;

    for (int stream_index = 0; stream_index < service->stored_stream_count; stream_index++) {
      service->streams[stream_index].pid = pmt_stream_pid(dvb_data, service->program_pid, stream_index);
      service->streams[stream_index].type = pmt_stream_type(dvb_data, service->program_pid, stream_index);
    }

    service->running_status = si_sdt_service_running_status(dvb_data, service_index);
    service->free_ca_mode = si_sdt_service_free_ca_mode(dvb_data, service_index);
    service->provider_name_len = si_read_sdt_service_provider_name(dvb_data, service_index, service->provider_name);
    service->provider_name_len = clean_si_text(service->provider_name, service->provider_name_len);
    service->languages_len = pmt_read_audio_languages(dvb_data, service->program_pid, service->languages, sizeof(service->languages));
    service->ca_detail_len = pmt_read_ca_details(dvb_data, service->program_pid, service->ca_detail, sizeof(service->ca_detail));
    snapshot->service_count++;
  }

  pthread_mutex_unlock(&dvb_data->data_lock);

  return 0;
}
