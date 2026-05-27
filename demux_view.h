#ifndef DEMUX_VIEW_H
#define DEMUX_VIEW_H

#include "demux_monitor.h"

// Render the network name and service list for one demux snapshot.
int demux_main_info(struct dvb_data_s *dvb_data, unsigned int channel_offset_seed);

// Render the selectable row that opens the future demux/service detail view.
int detail_line(unsigned int frontend_index, bool selected, struct dvb_data_s *dvb_data);

#endif
