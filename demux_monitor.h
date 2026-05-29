#ifndef DEMUX_MONITOR_H
#define DEMUX_MONITOR_H

#include <pthread.h>

#define TS_SIZE 208
#define TS_MAX_PID 8191
#define TS_PID_COUNT (TS_MAX_PID + 1)
#define DEMUX_SECTION_DATA_SIZE (TS_SIZE * 5)
#define DEMUX_NETWORK_NAME_SIZE 100
#define DEMUX_PROVIDER_NAME_SIZE 100
#define DEMUX_SERVICE_NAME_SIZE 100
#define DEMUX_LANGUAGE_TEXT_SIZE 16
#define DEMUX_TELETEXT_TEXT_SIZE 16
#define DEMUX_SUBTITLE_TEXT_SIZE 16
#define DEMUX_CA_DETAIL_TEXT_SIZE 160
#define DEMUX_MAX_SERVICES 256
#define DEMUX_MAX_SERVICE_STREAMS 16
#define DEMUX_MAX_SDT_SECTIONS 256

// Raw PSI/SI section bytes cached per PID by the demux reader thread.
struct pid_data_s {
  unsigned char data[DEMUX_SECTION_DATA_SIZE];
  unsigned int len;
};

// One cached SDT section. Large service lists can span several SDT sections.
struct demux_section_s {
  unsigned char data[DEMUX_SECTION_DATA_SIZE];
  unsigned int len;
};

// Multi-section SDT cache keyed by transport stream/version metadata.
struct sdt_section_cache_s {
  int initialized;
  int complete;
  int table_id;
  int transport_stream_id;
  int version_number;
  int last_section_number;
  struct demux_section_s sections[DEMUX_MAX_SDT_SECTIONS];
};

// One discovered DVB frontend/demux pair and its demux reader state.
struct dvb_data_s {
  int adapter;
  int subadapter;
  int fefd;
  int defd;
  struct pid_data_s *pid_data;
  struct sdt_section_cache_s *sdt_cache;
  pthread_mutex_t data_lock;
  pthread_t demux_thread;
  int demux_thread_started;
  volatile int stop_demux_thread;
};

// One elementary stream copied from a service PMT.
struct demux_stream_snapshot {
  int pid;
  int type;
};

// One named service from the SDT, including fields shown by the detail view.
struct demux_service_snapshot {
  int service_id;
  int service_type;
  int program_pid;
  int pcr_pid;
  int stream_count;
  int stored_stream_count;
  int running_status;
  int free_ca_mode;
  int provider_name_len;
  int languages_len;
  int teletext_len;
  int subtitle_len;
  int ca_detail_len;
  int name_len;
  struct demux_stream_snapshot streams[DEMUX_MAX_SERVICE_STREAMS];
  char provider_name[DEMUX_PROVIDER_NAME_SIZE];
  char languages[DEMUX_LANGUAGE_TEXT_SIZE];
  char teletext[DEMUX_TELETEXT_TEXT_SIZE];
  char subtitles[DEMUX_SUBTITLE_TEXT_SIZE];
  char ca_detail[DEMUX_CA_DETAIL_TEXT_SIZE];
  char name[DEMUX_SERVICE_NAME_SIZE];
};

// Stable UI-facing copy of demux data collected under data_lock.
struct demux_snapshot {
  int network_name_len;
  char network_name[DEMUX_NETWORK_NAME_SIZE];
  int service_count;
  struct demux_service_snapshot services[DEMUX_MAX_SERVICES];
};

// Start, stop, and join the per-device demux reader thread.
int start_dvb_reader(struct dvb_data_s *dvb_data);
void request_dvb_reader_stop(struct dvb_data_s *dvb_data);
void join_dvb_reader(struct dvb_data_s *dvb_data);

// Copy the current NIT/SDT display data into a standalone snapshot for rendering.
int read_demux_snapshot(struct dvb_data_s *dvb_data, struct demux_snapshot *snapshot);

// PMT helpers used by the demux/service detail view.
int count_pmt_streams(struct dvb_data_s *dvb_data, int program_pid);
int pmt_stream_pid(struct dvb_data_s *dvb_data, int program_pid, int stream_index);

#endif
