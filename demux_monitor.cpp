#include "demux_internal.h"

// Validate MPEG-TS PID array indexes before touching pid_data.
int demux_valid_pid(int pid) {
  return pid >= 0 && pid <= TS_MAX_PID;
}

// Shared stop predicate used by blocking demux reads and the reader loop.
int demux_stop_requested(struct dvb_data_s *dvb_data) {
  return dvb_data == NULL || dvb_data->stop_demux_thread;
}

// Check that cached bytes for a PID exist and contain the requested minimum.
int demux_has_pid_data(struct dvb_data_s *dvb_data, int pid, unsigned int min_len) {
  return dvb_data != NULL &&
         dvb_data->pid_data != NULL &&
         demux_valid_pid(pid) &&
         dvb_data->pid_data[pid].len >= min_len;
}
