#pragma once
#include <stdint.h>
#include <sys/time.h>
void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval *));
void sntp_set_sync_interval(uint32_t interval);
void configTime(long offset, int daylight, const char *server);
