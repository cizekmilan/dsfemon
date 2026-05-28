#ifndef DEMUX_INTERNAL_H
#define DEMUX_INTERNAL_H

#include <stddef.h>

#include "demux_monitor.h"

int demux_valid_pid(int pid);
int demux_stop_requested(struct dvb_data_s *dvb_data);
int demux_has_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned int min_len);

int si_count_pat_programs(struct dvb_data_s *dvb_data);
int si_pat_program_pid(struct dvb_data_s *dvb_data, int pat_section);
int si_find_program_pid(struct dvb_data_s *dvb_data, int program_number);
int si_find_nit_pid(struct dvb_data_s *dvb_data);
int si_read_nit_network_name(struct dvb_data_s *dvb_data, int program_pid, char *network_name);
int si_count_sdt_services(struct dvb_data_s *dvb_data);
int si_sdt_service_id(struct dvb_data_s *dvb_data, int service_index);
int si_sdt_service_type(struct dvb_data_s *dvb_data, int service_index);
int si_sdt_service_running_status(struct dvb_data_s *dvb_data, int service_index);
int si_sdt_service_free_ca_mode(struct dvb_data_s *dvb_data, int service_index);
int si_read_sdt_service_provider_name(struct dvb_data_s *dvb_data, int service_index, char *provider_name);
int si_read_sdt_service_name(struct dvb_data_s *dvb_data, int service_index, char *service_name);
int pmt_pcr_pid(struct dvb_data_s *dvb_data, int program_pid);
int pmt_stream_type(struct dvb_data_s *dvb_data, int program_pid, int stream_index);
int pmt_read_audio_languages(struct dvb_data_s *dvb_data, int program_pid, char *languages, size_t languages_size);
int pmt_read_ca_details(struct dvb_data_s *dvb_data, int program_pid, char *ca_details, size_t ca_details_size);

#endif
