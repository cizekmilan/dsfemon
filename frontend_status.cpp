#include "frontend_status.h"

#include <string.h>
#include <sys/ioctl.h>

// Legacy signal/SNR ioctls still provide stable relative values for bars.
// DVBv5 statistics are collected alongside them for modern numeric labels.
int read_frontend_status_snapshot(int fd, struct frontend_status_snapshot *snapshot) {
  if (fd < 0 || snapshot == NULL)
    return -1;

  memset(snapshot, 0, sizeof(*snapshot));

  snapshot->has_info = ioctl(fd, FE_GET_INFO, &snapshot->info) >= 0;
  snapshot->has_status = ioctl(fd, FE_READ_STATUS, &snapshot->status) >= 0;
  snapshot->has_legacy_signal = ioctl(fd, FE_READ_SIGNAL_STRENGTH, &snapshot->legacy_signal) >= 0;
  snapshot->has_legacy_snr = ioctl(fd, FE_READ_SNR, &snapshot->legacy_snr) >= 0;
  snapshot->has_v5 = read_frontend_v5_snapshot(fd, &snapshot->v5) >= 0;
  snapshot->has_legacy_ber = ioctl(fd, FE_READ_BER, &snapshot->legacy_ber) >= 0;
  snapshot->has_legacy_uncorrected_blocks = ioctl(fd, FE_READ_UNCORRECTED_BLOCKS, &snapshot->legacy_uncorrected_blocks) >= 0;

  return 0;
}
