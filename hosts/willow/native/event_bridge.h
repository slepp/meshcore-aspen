#ifndef MC_EVENT_BRIDGE_H
#define MC_EVENT_BRIDGE_H
#include <stdint.h>
int64_t mc_events_new(void);
void mc_events_retain(int64_t handle);
void mc_events_release(int64_t handle);
int32_t mc_events_wake(int64_t handle);
int32_t mc_events_stop(int64_t handle);
int64_t mc_events_add(int64_t handle, int32_t fd, int32_t flags);
int32_t mc_events_arm(int64_t handle, int64_t key, int32_t flags);
int32_t mc_events_remove(int64_t handle, int64_t key);
int32_t mc_events_descriptor(int64_t handle);
#endif
