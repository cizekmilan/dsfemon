#ifndef DEMUX_DETAIL_VIEW_H
#define DEMUX_DETAIL_VIEW_H

#include "demux_monitor.h"

// Mutable state owned by the fullscreen demux detail view.
struct demux_detail_state {
  unsigned int selected_service;
  unsigned int scroll_offset;
  unsigned int service_count;
  unsigned int page_capacity;
  bool snapshot_valid;
  unsigned int snapshot_frontend;
  struct demux_snapshot snapshot;
};

void init_demux_detail_state(struct demux_detail_state *state);
void reset_demux_detail_state(struct demux_detail_state *state, unsigned int selected_frontend);
void demux_detail_select_next(struct demux_detail_state *state);
void demux_detail_select_previous(struct demux_detail_state *state);
void demux_detail_select_next_page(struct demux_detail_state *state);
void demux_detail_select_previous_page(struct demux_detail_state *state);
void demux_detail_select_first(struct demux_detail_state *state);
void demux_detail_select_last(struct demux_detail_state *state);

unsigned int render_demux_detail(struct demux_detail_state *state,
                                 struct dvb_data_s *selected_dvb_data,
                                 unsigned int selected_frontend,
                                 unsigned int frontend_count,
                                 unsigned int line,
                                 int footer_row);

#endif
