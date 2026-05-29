/*
 * File role: background frontend status cache.
 *
 * Owns the single worker thread that reads opened frontends sequentially at
 * the configured refresh interval and publishes snapshots for the UI thread.
 */

#include "frontend_status_cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "demux_monitor.h"

// Worker shutdown polling cadence; short enough to keep quit responsive.
#define FRONTEND_STATUS_STOP_POLL_US 25000

// One cached frontend status snapshot published by the worker.
struct frontend_status_cache_entry {
  bool valid;
  struct frontend_status_snapshot snapshot;
};

// Worker-owned frontend status cache shared with the ncurses UI thread.
struct frontend_status_cache {
  pthread_mutex_t lock;
  pthread_t thread;
  int thread_started;
  volatile int stop_thread;
  struct dvb_data_s *dvb_data;
  int device_count;
  unsigned long long refresh_interval_us;
  struct frontend_status_cache_entry *entries;
};

// Monotonic microsecond clock used for the worker refresh interval.
static unsigned long long monotonic_time_us(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);

  return (unsigned long long)now.tv_sec * 1000000 + (unsigned long long)now.tv_nsec / 1000;
}

// Store one freshly collected snapshot without exposing partial writes to UI code.
static void store_frontend_status_cache_entry(struct frontend_status_cache *cache, int device_index, const struct frontend_status_snapshot *snapshot, bool valid) {
  if (cache == NULL || device_index < 0 || device_index >= cache->device_count)
    return;

  pthread_mutex_lock(&cache->lock);
  cache->entries[device_index].valid = valid;
  if (valid && snapshot != NULL)
    cache->entries[device_index].snapshot = *snapshot;
  pthread_mutex_unlock(&cache->lock);
}

// Sleep in short chunks so shutdown does not wait for the full refresh interval.
static void sleep_until_or_stopped(struct frontend_status_cache *cache, unsigned long long deadline_us) {
  while (cache != NULL && !cache->stop_thread) {
    unsigned long long now_us = monotonic_time_us();
    if (now_us >= deadline_us)
      return;

    unsigned long long remaining_us = deadline_us - now_us;
    if (remaining_us > FRONTEND_STATUS_STOP_POLL_US)
      remaining_us = FRONTEND_STATUS_STOP_POLL_US;
    usleep((unsigned int)remaining_us);
  }
}

// Background worker: collect frontend statuses sequentially and publish snapshots.
static void *frontend_status_worker(void *arg) {
  struct frontend_status_cache *cache = (struct frontend_status_cache *)arg;

  if (cache == NULL)
    return NULL;

  while (!cache->stop_thread) {
    unsigned long long next_refresh_us = monotonic_time_us() + cache->refresh_interval_us;

    for (int device_index = 0; device_index < cache->device_count && !cache->stop_thread; device_index++) {
      struct dvb_data_s *current_dvb_data = &cache->dvb_data[device_index];
      if (current_dvb_data->fefd < 0) {
        store_frontend_status_cache_entry(cache, device_index, NULL, false);
        continue;
      }

      struct frontend_status_snapshot snapshot;
      bool valid = read_frontend_status_snapshot(current_dvb_data->fefd, &snapshot) == 0;
      store_frontend_status_cache_entry(cache, device_index, &snapshot, valid);
    }

    sleep_until_or_stopped(cache, next_refresh_us);
  }

  return NULL;
}

struct frontend_status_cache *create_frontend_status_cache(struct dvb_data_s *dvb_data, int device_count, unsigned long long refresh_interval_us) {
  if (dvb_data == NULL || device_count <= 0)
    return NULL;

  struct frontend_status_cache *cache = (frontend_status_cache *)calloc(1, sizeof(*cache));
  if (cache == NULL)
    return NULL;

  cache->entries = (frontend_status_cache_entry *)calloc(device_count, sizeof(*cache->entries));
  if (cache->entries == NULL) {
    free(cache);

    return NULL;
  }

  cache->dvb_data = dvb_data;
  cache->device_count = device_count;
  cache->refresh_interval_us = refresh_interval_us;

  if (pthread_mutex_init(&cache->lock, NULL) != 0) {
    free(cache->entries);
    free(cache);

    return NULL;
  }

  return cache;
}

int start_frontend_status_cache(struct frontend_status_cache *cache) {
  if (cache == NULL)
    return -1;

  if (cache->thread_started)
    return 0;

  cache->stop_thread = 0;
  if (pthread_create(&cache->thread, NULL, frontend_status_worker, cache) != 0)
    return -1;

  cache->thread_started = 1;

  return 0;
}

static void stop_frontend_status_cache(struct frontend_status_cache *cache) {
  if (cache == NULL)
    return;

  cache->stop_thread = 1;
  if (cache->thread_started) {
    pthread_join(cache->thread, NULL);
    cache->thread_started = 0;
  }
}

void copy_frontend_status_cache_snapshot(struct frontend_status_cache *cache, int device_index, struct frontend_status_snapshot *snapshot) {
  if (snapshot == NULL)
    return;

  memset(snapshot, 0, sizeof(*snapshot));
  if (cache == NULL || device_index < 0 || device_index >= cache->device_count)
    return;

  pthread_mutex_lock(&cache->lock);
  if (cache->entries[device_index].valid)
    *snapshot = cache->entries[device_index].snapshot;
  pthread_mutex_unlock(&cache->lock);
}

void destroy_frontend_status_cache(struct frontend_status_cache *cache) {
  if (cache == NULL)
    return;

  stop_frontend_status_cache(cache);
  pthread_mutex_destroy(&cache->lock);
  free(cache->entries);
  free(cache);
}
