#ifndef FRONTEND_STATUS_CACHE_H
#define FRONTEND_STATUS_CACHE_H

#include "frontend_status.h"

struct dvb_data_s;
struct frontend_status_cache;

// Create a shared cache for frontend status snapshots owned by one worker thread.
struct frontend_status_cache *create_frontend_status_cache(struct dvb_data_s *dvb_data, int device_count, unsigned long long refresh_interval_us);

// Start the single background worker that reads frontend status ioctls.
int start_frontend_status_cache(struct frontend_status_cache *cache);

// Copy the last complete snapshot for one device index into caller-owned storage.
void copy_frontend_status_cache_snapshot(struct frontend_status_cache *cache, int device_index, struct frontend_status_snapshot *snapshot);

// Stop the worker, release the cache lock, and free cache memory.
void destroy_frontend_status_cache(struct frontend_status_cache *cache);

#endif
