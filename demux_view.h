#ifndef DEMUX_VIEW_H
#define DEMUX_VIEW_H

#include <stdbool.h>

struct dvb_data_s;

// Render the network name and service list for one demux snapshot.
int demux_main_info(struct dvb_data_s *dvb_data, unsigned int channel_offset_seed);

// Render the selectable row that opens the demux/service detail view.
int detail_line(unsigned int frontend_index, bool selected);

#endif
